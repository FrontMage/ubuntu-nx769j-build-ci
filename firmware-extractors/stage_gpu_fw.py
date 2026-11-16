#!/usr/bin/env python3
"""
Idempotent Adreno 750 (gen70900 / 0x43051401) GPU + GMU + Zap-Shader firmware stager
for Nubia RedMagic 9 Pro (NX769J, SM8650).

Reads factory Android dynamic partition `/dev/disk/by-partlabel/super` (`/dev/sda7`)
strictly read-only in pure Python (Android liblp metadata + ext4 extent parser;
ZERO `mount`, ZERO `dmsetup`, ZERO `losetup` required) and extracts from `vendor_b`
(or `vendor_a` fallback) `/firmware/`:
  - gen70900_sqe.fw   (84,756 B) -> requested by msm.ko as `qcom/gen70900_sqe.fw`
  - gmu_gen70900.bin  (78,588 B) -> requested by msm.ko as `qcom/gmu_gen70900.bin`
  - gen70900_zap.mbn  (12,088 B) -> ZTE production-signed PAS_ID=13 zap shader
  - gen70900_aqe.fw   (33,176 B) -> companion AQE microcode

Validates SHA256 digests, single-file ELF32 MBN structure (`is_split == 0`),
`QCOM_MDT_RELOCATABLE` (`BIT(27)`), `qcom_mdt_get_size() == 0x1000 <= 0x2000`
(`gpu-micro-code@9b09a000` carveout), and OEM `'ZTE Signing CA0'` X.509 signature
(explicitly rejecting Qualcomm test-signed `gen70900_zap.mbn.back`).

Stages canonical files into:
  /lib/firmware/qcom/sm8650/nubia/nx769j/
and creates relative symlinks in:
  /lib/firmware/qcom/
  /lib/firmware/qcom/sm8650/

Supports `--dry-run` for 100% read-only verification on live hardware.
"""

import argparse
import hashlib
import os
import struct
import sys
from typing import BinaryIO, Dict, List, Optional, Tuple


LP_METADATA_GEOMETRY_MAGIC = 0x616C4467
LP_METADATA_GEOMETRY_OFFSET = 0x1000
LP_METADATA_GEOMETRY_SIZE = 0x1000
LP_METADATA_HEADER_MAGIC = 0x414C5030
LP_TARGET_TYPE_LINEAR = 0
SECTOR_SIZE = 512

EXT4_SUPER_OFFSET = 0x400
EXT4_SUPER_MAGIC = 0xEF53
EXT4_EXTENTS_FL = 0x00080000
EXT4_EXT_MAGIC = 0xF30A
EXT4_ROOT_INO = 2

ZAP_CARVEOUT_PA = 0x9B09A000
ZAP_CARVEOUT_SIZE = 0x2000  # 8,192 bytes (`gpu-micro-code@9b09a000`)

EXPECTED_FIRMWARE: Dict[str, Tuple[int, str]] = {
    "gen70900_sqe.fw": (
        84756,
        "557859a5f5adef35263c6ef1ec8eced8ced0bcdb8e0d446d4162d8500ac065c1",
    ),
    "gmu_gen70900.bin": (
        78588,
        "9685f62c42543befcaee68d25355e16bf55cdb0d573bddcc9f6633f59eec3f72",
    ),
    "gen70900_zap.mbn": (
        12088,
        "efaf9524a6f8e92c4c076fbb14759e95c1e068e7a7de26431578d15585f6c685",
    ),
    "gen70900_aqe.fw": (
        33176,
        "25237c161d727b8ea92b4beb837a31651b44ea78bfc34d95672af8f346a45825",
    ),
}


class LinearExtentsReader:
    """Read-only view over a sequence of linear `(physical_byte_offset, byte_length)` extents."""

    def __init__(self, fp: BinaryIO, extents: List[Tuple[int, int]]):
        self._fp = fp
        self._extents: List[Tuple[int, int, int]] = []
        logical = 0
        for phys_off, length in extents:
            self._extents.append((logical, logical + length, phys_off))
            logical += length
        self.total_size = logical

    def pread(self, offset: int, size: int) -> bytes:
        if size <= 0:
            return b""
        if offset < 0 or offset + size > self.total_size:
            raise RuntimeError(
                f"LinearExtentsReader out-of-bounds read: off=0x{offset:x} size=0x{size:x} total=0x{self.total_size:x}"
            )
        out = bytearray()
        cur = offset
        rem = size
        for log_start, log_end, phys_base in self._extents:
            if cur >= log_end:
                continue
            if cur < log_start:
                break
            take = min(rem, log_end - cur)
            self._fp.seek(phys_base + (cur - log_start))
            chunk = self._fp.read(take)
            if len(chunk) != take:
                raise RuntimeError(f"Short read on backing block device at 0x{phys_base + (cur - log_start):x}")
            out.extend(chunk)
            cur += take
            rem -= take
            if rem == 0:
                break
        if rem != 0:
            raise RuntimeError(f"Incomplete extent read: {rem} bytes remaining")
        return bytes(out)


