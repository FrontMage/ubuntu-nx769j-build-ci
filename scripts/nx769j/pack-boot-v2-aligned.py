#!/usr/bin/env python3
"""Verify and assemble an offline Cerro-Scheme-E-shaped v2 boot image.

This is an offline alignment tool. It verifies that the kernel artifact
really contains its advertised embedded initramfs, then reproduces the
Nubia Cerro layout: padded Image, DTB appended at ARM64 image_size,
127-byte stub ramdisk, Android boot header v2 and exact 96 MiB output.
"""
import gzip
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

PARTITION_SIZE = 100_663_296
CMDLINE = (
    "ignore_loglevel pd_ignore_unused clk_ignore_unused "
    "console=ttyMSM0,115200n8 earlycon=qcom_geni,0x0089c000 "
    "log_buf_len=2M panic=0"
)


def init_entry(raw):
    offset = 0
    while offset + 110 <= len(raw) and raw[offset:offset + 6] == b"070701":
        fields = [int(raw[offset + 6 + i * 8:offset + 14 + i * 8], 16)
                  for i in range(13)]
        size, name_size = fields[6], fields[11]
        name_start = offset + 110
        data_start = name_start + name_size
        data_start += -data_start % 4
        name = raw[name_start:name_start + name_size - 1].decode()
        if name == "init":
            return raw[data_start:data_start + size]
        if name == "TRAILER!!!":
            return None
        offset = data_start + size
        offset += -offset % 4
    return None


def find_member(image, expected):
    expected_init = init_entry(expected)
    if expected_init is None:
        return False
    offset = 0
    while True:
        offset = image.find(b"\x1f\x8b\x08", offset)
        if offset < 0:
            return False
        try:
            d = zlib.decompressobj(16 + zlib.MAX_WBITS)
            raw = d.decompress(image[offset:], 16 * 1024 * 1024)
            if d.eof and init_entry(raw) == expected_init:
                return True
        except (zlib.error, OverflowError, ValueError):
            pass
        offset += 3


def main():
    if len(sys.argv) != 4:
        raise SystemExit(
            "usage: pack-boot-v2-aligned.py KERNEL_ARTIFACT_DIR TOOLS_DIR OUTPUT_DIR")
    kernel_dir, tools_dir, output_dir = map(pathlib.Path, sys.argv[1:])
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        raise SystemExit(f"refusing nonempty output directory: {output_dir}")

    image = (kernel_dir / "Image").read_bytes()
    dtb = (kernel_dir / "nx769j.dtb").read_bytes()
    embedded = (kernel_dir / "embedded-initramfs.cpio.gz").read_bytes()
    manifest = json.loads((kernel_dir / "kernel-manifest.json").read_text())
    if manifest.get("embedded_initramfs_included") is not True:
        raise SystemExit("kernel artifact does not declare an embedded initramfs")
    expected = gzip.decompress(embedded)
    if not find_member(image, expected):
        raise SystemExit("embedded initramfs payload not found inside Image")
    if not dtb.startswith(bytes.fromhex("d00dfeed")):
        raise SystemExit("DTB magic missing")

    image_size = struct.unpack_from("<Q", image, 16)[0]
    if image_size < len(image):
        raise SystemExit(f"ARM64 image_size {image_size} < file {len(image)}")
    payload = image + bytes(image_size - len(image)) + dtb

    repo = pathlib.Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="nx769j-v2-") as td:
        td = pathlib.Path(td)
        kernel_blob = td / "kernel-padded-dtb"
        kernel_blob.write_bytes(payload)
        stub = td / "ramdisk-stub"
        subprocess.run(
            [sys.executable, str(repo / "scripts/nx769j/make-stub-ramdisk.py"), str(stub)],
            check=True)
        if stub.stat().st_size != 127:
            raise SystemExit("stub ramdisk is not 127 bytes")
        raw = td / "boot.img"
        env = {"PYTHONPATH": str(tools_dir)}
        subprocess.run([
            sys.executable, str(tools_dir / "mkbootimg.py"),
            "--header_version", "2",
            "--os_version", "16.0.0", "--os_patch_level", "2026-07",
            "--kernel", str(kernel_blob), "--ramdisk", str(stub),
            "--dtb", str(kernel_dir / "nx769j.dtb"),
            "--pagesize", "4096", "--base", "0x00000000",
            "--kernel_offset", "0x00008000", "--ramdisk_offset", "0x01000000",
            "--second_offset", "0x00000000", "--tags_offset", "0x00000100",
            "--dtb_offset", "0x01f00000", "--cmdline", CMDLINE,
            "--output", str(raw)], check=True, env={**__import__("os").environ, **env})
        blob = raw.read_bytes()
        if len(blob) > PARTITION_SIZE:
            raise SystemExit("assembled boot image exceeds partition size")
        blob += bytes(PARTITION_SIZE - len(blob))

    out = output_dir / "boot-v2.img"
    out.write_bytes(blob)
    unpack = subprocess.run(
        [sys.executable, str(tools_dir / "unpack_bootimg.py"),
         "--boot_img", str(out), "--format=info"],
        capture_output=True, text=True,
        env={**__import__("os").environ, "PYTHONPATH": str(tools_dir)}, check=True)
    for needle in ("boot image header version: 2", "page size: 4096",
                   "ramdisk size: 127", "@0x0089c000"):
        if needle == "@0x0089c000":
            if "0x0089c000" not in unpack.stdout:
                raise SystemExit("assembled image did not round-trip expected cmdline")
        elif needle not in unpack.stdout:
            raise SystemExit(f"assembled image missing {needle!r}:\n{unpack.stdout}")

    report = {
        "status": "OFFLINE_ALIGNED_CANDIDATE_NOT_TESTED_ON_DEVICE",
        "kernel_release": manifest.get("kernel_release"),
        "kernel_commit": manifest.get("kernel_commit"),
        "image_size": image_size,
        "kernel_payload_bytes": len(payload),
        "ramdisk_bytes": 127,
        "partition_bytes": PARTITION_SIZE,
        "header_version": 2,
        "page_size": 4096,
        "cmdline": CMDLINE,
        "boot_image_sha256": hashlib.sha256(blob).hexdigest(),
    }
    (output_dir / "alignment-report.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n")
    (output_dir / "SHA256SUMS").write_text(
        f"{report['boot_image_sha256']}  boot-v2.img\n")
    print(f"aligned offline NX769J v2 boot image: {out} (not flashed)")


if __name__ == "__main__":
    main()
