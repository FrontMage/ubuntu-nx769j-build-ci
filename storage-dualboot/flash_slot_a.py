#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Slot-A Permanent Dual-Boot Flasher for RedMagic 9 Pro (NX769J).

Keeps Android intact on Slot B (`*_b`) while flashing ONLY:
  1. `vbmeta_a` <- unsigned AVB disabled image (`flags=2`, built from stock `vbmeta_a.img`)
  2. `boot_a`   <- verified `uboot-nx769j-boot.img` (`header_version=4`, BootShim + U-Boot)
  3. Sets active slot to `_a` (Android remains untouched on `_b`).

Safety guardrails:
  - Runs in offline `--dry-run` mode by default unless `--execute` is passed.
  - Enforces strict fastboot command allowlist: only `getvar`, `flash vbmeta_a`,
    `flash boot_a`, `--set-active=a`, and `reboot`. Any reference to `_b`
    partitions or `set-active=b` in the flash sequence causes immediate abort.
  - Validates `vbmeta_a` (`0x10000`) and `boot_a` (`0x6000000`) sizes via fastboot
    and verifies the ARM64 BootShim magic (`0xFA405A4D`, `"ARMd"`) before flashing.
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
from typing import Any

VBMETA_HEADER_SIZE = 256
VBMETA_PARTITION_SIZE = 0x10000  # 64 KiB
BOOT_A_PARTITION_SIZE = 0x6000000  # 96 MiB
AVB_FLAG_VERIFICATION_DISABLED = 2

