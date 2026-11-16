#!/usr/bin/env python3
"""
Idempotent ADSP + BattMgr firmware staging script for Nubia RedMagic 9 Pro (NX769J).

Reads factory partition `/dev/disk/by-partlabel/modem_b` (read-only FAT16 parser,
zero mount required) and stages:
  - image/adsp.mdt + image/adsp.b00 .. adsp.b56
  - image/adsp_dtb.mdt + image/adsp_dtb.b00 .. adsp_dtb.b02
  - image/adspr.jsn, image/adsps.jsn, image/adspua.jsn, image/battmgr.jsn
into:
  /lib/firmware/qcom/sm8650/nubia/nx769j/
and creates compatibility symlinks in:
  /lib/firmware/qcom/sm8650/
  /lib/firmware/

Supports `--dry-run` for 100% read-only verification on live hardware.
"""

import argparse
import hashlib
import os
import struct
import sys
from typing import Dict, List, Tuple


EXPECTED_CARVEOUTS = {
    "adsp.mdt": (0x9DF00000, 0xA1F80000),
    "adsp_dtb.mdt": (0x9DE80000, 0x9DF00000),
}

REQUIRED_JSNS = (
    "adspr.jsn",
    "adsps.jsn",
    "adspua.jsn",
    "battmgr.jsn",
)


def parse_fat16_image_dir(part_path: str) -> Dict[str, bytes]:
    """Read `image/*` files directly from a FAT16 partition image opened read-only."""
    files: Dict[str, bytes] = {}
    with open(part_path, "rb") as f:
        bpb = f.read(512)
        if len(bpb) < 512 or bpb[510:512] != b"\x55\xaa":
            raise RuntimeError(f"{part_path}: invalid FAT boot sector signature")

        bytes_per_sec = struct.unpack_from("<H", bpb, 11)[0]
        sec_per_clus = bpb[13]
        rsvd_sec_cnt = struct.unpack_from("<H", bpb, 14)[0]
        num_fats = bpb[16]
        root_ent_cnt = struct.unpack_from("<H", bpb, 17)[0]
        fat_sz_16 = struct.unpack_from("<H", bpb, 22)[0]
        if root_ent_cnt == 0 or fat_sz_16 == 0:
            raise RuntimeError(f"{part_path}: expected FAT16 volume (root_ent_cnt={root_ent_cnt}, fat_sz_16={fat_sz_16})")

        fat_offset = rsvd_sec_cnt * bytes_per_sec
        f.seek(fat_offset)
        fat_bytes = f.read(fat_sz_16 * bytes_per_sec)

        root_dir_offset = (rsvd_sec_cnt + num_fats * fat_sz_16) * bytes_per_sec
        root_dir_bytes = root_ent_cnt * 32
        root_dir_secs = (root_dir_bytes + bytes_per_sec - 1) // bytes_per_sec
        data_offset = root_dir_offset + root_dir_secs * bytes_per_sec
        clus_bytes = sec_per_clus * bytes_per_sec

        def read_chain(start_clus: int, size: int | None = None) -> bytes:
            if start_clus < 2:
                return b""
            chunks: List[bytes] = []
            clus = start_clus
            seen = set()
            total = 0
            while 2 <= clus < 0xFFF8:
                if clus in seen:
                    raise RuntimeError(f"FAT16 cluster loop at {clus}")
                seen.add(clus)
                f.seek(data_offset + (clus - 2) * clus_bytes)
                chunk = f.read(clus_bytes)
                chunks.append(chunk)
                total += len(chunk)
                if size is not None and total >= size:
                    break
                clus = struct.unpack_from("<H", fat_bytes, clus * 2)[0]
            raw = b"".join(chunks)
            return raw[:size] if size is not None else raw

        def iter_dir_entries(buf: bytes) -> List[Tuple[str, int, int, int]]:
            out: List[Tuple[str, int, int, int]] = []
            lfn_parts: List[Tuple[int, str]] = []
            for off in range(0, len(buf), 32):
                ent = buf[off : off + 32]
                if len(ent) < 32 or ent[0] == 0x00:
                    break
                if ent[0] == 0xE5:
                    lfn_parts.clear()
                    continue
                attr = ent[11]
                if attr == 0x0F:
                    seq = ent[0] & 0x1F
                    raw_chars = ent[1:11] + ent[14:26] + ent[28:32]
                    chars = []
                    for i in range(0, len(raw_chars), 2):
                        ch = struct.unpack_from("<H", raw_chars, i)[0]
                        if ch in (0x0000, 0xFFFF):
                            break
                        chars.append(chr(ch))
                    lfn_parts.append((seq, "".join(chars)))
                    continue
                if attr & 0x08:
                    lfn_parts.clear()
                    continue
                if lfn_parts:
                    lfn_parts.sort(key=lambda x: x[0])
                    name = "".join(p for _, p in lfn_parts)
                    lfn_parts.clear()
                else:
                    stem = ent[0:8].decode("ascii", errors="replace").rstrip(" ")
                    ext = ent[8:11].decode("ascii", errors="replace").rstrip(" ")
                    name = f"{stem}.{ext}" if ext else stem
                clus = struct.unpack_from("<H", ent, 26)[0]
                size = struct.unpack_from("<I", ent, 28)[0]
                out.append((name, attr, clus, size))
            return out

        f.seek(root_dir_offset)
        root_buf = f.read(root_dir_bytes)
        image_clus = None
        for name, attr, clus, _ in iter_dir_entries(root_buf):
            if name.lower() == "image" and (attr & 0x10):
                image_clus = clus
                break
        if image_clus is None:
            raise RuntimeError(f"{part_path}: `image/` directory not found in FAT16 root")

        image_dir_buf = read_chain(image_clus)
        for name, attr, clus, size in iter_dir_entries(image_dir_buf):
            if attr & 0x10:
                continue
            lname = name.lower()
            if lname.startswith("adsp") or lname == "battmgr.jsn":
                files[lname] = read_chain(clus, size)

    return files


