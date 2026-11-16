#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Zero-data-loss pinned F2FS dm-linear allocator for RedMagic 9 Pro (NX769J).

Why this exists:
  Android's `/data` partition uses encrypted F2FS. Instead of repartitioning
  UFS (risking brick or wiping Android userdata), Omarchy Mobile stores its
  root filesystem inside one or more F2FS-pinned files on `/data/nxlinux/`:
    1) Initial rootfs image (e.g. `rootfs.img`, 100 GiB)
    2) Optional expansion segment (`rootfs_ext2.img`, +156 GiB -> 256 GiB total)

  `nx_dm_tool pin-alloc` calls `FS_IOC_SET_PIN_FILE(1)` + `fallocate` before
  extracting physical UFS `/dev/sda12` sector extents via `FS_IOC_FIEMAP`.
  Those immutable physical sector runs are embedded as `.extents` inside the
  initramfs so Linux mounts `/dev/mapper/nxroot` directly over `dm-linear`
  without touching Android FBE encryption keys or mounted F2FS metadata.

Safety:
  - Runs in offline `--dry-run` mode by default unless `--execute` is passed.
  - Requires explicit `--serial` when performing live ADB operations.
  - Refuses to proceed if existing `rootfs.img` extents changed or if `/data`
    free space after allocation would drop below `--min-free-gib` (default 45 GiB).
  - Verifies zero physical sector overlap across base + extension extents and
    exact prefix preservation before writing `rootfs_256g.extents`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Any

SECTOR_SIZE = 512
GIB = 1024 ** 3
SECTORS_PER_GIB = GIB // SECTOR_SIZE
DEFAULT_MIN_FREE_GIB = 45
DEFAULT_MAX_EXTENTS = 4096

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_NX_DM_TOOL = REPO_ROOT / "storage-dualboot" / "nx_dm_tool"
DEFAULT_SLOTCTL_BIN = REPO_ROOT / "storage-dualboot" / "nx769j-slotctl"


class ExtentValidationError(RuntimeError):
    """Raised when extent syntax, continuity, or physical non-overlap fails."""


class InsufficientDataSpaceError(RuntimeError):
    """Raised when Android /data cannot preserve the safety headroom."""


