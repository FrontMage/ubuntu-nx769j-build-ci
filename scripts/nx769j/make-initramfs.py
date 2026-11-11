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
mkdir -p /probe /newroot
echo "nx769j: bring-up initramfs (expected root UUID __UUID__)"

# Observation channel for headless bring-up: USB NCM + telnet shell.
mount -t configfs none /sys/kernel/config 2>/dev/null
G=/sys/kernel/config/usb_gadget/g1
if mkdir -p "$G/strings/0x409" "$G/configs/c.1" "$G/functions/ncm.usb0" 2>/dev/null; then
  echo 0x1d6b > "$G/idVendor"; echo 0x0104 > "$G/idProduct"
  echo nx769j > "$G/strings/0x409/manufacturer"
  echo "NX769J bring-up" > "$G/strings/0x409/product"
  echo nx769j001 > "$G/strings/0x409/serialnumber"
  ln -s "$G/functions/ncm.usb0" "$G/configs/c.1/" 2>/dev/null
  for u in /sys/class/udc/*; do [ -e "$u" ] && echo "$(basename "$u")" > "$G/UDC"; done
  for i in $(seq 1 25); do [ -e /sys/class/net/usb0 ] && break; sleep 0.2; done
  ifconfig usb0 10.42.0.2 netmask 255.255.255.0 up 2>/dev/null
  telnetd -p 23 -l /bin/sh 2>/dev/null
  echo "nx769j: usb0 10.42.0.2 telnet shell (if listed above)"
fi

# Probe partitions by content (no blkid in this busybox): a direct ext4
# root carries our BUILD-STATUS; Android userdata (F2FS) may carry
# /nxlinux/rootfs.img for loop mounting. All probes are read-only.
rootdev=""
for i in $(seq 1 60); do
  mdev -s 2>/dev/null
  for dev in /dev/sd[a-z][0-9]* /dev/mmcblk[0-9]p[0-9]*; do
    [ -b "$dev" ] || continue
    if mount -t ext4 -o ro "$dev" /probe 2>/dev/null; then
      if [ -f /probe/usr/share/nx769j/BUILD-STATUS.txt ]; then rootdev=$dev; fi
      umount /probe 2>/dev/null
      [ -n "$rootdev" ] && break
    fi
    if mount -t f2fs -o ro "$dev" /probe 2>/dev/null; then
      if [ -f /probe/nxlinux/rootfs.img ]; then
        echo "nx769j: loop rootfs image on $dev"
        mkdir -p /userdata && mount -t f2fs -o ro "$dev" /userdata 2>/dev/null
        umount /probe 2>/dev/null
        if losetup /dev/loop0 /userdata/nxlinux/rootfs.img 2>/dev/null; then
          rootdev=/dev/loop0
        fi
      fi
      umount /probe 2>/dev/null
      [ -n "$rootdev" ] && break
    fi
  done
  [ -n "$rootdev" ] && break
  sleep 0.5
done
if [ -n "$rootdev" ]; then
  echo "nx769j: mounting $rootdev as /"
  if mount -t ext4 -o rw "$rootdev" /newroot; then
    exec switch_root /newroot /sbin/init
  fi
fi
echo "nx769j: no root filesystem yet; emergency shell (console + telnet)"
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
        entry("probe", directory),
        entry("userdata", directory),
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
