#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
atomic_pivot_to_arch.py — Zero-Handle Initrd-Safe Rootfs Swap & Emergency Rollback
Controller for RedMagic 9 Pro (NX769J) Omarchy Mobile.

Works hand-in-hand with the boot initramfs before systemd PID 1 mounts `/newroot`:
  * `--status`               Report active OS (`kubuntu` or `arch`), `/arch-stage`, and sentinels
  * `--arm`                  Verify staging tree with `verify_arch_stage.py` & create `/.pivot-arch-stage`
  * `--disarm`               Remove `/.pivot-arch-stage` / `/.rollback-old-kubuntu`
  * `--arm-rollback`         Arm `/.rollback-old-kubuntu` to restore `/old-kubuntu` on next boot
  * `--cleanup-old-kubuntu`  Delete `/old-kubuntu` after verifying live Arch boot (~11 GiB reclaimed)
  * `--execute-offline-root` Test or apply swap/rollback offline on an unmounted root tree
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

KUBUNTU_SAVE_ENTRIES = (
    ".config", "bin", "boot", "etc", "home", "lib", "lib64",
    "media", "mnt", "opt", "root", "sbin", "srv", "usr", "var",
)
ARCH_PROMOTE_ENTRIES = (
    "bin", "boot", "etc", "home", "lib", "lib64",
    "mnt", "opt", "root", "sbin", "srv", "usr", "var",
)


def path_exists_or_link(path: str) -> bool:
    return os.path.lexists(path)


def detect_active_distro(root: str) -> str:
    if os.path.isfile(os.path.join(root, "etc/arch-release")):
        return "arch"
    osr = os.path.join(root, "etc/os-release")
    if os.path.isfile(osr):
        with open(osr, "r", encoding="utf-8", errors="replace") as f:
            txt = f.read().lower()
        if "ubuntu" in txt:
            return "kubuntu"
        if "arch" in txt:
            return "arch"
    return "unknown"


def load_verifier():
    vpath = os.path.join(SCRIPT_DIR, "verify_arch_stage.py")
    spec = importlib.util.spec_from_file_location("verify_arch_stage", vpath)
    mod = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(mod)
    return mod


def cmd_status(root: str) -> int:
    info = {
        "root": os.path.abspath(root),
        "active_distro": detect_active_distro(root),
        "arch_stage_present": os.path.isdir(os.path.join(root, "arch-stage")),
        "arch_stage_ready": os.path.isfile(os.path.join(root, "arch-stage/.ready")),
        "pivot_armed": os.path.isfile(os.path.join(root, ".pivot-arch-stage")),
        "rollback_armed": os.path.isfile(os.path.join(root, ".rollback-old-kubuntu")),
        "old_kubuntu_backup": os.path.isdir(os.path.join(root, "old-kubuntu/usr")),
        "pivoted_ok_marker": os.path.isfile(os.path.join(root, ".omarchy-pivoted-ok")),
    }
    print(json.dumps(info, indent=2))
    return 0