def resolve_adb(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("ADB")
    if env:
        return env
    found = shutil.which("adb")
    if not found:
        raise FileNotFoundError("`adb` not found in PATH (pass --adb /path/to/adb)")
    return found


def select_adb_serial(adb_bin: str, explicit_serial: str | None) -> str:
    devs = subprocess.check_output([adb_bin, "devices"], text=True)
    online: list[str] = []
    for ln in devs.splitlines()[1:]:
        parts = ln.split()
        if len(parts) >= 2 and parts[1] == "device":
            online.append(parts[0])
    if explicit_serial:
        if explicit_serial not in online:
            raise RuntimeError(
                f"Target ADB serial {explicit_serial!r} is not online (online: {online})"
            )
        return explicit_serial
    if len(online) == 1:
        return online[0]
    if not online:
        raise RuntimeError("No online ADB devices detected (`adb devices`).")
    raise RuntimeError(
        f"Multiple ADB devices connected ({online}); pass `--serial <SERIAL>` explicitly."
    )


def parse_extents(
    text: str,
    expected_sectors: int | None = None,
    label: str = "extents",
) -> list[tuple[int, int, int]]:
    """Parse `logical_sector length_sectors physical_sector` table and check continuity."""
    rows: list[tuple[int, int, int]] = []
    cur_logical = 0
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 3:
            raise ExtentValidationError(
                f"{label}:{lineno}: expected 3 fields, got {len(parts)}: {raw!r}"
            )
        logical, length, phys = (int(parts[0]), int(parts[1]), int(parts[2]))
        if logical != cur_logical:
            raise ExtentValidationError(
                f"{label}:{lineno}: non-contiguous logical sector {logical} != expected {cur_logical}"
            )
        if length <= 0 or phys <= 0:
            raise ExtentValidationError(
                f"{label}:{lineno}: invalid extent length/phys ({length}, {phys})"
            )
        rows.append((logical, length, phys))
        cur_logical += length

    if not rows:
        raise ExtentValidationError(f"{label}: extent table is empty")
    if expected_sectors is not None and cur_logical != expected_sectors:
        raise ExtentValidationError(
            f"{label}: total sectors {cur_logical} != expected {expected_sectors}"
        )
    return rows


def format_extents(rows: list[tuple[int, int, int]]) -> str:
    return "\n".join(f"{l} {ln} {p}" for (l, ln, p) in rows) + "\n"


def check_free_space_budget(
    free_bytes: int,
    add_bytes: int,
    base_gib: int,
    min_after_bytes: int,
    already_allocated_bytes: int = 0,
) -> dict[str, Any]:
    if free_bytes < 0 or already_allocated_bytes < 0:
        raise ValueError("free_bytes and already_allocated_bytes must be non-negative")
    needed_new_bytes = max(0, add_bytes - already_allocated_bytes)
    remaining_after = free_bytes - needed_new_bytes
    usable_pool = free_bytes + already_allocated_bytes - min_after_bytes
    max_safe_add_gib = max(0, usable_pool // GIB)
    max_safe_total_gib = base_gib + max_safe_add_gib
    info = {
        "free_bytes_before": free_bytes,
        "free_gib_before": round(free_bytes / GIB, 2),
        "already_allocated_bytes": already_allocated_bytes,
        "needed_new_bytes": needed_new_bytes,
        "remaining_bytes_after": remaining_after,
        "remaining_gib_after": round(remaining_after / GIB, 2),
        "min_required_after_gib": round(min_after_bytes / GIB, 2),
        "max_safe_add_gib": int(max_safe_add_gib),
        "max_safe_total_gib": int(max_safe_total_gib),
    }
    if remaining_after < min_after_bytes:
        raise InsufficientDataSpaceError(
            f"/data free space insufficient for +{add_bytes // GIB} GiB while keeping "
            f">= {info['min_required_after_gib']} GiB free: "
            f"free_before={info['free_gib_before']} GiB, remaining_after={info['remaining_gib_after']} GiB. "
            f"Maximum safe rootfs size on current /data: {max_safe_total_gib} GiB (+{max_safe_add_gib} GiB)."
        )
    return info


def merge_and_validate_extents(
    live_base_text: str,
    ext2_text: str,
    expected_base_text: str,
    base_sectors: int,
    add_sectors: int,
    max_phys_sectors: int | None = None,
    max_extents: int = DEFAULT_MAX_EXTENTS,
) -> str:
    if live_base_text != expected_base_text:
        h_live = hashlib.sha256(live_base_text.encode("utf-8")).hexdigest()
        h_exp = hashlib.sha256(expected_base_text.encode("utf-8")).hexdigest()
        raise ExtentValidationError(
            f"CRITICAL: live /data/nxlinux/rootfs.img FIEMAP ({h_live[:16]}...) does NOT "
            f"byte-match existing rootfs.extents ({h_exp[:16]}...). Refusing to proceed."
        )

    base_extents = parse_extents(live_base_text, expected_sectors=base_sectors, label="rootfs.img")
    ext2_raw = parse_extents(ext2_text, expected_sectors=add_sectors, label="rootfs_ext2.img")

    shifted_ext2 = [(l + base_sectors, ln, p) for (l, ln, p) in ext2_raw]
    combined = base_extents + shifted_ext2

    if len(combined) > max_extents:
        raise ExtentValidationError(
            f"Combined extent count {len(combined)} exceeds safety limit {max_extents}"
        )

    total_sectors = base_sectors + add_sectors
    combined_text = format_extents(combined)
    reparsed = parse_extents(combined_text, expected_sectors=total_sectors, label="merged.extents")

    phys_intervals: list[tuple[int, int, str, int]] = []
    for l, ln, p in base_extents:
        if max_phys_sectors is not None and p + ln > max_phys_sectors:
            raise ExtentValidationError(
                f"rootfs.img extent [{p}..{p + ln}) exceeds userdata sectors {max_phys_sectors}"
            )
        phys_intervals.append((p, p + ln, "rootfs.img", l))
    for l, ln, p in shifted_ext2:
        if max_phys_sectors is not None and p + ln > max_phys_sectors:
            raise ExtentValidationError(
                f"rootfs_ext2.img extent [{p}..{p + ln}) exceeds userdata sectors {max_phys_sectors}"
            )
        phys_intervals.append((p, p + ln, "rootfs_ext2.img", l))

    phys_intervals.sort(key=lambda item: (item[0], item[1]))
    for i in range(1, len(phys_intervals)):
        ps, pe, psrc, pl = phys_intervals[i - 1]
        cs, ce, csrc, cl = phys_intervals[i]
        if pe > cs:
            raise ExtentValidationError(
                f"CRITICAL physical sector overlap between {psrc}@logical={pl} [{ps}..{pe}) "
                f"and {csrc}@logical={cl} [{cs}..{ce})!"
            )

    if reparsed[: len(base_extents)] != base_extents:
        raise ExtentValidationError("Prefix extents mutated unexpectedly")

    return combined_text


def build_android_reboot_to_omarchy_script() -> str:
    return """#!/system/bin/sh
# ==============================================================================
# Reboot-to-Omarchy.sh — Switch Active Boot Slot to _a (Omarchy Mobile) & Reboot
#
# Usage from Android (Termux / ADB shell / Magisk root shell):
#   su -c /data/nxlinux/reboot-to-omarchy.sh [--dry-run] [--no-reboot]
# ==============================================================================
set -e

DRY_RUN=0
NO_REBOOT=0
for arg in "$@"; do
  case "$arg" in
    --dry-run)   DRY_RUN=1 ;;
    --no-reboot) NO_REBOOT=1 ;;
    -h|--help)
      echo "Usage: su -c /data/nxlinux/reboot-to-omarchy.sh [--dry-run] [--no-reboot]"
      exit 0
      ;;
    *)
      echo "Unknown option: $arg" >&2
      exit 1
      ;;
  esac
done

if [ "$(id -u)" != "0" ]; then
  exec su -c "/system/bin/sh \\"$0\\" $*"
fi

SLOTCTL="/data/nxlinux/nx769j-slotctl"
if [ ! -x "$SLOTCTL" ]; then
  SLOTCTL="/data/local/tmp/nx769j-slotctl"
fi

if [ "$DRY_RUN" = "1" ]; then
  echo "[Reboot-to-Omarchy] Dry-run status check:"
  if command -v bootctl >/dev/null 2>&1; then
    echo "  bootctl current-slot: $(bootctl get-current-slot 2>/dev/null || echo unknown)"
  fi
  if [ -x "$SLOTCTL" ]; then
    "$SLOTCTL" status
  fi
  exit 0
fi

echo "[Reboot-to-Omarchy] Switching active boot slot to Slot A (0 = Omarchy Mobile)..."
SWITCHED=0
if command -v bootctl >/dev/null 2>&1; then
  if bootctl set-active-boot-slot 0; then
    SWITCHED=1
  fi
fi

if [ -x "$SLOTCTL" ]; then
  if [ "$SWITCHED" = "0" ]; then
    "$SLOTCTL" set-active a
    SWITCHED=1
  else
    ACT=$("$SLOTCTL" -a 2>/dev/null || true)
    if [ "$ACT" != "_a" ]; then
      "$SLOTCTL" set-active a
    fi
  fi
fi

if [ "$SWITCHED" != "1" ]; then
  echo "[Reboot-to-Omarchy] ERROR: Neither bootctl nor $SLOTCTL could switch to slot _a!" >&2
  exit 1
fi

sync
if [ "$NO_REBOOT" = "1" ]; then
  echo "[Reboot-to-Omarchy] Slot _a is now active (--no-reboot)."
  exit 0
fi

echo "[Reboot-to-Omarchy] Rebooting into Omarchy Mobile..."
svc power reboot || reboot
"""


def build_magisk_omarchy_helper_script() -> str:
    return """#!/system/bin/sh
# Magisk late_start service helper: keeps switchers in /data/local/tmp and /sdcard synced.
mkdir -p /data/local/tmp 2>/dev/null || true
if [ -f /data/nxlinux/nx769j-slotctl ]; then
  cp -f /data/nxlinux/nx769j-slotctl /data/local/tmp/nx769j-slotctl 2>/dev/null || true
  chmod 0755 /data/local/tmp/nx769j-slotctl 2>/dev/null || true
fi
if [ -f /data/nxlinux/reboot-to-omarchy.sh ]; then
  cp -f /data/nxlinux/reboot-to-omarchy.sh /data/local/tmp/reboot-to-omarchy.sh 2>/dev/null || true
  chmod 0755 /data/local/tmp/reboot-to-omarchy.sh 2>/dev/null || true
fi
(
  i=0
  while [ "$i" -lt 300 ]; do
    if [ -d /sdcard/Download ] && [ -f /data/nxlinux/reboot-to-omarchy.sh ]; then
      cp -f /data/nxlinux/reboot-to-omarchy.sh /sdcard/Reboot-to-Omarchy.sh 2>/dev/null && break
    fi
    i=$(( i + 1 ))
    sleep 2
  done
) >/dev/null 2>&1 &
exit 0
"""


def adb_run(
    adb_bin: str,
    serial: str,
    args: list[str],
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [adb_bin, "-s", serial] + args,
        check=check,
        text=True,
        capture_output=True,
    )


def adb_su(adb_bin: str, serial: str, shell_cmd: str, check: bool = True) -> str:
    res = adb_run(adb_bin, serial, ["shell", f"su -c {json.dumps(shell_cmd)}"], check=check)
    return res.stdout


def verify_adb_target(adb_bin: str, serial: str) -> None:
    uid_out = adb_su(adb_bin, serial, "id -u").strip()
    if uid_out != "0":
        raise RuntimeError(f"Magisk root check (`su -c 'id -u'`) failed on {serial}: got {uid_out!r}")


def query_userdata_sectors(adb_bin: str, serial: str) -> int | None:
    out = adb_su(
        adb_bin,
        serial,
        "blockdev --getsz /dev/block/by-name/userdata 2>/dev/null || true",
        check=False,
    ).strip()
    if re.fullmatch(r"\d+", out):
        return int(out)
    return None


def stage_helpers_and_switchers(
    adb_bin: str,
    serial: str,
    nx_dm_tool_path: pathlib.Path,
    slotctl_path: pathlib.Path,
) -> tuple[bool, bool]:
    adb_run(adb_bin, serial, ["push", str(nx_dm_tool_path), "/data/local/tmp/nx_dm_tool"])
    adb_run(adb_bin, serial, ["push", str(slotctl_path), "/data/local/tmp/nx769j-slotctl"])
    adb_su(
        adb_bin,
        serial,
        "mkdir -p /data/nxlinux && "
        "cp -f /data/local/tmp/nx_dm_tool /data/nxlinux/nx_dm_tool && "
        "cp -f /data/local/tmp/nx769j-slotctl /data/nxlinux/nx769j-slotctl && "
        "chmod 0755 /data/local/tmp/nx_dm_tool /data/local/tmp/nx769j-slotctl "
        "/data/nxlinux/nx_dm_tool /data/nxlinux/nx769j-slotctl",
    )
    with tempfile.TemporaryDirectory(prefix="nx769j_sw_") as tmpdir:
        tmp_sh = pathlib.Path(tmpdir) / "reboot-to-omarchy.sh"
        tmp_sh.write_text(build_android_reboot_to_omarchy_script(), encoding="utf-8")
        tmp_mg = pathlib.Path(tmpdir) / "99-nx769j-omarchy-helper.sh"
        tmp_mg.write_text(build_magisk_omarchy_helper_script(), encoding="utf-8")
        adb_run(adb_bin, serial, ["push", str(tmp_sh), "/data/local/tmp/reboot-to-omarchy.sh"])
        adb_run(adb_bin, serial, ["push", str(tmp_mg), "/data/local/tmp/99-nx769j-omarchy-helper.sh"])
        stage_out = adb_su(
            adb_bin,
            serial,
            "cp -f /data/local/tmp/reboot-to-omarchy.sh /data/nxlinux/reboot-to-omarchy.sh && "
            "chmod 0755 /data/local/tmp/reboot-to-omarchy.sh /data/nxlinux/reboot-to-omarchy.sh && "
            "SD_OK=0; if cp -f /data/nxlinux/reboot-to-omarchy.sh /sdcard/Reboot-to-Omarchy.sh 2>/dev/null; then SD_OK=1; fi; "
            "MG_OK=0; if [ -d /data/adb/service.d ]; then "
            "  cp -f /data/local/tmp/99-nx769j-omarchy-helper.sh /data/adb/service.d/99-nx769j-omarchy-helper.sh && "
            "  chmod 0755 /data/adb/service.d/99-nx769j-omarchy-helper.sh && MG_OK=1; fi; "
            "rm -f /data/local/tmp/99-nx769j-omarchy-helper.sh && sync && "
            'echo "SD_OK=$SD_OK MG_OK=$MG_OK"',
        ).strip()
    return ("SD_OK=1" in stage_out, "MG_OK=1" in stage_out)


def execute_create_initial(
    adb_bin: str,
    serial: str,
    size_gib: int,
    min_free_gib: int,
    out_extents_path: pathlib.Path,
    nx_dm_tool_path: pathlib.Path,
    slotctl_path: pathlib.Path,
) -> dict[str, Any]:
    verify_adb_target(adb_bin, serial)
    sdcard_ok, magisk_ok = stage_helpers_and_switchers(adb_bin, serial, nx_dm_tool_path, slotctl_path)

    size_bytes = size_gib * GIB
    size_sectors = size_gib * SECTORS_PER_GIB
    statfs_out = adb_su(adb_bin, serial, "stat -f -c '%a %S' /data").strip().split()
    free_bytes = int(statfs_out[0]) * int(statfs_out[1])
    space_info = check_free_space_budget(
        free_bytes=free_bytes,
        add_bytes=size_bytes,
        base_gib=0,
        min_after_bytes=min_free_gib * GIB,
    )

    exists = adb_su(
        adb_bin, serial, "if [ -e /data/nxlinux/rootfs.img ]; then echo YES; else echo NO; fi"
    ).strip()
    if exists == "YES":
        raise RuntimeError(
            "/data/nxlinux/rootfs.img already exists on device! "
            "Use `--mode expand` to extend it, or inspect it manually before replacing."
        )

    alloc_out = adb_su(
        adb_bin,
        serial,
        f"/data/local/tmp/nx_dm_tool pin-alloc /data/nxlinux/rootfs.img {size_bytes} "
        f"> /data/nxlinux/rootfs.extents && chmod 0644 /data/nxlinux/rootfs.extents && sync && "
        f"cat /data/nxlinux/rootfs.extents",
    )
    fiemap_out = adb_su(adb_bin, serial, "/data/local/tmp/nx_dm_tool fiemap /data/nxlinux/rootfs.img")
    if fiemap_out != alloc_out:
        raise ExtentValidationError("Post-sync FIEMAP differed from pin-alloc output!")
    extents = parse_extents(fiemap_out, expected_sectors=size_sectors, label="rootfs.img")

    out_extents_path.parent.mkdir(parents=True, exist_ok=True)
    out_extents_path.write_text(fiemap_out, encoding="utf-8")
    return {
        "status": "initial_rootfs_pinned",
        "serial": serial,
        "total_gib": size_gib,
        "total_sectors": size_sectors,
        "extent_count": len(extents),
        "out_extents_path": str(out_extents_path),
        "extents_sha256": hashlib.sha256(fiemap_out.encode("utf-8")).hexdigest(),
        "space_budget": space_info,
        "sdcard_copied": sdcard_ok,
        "magisk_helper_installed": magisk_ok,
    }


def execute_live_expansion(
    adb_bin: str,
    serial: str,
    base_extents_path: pathlib.Path,
    out_extents_path: pathlib.Path,
    nx_dm_tool_path: pathlib.Path,
    slotctl_path: pathlib.Path,
    base_gib: int,
    add_gib: int,
    min_free_gib: int,
) -> dict[str, Any]:
    base_sectors = base_gib * SECTORS_PER_GIB
    add_bytes = add_gib * GIB
    add_sectors = add_gib * SECTORS_PER_GIB
    total_sectors = base_sectors + add_sectors
    min_after_bytes = min_free_gib * GIB

    expected_base_text = base_extents_path.read_text(encoding="utf-8")
    parse_extents(expected_base_text, expected_sectors=base_sectors, label=str(base_extents_path))

    verify_adb_target(adb_bin, serial)
    max_phys_sectors = query_userdata_sectors(adb_bin, serial)
    sdcard_copied, magisk_helper_installed = stage_helpers_and_switchers(
        adb_bin, serial, nx_dm_tool_path, slotctl_path
    )

    live_base_text = adb_su(adb_bin, serial, "/data/local/tmp/nx_dm_tool fiemap /data/nxlinux/rootfs.img")
    if live_base_text != expected_base_text:
        raise ExtentValidationError(
            "CRITICAL: live /data/nxlinux/rootfs.img FIEMAP does NOT match host rootfs.extents!"
        )
    dev_extents_check = adb_su(adb_bin, serial, "cat /data/nxlinux/rootfs.extents")
    if dev_extents_check != expected_base_text:
        raise ExtentValidationError(
            "CRITICAL: /data/nxlinux/rootfs.extents on device differs from host rootfs.extents!"
        )

    existing_size_str = adb_su(
        adb_bin,
        serial,
        "if [ -f /data/nxlinux/rootfs_ext2.img ]; then stat -c %s /data/nxlinux/rootfs_ext2.img; else echo 0; fi",
    ).strip()
    existing_size = int(existing_size_str or "0")
    reuse_ext2 = False
    ext2_text = ""

    if existing_size == add_bytes:
        probe = adb_run(
            adb_bin,
            serial,
            ["shell", 'su -c "/data/local/tmp/nx_dm_tool fiemap /data/nxlinux/rootfs_ext2.img"'],
            check=False,
        )
        if probe.returncode == 0:
            try:
                parse_extents(probe.stdout, expected_sectors=add_sectors, label="existing rootfs_ext2.img")
                reuse_ext2 = True
                ext2_text = probe.stdout
            except ExtentValidationError:
                reuse_ext2 = False

    if existing_size > 0 and not reuse_ext2:
        adb_su(adb_bin, serial, "rm -f /data/nxlinux/rootfs_ext2.img && sync")

    statfs_out = adb_su(adb_bin, serial, "stat -f -c '%a %S' /data").strip().split()
    free_bytes = int(statfs_out[0]) * int(statfs_out[1])
    space_info = check_free_space_budget(
        free_bytes=free_bytes,
        add_bytes=add_bytes,
        base_gib=base_gib,
        min_after_bytes=min_after_bytes,
        already_allocated_bytes=(add_bytes if reuse_ext2 else 0),
    )

    if not reuse_ext2:
        alloc_out = adb_su(
            adb_bin,
            serial,
            f"/data/local/tmp/nx_dm_tool pin-alloc /data/nxlinux/rootfs_ext2.img {add_bytes} "
            f"> /data/nxlinux/rootfs_ext2.extents && chmod 0644 /data/nxlinux/rootfs_ext2.extents && sync && "
            f"cat /data/nxlinux/rootfs_ext2.extents",
        )
        ext2_text = adb_su(adb_bin, serial, "/data/local/tmp/nx_dm_tool fiemap /data/nxlinux/rootfs_ext2.img")
        if ext2_text != alloc_out:
            raise ExtentValidationError("Post-sync FIEMAP of rootfs_ext2.img differed from pin-alloc output!")

    merged_text = merge_and_validate_extents(
        live_base_text=live_base_text,
        ext2_text=ext2_text,
        expected_base_text=expected_base_text,
        base_sectors=base_sectors,
        add_sectors=add_sectors,
        max_phys_sectors=max_phys_sectors,
    )

    out_extents_path.parent.mkdir(parents=True, exist_ok=True)
    out_extents_path.write_text(merged_text, encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix="nx769j_ext_") as tmpdir:
        tmp_ext = pathlib.Path(tmpdir) / "rootfs_merged.extents"
        tmp_ext.write_text(merged_text, encoding="utf-8")
        adb_run(adb_bin, serial, ["push", str(tmp_ext), "/data/local/tmp/rootfs_256g.extents"])
        adb_su(
            adb_bin,
            serial,
            "cp -f /data/local/tmp/rootfs_256g.extents /data/nxlinux/rootfs_256g.extents && "
            "chmod 0644 /data/nxlinux/rootfs_256g.extents && sync",
        )

    post_statfs = adb_su(adb_bin, serial, "stat -f -c '%a %S' /data").strip().split()
    free_after_bytes = int(post_statfs[0]) * int(post_statfs[1])
    if free_after_bytes < min_after_bytes:
        raise InsufficientDataSpaceError(
            f"Post-allocation /data free space dropped below {min_free_gib} GiB: "
            f"{free_after_bytes / GIB:.2f} GiB"
        )

    merged_extents = parse_extents(merged_text, expected_sectors=total_sectors, label="merged")
    ext2_extents = parse_extents(ext2_text, expected_sectors=add_sectors, label="ext2")
    return {
        "status": f"expanded_{base_gib + add_gib}g_ready",
        "serial": serial,
        "reused_existing_ext2": reuse_ext2,
        "base_extents": len(parse_extents(expected_base_text)),
        "added_extents": len(ext2_extents),
        "combined_extents": len(merged_extents),
        "total_sectors": total_sectors,
        "total_gib": base_gib + add_gib,
        "free_gib_after": round(free_after_bytes / GIB, 2),
        "space_budget": space_info,
        "out_extents_path": str(out_extents_path),
        "merged_sha256": hashlib.sha256(merged_text.encode("utf-8")).hexdigest(),
        "sdcard_copied": sdcard_copied,
        "magisk_helper_installed": magisk_helper_installed,
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Pinned F2FS dm-linear allocator (create initial rootfs or losslessly expand 100 GiB -> 256 GiB)"
    )
    ap.add_argument("--mode", choices=["expand", "create"], default="expand", help="Operation mode (default: expand)")
    ap.add_argument("--serial", default=None, help="Target ADB serial (auto-selected only if exactly 1 device)")
    ap.add_argument("--adb", default=None, help="Optional explicit path to adb binary")
    ap.add_argument("--base-gib", type=int, default=100, help="Existing base rootfs size in GiB for expand (default: 100)")
    ap.add_argument("--add-gib", type=int, default=156, help="Additional size in GiB for expand (default: 156)")
    ap.add_argument("--size-gib", type=int, default=100, help="Initial image size in GiB for create mode (default: 100)")
    ap.add_argument("--min-free-gib", type=int, default=DEFAULT_MIN_FREE_GIB, help="Minimum free headroom on /data (default: 45 GiB)")
    ap.add_argument("--base-extents", default="rootfs.extents", help="Existing base rootfs.extents path (expand mode)")
    ap.add_argument("--out-extents", default="rootfs_256g.extents", help="Output merged extents path")
    ap.add_argument("--nx-dm-tool", default=str(DEFAULT_NX_DM_TOOL), help="Static ARM64 nx_dm_tool binary path")
    ap.add_argument("--slotctl", default=str(DEFAULT_SLOTCTL_BIN), help="Static ARM64 nx769j-slotctl binary path")
    ap.add_argument(
        "--execute",
        "--apply",
        dest="execute",
        action="store_true",
        help="Execute live allocation over ADB (default is safe offline --dry-run)",
    )
    args = ap.parse_args()

    out_path = pathlib.Path(args.out_extents)
    dm_tool_path = pathlib.Path(args.nx_dm_tool)
    slotctl_path = pathlib.Path(args.slotctl)

    if args.mode == "expand":
        base_path = pathlib.Path(args.base_extents)
        if not base_path.is_file():
            print(f"ERROR: missing base extents file: {base_path}", file=sys.stderr)
            return 1
        base_text = base_path.read_text(encoding="utf-8")
        base_extents = parse_extents(
            base_text, expected_sectors=args.base_gib * SECTORS_PER_GIB, label=str(base_path)
        )
        if not args.execute:
            print(
                json.dumps(
                    {
                        "mode": "dry-run-expand",
                        "target_serial": args.serial or "<auto-if-single>",
                        "base_extents_path": str(base_path),
                        "base_extents_count": len(base_extents),
                        "base_gib": args.base_gib,
                        "add_gib": args.add_gib,
                        "target_total_gib": args.base_gib + args.add_gib,
                        "min_free_after_gib": args.min_free_gib,
                        "out_extents_path": str(out_path),
                        "nx_dm_tool_exists": dm_tool_path.is_file(),
                        "slotctl_exists": slotctl_path.is_file(),
                    },
                    indent=2,
                )
            )
            return 0
    else:
        if not args.execute:
            print(
                json.dumps(
                    {
                        "mode": "dry-run-create",
                        "target_serial": args.serial or "<auto-if-single>",
                        "size_gib": args.size_gib,
                        "min_free_after_gib": args.min_free_gib,
                        "out_extents_path": str(out_path),
                        "nx_dm_tool_exists": dm_tool_path.is_file(),
                        "slotctl_exists": slotctl_path.is_file(),
                    },
                    indent=2,
                )
            )
            return 0

    for label, p in [("nx_dm_tool", dm_tool_path), ("nx769j-slotctl", slotctl_path)]:
        if not p.is_file():
            print(f"ERROR: missing static ARM64 {label}: {p} (compile from storage-dualboot/)", file=sys.stderr)
            return 1

    adb_bin = resolve_adb(args.adb)
    serial = select_adb_serial(adb_bin, args.serial)

    if args.mode == "create":
        res = execute_create_initial(
            adb_bin=adb_bin,
            serial=serial,
            size_gib=args.size_gib,
            min_free_gib=args.min_free_gib,
            out_extents_path=out_path,
            nx_dm_tool_path=dm_tool_path,
            slotctl_path=slotctl_path,
        )
    else:
        res = execute_live_expansion(
            adb_bin=adb_bin,
            serial=serial,
            base_extents_path=pathlib.Path(args.base_extents),
            out_extents_path=out_path,
            nx_dm_tool_path=dm_tool_path,
            slotctl_path=slotctl_path,
            base_gib=args.base_gib,
            add_gib=args.add_gib,
            min_free_gib=args.min_free_gib,
        )
    print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
