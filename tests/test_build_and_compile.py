#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Offline build, syntax, cross-compile, DTB, topology, and boot.img packaging tests
for nx769j-omarchy-linux.

Optional full end-to-end boot.img packaging test runs when `NX769J_TEST_INPUTS_DIR`
(or an explicit `--inputs-dir`) provides:
  Image-7.1.1, nx769j.dtb, modules.tar.zst, u-boot.bin, rootfs_256g.extents (or rootfs.extents)
"""

from __future__ import annotations

import os
import pathlib
import py_compile
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


class TestOfflineSyntaxAndCompile(unittest.TestCase):
    def test_all_python_scripts_compile(self) -> None:
        py_files = [
            p
            for p in sorted(REPO_ROOT.rglob("*.py"))
            if "__pycache__" not in p.parts and "build" not in p.parts
        ]
        self.assertGreater(len(py_files), 10)
        for pyf in py_files:
            py_compile.compile(str(pyf), doraise=True)

    def test_all_shell_scripts_bash_n(self) -> None:
        sh_files = [p for p in sorted(REPO_ROOT.rglob("*.sh")) if "overlay" not in p.parts]
        self.assertGreater(len(sh_files), 0)
        for shf in sh_files:
            subprocess.run(["bash", "-n", str(shf)], check=True)
        # Also validate POSIX sh/bash scripts inside overlay/usr/local
        for cand in (REPO_ROOT / "rootfs" / "overlay" / "usr" / "local").rglob("*"):
            if not cand.is_file():
                continue
            first_line = cand.read_text(encoding="utf-8", errors="replace").splitlines()[:1]
            if first_line and first_line[0].startswith("#!") and ("sh" in first_line[0]):
                shell_bin = "bash" if "bash" in first_line[0] else "sh"
                subprocess.run([shell_bin, "-n", str(cand)], check=True)

    def test_topology_validator(self) -> None:
        subprocess.run(
            [sys.executable, str(REPO_ROOT / "drivers" / "audio" / "gen_nx769j_tplg.py"), "--verify-only"],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_cross_compile_static_dualboot_tools(self) -> None:
        cc = shutil.which("aarch64-linux-gnu-gcc")
        if not cc:
            self.skipTest("aarch64-linux-gnu-gcc not installed on host")
        with tempfile.TemporaryDirectory(prefix="nx769j_test_cc_") as tmpdir:
            subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "storage-dualboot" / "stage_dualboot_tools.py"),
                    "--compile-only",
                    "--cc",
                    cc,
                    "--out-dir",
                    tmpdir,
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            for name in ("nx_dm_tool", "nx769j-slotctl"):
                bin_p = pathlib.Path(tmpdir) / name
                self.assertTrue(bin_p.is_file())
                hdr = bin_p.read_bytes()[:20]
                self.assertEqual(hdr[:4], b"\x7fELF")

    def test_c_drivers_syntax_check(self) -> None:
        cc = shutil.which("aarch64-linux-gnu-gcc")
        if not cc:
            self.skipTest("aarch64-linux-gnu-gcc not installed on host")
        c_files = [
            REPO_ROOT / "drivers" / "display" / "nx769j_fb_wl.c",
            REPO_ROOT / "drivers" / "display" / "nx769j_vkbd.c",
            REPO_ROOT / "drivers" / "thermal-fan" / "nx769j-fan.c",
            REPO_ROOT / "drivers" / "thermal-fan" / "nx769j_perf.c",
            REPO_ROOT / "drivers" / "power" / "nx769j_powerkey.c",
        ]
        # Skip -fsyntax-only if target lacks cross-libc headers (e.g. bare elf/homebrew nostdlib gcc)
        probe = subprocess.run(
            [cc, "-x", "c", "-fsyntax-only", "-"],
            input="#include <stdio.h>\nint x = 0;\n",
            text=True,
            capture_output=True,
        )
        if probe.returncode != 0:
            self.skipTest(f"{cc} is bare/nostdlib without libc sysroot headers")
        for cf in c_files:
            subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-fsyntax-only", str(cf)], check=True)

    def test_atomic_pivot_offline_simulation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="nx769j_pivot_sim_") as tmpdir:
            root = pathlib.Path(tmpdir)
            for d in ("bin", "etc", "lib", "usr", "sbin"):
                (root / d).mkdir()
            (root / "etc" / "os-release").write_text('ID=ubuntu\nNAME="Kubuntu"\n', encoding="utf-8")
            stage = root / "arch-stage"
            for d in ("bin", "etc", "usr", "sbin", "lib"):
                (stage / d).mkdir(parents=True)
            (stage / "etc" / "arch-release").write_text("", encoding="utf-8")
            (root / ".pivot-arch-stage").write_text("PIVOT\n", encoding="utf-8")

            subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "rootfs" / "atomic_pivot_to_arch.py"),
                    "--execute-offline-root",
                    str(root),
                ],
                check=True,
            )
            self.assertTrue((root / "etc" / "arch-release").is_file())
            self.assertTrue((root / "old-kubuntu" / "etc" / "os-release").is_file())
            self.assertTrue((root / ".omarchy-pivoted-ok").is_file())

    def test_end_to_end_boot_img_build_if_inputs_available(self) -> None:
        inputs_dir_env = os.environ.get("NX769J_TEST_INPUTS_DIR", "").strip()
        if not inputs_dir_env:
            self.skipTest("Set NX769J_TEST_INPUTS_DIR to run full boot.img end-to-end build test")
        inp = pathlib.Path(inputs_dir_env)
        image = inp / "Image-7.1.1"
        dtb = inp / "nx769j.dtb"
        modules = inp / "modules.tar.zst"
        uboot = inp / "u-boot.bin"
        extents = inp / "rootfs_256g.extents"
        if not extents.is_file():
            extents = inp / "rootfs.extents"
        for req in (image, dtb, modules, uboot, extents):
            if not req.is_file():
                self.fail(f"Missing required test input: {req}")

        with tempfile.TemporaryDirectory(prefix="nx769j_e2e_boot_") as tmpdir:
            tdir = pathlib.Path(tmpdir)
            out_boot = tdir / "test-boot.img"
            subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "boot" / "build_boot_img.py"),
                    "--image",
                    str(image),
                    "--dtb",
                    str(dtb),
                    "--modules",
                    str(modules),
                    "--uboot-bin",
                    str(uboot),
                    "--extents",
                    str(extents),
                    "--work-dir",
                    str(tdir / "work"),
                    "-o",
                    str(out_boot),
                ],
                check=True,
            )
            self.assertEqual(out_boot.stat().st_size, 67112960)

            # Validate boot image header via unpack_bootimg.py and dry-run flash_slot_a.py
            unpack_dir = tdir / "unpacked"
            subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "boot" / "tools" / "unpack_bootimg.py"),
                    "--boot_img",
                    str(out_boot),
                    "--out",
                    str(unpack_dir),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual((unpack_dir / "kernel").stat().st_size, 0x4000000)

            subprocess.run(
                [
                    sys.executable,
                    str(REPO_ROOT / "storage-dualboot" / "flash_slot_a.py"),
                    "--serial",
                    "TESTSERIAL",
                    "--boot-img",
                    str(out_boot),
                    "--vbmeta-out",
                    str(tdir / "vbmeta_disabled.img"),
                ],
                check=True,
                capture_output=True,
                text=True,
            )


if __name__ == "__main__":
    unittest.main(verbosity=2)
