#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
verify_arch_stage.py — Pre-pivot Verification Suite for NX769J Arch / Omarchy Stage.

Verifies before atomic rootfs swap that `/arch-stage` contains:
  - Merged `/usr/lib`, `/usr/bin`, `/sbin/init -> /usr/lib/systemd/systemd` (ARM64 ELF)
  - Valid `/etc/fstab` (`/dev/nxroot`, `/swapfile`) and `DisableSandbox` pacman config
  - Kernel modules + `modules.dep.bin` + uncompressed firmware + sensor calibration
  - Native AArch64 HAL binaries (`libhexagonrpc`, `nx769j-hexagonrpcd`, `nx769j-imu-daemon`,
    `nx769j-fan`, `nx769j-powerkey`, `nx769j-perf`) and Python/shell helpers
  - Required systemd multi-user units enabled and sleep/suspend targets masked
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import struct
import sys
import time

KERNEL_REL = "7.1.1-nx769j+"


def read_text_closed(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def resolve_in_root(stage_root: str, rel_path: str) -> str:
    """Resolve symlinks strictly inside `stage_root` without escaping to host `/`."""
    cur = os.path.abspath(stage_root)
    parts = [p for p in rel_path.lstrip("/").split("/") if p]
    seen = 0
    while parts:
        part = parts.pop(0)
        if part == ".":
            continue
        if part == "..":
            cur = os.path.dirname(cur)
            if not cur.startswith(os.path.abspath(stage_root)):
                cur = os.path.abspath(stage_root)
            continue
        nxt = os.path.join(cur, part)
        if os.path.islink(nxt):
            seen += 1
            if seen > 40:
                raise RuntimeError(f"Symlink loop resolving {rel_path}")
            target = os.readlink(nxt)
            if target.startswith("/"):
                cur = os.path.abspath(stage_root)
                parts = [p for p in target.lstrip("/").split("/") if p] + parts
            else:
                parts = target.split("/") + parts
        else:
            cur = nxt
    return cur


def is_elf64_aarch64(path: str) -> tuple[bool, str]:
    if not os.path.isfile(path):
        return False, "missing"
    with open(path, "rb") as f:
        hdr = f.read(20)
    if len(hdr) < 20 or hdr[:4] != b"\x7fELF":
        return False, "not ELF"
    if hdr[4] != 2 or hdr[5] != 1:
        return False, f"unexpected ELF class/endian ({hdr[4]}, {hdr[5]})"
    e_machine = struct.unpack_from("<H", hdr, 18)[0]
    if e_machine != 183:  # EM_AARCH64
        return False, f"unexpected e_machine={e_machine}"
    return True, "ELF64 aarch64"


def find_submounts(stage_root: str) -> list[str]:
    target = os.path.abspath(stage_root).rstrip("/")
    mounts: list[str] = []
    if not os.path.isfile("/proc/mounts"):
        return mounts
    for ln in read_text_closed("/proc/mounts").splitlines():
        parts = ln.split()
        if len(parts) >= 2 and parts[1].startswith(target + "/"):
            mounts.append(parts[1])
    return mounts


def verify_stage(stage_root: str, require_ui: bool = False) -> tuple[list[str], list[str]]:
    checks_ok: list[str] = []
    errors: list[str] = []

    def p(rel: str) -> str:
        return os.path.join(stage_root, rel.lstrip("/"))

    def ok(msg: str) -> None:
        checks_ok.append(msg)

    def err(msg: str) -> None:
        errors.append(msg)

    # 1. Symlink merge checks
    for s in ("lib", "lib64", "sbin", "bin"):
        sp = p(s)
        if os.path.islink(sp):
            ok(f"/{s} symlinked -> {os.readlink(sp)}")
        else:
            err(f"/{s} is not a merged-usr symlink")

    # 2. Core Arch init & boot files
    if os.path.isfile(p("etc/arch-release")):
        ok("/etc/arch-release present")
    else:
        err("Missing /etc/arch-release")

    try:
        init_real = resolve_in_root(stage_root, "sbin/init")
        ok_elf, detail = is_elf64_aarch64(init_real)
        if ok_elf and init_real.endswith("/usr/lib/systemd/systemd"):
            ok(f"/sbin/init resolves cleanly to systemd ({detail})")
        else:
            err(f"/sbin/init invalid: {init_real} ({detail})")
    except Exception as exc:
        err(f"/sbin/init resolution failed: {exc}")

    fstab_p = p("etc/fstab")
    if os.path.isfile(fstab_p):
        ftxt = read_text_closed(fstab_p)
        if "/dev/nxroot" in ftxt and "/swapfile" in ftxt:
            ok("/etc/fstab mounts /dev/nxroot and /swapfile")
        else:
            err("/etc/fstab missing /dev/nxroot or /swapfile")
    else:
        err("Missing /etc/fstab")

    pac_p = p("etc/pacman.conf")
    if os.path.isfile(pac_p) and "DisableSandbox" in read_text_closed(pac_p):
        ok("/etc/pacman.conf configures DisableSandbox for custom kernels")
    else:
        err("/etc/pacman.conf missing DisableSandbox")

    # 3. Kernel modules & depmod index
    mod_dir = p(f"usr/lib/modules/{KERNEL_REL}")
    dep_bin = os.path.join(mod_dir, "modules.dep.bin")
    dep_txt = os.path.join(mod_dir, "modules.dep")
    if os.path.isfile(dep_bin) and os.path.getsize(dep_bin) > 0 and os.path.isfile(dep_txt):
        mod_text = read_text_closed(dep_txt)
        req_mods = [
            "ath12k.ko", "btqca.ko", "hci_uart.ko", "fastrpc.ko", "cuse.ko",
            "qrtr.ko", "qcom_q6v5_pas.ko", "qcom_battmgr.ko", "snd-soc-sc8280xp.ko",
            "leds-qcom-lpg.ko", "pwm-fan.ko", "cpufreq_powersave.ko", "msm.ko",
        ]
        missing_mods = [m for m in req_mods if m not in mod_text]
        if missing_mods:
            err(f"Missing required modules in modules.dep: {missing_mods}")
        else:
            ok(f"Kernel modules {KERNEL_REL} + depmod verified ({len(req_mods)} key modules)")
    else:
        err(f"Missing modules.dep(.bin) under /usr/lib/modules/{KERNEL_REL}")

    # 4. Uncompressed firmware, sensor calibration & UCM2
    req_fw = [
        "usr/lib/firmware/ath12k/WCN7850/hw2.0/amss.bin",
        "usr/lib/firmware/ath12k/WCN7850/hw2.0/board-2.bin",
        "usr/lib/firmware/ath12k/WCN7850/hw2.0/m3.bin",
        "usr/lib/firmware/qca/hmtbtfw20.tlv",
        "usr/lib/firmware/qca/hmtnv20.bin",
        "usr/lib/firmware/qcom/a750_sqe.fw",
        "usr/lib/firmware/qcom/gen70900_sqe.fw",
        "usr/lib/firmware/qcom/gen70900_aqe.fw",
        "usr/lib/firmware/qcom/gen70900_zap.mbn",
        "usr/lib/firmware/qcom/gmu_gen70900.bin",
        "usr/lib/firmware/qcom/sm8650/adsp.mdt",
        "usr/lib/firmware/qcom/sm8650/adsp_dtb.mdt",
        "usr/lib/firmware/qcom/sm8650/battmgr.jsn",
        "usr/lib/firmware/qcom/sm8650/adspr.jsn",
        "usr/lib/firmware/qcom/sm8650/adsps.jsn",
        "usr/lib/firmware/qcom/sm8650/SM8650-Nubia-NX769J-tplg.bin",
    ]
    missing_fw: list[str] = []
    for fw in req_fw:
        try:
            real_fw = resolve_in_root(stage_root, fw)
            if not os.path.isfile(real_fw) or os.path.getsize(real_fw) == 0:
                missing_fw.append(fw)
            else:
                with open(real_fw, "rb") as ff:
                    if ff.read(4) == b"\x28\xb5\x2f\xfd":
                        err(f"Firmware {fw} is Zstd-compressed (must be uncompressed!)")
        except Exception:
            missing_fw.append(fw)
    if missing_fw:
        err(f"Missing uncompressed firmware files: {missing_fw}")
    else:
        ok(f"All {len(req_fw)} uncompressed firmware files verified")

    persist_dir = p("usr/share/qcom/sm8650/nubia/nx769j/persist_sensors")
    dj_link = p("var/lib/droid-juicer/sensors")
    if os.path.isdir(persist_dir) and os.listdir(persist_dir) and os.path.islink(dj_link):
        ok("SM8650 sensor calibration + droid-juicer symlink verified")
    else:
        err("Missing persist_sensors or /var/lib/droid-juicer/sensors")

    req_ucm = [
        "usr/share/alsa/ucm2/Qualcomm/sm8650/NX769J/SM8650-Nubia-NX769J.conf",
        "usr/share/alsa/ucm2/Qualcomm/sm8650/NX769J/HiFi.conf",
        "usr/share/alsa/ucm2/conf.d/sm8650/SM8650-Nubia-NX769J.conf",
    ]
    if all(os.path.isfile(p(u)) for u in req_ucm):
        ok("NX769J ALSA UCM2 speaker/mic profiles verified")
    else:
        err(f"Missing ALSA UCM2 files: {[u for u in req_ucm if not os.path.isfile(p(u))]}")

    # 5. Native ELF64 aarch64 binaries & Python/shell helpers
    elf_targets = [
        "usr/lib/libhexagonrpc.so.0.4.0",
        "usr/local/libexec/nx769j-hexagonrpcd",
        "usr/local/sbin/nx769j-imu-daemon",
        "usr/local/bin/nx769j-fan",
        "usr/local/sbin/nx769j-powerkey",
    ]
    for et in elf_targets:
        ok_elf, detail = is_elf64_aarch64(p(et))
        if ok_elf:
            ok(f"Native binary /{et}: {detail}")
        else:
            err(f"Invalid/missing native binary /{et}: {detail}")

    if not os.path.islink(p("usr/lib/libhexagonrpc.so.0.4")):
        err("Missing /usr/lib/libhexagonrpc.so.0.4 SONAME symlink")

    for st_rel in [
        "usr/local/sbin/aw88261-init",
        "usr/local/bin/nx769j-charge-limit",
        "usr/local/libexec/nx769j-sensor-proxy",
        "usr/local/bin/nx769j-imu",
        "usr/local/bin/nx769j-zink",
        "usr/local/sbin/nx769j-usb-gadget.sh",
    ]:
        sp = p(st_rel)
        if os.path.isfile(sp) and os.access(sp, os.X_OK):
            ok(f"Executable script /{st_rel}")
        else:
            err(f"Missing or non-executable script /{st_rel}")

    gad_p = p("usr/local/sbin/nx769j-usb-gadget.sh")
    if os.path.isfile(gad_p):
        gtxt = read_text_closed(gad_p)
        if "NX769J" in gtxt and "10.42.0.2/24" in gtxt and "169.254.72.2/16" in gtxt:
            ok("USB gadget provides dual IPv4 addresses (10.42.0.2 & 169.254.72.2)")
        else:
            err("nx769j-usb-gadget.sh missing 10.42.0.2 or 169.254.72.2")

    # 6. Accounts, sudoers, SSH, unmanaged usb0
    pw_p, gr_p = p("etc/passwd"), p("etc/group")
    if os.path.isfile(pw_p) and os.path.isfile(gr_p):
        pw_lines = read_text_closed(pw_p).splitlines()
        gr_lines = read_text_closed(gr_p).splitlines()
        if any(ln.startswith("alarm:") for ln in pw_lines):
            err("Default alarm user still present in /etc/passwd (conflicts with uid 1000)")
        dev_pw = [ln for ln in pw_lines if ln.startswith("developer:")]
        if dev_pw and ":1000:1000:" in dev_pw[0]:
            ok("User developer (uid=1000, gid=1000) verified")
        else:
            err(f"User developer missing or wrong uid/gid: {dev_pw}")
        if any(ln.startswith("fastrpc:") for ln in pw_lines):
            ok("System user fastrpc present")
        else:
            err("Missing fastrpc system user in /etc/passwd")

        req_groups = {"wheel", "render", "video", "audio", "input"}
        present_groups: set[str] = set()
        for ln in gr_lines:
            parts = ln.split(":")
            if len(parts) >= 4 and parts[0] in req_groups:
                members = set(parts[3].split(",")) if parts[3] else set()
                if "developer" in members:
                    present_groups.add(parts[0])
        if present_groups == req_groups:
            ok("User developer belongs to wheel,render,video,audio,input")
        else:
            err(f"User developer missing groups: {sorted(req_groups - present_groups)}")
    else:
        err("Missing /etc/passwd or /etc/group")

    sudo_p = p("etc/sudoers.d/90-nx769j-wheel")
    if os.path.isfile(sudo_p):
        smode = stat.S_IMODE(os.lstat(sudo_p).st_mode)
        if smode in (0o440, 0o400):
            ok(f"/etc/sudoers.d/90-nx769j-wheel mode {oct(smode)}")
        else:
            err(f"/etc/sudoers.d/90-nx769j-wheel unsafe mode {oct(smode)}")
    else:
        err("Missing /etc/sudoers.d/90-nx769j-wheel")

    nm_usb = p("etc/NetworkManager/conf.d/99-unmanaged-usb0.conf")
    nd_usb = p("etc/systemd/network/00-unmanaged-usb0.network")
    if (
        os.path.isfile(nm_usb)
        and "unmanaged-devices=interface-name:usb0" in read_text_closed(nm_usb)
        and os.path.isfile(nd_usb)
        and "Unmanaged=yes" in read_text_closed(nd_usb)
    ):
        ok("usb0 unmanaged protection verified")
    else:
        err("Missing usb0 unmanaged protection config")

    # 7. Udev rules & systemd wants/masks
    for ur in ["etc/udev/rules.d/99-nx769j-headless-gpu.rules", "etc/udev/rules.d/99-nx769j-imu.rules"]:
        if os.path.isfile(p(ur)):
            ok(f"/{ur} present")
        else:
            err(f"Missing /{ur}")

    req_wants = [
        "sshd.service",
        "NetworkManager.service",
        "bluetooth.service",
        "upower.service",
        "nx769j-usb-gadget.service",
        "nx769j-fan-init.service",
        "nx769j-aw88261.service",
        "nx769j-charge-limit.service",
        "hexagonrpcd-rootpd.service",
        "hexagonrpcd.service",
        "nx769j-imu-daemon.service",
        "iio-sensor-proxy.service",
        "nx769j-powerkey.service",
    ]
    if require_ui:
        req_wants.extend(["nx769j-fb-wl.service", "nx769j-omamobile-session.service"])

    wants_dir = p("etc/systemd/system/multi-user.target.wants")
    missing_wants = [u for u in req_wants if not os.path.islink(os.path.join(wants_dir, u))]
    if missing_wants:
        err(f"Units not enabled in multi-user.target.wants: {missing_wants}")
    else:
        ok(f"All {len(req_wants)} required systemd services enabled")

    for masked in [
        "qrtr-ns.service",
        "sleep.target",
        "suspend.target",
        "hibernate.target",
        "hybrid-sleep.target",
    ]:
        mp = p(f"etc/systemd/system/{masked}")
        if os.path.islink(mp) and os.readlink(mp) == "/dev/null":
            ok(f"Masked {masked} -> /dev/null")
        else:
            err(f"Unit {masked} is NOT masked to /dev/null")

    return checks_ok, errors


def main() -> int:
    ap = argparse.ArgumentParser(description="Verify /arch-stage readiness prior to atomic pivot.")
    ap.add_argument("--stage-root", default="/arch-stage", help="Staging root directory")
    ap.add_argument("--require-ui", action="store_true", help="Also require Omarchy UI systemd units")
    ap.add_argument("--mark-ready", action="store_true", help="Write <stage-root>/.ready if all checks pass")
    ap.add_argument("--allow-mounted", action="store_true", help="Ignore active submounts (inspection only)")
    ap.add_argument("--json", action="store_true", help="Emit JSON report")
    args = ap.parse_args()

    checks_ok, errors = verify_stage(args.stage_root, require_ui=args.require_ui)
    submounts = find_submounts(args.stage_root)
    if submounts and not args.allow_mounted:
        errors.append(f"Active bind/chroot mounts still attached under {args.stage_root}: {submounts}")

    ready_path = os.path.join(args.stage_root, ".ready")
    passed = len(errors) == 0

    if args.mark_ready:
        if not passed:
            if os.path.exists(ready_path):
                os.unlink(ready_path)
        else:
            manifest = {
                "stage_root": os.path.abspath(args.stage_root),
                "kernel_release": KERNEL_REL,
                "require_ui": args.require_ui,
                "checks_passed": len(checks_ok),
                "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            }
            manifest_bytes = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
            manifest["sha256"] = hashlib.sha256(manifest_bytes).hexdigest()
            with open(ready_path, "w", encoding="utf-8") as f:
                json.dump(manifest, f, indent=2)
                f.write("\n")
            os.sync()

    if args.json:
        print(
            json.dumps(
                {
                    "passed": passed,
                    "checks_ok": checks_ok,
                    "errors": errors,
                    "ready_sentinel": os.path.isfile(ready_path),
                },
                indent=2,
            )
        )
    else:
        for item in checks_ok:
            print(f"  [PASS] {item}")
        for item in errors:
            print(f"  [FAIL] {item}", file=sys.stderr)
        status = "READY" if (passed and os.path.isfile(ready_path)) else ("PASS" if passed else "FAIL")
        print(f"\nSummary: {status} ({len(checks_ok)} passed, {len(errors)} failed)")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
