#!/usr/bin/env python3
"""Build the minimal NX769J bring-up initramfs as a deterministic newc cpio.gz.

The init script waits for the offline-candidate root filesystem by UUID,
mounts it and switch_roots to its systemd. On any failure it drops to a
busybox shell on the console; it writes to no other filesystem.

usage: make-initramfs.py BUSYBOX_BINARY ROOTFS_UUID OUTPUT.cpio.gz
"""

import gzip
import hashlib
import pathlib
import re
import sys

INIT = """#!/bin/sh
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "nx769j: experimental initramfs, waiting for root UUID __UUID__"
dev=""
for i in $(seq 1 100); do
  mdev -s 2>/dev/null
  for d in $(blkid 2>/dev/null | grep "__UUID__" | cut -d: -f1); do dev=$d; done
  [ -n "$dev" ] && break
  sleep 0.2
done
if [ -n "$dev" ]; then
  echo "nx769j: mounting $dev as /"
  mkdir -p /newroot
  if mount -t ext4 -o rw "$dev" /newroot; then
    killall mdev 2>/dev/null
    exec switch_root /newroot /sbin/init
  fi
fi
echo "nx769j: root filesystem not found or not mountable; emergency shell"
exec sh
"""


def entry(name, mode, data=b"", rdev=(0, 0), uid=0, gid=0):
    raw_name = name.encode() + b"\0"
    def pad(blob):
        return blob + b"\0" * (-len(blob) % 4)
    header = (
        b"070701"
        + b"".join(
            f"{v:08x}".encode()
            for v in (
                1, mode, uid, gid, 1, 0, len(data), 0, 0,
                rdev[0], rdev[1], len(raw_name), 0,
            )
        )
    )
    return pad(header + raw_name) + pad(data)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    busybox_path, uuid, output = sys.argv[1:4]
    if not re.fullmatch(
        r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", uuid
    ):
        raise SystemExit("invalid root filesystem UUID")
    busybox = pathlib.Path(busybox_path).read_bytes()
    if busybox[:4] != b"\x7fELF" or busybox[18:20] != b"\xb7\x00":
        raise SystemExit("busybox input is not an AArch64 ELF")
    symlink = 0o120777
    directory = 0o040755
    parts = [
        entry(".", directory),
        entry("bin", directory),
        entry("dev", directory),
        entry("proc", directory),
        entry("sys", directory),
        entry("newroot", directory),
        entry("dev/console", 0o020600, rdev=(5, 1)),
        entry("dev/null", 0o020666, rdev=(1, 3)),
        entry("bin/busybox", 0o100755, busybox),
        entry("bin/sh", symlink, b"/bin/busybox"),
        entry("init", 0o100755, INIT.replace("__UUID__", uuid).encode()),
        entry("TRAILER!!!", 0),
    ]
    archive = b"".join(parts)
    with open(output, "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, mtime=0) as gz:
            gz.write(archive)
    digest = hashlib.sha256(pathlib.Path(output).read_bytes()).hexdigest()
    print(f"initramfs: {output} sha256={digest}")


if __name__ == "__main__":
    main()