def validate_mdt_bundle(mdt_name: str, files: Dict[str, bytes]) -> List[str]:
    """Validate Qualcomm split-ELF `.mdt` + `.bXX` segments against mdt_loader rules."""
    if mdt_name not in files:
        raise RuntimeError(f"Missing required firmware file: {mdt_name}")
    mdt = files[mdt_name]
    if mdt[:4] != b"\x7fELF":
        raise RuntimeError(f"{mdt_name}: invalid ELF magic")

    ei_class = mdt[4]
    if ei_class == 1:
        e_phoff, e_phentsize, e_phnum = struct.unpack_from("<IHH", mdt, 28)[0], struct.unpack_from("<H", mdt, 42)[0], struct.unpack_from("<H", mdt, 44)[0]
        ph_fmt = "<IIIIIIII"
    elif ei_class == 2:
        e_phoff = struct.unpack_from("<Q", mdt, 32)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", mdt, 54)
        ph_fmt = None
    else:
        raise RuntimeError(f"{mdt_name}: unsupported ELF class {ei_class}")

    stem = mdt_name[:-4]
    needed = [mdt_name]
    min_pa = None
    max_pa = None

    for idx in range(e_phnum):
        off = idx * e_phentsize
        # In split .mdt files, phdrs sit right after ehdr (at e_phoff).
        if ei_class == 1:
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(
                ph_fmt, mdt, e_phoff + off
            )
        else:
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
                "<IIQQQQQQ", mdt, e_phoff + off
            )

        seg_name = f"{stem}.b{idx:02d}"
        if p_filesz > 0:
            if seg_name not in files:
                raise RuntimeError(f"{mdt_name}: missing non-empty segment {seg_name} (expected {p_filesz} bytes)")
            actual_sz = len(files[seg_name])
            # Check mdt_phdr_valid() loadable segments:
            # p_type == PT_LOAD (1), (p_flags & QCOM_MDT_TYPE_MASK [0x7000000]) != QCOM_MDT_TYPE_HASH [0x2000000], p_memsz != 0
            if p_type == 1 and ((p_flags & 0x07000000) != 0x02000000) and p_memsz > 0:
                if actual_sz != p_filesz:
                    raise RuntimeError(
                        f"{seg_name}: size mismatch (actual={actual_sz}, phdr.p_filesz={p_filesz})"
                    )
            needed.append(seg_name)
        elif seg_name in files:
            needed.append(seg_name)

        if p_type == 1 and ((p_flags & 0x07000000) != 0x02000000) and p_memsz > 0:
            min_pa = p_paddr if min_pa is None else min(min_pa, p_paddr)
            max_pa = (p_paddr + p_memsz) if max_pa is None else max(max_pa, p_paddr + p_memsz)

    lo_carve, hi_carve = EXPECTED_CARVEOUTS[mdt_name]
    if min_pa is None or max_pa is None:
        raise RuntimeError(f"{mdt_name}: no loadable PT_LOAD segments found")
    if min_pa < lo_carve or max_pa > hi_carve:
        raise RuntimeError(
            f"{mdt_name}: segment span [0x{min_pa:x}..0x{max_pa:x}] overflows DT carveout [0x{lo_carve:x}..0x{hi_carve:x}]"
        )

    print(
        f"[OK] {mdt_name}: {e_phnum} phdrs, {len(needed)} files, "
        f"PA [0x{min_pa:08x}..0x{max_pa:08x}] fits DT carveout [0x{lo_carve:08x}..0x{hi_carve:08x}]"
    )
    return sorted(set(needed))


