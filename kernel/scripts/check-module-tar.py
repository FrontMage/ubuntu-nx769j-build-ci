#!/usr/bin/env python3
"""Validate an uncompressed kernel module tar stream before extracting it."""

import pathlib
import re
import sys
import tarfile


def check(stream, release):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", release):
        raise ValueError("invalid kernel release")
    expected = f"lib/modules/{release}"
    files = 0
    with tarfile.open(fileobj=stream, mode="r|") as archive:
        for entry in archive:
            raw = entry.name
            name = raw.removeprefix("./").rstrip("/")
            if raw.startswith("/") or ".." in pathlib.PurePosixPath(name).parts:
                raise ValueError(f"unsafe module path: {raw}")
            if name not in ("", ".", "lib", "lib/modules", expected) and not name.startswith(expected + "/"):
                raise ValueError(f"unexpected module path: {raw}")
            if not (entry.isdir() or entry.isfile()):
                raise ValueError(f"unsupported module archive entry type: {raw}")
            if entry.isfile() and name.startswith(expected + "/"):
                files += 1
    if not files:
        raise ValueError("modules archive contains no files for the expected release")
    return files


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: zstd -dc modules.tar.zst | check-module-tar.py KERNEL_RELEASE")
    try:
        count = check(sys.stdin.buffer, sys.argv[1])
    except (ValueError, tarfile.TarError) as exc:
        raise SystemExit(str(exc)) from exc
    print(f"validated {count} module files for {sys.argv[1]}")


if __name__ == "__main__":
    main()