ALLOWED_GETVAR_NAMES = frozenset(
    {
        "unlocked",
        "current-slot",
        "partition-size:vbmeta_a",
        "partition-size:boot_a",
    }
)


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def resolve_fastboot(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    env = os.environ.get("FASTBOOT")
    if env:
        return env
    found = shutil.which("fastboot")
    if not found:
        raise FileNotFoundError("`fastboot` not found in PATH (pass --fastboot /path/to/fastboot)")
    return found


def select_fastboot_serial(fb_bin: str, explicit_serial: str | None) -> str:
    devs = subprocess.check_output([fb_bin, "devices"], text=True)
    online = [ln.split()[0] for ln in devs.splitlines() if ln.strip() and "fastboot" in ln]
    if explicit_serial:
        if explicit_serial not in online:
            raise RuntimeError(
                f"Target fastboot serial {explicit_serial!r} not online (online: {online})"
            )
        return explicit_serial
    if len(online) == 1:
        return online[0]
    if not online:
        raise RuntimeError("No devices detected in fastboot mode.")
    raise RuntimeError(
        f"Multiple fastboot devices connected ({online}); pass `--serial <SERIAL>` explicitly."
    )


def parse_vbmeta_header(data: bytes) -> dict[str, Any]:
    if len(data) < VBMETA_HEADER_SIZE:
        raise ValueError(f"vbmeta image too small: {len(data)} bytes")
    magic = data[0:4]
    if magic != b"AVB0":
        raise ValueError(f"Invalid AVB magic: {magic!r} (expected b'AVB0')")
    ver_major, ver_minor = struct_unpack(">II", data, 4)
    auth_size, aux_size = struct_unpack(">QQ", data, 12)
    alg_type = struct_unpack(">I", data, 28)[0]
    flags = struct_unpack(">I", data, 120)[0]
    rollback_loc = struct_unpack(">I", data, 124)[0]
    release_str = data[128:176].split(b"\x00", 1)[0].decode("ascii", errors="replace")
    return {
        "magic": magic.decode("ascii"),
        "version_major": ver_major,
        "version_minor": ver_minor,
        "authentication_data_block_size": auth_size,
        "auxiliary_data_block_size": aux_size,
        "algorithm_type": alg_type,
        "flags": flags,
        "byte_123": data[123],
        "rollback_index_location": rollback_loc,
        "release_string": release_str,
        "total_size": len(data),
    }


def struct_unpack(fmt: str, data: bytes, offset: int) -> tuple[Any, ...]:
    import struct

    return struct.unpack_from(fmt, data, offset)


def build_vbmeta_disabled_image(
    stock_vbmeta_a_bytes: bytes | None = None,
    flags: int = AVB_FLAG_VERIFICATION_DISABLED,
    padded_size: int = VBMETA_PARTITION_SIZE,
) -> bytes:
    """
    Build minimal unsigned AvbVBMetaImageHeader matching:
      `avbtool make_vbmeta_image --flags 2 --padding_size 65536`
    """
    import struct

    ver_major, ver_minor = 1, 0
    release_string = "avbtool 1.2.0"
    if stock_vbmeta_a_bytes is not None:
        stock_info = parse_vbmeta_header(stock_vbmeta_a_bytes)
        if stock_info["version_major"] != 1:
            raise ValueError(f"Unexpected stock vbmeta major version: {stock_info['version_major']}")
        ver_major = stock_info["version_major"]
        ver_minor = stock_info["version_minor"]
        if stock_info["release_string"]:
            release_string = stock_info["release_string"]

    if padded_size < VBMETA_HEADER_SIZE or (padded_size % 4096) != 0:
        raise ValueError(f"Invalid padded_size {padded_size} (must be >= 256 and 4096-aligned)")
    if flags not in (2, 3):
        raise ValueError(f"Refusing unexpected vbmeta flags={flags} (expected 2 or 3)")

    buf = bytearray(padded_size)
    buf[0:4] = b"AVB0"
    struct.pack_into(">II", buf, 4, ver_major, ver_minor)
    struct.pack_into(">QQI", buf, 12, 0, 0, 0)
    struct.pack_into(">I", buf, 120, flags)
    rel_bytes = release_string.encode("ascii")[:47]
    buf[128 : 128 + len(rel_bytes)] = rel_bytes

    out = bytes(buf)
    parsed = parse_vbmeta_header(out)
    assert parsed["magic"] == "AVB0"
    assert parsed["flags"] == flags and parsed["byte_123"] == flags
    assert parsed["algorithm_type"] == 0
    return out


def verify_boot_image(boot_img: pathlib.Path) -> dict[str, Any]:
    import struct

    if not boot_img.is_file():
        raise FileNotFoundError(f"Missing boot image: {boot_img}")
    size = boot_img.stat().st_size
    if size < 8192 or size > BOOT_A_PARTITION_SIZE:
        raise ValueError(
            f"{boot_img} size {size} out of bounds for boot_a ({BOOT_A_PARTITION_SIZE} bytes)"
        )
    with open(boot_img, "rb") as f:
        hdr = f.read(4096)
        kern_hdr = f.read(64)

    if hdr[0:8] != b"ANDROID!":
        raise ValueError(f"Invalid boot image magic in {boot_img}: {hdr[0:8]!r}")
    kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from("<IIII", hdr, 8)
    header_version = struct.unpack_from("<I", hdr, 40)[0]
    if header_version != 4:
        raise ValueError(f"Expected boot header_version 4, got {header_version}")
    if kernel_size != 0x4000000 or ramdisk_size != 0:
        raise ValueError(f"Unexpected payload sizes: kernel={kernel_size}, ramdisk={ramdisk_size}")
    shim_inst = struct.unpack_from("<I", kern_hdr, 0)[0]
    arm64_magic = kern_hdr[56:60]
    if shim_inst != 0xFA405A4D or arm64_magic != b"ARMd":
        raise ValueError(
            f"Invalid BootShim ARM64 header in {boot_img}: inst=0x{shim_inst:08x} magic={arm64_magic!r}"
        )

    return {
        "path": str(boot_img),
        "size": size,
        "sha256": sha256_file(boot_img),
        "header_version": header_version,
        "header_size": header_size,
        "os_version": os_version,
        "kernel_size": kernel_size,
        "ramdisk_size": ramdisk_size,
    }


def assert_safe_fastboot_args(fb_args: list[str]) -> None:
    if not fb_args:
        raise RuntimeError("SAFETY ABORT: Empty fastboot command")
    for tok in fb_args:
        if os.path.sep in tok:
            if "_b" in os.path.basename(tok):
                raise RuntimeError(f"SAFETY ABORT: Refusing _b filename in fastboot args: {tok!r}")
            continue
        if tok in ("--set-active=b", "--set-active=_b", "set_active") or "_b" in tok:
            raise RuntimeError(f"SAFETY ABORT: Forbidden Slot-B token {tok!r}")

    if len(fb_args) == 1 and fb_args[0] in ("--set-active=a", "reboot"):
        return
    if len(fb_args) == 2 and fb_args[0] == "getvar" and fb_args[1] in ALLOWED_GETVAR_NAMES:
        return
    if len(fb_args) == 3 and fb_args[0] == "flash" and fb_args[1] in ("vbmeta_a", "boot_a"):
        return
    raise RuntimeError(f"SAFETY ABORT: Fastboot command outside strict Slot-A allowlist: {fb_args!r}")


def fb_run(fb_bin: str, serial: str, fb_args: list[str]) -> str:
    assert_safe_fastboot_args(fb_args)
    cmd = [fb_bin, "-s", serial] + fb_args
    proc = subprocess.run(cmd, check=True, text=True, capture_output=True)
    return (proc.stdout or "") + "\n" + (proc.stderr or "")


def parse_fastboot_var(output: str, var_name: str) -> str:
    pat = re.compile(rf"^{re.escape(var_name)}:\s*(.+)$", re.MULTILINE)
    m = pat.search(output)
    return m.group(1).strip() if m else ""


def execute_slot_a_flash(
    fb_bin: str,
    serial: str,
    vbmeta_disabled_path: pathlib.Path,
    boot_img_path: pathlib.Path,
    do_reboot: bool = False,
) -> dict[str, Any]:
    unlocked = parse_fastboot_var(fb_run(fb_bin, serial, ["getvar", "unlocked"]), "unlocked")
    if unlocked.lower() != "yes":
        raise RuntimeError(f"Bootloader unlocked state is {unlocked!r} (expected 'yes')")

    vbmeta_sz_hex = parse_fastboot_var(
        fb_run(fb_bin, serial, ["getvar", "partition-size:vbmeta_a"]), "partition-size:vbmeta_a"
    )
    boot_sz_hex = parse_fastboot_var(
        fb_run(fb_bin, serial, ["getvar", "partition-size:boot_a"]), "partition-size:boot_a"
    )
    if int(vbmeta_sz_hex, 16) != VBMETA_PARTITION_SIZE:
        raise RuntimeError(f"Unexpected vbmeta_a size {vbmeta_sz_hex} (expected 0x10000)")
    if int(boot_sz_hex, 16) != BOOT_A_PARTITION_SIZE:
        raise RuntimeError(f"Unexpected boot_a size {boot_sz_hex} (expected 0x6000000)")

    log_vbmeta = fb_run(fb_bin, serial, ["flash", "vbmeta_a", str(vbmeta_disabled_path)])
    log_boot = fb_run(fb_bin, serial, ["flash", "boot_a", str(boot_img_path)])
    log_active = fb_run(fb_bin, serial, ["--set-active=a"])

    cur_slot = parse_fastboot_var(fb_run(fb_bin, serial, ["getvar", "current-slot"]), "current-slot")
    if cur_slot.lower().lstrip("_") != "a":
        raise RuntimeError(f"Post-switch current-slot failed: got {cur_slot!r} (expected 'a')")

    if do_reboot:
        fb_run(fb_bin, serial, ["reboot"])

    return {
        "status": "slot_a_flashed_and_active",
        "serial": serial,
        "flashed_partitions": ["vbmeta_a", "boot_a"],
        "untouched_slot_b": ["boot_b", "init_boot_b", "vendor_boot_b", "dtbo_b", "vbmeta_b", "abl_b"],
        "active_slot": cur_slot,
        "rebooted": do_reboot,
        "fastboot_logs": {
            "vbmeta_a": log_vbmeta.strip(),
            "boot_a": log_boot.strip(),
            "set_active_a": log_active.strip(),
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Build vbmeta_a_disabled.img (flags=2) and safely flash ONLY vbmeta_a + boot_a on NX769J"
    )
    ap.add_argument("--serial", default=None, help="Fastboot target serial (auto-selected only if 1 device)")
    ap.add_argument("--fastboot", default=None, help="Optional explicit path to fastboot binary")
    ap.add_argument(
        "--stock-vbmeta-a",
        default=None,
        help="Optional backup stock vbmeta_a.img to mirror AVB version/release string",
    )
    ap.add_argument("--boot-img", required=True, help="Path to verified uboot-nx769j boot image")
    ap.add_argument(
        "--vbmeta-out",
        default="vbmeta_a_disabled.img",
        help="Output path for vbmeta_a_disabled.img",
    )
    ap.add_argument(
        "--execute",
        "--flash",
        dest="execute",
        action="store_true",
        help="Perform live fastboot flash of vbmeta_a + boot_a + --set-active=a (default is --dry-run)",
    )
    ap.add_argument("--reboot", action="store_true", help="With --execute, reboot after flashing")
    ap.add_argument("--rollback-commands", action="store_true", help="Print stock rollback commands and exit")
    args = ap.parse_args()

    serial_disp = args.serial or "<SERIAL>"
    if args.rollback_commands:
        print(
            f"fastboot -s {serial_disp} flash vbmeta_a stock_vbmeta_a.img\n"
            f"fastboot -s {serial_disp} flash boot_a stock_boot_a.img\n"
            f"fastboot -s {serial_disp} --set-active=b\n"
            f"fastboot -s {serial_disp} reboot"
        )
        return 0

    stock_bytes = pathlib.Path(args.stock_vbmeta_a).read_bytes() if args.stock_vbmeta_a else None
    disabled_bytes = build_vbmeta_disabled_image(stock_bytes, flags=AVB_FLAG_VERIFICATION_DISABLED)
    vbmeta_out = pathlib.Path(args.vbmeta_out)
    vbmeta_out.parent.mkdir(parents=True, exist_ok=True)
    vbmeta_out.write_bytes(disabled_bytes)

    boot_img = pathlib.Path(args.boot_img)
    boot_info = verify_boot_image(boot_img)

    if not args.execute:
        print(
            json.dumps(
                {
                    "mode": "dry-run",
                    "serial": serial_disp,
                    "vbmeta_a_disabled": {
                        "path": str(vbmeta_out),
                        "size": len(disabled_bytes),
                        "sha256": hashlib.sha256(disabled_bytes).hexdigest(),
                        "header": parse_vbmeta_header(disabled_bytes),
                    },
                    "boot_img": boot_info,
                    "planned_fastboot_commands": [
                        f"fastboot -s {serial_disp} flash vbmeta_a {vbmeta_out}",
                        f"fastboot -s {serial_disp} flash boot_a {boot_img}",
                        f"fastboot -s {serial_disp} --set-active=a",
                    ],
                    "slot_b_touched": False,
                },
                indent=2,
            )
        )
        return 0

    fb_bin = resolve_fastboot(args.fastboot)
    serial = select_fastboot_serial(fb_bin, args.serial)
    res = execute_slot_a_flash(
        fb_bin=fb_bin,
        serial=serial,
        vbmeta_disabled_path=vbmeta_out,
        boot_img_path=boot_img,
        do_reboot=args.reboot,
    )
    print(json.dumps(res, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