def write_atomic(dst_path: str, content: bytes) -> bool:
    """Write `content` atomically if `dst_path` does not already have identical bytes."""
    if os.path.exists(dst_path) and not os.path.islink(dst_path):
        with open(dst_path, "rb") as f:
            if f.read() == content:
                return False
    tmp_path = f"{dst_path}.tmp.{os.getpid()}"
    with open(tmp_path, "wb") as f:
        f.write(content)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp_path, 0o644)
    os.replace(tmp_path, dst_path)
    return True


def ensure_rel_symlink(link_dir: str, fname: str, target_rel: str) -> bool:
    """Create or update `link_dir/fname -> target_rel` without clobbering real non-matching files."""
    link_path = os.path.join(link_dir, fname)
    if os.path.islink(link_path):
        if os.readlink(link_path) == target_rel:
            return False
        os.unlink(link_path)
    elif os.path.exists(link_path):
        return False
    os.symlink(target_rel, link_path)
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage NX769J ADSP & BattMgr firmware from modem_b")
    ap.add_argument(
        "--modem-part",
        default="/dev/disk/by-partlabel/modem_b",
        help="Path to factory modem_b block device (opened read-only)",
    )
    ap.add_argument(
        "--fw-root",
        default="/lib/firmware",
        help="Root firmware directory (default: /lib/firmware)",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Read and validate all firmware files without writing to --fw-root",
    )
    args = ap.parse_args()

    files = parse_fat16_image_dir(args.modem_part)
    needed_files: List[str] = []
    needed_files.extend(validate_mdt_bundle("adsp.mdt", files))
    needed_files.extend(validate_mdt_bundle("adsp_dtb.mdt", files))

    # Verify embedded Nubia NX769J battman FDT inside adsp_dtb.b01
    if b"battman" not in files.get("adsp_dtb.b01", b""):
        raise RuntimeError("adsp_dtb.b01 does not contain expected `battman` FDT node")
    print("[OK] adsp_dtb.b01: verified embedded `battman` charger/gauge DT configuration")

    for jsn in REQUIRED_JSNS:
        if jsn not in files or len(files[jsn]) == 0:
            raise RuntimeError(f"Missing required JSON service registry file: {jsn}")
        needed_files.append(jsn)
    print(f"[OK] Service registry JSONs present: {', '.join(REQUIRED_JSNS)}")

    needed_files = sorted(set(needed_files))
    total_bytes = sum(len(files[k]) for k in needed_files)
    adsp_mdt_sha = hashlib.sha256(files["adsp.mdt"]).hexdigest()[:16]
    dtb_mdt_sha = hashlib.sha256(files["adsp_dtb.mdt"]).hexdigest()[:16]
    print(
        f"[OK] Validated {len(needed_files)} firmware files ({total_bytes:,} bytes total; "
        f"adsp.mdt sha256={adsp_mdt_sha}..., adsp_dtb.mdt sha256={dtb_mdt_sha}...)"
    )

    if args.dry_run:
        print("[DRY-RUN] Validation succeeded; no filesystem changes written.")
        return 0

    board_dir = os.path.join(args.fw_root, "qcom", "sm8650", "nubia", "nx769j")
    soc_dir = os.path.join(args.fw_root, "qcom", "sm8650")
    os.makedirs(board_dir, mode=0o755, exist_ok=True)
    os.makedirs(soc_dir, mode=0o755, exist_ok=True)

    written = 0
    linked = 0
    for fname in needed_files:
        if write_atomic(os.path.join(board_dir, fname), files[fname]):
            written += 1
        if ensure_rel_symlink(soc_dir, fname, os.path.join("nubia", "nx769j", fname)):
            linked += 1
        if ensure_rel_symlink(args.fw_root, fname, os.path.join("qcom", "sm8650", "nubia", "nx769j", fname)):
            linked += 1

    print(
        f"[DONE] Staged {len(needed_files)} files in {board_dir} "
        f"({written} updated, {linked} compatibility symlinks created/updated)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