def find_lp_partition_extents(fp: BinaryIO, prefer_names: Tuple[str, ...]) -> Tuple[str, int, List[Tuple[int, int]]]:
    """Parse Android `super` LP metadata and return `(part_name, slot_idx, [(phys_off, byte_len), ...])`."""
    fp.seek(LP_METADATA_GEOMETRY_OFFSET)
    geom = fp.read(LP_METADATA_GEOMETRY_SIZE)
    if len(geom) < 52:
        raise RuntimeError("Failed to read LP metadata geometry block")
    magic, struct_sz = struct.unpack_from("<II", geom, 0)
    if magic != LP_METADATA_GEOMETRY_MAGIC:
        raise RuntimeError(f"Invalid LP geometry magic: 0x{magic:08x} (expected 0x{LP_METADATA_GEOMETRY_MAGIC:08x})")
    max_meta_sz, slot_cnt = struct.unpack_from("<II", geom, 36)
    if slot_cnt == 0 or max_meta_sz == 0:
        raise RuntimeError(f"Invalid LP geometry: max_meta_sz={max_meta_sz}, slot_cnt={slot_cnt}")

    meta_base = LP_METADATA_GEOMETRY_OFFSET + LP_METADATA_GEOMETRY_SIZE * 2  # primary + backup geometry = 0x3000
    # Prefer slot 1 (`_b`) first on NX769J, then slot 0 (`_a`)
    slot_order = [1, 0] if slot_cnt >= 2 else [0]
    for s in range(slot_cnt):
        if s not in slot_order:
            slot_order.append(s)

    for slot in slot_order:
        slot_off = meta_base + slot * max_meta_sz
        fp.seek(slot_off)
        hdr = fp.read(256)
        if len(hdr) < 128:
            continue
        h_magic, major, minor, h_sz = struct.unpack_from("<IHHI", hdr, 0)
        if h_magic != LP_METADATA_HEADER_MAGIC:
            continue
        tables_sz = struct.unpack_from("<I", hdr, 44)[0]
        # Table descriptors at header offset 80 (after 32-byte SHA256 tables_checksum at 48..80):
        #   0: partitions (offset 80)
        #   1: extents    (offset 92)
        #   2: groups     (offset 104)
        #   3: block_devs (offset 116)
        p_off, p_cnt, p_esz = struct.unpack_from("<III", hdr, 80)
        e_off, e_cnt, e_esz = struct.unpack_from("<III", hdr, 92)
        b_off, b_cnt, b_esz = struct.unpack_from("<III", hdr, 116)

        fp.seek(slot_off + h_sz)
        tables = fp.read(tables_sz)
        if len(tables) < tables_sz:
            continue

        # Block device 0 `first_logical_sector` is uint64 at offset 0 of LpMetadataBlockDevice
        if b_cnt < 1 or b_esz < 8:
            continue
        # Verify block_devices[0].partition_name == "super" (at +16, 36 bytes); first_logical_sector is at +0
        parts_found: Dict[str, List[Tuple[int, int]]] = {}
        for i in range(p_cnt):
            po = p_off + i * p_esz
            raw_name = tables[po : po + 36].split(b"\x00", 1)[0].decode("ascii", errors="replace")
            first_ext, num_exts = struct.unpack_from("<II", tables, po + 40)
            if num_exts == 0:
                continue
            exts: List[Tuple[int, int]] = []
            valid = True
            for ei in range(first_ext, first_ext + num_exts):
                eo = e_off + ei * e_esz
                num_sec, target_type, target_data, target_src = struct.unpack_from("<QIQI", tables, eo)
                if target_type != LP_TARGET_TYPE_LINEAR or target_src != 0:
                    valid = False
                    break
                exts.append((target_data * SECTOR_SIZE, num_sec * SECTOR_SIZE))
            if valid and exts:
                parts_found[raw_name] = exts

        for wanted in prefer_names:
            if wanted in parts_found:
                return wanted, slot, parts_found[wanted]

    raise RuntimeError(f"Could not find any of {prefer_names} in super LP metadata")