def cmd_arm(root: str, require_ui: bool = False) -> int:
    stage_root = os.path.join(root, "arch-stage")
    if not os.path.isdir(stage_root):
        print(f"ERROR: Staging directory missing: {stage_root}", file=sys.stderr)
        return 1

    verifier = load_verifier()
    checks_ok, errors = verifier.verify_stage(stage_root, require_ui=require_ui)
    submounts = verifier.find_submounts(stage_root)
    if submounts:
        errors.append(f"Active bind/chroot mounts still present under {stage_root}: {submounts}")

    if errors:
        print("ABORT: verify_arch_stage failed; refusing to arm pivot:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    ready_path = os.path.join(stage_root, ".ready")
    with open(ready_path, "w", encoding="utf-8") as f:
        json.dump(
            {
                "stage_root": os.path.abspath(stage_root),
                "checks_passed": len(checks_ok),
                "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            },
            f,
            indent=2,
        )
        f.write("\n")

    rb_path = os.path.join(root, ".rollback-old-kubuntu")
    if os.path.exists(rb_path):
        os.unlink(rb_path)

    sentinel_path = os.path.join(root, ".pivot-arch-stage")
    with open(sentinel_path, "w", encoding="utf-8") as f:
        f.write(
            f"PIVOT_ARMED_UTC={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n"
            f"CHECKS_PASSED={len(checks_ok)}\n"
        )
    os.sync()
    print(f"ARMED: {sentinel_path} ready ({len(checks_ok)} stage checks passed).")
    return 0


def cmd_disarm(root: str) -> int:
    for name in (".pivot-arch-stage", ".rollback-old-kubuntu"):
        p = os.path.join(root, name)
        if os.path.exists(p):
            os.unlink(p)
    os.sync()
    print("DISARMED: Removed pivot/rollback sentinels.")
    return 0


def cmd_arm_rollback(root: str) -> int:
    old_kub = os.path.join(root, "old-kubuntu")
    if not (os.path.isdir(os.path.join(old_kub, "usr")) and path_exists_or_link(os.path.join(old_kub, "sbin"))):
        print("ERROR: /old-kubuntu backup is incomplete; cannot arm rollback.", file=sys.stderr)
        return 1
    pivot_sentinel = os.path.join(root, ".pivot-arch-stage")
    if os.path.exists(pivot_sentinel):
        os.unlink(pivot_sentinel)
    rb = os.path.join(root, ".rollback-old-kubuntu")
    with open(rb, "w", encoding="utf-8") as f:
        f.write(f"ROLLBACK_UTC={time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}\n")
    os.sync()
    print(f"ROLLBACK ARMED: {rb} ready for next initrd boot.")
    return 0


def cmd_cleanup_old_kubuntu(root: str) -> int:
    if detect_active_distro(root) != "arch":
        print("ERROR: Active rootfs is NOT Arch Linux (/etc/arch-release missing)!", file=sys.stderr)
        return 1
    if os.path.exists(os.path.join(root, ".rollback-old-kubuntu")):
        print("ERROR: .rollback-old-kubuntu is armed! Disarm first before cleanup.", file=sys.stderr)
        return 1
    old_kub = os.path.join(root, "old-kubuntu")
    if not os.path.exists(old_kub):
        print("OK: /old-kubuntu already absent.")
        return 0
    shutil.rmtree(old_kub)
    os.sync()
    print("CLEANED: Removed /old-kubuntu backup directory.")
    return 0


def execute_offline_swap(newroot: str) -> int:
    """Offline initrd-equivalent swap/rollback executor on an unmounted/mock root tree."""
    if os.path.abspath(newroot) == "/":
        print("ERROR: Refusing offline directory swap on live '/'; use `--arm` instead.", file=sys.stderr)
        return 1

    pivot_flag = os.path.join(newroot, ".pivot-arch-stage")
    rollback_flag = os.path.join(newroot, ".rollback-old-kubuntu")
    stage = os.path.join(newroot, "arch-stage")
    old_kub = os.path.join(newroot, "old-kubuntu")

    if os.path.isfile(rollback_flag) and os.path.isdir(os.path.join(old_kub, "usr")):
        rb_dir = os.path.join(newroot, "arch-rolled-back")
        os.makedirs(rb_dir, exist_ok=True)
        for d in KUBUNTU_SAVE_ENTRIES:
            src, dst = os.path.join(newroot, d), os.path.join(rb_dir, d)
            if path_exists_or_link(src) and not path_exists_or_link(dst):
                os.rename(src, dst)
        for d in KUBUNTU_SAVE_ENTRIES:
            src, dst = os.path.join(old_kub, d), os.path.join(newroot, d)
            if path_exists_or_link(src):
                os.rename(src, dst)
        for mp in ("dev", "proc", "sys", "run", "tmp", "mnt", "media"):
            os.makedirs(os.path.join(newroot, mp), exist_ok=True)
        os.chmod(os.path.join(newroot, "tmp"), 0o1777)
        os.unlink(rollback_flag)
        moved_mark = os.path.join(old_kub, ".kubuntu-moved")
        if os.path.exists(moved_mark):
            os.unlink(moved_mark)
        return 0

    if os.path.isfile(pivot_flag):
        os.makedirs(old_kub, exist_ok=True)
        moved_mark = os.path.join(old_kub, ".kubuntu-moved")
        if not os.path.isfile(moved_mark):
            for d in KUBUNTU_SAVE_ENTRIES:
                src, dst = os.path.join(newroot, d), os.path.join(old_kub, d)
                if path_exists_or_link(src):
                    os.rename(src, dst)
            with open(moved_mark, "w", encoding="utf-8") as f:
                f.write("pivoted\n")

        for d in ARCH_PROMOTE_ENTRIES:
            src, dst = os.path.join(stage, d), os.path.join(newroot, d)
            if path_exists_or_link(src) and not path_exists_or_link(dst):
                os.rename(src, dst)

        for mp in ("dev", "proc", "sys", "run", "tmp", "mnt", "media"):
            os.makedirs(os.path.join(newroot, mp), exist_ok=True)
        os.chmod(os.path.join(newroot, "tmp"), 0o1777)

        shutil.rmtree(stage, ignore_errors=True)
        os.unlink(pivot_flag)
        with open(os.path.join(newroot, ".omarchy-pivoted-ok"), "w", encoding="utf-8") as f:
            f.write("OK\n")
        return 0

    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Zero-brick atomic pivot controller for NX769J Omarchy Mobile.")
    ap.add_argument("--root", default="/", help="Target root filesystem mountpoint (default: /)")
    ap.add_argument("--require-ui", action="store_true", help="Require UI services when arming")
    grp = ap.add_mutually_exclusive_group(required=True)
    grp.add_argument("--status", action="store_true", help="Show current pivot/rootfs status JSON")
    grp.add_argument("--arm", action="store_true", help="Verify /arch-stage and arm /.pivot-arch-stage")
    grp.add_argument("--disarm", action="store_true", help="Remove pivot/rollback sentinels")
    grp.add_argument("--arm-rollback", action="store_true", help="Arm /.rollback-old-kubuntu")
    grp.add_argument("--cleanup-old-kubuntu", action="store_true", help="Delete /old-kubuntu after Arch verification")
    grp.add_argument("--execute-offline-root", metavar="DIR", help="Run offline swap/rollback on mock root DIR")
    args = ap.parse_args()

    if args.status:
        return cmd_status(args.root)
    if args.arm:
        return cmd_arm(args.root, require_ui=args.require_ui)
    if args.disarm:
        return cmd_disarm(args.root)
    if args.arm_rollback:
        return cmd_arm_rollback(args.root)
    if args.cleanup_old_kubuntu:
        return cmd_cleanup_old_kubuntu(args.root)
    if args.execute_offline_root:
        return execute_offline_swap(args.execute_offline_root)
    return 2


if __name__ == "__main__":
    sys.exit(main())
