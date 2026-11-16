#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Download and extract the pinned Debian `u-boot-qcom` binary required by
`boot/build_boot_img.py`.

Because `build_boot_img.py` patches exact ELF relocation addends and machine
instructions inside U-Boot (`0x15cd50` bytes), the input `u-boot.bin` must
match the pinned build SHA-256 verified below.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import urllib.request

EXPECTED_DEB_SHA256 = "09f34a775643821a1ec6042debe5bd34a55ae0f5c2b5e5a85c6ce7af4fd76c11"
EXPECTED_UBOOT_NODTB_LEN = 0x15CD50
EXPECTED_UBOOT_NODTB_SHA256 = "af23d4d522bfbee7276f750d9ed635fbccf00c916247833ccd34a421d93a8874"
EXPECTED_UBOOT_FULL_SHA256 = "abbfe1c9311e6d55fac823ef9f2d2c4b9bf6afaa6bb2bc644c993fd628caf5da"

DEFAULT_URLS = [
    "https://snapshot.debian.org/archive/debian/20261001T000000Z/pool/main/u/u-boot/u-boot-qcom_2026.10~rc5-1_arm64.deb",
    "https://deb.debian.org/debian/pool/main/u/u-boot/u-boot-qcom_2026.10~rc5-1_arm64.deb",
]


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_ar_members(deb_bytes: bytes) -> list[tuple[str, bytes]]:
    if not deb_bytes.startswith(b"!<arch>\n"):
        raise ValueError("Not a valid Unix `ar` / Debian `.deb` archive")
    pos = 8
    members: list[tuple[str, bytes]] = []
    while pos + 60 <= len(deb_bytes):
        hdr = deb_bytes[pos : pos + 60]
        pos += 60
        name = hdr[0:16].decode("ascii", errors="replace").strip().rstrip("/")
        size = int(hdr[48:58].decode("ascii").strip())
        if hdr[58:60] != b"`\n":
            raise ValueError(f"Corrupt ar member header for {name!r}")
        payload = deb_bytes[pos : pos + size]
        pos += size
        if pos % 2 == 1:
            pos += 1
        members.append((name, payload))
    return members


def decompress_tar_payload(name: str, payload: bytes) -> bytes:
    if name.endswith(".tar.xz"):
        import lzma

        return lzma.decompress(payload)
    if name.endswith(".tar.gz"):
        import gzip

        return gzip.decompress(payload)
    if name.endswith(".tar.zst"):
        zstd = shutil.which("zstd")
        if not zstd:
            raise RuntimeError("Need `zstd` binary to decompress data.tar.zst inside .deb")
        proc = subprocess.run([zstd, "-dc"], input=payload, capture_output=True, check=True)
        return proc.stdout
    if name == "data.tar":
        return payload
    raise ValueError(f"Unsupported .deb data archive format: {name}")


def extract_uboot_bin_from_deb(deb_bytes: bytes, verify_deb_hash: bool = True) -> bytes:
    if verify_deb_hash:
        got_deb = sha256_bytes(deb_bytes)
        if got_deb != EXPECTED_DEB_SHA256:
            raise ValueError(
                f"u-boot-qcom .deb SHA-256 mismatch: got {got_deb}, expected {EXPECTED_DEB_SHA256}"
            )
    for name, payload in parse_ar_members(deb_bytes):
        if name.startswith("data.tar"):
            tar_bytes = decompress_tar_payload(name, payload)
            with tarfile.open(fileobj=io.BytesIO(tar_bytes), mode="r:*") as tf:
                for member in tf.getmembers():
                    clean_name = member.name.lstrip("./")
                    if clean_name == "usr/lib/u-boot/qcm6490/u-boot.bin":
                        f = tf.extractfile(member)
                        if f is None:
                            break
                        ub = f.read()
                        prefix = ub[:EXPECTED_UBOOT_NODTB_LEN]
                        got_ub = sha256_bytes(prefix)
                        if got_ub != EXPECTED_UBOOT_NODTB_SHA256:
                            raise ValueError(
                                f"u-boot.bin (first 0x15cd50 bytes) SHA-256 mismatch: "
                                f"got {got_ub}, expected {EXPECTED_UBOOT_NODTB_SHA256}"
                            )
                        return ub
    raise FileNotFoundError("usr/lib/u-boot/qcm6490/u-boot.bin not found in .deb")


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch or extract pinned U-Boot qcm6490 binary for NX769J")
    ap.add_argument(
        "--deb",
        default=None,
        help="Optional path to local u-boot-qcom_2026.10~rc5-1_arm64.deb (skip network download)",
    )
    ap.add_argument(
        "--url",
        action="append",
        default=[],
        help="Custom download URL for u-boot-qcom_2026.10~rc5-1_arm64.deb",
    )
    ap.add_argument(
        "-o",
        "--out",
        default=str(pathlib.Path(__file__).resolve().parent / "cache" / "u-boot.bin"),
        help="Output path for verified u-boot.bin",
    )
    ap.add_argument(
        "--no-verify-deb-hash",
        action="store_true",
        help="Skip .deb envelope hash check (still verifies extracted u-boot.bin SHA-256)",
    )
    args = ap.parse_args()

    out_path = pathlib.Path(args.out)
    if out_path.is_file():
        data = out_path.read_bytes()
        if (
            len(data) >= EXPECTED_UBOOT_NODTB_LEN
            and sha256_bytes(data[:EXPECTED_UBOOT_NODTB_LEN]) == EXPECTED_UBOOT_NODTB_SHA256
        ):
            print(f"[ok] verified existing {out_path} ({len(data)} bytes)")
            return 0

    if args.deb:
        deb_bytes = pathlib.Path(args.deb).read_bytes()
    else:
        urls = args.url or DEFAULT_URLS
        deb_bytes = b""
        last_err: Exception | None = None
        for url in urls:
            try:
                print(f"[fetch] downloading {url} ...")
                req = urllib.request.Request(url, headers={"User-Agent": "nx769j-omarchy-linux/1.0"})
                with urllib.request.urlopen(req, timeout=30) as resp:
                    deb_bytes = resp.read()
                if deb_bytes:
                    break
            except Exception as exc:
                last_err = exc
                print(f"[warn] failed {url}: {exc}", file=sys.stderr)
        if not deb_bytes:
            raise RuntimeError(f"Unable to download u-boot-qcom .deb: {last_err}")

    ub_bytes = extract_uboot_bin_from_deb(deb_bytes, verify_deb_hash=not args.no_verify_deb_hash)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(ub_bytes)
    print(f"[ok] wrote verified u-boot.bin ({len(ub_bytes)} bytes) -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