def read_ext4_firmware_dir(reader: LinearExtentsReader, wanted_names: Tuple[str, ...]) -> Dict[str, bytes]:
    """Read `/firmware/<name>` files from a read-only ext4 volume."""
    sb = reader.pread(EXT4_SUPER_OFFSET, 1024)
    magic = struct.unpack_from("<H", sb, 0x38)[0]
    if magic != EXT4_SUPER_MAGIC:
        raise RuntimeError(f"Invalid ext4 superblock magic: 0x{magic:04x}")

    inodes_per_group = struct.unpack_from("<I", sb, 0x28)[0]
    log_block_size = struct.unpack_from("<I", sb, 0x18)[0]
    block_size = 1024 << log_block_size
    inode_size = struct.unpack_from("<H", sb, 0x58)[0]
    incompat = struct.unpack_from("<I", sb, 0x60)[0]
    desc_size = struct.unpack_from("<H", sb, 0xFE)[0] if (incompat & 0x80) else 32
    if desc_size < 32:
        desc_size = 32
    gdt_offset = block_size if block_size > 1024 else 2048

    def read_inode(ino: int) -> bytes:
        grp = (ino - 1) // inodes_per_group
        idx = (ino - 1) % inodes_per_group
        gd = reader.pread(gdt_offset + grp * desc_size, desc_size)
        itable_lo = struct.unpack_from("<I", gd, 8)[0]
        itable_hi = struct.unpack_from("<I", gd, 40)[0] if desc_size >= 64 else 0
        itable_blk = itable_lo | (itable_hi << 32)
        return reader.pread(itable_blk * block_size + idx * inode_size, inode_size)

    def read_extent_node(ext_buf: bytes, out_extents: List[Tuple[int, int, int]]) -> None:
        eh_magic, eh_entries, _eh_max, eh_depth, _eh_gen = struct.unpack_from("<HHHHH", ext_buf, 0)
        if eh_magic != EXT4_EXT_MAGIC:
            raise RuntimeError(f"Invalid ext4 extent header magic: 0x{eh_magic:04x}")
        if eh_depth == 0:
            for i in range(eh_entries):
                ee_block, ee_len, ee_start_hi, ee_start_lo = struct.unpack_from("<IHHI", ext_buf, 12 + i * 12)
                if ee_len > 0x8000:
                    # Uninitialized extent
                    continue
                phys_blk = ee_start_lo | (ee_start_hi << 32)
                out_extents.append((ee_block, ee_len, phys_blk))
        else:
            for i in range(eh_entries):
                _ei_block, ei_leaf_lo, ei_leaf_hi, _ei_unused = struct.unpack_from("<IIHH", ext_buf, 12 + i * 12)
                leaf_blk = ei_leaf_lo | (ei_leaf_hi << 32)
                child_buf = reader.pread(leaf_blk * block_size, block_size)
                read_extent_node(child_buf, out_extents)

    def read_inode_data(ino: int) -> bytes:
        ibuf = read_inode(ino)
        size_lo = struct.unpack_from("<I", ibuf, 4)[0]
        size_hi = struct.unpack_from("<I", ibuf, 0x6C)[0]
        file_size = size_lo | (size_hi << 32)
        flags = struct.unpack_from("<I", ibuf, 0x20)[0]
        if not (flags & EXT4_EXTENTS_FL):
            raise RuntimeError(f"Inode {ino} does not use ext4 extents (flags=0x{flags:x})")
        ext_list: List[Tuple[int, int, int]] = []
        read_extent_node(ibuf[0x28 : 0x28 + 60], ext_list)
        ext_list.sort(key=lambda x: x[0])

        buf = bytearray(file_size)
        for ee_block, ee_len, phys_blk in ext_list:
            start_off = ee_block * block_size
            if start_off >= file_size:
                continue
            read_len = min(ee_len * block_size, file_size - start_off)
            buf[start_off : start_off + read_len] = reader.pread(phys_blk * block_size, read_len)
        return bytes(buf)

    def list_dir(ino: int) -> Dict[str, Tuple[int, int]]:
        raw = read_inode_data(ino)
        entries: Dict[str, Tuple[int, int]] = {}
        pos = 0
        n = len(raw)
        while pos + 8 <= n:
            d_ino, rec_len, name_len, file_type = struct.unpack_from("<IHBB", raw, pos)
            if rec_len < 8:
                break
            if d_ino != 0 and name_len > 0:
                name = raw[pos + 8 : pos + 8 + name_len].decode("utf-8", errors="replace")
                entries[name] = (d_ino, file_type)
            pos += rec_len
        return entries

    root_entries = list_dir(EXT4_ROOT_INO)
    if "firmware" not in root_entries:
        raise RuntimeError("`/firmware` directory not found in vendor ext4 root")
    fw_ino, _ = root_entries["firmware"]
    fw_entries = list_dir(fw_ino)

    out: Dict[str, bytes] = {}
    for fname in wanted_names:
        if fname not in fw_entries:
            raise RuntimeError(f"Missing `/firmware/{fname}` in vendor partition")
        f_ino, _ = fw_entries[fname]
        out[fname] = read_inode_data(f_ino)
    return out


