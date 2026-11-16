#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Install dual-boot slot switchers & resize utilities into a running Omarchy
Mobile system over SSH (or compile them cleanly from storage-dualboot/).

Installs:
  - /usr/local/sbin/nx769j-slotctl (setuid-capable A/B GPT slot & UFS boot LUN tool)
  - /usr/local/sbin/nx_dm_tool (pinned F2FS FIEMAP/fallocate helper)
  - /usr/local/bin/reboot-to-android (one-tap/CLI switch to Slot _b + reboot)
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shlex
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DUALBOOT_DIR = REPO_ROOT / "storage-dualboot"
OVERLAY_SWITCHER = REPO_ROOT / "rootfs" / "overlay" / "usr" / "local" / "bin" / "reboot-to-android"
DEFAULT_SSH_WRAPPER = DUALBOOT_DIR / "ssh_nx769j.sh"


def compile_static_binaries(out_dir: pathlib.Path, cc: str | None = None) -> tuple[pathlib.Path, pathlib.Path]:
    compiler = cc or shutil.which("aarch64-linux-gnu-gcc") or shutil.which("gcc")
    if not compiler:
        raise FileNotFoundError("No aarch64 cross-compiler found (`aarch64-linux-gnu-gcc`)")

    out_dir.mkdir(parents=True, exist_ok=True)
    dm_bin = out_dir / "nx_dm_tool"
    slot_bin = out_dir / "nx769j-slotctl"

    subprocess.run(
        [
            compiler,
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-nostdlib",
            "-static",
            "-o",
            str(dm_bin),
            str(DUALBOOT_DIR / "nx_dm_tool.c"),
        ],
        check=True,
    )
    subprocess.run(
        [
            compiler,
            "-DNX_NOSTDLIB_ARM64",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-nostdlib",
            "-static",
            "-o",
            str(slot_bin),
            str(DUALBOOT_DIR / "nx769j_slotctl.c"),
        ],
        check=True,
    )
    return dm_bin, slot_bin


def build_ssh_base_cmd(host: str | None, user: str, key: str | None) -> list[str]:
    if not host and DEFAULT_SSH_WRAPPER.is_file():
        return [str(DEFAULT_SSH_WRAPPER)]
    if not host:
        raise RuntimeError("Pass --host <NX769J_IP> or use storage-dualboot/ssh_nx769j.sh")
    cmd = [
        "ssh",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "ServerAliveInterval=3",
        "-o",
        "ServerAliveCountMax=2",
        "-o",
        "StrictHostKeyChecking=accept-new",
        "-o",
        "UserKnownHostsFile=/dev/null",
    ]
    if key:
        cmd += ["-i", key]
    cmd.append(f"{user}@{host}")
    return cmd


def main() -> int:
    ap = argparse.ArgumentParser(description="Compile & stage NX769J dual-boot tools onto device over SSH")
    ap.add_argument("--compile-only", action="store_true", help="Compile static ARM64 helpers locally and exit")
    ap.add_argument("--out-dir", default=str(DUALBOOT_DIR / "build"), help="Build output directory")
    ap.add_argument("--cc", default=None, help="Explicit cross-compiler (e.g. aarch64-linux-gnu-gcc)")
    ap.add_argument("--host", default=None, help="Target device IP (defaults to ssh_nx769j.sh auto-discovery)")
    ap.add_argument("--user", default="arch", help="SSH username (default: arch)")
    ap.add_argument("--ssh-key", default=os.environ.get("NX769J_SSH_KEY"), help="Optional SSH key path")
    ap.add_argument("--sudo-pass", default=None, help="Optional sudo password if NOPASSWD is disabled")
    args = ap.parse_args()

    dm_bin, slot_bin = compile_static_binaries(pathlib.Path(args.out_dir), cc=args.cc)
    print(f"[ok] built static ARM64 helpers:\n  {dm_bin}\n  {slot_bin}")
    if args.compile_only:
        return 0

    ssh_cmd = build_ssh_base_cmd(args.host, args.user, args.ssh_key)
    sudo_prefix = (
        f"echo {shlex.quote(args.sudo_pass)} | sudo -S " if args.sudo_pass else "sudo "
    )

    for local_file, remote_dest, mode in [
        (dm_bin, "/usr/local/sbin/nx_dm_tool", "0755"),
        (slot_bin, "/usr/local/sbin/nx769j-slotctl", "4755"),
        (OVERLAY_SWITCHER, "/usr/local/bin/reboot-to-android", "0755"),
    ]:
        data = local_file.read_bytes()
        remote_cmd = (
            f"{sudo_prefix}install -d -m 0755 {shlex.quote(str(pathlib.Path(remote_dest).parent))} && "
            f"{sudo_prefix}tee {shlex.quote(remote_dest)} >/dev/null && "
            f"{sudo_prefix}chmod {mode} {shlex.quote(remote_dest)}"
        )
        subprocess.run(ssh_cmd + [remote_cmd], input=data, check=True)
        print(f"[ok] installed {remote_dest} (mode {mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
