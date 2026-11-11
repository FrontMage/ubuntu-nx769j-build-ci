#!/usr/bin/env python3
"""Static contract checks for the NX769J offline candidate build.

Run from anywhere: python3 tests/nx769j/test_static.py
These checks prove the scripts agree with each other; they do not build a
kernel or rootfs, and never touch a device.
"""

import io
import pathlib
import re
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
NX = ROOT / "scripts" / "nx769j"
WORKFLOW = ROOT / ".github" / "workflows" / "nx769j-candidate.yml"
ARTIFACTS = {"Image", "nx769j.dtb", "modules.tar.zst", "perf", "kernel-manifest.json"}
failures = []


def check(label, ok, detail=""):
    print(f"{'ok' if ok else 'FAIL'}: {label}" + (f" ({detail})" if detail and not ok else ""))
    if not ok:
        failures.append(label)


def main():
    workflow = WORKFLOW.read_text(encoding="utf-8")
    try:
        import yaml

        yaml.safe_load(workflow)
        check("workflow YAML parses", True)
    except Exception as exc:  # missing library or bad YAML both surface here
        check("workflow YAML parses", False, str(exc))

    referenced = sorted(set(re.findall(r"scripts/nx769j/[\w.\-]+", workflow)))
    for rel in referenced:
        check(f"workflow reference exists: {rel}", (ROOT / rel).is_file())

    for script in sorted(NX.glob("*.sh")):
        proc = subprocess.run(
            ["bash", "-n", str(script)], capture_output=True, text=True
        )
        check(f"bash -n {script.name}", proc.returncode == 0, proc.stderr.strip())
        check(f"executable: {script.name}", bool(script.stat().st_mode & 0o111))

    kernel_script = (NX / "build-kernel.sh").read_text(encoding="utf-8")
    rootfs_script = (NX / "build-rootfs.sh").read_text(encoding="utf-8")
    for name in sorted(ARTIFACTS):
        check(f"build-kernel.sh produces {name}", name in kernel_script)
        check(f"build-rootfs.sh consumes {name}", name in rootfs_script)
    check(
        "manifest carries kernel_release",
        '"kernel_release"' in kernel_script and "kernel_release" in rootfs_script,
    )
    check(
        "kernel source commit is pinned (40 hex)",
        bool(re.search(r"KERNEL_COMMIT:-[0-9a-f]{40}\}", kernel_script)),
    )
    check(
        "perf avoids runner-version-locked libraries",
        "NO_LIBPYTHON=1" in kernel_script and "NO_LIBLLVM=1" in kernel_script,
    )
    check(
        "rootfs installs perf runtime libraries",
        "libtraceevent-dev" in rootfs_script and "libelf-dev" in rootfs_script,
    )
    check(
        "no boot image is produced",
        "boot.img" not in kernel_script and "mkbootimg" not in kernel_script,
    )

    pack_script = (NX / "pack-boot-candidate.sh").read_text(encoding="utf-8")
    check(
        "boot candidate pins stock 4096 page size",
        "--pagesize 4096" in pack_script,
    )
    check(
        "boot candidate is marked do-not-flash",
        "DO_NOT_FLASH" in pack_script,
    )

    for rel in (
        "scripts/nx769j/make-initramfs.py",
        "devices/nx769j/kernel/sm8650-nubia-nx769j.dts",
        "devices/nx769j/kernel/nx769j.config",
        "devices/nx769j/kernel/qcom-nx769j-binding.patch",
        "devices/nx769j/kernel/qcom-nx769j-makefile.patch",
        "patches/nx769j/y700-rootfs-generic.patch",
    ):
        check(f"device input exists: {rel}", (ROOT / rel).is_file())

    sys.path.insert(0, str(NX))
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "check_module_tar", NX / "check-module-tar.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def make_tar(entries):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as archive:
            for name, kind in entries:
                info = tarfile.TarInfo(name)
                if kind == "dir":
                    info.type = tarfile.DIRTYPE
                    archive.addfile(info)
                elif kind == "symlink":
                    info.type = tarfile.SYMTYPE
                    info.linkname = "/tmp/elsewhere"
                    archive.addfile(info)
                else:
                    data = b"x"
                    info.size = len(data)
                    archive.addfile(info, io.BytesIO(data))
        buf.seek(0)
        return buf

    good = make_tar(
        [
            ("lib/modules/7.1.1-nx769j", "dir"),
            ("lib/modules/7.1.1-nx769j/kernel/foo.ko", "file"),
        ]
    )
    try:
        count = module.check(good, "7.1.1-nx769j")
        check("module tar accepts matching release", count == 1)
    except Exception as exc:
        check("module tar accepts matching release", False, str(exc))

    for label, entries, release in (
        (
            "module tar rejects other release",
            [("lib/modules/9.9.9/kernel/foo.ko", "file")],
            "7.1.1-nx769j",
        ),
        (
            "module tar rejects symlinks",
            [("lib/modules/7.1.1-nx769j/build", "symlink")],
            "7.1.1-nx769j",
        ),
        (
            "module tar rejects traversal",
            [("lib/modules/7.1.1-nx769j/../../etc/passwd", "file")],
            "7.1.1-nx769j",
        ),
    ):
        try:
            module.check(make_tar(entries), release)
            check(label, False, "no error raised")
        except Exception:
            check(label, True)

    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\nall static checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
