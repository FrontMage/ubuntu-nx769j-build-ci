#!/usr/bin/env python3
"""Create Cerro's deterministic 127-byte placeholder ramdisk.

The file is a valid gzip stream containing an otherwise empty newc
archive. Cerro kernels carry the real initramfs inside the kernel image;
Android's external ramdisk field still carries this exact size.
"""
import binascii
import struct
import sys
import zlib


def trailer():
    name = b"TRAILER!!!\0"
    values = (1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0)
    blob = b"070701" + b"".join(f"{v:08x}".encode() for v in values) + name
    return blob + b"\0" * (-len(blob) % 4)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-stub-ramdisk.py OUTPUT")
    raw = trailer()
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15)
    compressed = compressor.compress(raw) + compressor.flush()
    extra_data = b"\0" * 70
    extra = b"NX" + struct.pack("<H", len(extra_data)) + extra_data
    gzip_header = (
        b"\x1f\x8b\x08\x04" + struct.pack("<I", 0) + b"\x00\xff"
        + struct.pack("<H", len(extra)) + extra
    )
    out = gzip_header + compressed + struct.pack(
        "<II", binascii.crc32(raw) & 0xFFFFFFFF, len(raw))
    if len(out) != 127:
        raise SystemExit(f"internal error: stub is {len(out)} bytes, want 127")
    with open(sys.argv[1], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