def validate_zap_mbn(mbn_name: str, mbn: bytes) -> None:
    """Validate single-file Qualcomm MBN ELF against `mdt_loader.ko` & `gpu-micro-code@9b09a000`."""
    if mbn[:4] != b"\x7fELF":
        raise RuntimeError(f"{mbn_name}: invalid ELF magic")

    ei_class = mbn[4]
    if ei_class == 1:
        e_phoff = struct.unpack_from("<I", mbn, 28)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", mbn, 42)
    elif ei_class == 2:
        e_phoff = struct.unpack_from("<Q", mbn, 32)[0]
        e_phentsize, e_phnum = struct.unpack_from("<HH", mbn, 54)
    else:
        raise RuntimeError(f"{mbn_name}: unsupported ELF class {ei_class}")

    min_addr: Optional[int] = None
    max_addr: Optional[int] = None
    relocatable = False
    load_segs = 0

    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if ei_class == 1:
            p_type, p_offset, _p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, _p_align = struct.unpack_from(
                "<IIIIIIII", mbn, off
            )
        else:
            p_type, p_flags, p_offset, _p_vaddr, p_paddr, p_filesz, p_memsz, _p_align = struct.unpack_from(
                "<IIQQQQQQ", mbn, off
            )

        # Match mdt_phdr_valid() in mdt_loader.ko:
        # p_type == PT_LOAD (1), (p_flags & QCOM_MDT_TYPE_MASK [0x07000000]) != QCOM_MDT_TYPE_HASH [0x02000000], p_memsz != 0
        if p_type == 1 and ((p_flags & 0x07000000) != 0x02000000) and p_memsz > 0:
            load_segs += 1
            if p_flags & (1 << 27):  # QCOM_MDT_RELOCATABLE
                relocatable = True
            # Verify non-split MBN (`p_offset + p_filesz <= fw->size`) so mdt_loader never requests .bXX
            if p_offset + p_filesz > len(mbn):
                raise RuntimeError(
                    f"{mbn_name}: segment {i} [0x{p_offset:x}+0x{p_filesz:x}] exceeds single-file MBN size 0x{len(mbn):x}"
                )
            seg_end_aligned = (p_paddr + p_memsz + 0xFFF) & ~0xFFF  # PAGE_ALIGN in qcom_mdt_get_size()
            min_addr = p_paddr if min_addr is None else min(min_addr, p_paddr)
            max_addr = seg_end_aligned if max_addr is None else max(max_addr, seg_end_aligned)

    if load_segs == 0 or min_addr is None or max_addr is None:
        raise RuntimeError(f"{mbn_name}: no valid loadable PT_LOAD segments found")
    if not relocatable:
        raise RuntimeError(f"{mbn_name}: QCOM_MDT_RELOCATABLE (BIT(27)) is not set on loadable segment")

    mdt_size = max_addr - min_addr
    if mdt_size > ZAP_CARVEOUT_SIZE:
        raise RuntimeError(
            f"{mbn_name}: qcom_mdt_get_size()=0x{mdt_size:x} exceeds gpu-micro-code@0x{ZAP_CARVEOUT_PA:x} size 0x{ZAP_CARVEOUT_SIZE:x}"
        )

    # Verify OEM production signature (`ZTE Signing CA0`) and reject test-signed `.back` images
    if b"TEST ROOT" in mbn or b"SECTOOLS" in mbn:
        raise RuntimeError(f"{mbn_name}: REJECTED test-signed Qualcomm SECTOOLS image!")
    if b"ZTE Signing CA" not in mbn:
        raise RuntimeError(f"{mbn_name}: missing expected OEM 'ZTE Signing CA' X.509 certificate!")

    print(
        f"[OK] {mbn_name}: {e_phnum} phdrs ({load_segs} PT_LOAD, single-file non-split MBN, RELOCATABLE=1), "
        f"qcom_mdt_get_size()=0x{mdt_size:x} <= carveout 0x{ZAP_CARVEOUT_SIZE:x} (@0x{ZAP_CARVEOUT_PA:x}), "
        f"signed by 'ZTE Signing CA0'"
    )


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
    ap = argparse.ArgumentParser(
        description="Stage NX769J Adreno 750 (gen70900) GPU/GMU/Zap firmware from super (vendor_b/vendor_a)"
    )
    ap.add_argument(
        "--super-part",
        default="/dev/disk/by-partlabel/super",
        help="Path to factory `super` block device (default: /dev/disk/by-partlabel/super, opened read-only)",
    )
    ap.add_argument(
        "--fw-root",
        default="/lib/firmware",
        help="Root firmware directory (default: /lib/firmware)",
    )
    ap.add_argument(
        "--allow-unknown-sha256",
        action="store_true",
        help="Allow differing SHA256 if factory firmware comes from another ZTE/Nubia OTA build",
    )
    ap.add_argument(
        "--dry-run",
        action="store_true",
        help="Read and validate all GPU firmware files read-only without modifying --fw-root",
    )
    args = ap.parse_args()

    wanted = tuple(EXPECTED_FIRMWARE.keys())
    with open(args.super_part, "rb") as fp:
        part_name, slot_idx, extents = find_lp_partition_extents(fp, ("vendor_b", "vendor_a"))
        reader = LinearExtentsReader(fp, extents)
        print(
            f"[OK] Located LP partition `{part_name}` (slot {slot_idx}, {len(extents)} linear extent(s), "
            f"{reader.total_size:,} bytes) in {args.super_part}"
        )
        files = read_ext4_firmware_dir(reader, wanted)

    for fname in wanted:
        data = files[fname]
        exp_sz, exp_sha = EXPECTED_FIRMWARE[fname]
        actual_sha = hashlib.sha256(data).hexdigest()
        if not args.allow_unknown_sha256:
            if len(data) != exp_sz or actual_sha != exp_sha:
                raise RuntimeError(
                    f"{fname}: size/SHA256 mismatch (got {len(data)} B / {actual_sha}, "
                    f"expected {exp_sz} B / {exp_sha})"
                )
        elif len(data) == 0:
            raise RuntimeError(f"{fname}: extracted file is empty")
        print(f"[OK] {fname}: {len(data):,} bytes, sha256={actual_sha}")

    validate_zap_mbn("gen70900_zap.mbn", files["gen70900_zap.mbn"])

    if args.dry_run:
        print("[DRY-RUN] All 4 Adreno 750 (gen70900) firmware files validated read-only; zero disk writes performed.")
        return 0

    qcom_dir = os.path.join(args.fw_root, "qcom")
    soc_dir = os.path.join(qcom_dir, "sm8650")
    board_dir = os.path.join(soc_dir, "nubia", "nx769j")
    os.makedirs(board_dir, mode=0o755, exist_ok=True)
    os.makedirs(soc_dir, mode=0o755, exist_ok=True)
    os.makedirs(qcom_dir, mode=0o755, exist_ok=True)

    written = 0
    linked = 0
    for fname in wanted:
        if write_atomic(os.path.join(board_dir, fname), files[fname]):
            written += 1
        # `msm.ko` requests `qcom/gen70900_sqe.fw`, `qcom/gmu_gen70900.bin`, and fallback `qcom/gen70900_zap.mbn`
        if ensure_rel_symlink(qcom_dir, fname, os.path.join("sm8650", "nubia", "nx769j", fname)):
            linked += 1
        # SoC-level symlink (`qcom/sm8650/<fname> -> nubia/nx769j/<fname>`)
        if ensure_rel_symlink(soc_dir, fname, os.path.join("nubia", "nx769j", fname)):
            linked += 1

    # Legacy alias symlink (`qcom/a750_sqe.fw -> sm8650/nubia/nx769j/gen70900_sqe.fw`) for tools/docs expecting a750_sqe.fw
    if ensure_rel_symlink(qcom_dir, "a750_sqe.fw", os.path.join("sm8650", "nubia", "nx769j", "gen70900_sqe.fw")):
        linked += 1

    print(
        f"[DONE] Staged {len(wanted)} Adreno 750 firmware files into {board_dir} "
        f"({written} written, {linked} symlinks created/updated in {qcom_dir} & {soc_dir})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
