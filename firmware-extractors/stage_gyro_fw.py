#!/usr/bin/env python3
"""
NX769J ADSP Sensor PD / HexagonRPCD Firmware & Registry Stager.

Reads factory partitions strictly READ-ONLY (no mount, no temp files):
  - /dev/disk/by-partlabel/super   (LP logical partition vendor_a / vendor_b)
  - /dev/disk/by-partlabel/dsp_b   (fallback dsp_a) -> /adsp/*
  - /dev/disk/by-partlabel/persist                  -> /sensors/registry/registry/*

Populates HexagonFS root at /usr/share/qcom/sm8650/nubia/nx769j (and symlinks
/var/lib/droid-juicer/sensors -> /usr/share/qcom/sm8650/nubia/nx769j):
  - acdb/                   (required empty directory for ADSP_AVS_CFG_PATH)
  - dsp/adsp/               (ADSP shared objects incl. sns_tppe.so)
  - sensors/config/         (all lanai_*.json / sns_*.json + json.lst)
  - sensors/sns_reg.conf    (sns_reg_config with unsupported property= lines stripped)
  - sensors/registry/       (factory calibrated binary protobuf files incl. LSM6DSO bias/orient)
  - socinfo/                (soc_id, revision, hw_platform=MTP, platform_subtype*, platform_version)
"""

import argparse
import hashlib
import json
import os
import struct
import sys
from typing import Dict, List, Optional, Tuple

LP_GEOMETRY_MAGIC = 0x616C4467
LP_HEADER_MAGIC = 0x414C5030
EXT4_SUPER_MAGIC = 0xEF53
EXT4_EXTENT_MAGIC = 0xF30A
EXT4_EXTENTS_FL = 0x00080000

DEFAULT_OUT_DIR = "/usr/share/qcom/sm8650/nubia/nx769j"
DROID_JUICER_LINK = "/var/lib/droid-juicer/sensors"


class LinearExtentReader:
    """Maps logical partition offsets onto physical block-device extents."""

    def __init__(self, path: str, extents: Optional[List[Tuple[int, int, int]]] = None):
        self.f = open(path, "rb")
        # Each tuple: (logical_start, byte_len, physical_start)
        self.extents = extents

    def close(self) -> None:
        self.f.close()

    def pread(self, offset: int, size: int) -> bytes:
        if self.extents is None:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        rem = size
        cur = offset
        while rem > 0:
            matched = False
            for log_off, length, phys_off in self.extents:
                if log_off <= cur < log_off + length:
                    chunk = min(rem, (log_off + length) - cur)
                    self.f.seek(phys_off + (cur - log_off))
                    data = self.f.read(chunk)
                    if len(data) != chunk:
                        raise IOError(f"Short read at phys 0x{phys_off + (cur - log_off):x}")
                    out.extend(data)
                    cur += chunk
                    rem -= chunk
                    matched = True
                    break
            if not matched:
                raise IOError(f"Logical offset 0x{cur:x} outside LP extents")
        return bytes(out)


def open_lp_partition(super_path: str, candidates: Tuple[str, ...]) -> Tuple[str, LinearExtentReader]:
    with open(super_path, "rb") as f:
        f.seek(0x1000)
        geom = f.read(4096)
        g_magic = struct.unpack_from("<I", geom, 0)[0]
        if g_magic != LP_GEOMETRY_MAGIC:
            raise RuntimeError(f"Invalid LP geometry magic 0x{g_magic:08x} on {super_path}")

        # Primary LP metadata slot starts at 0x3000 on Android super partitions
        f.seek(0x3000)
        hdr = f.read(256)
        h_magic, maj, minv, h_sz = struct.unpack_from("<IHHI", hdr, 0)
        if h_magic != LP_HEADER_MAGIC:
            raise RuntimeError(f"Invalid LP header magic 0x{h_magic:08x} on {super_path}")
        p_off, p_cnt, p_esz = struct.unpack_from("<III", hdr, 0x50)
        e_off, e_cnt, e_esz = struct.unpack_from("<III", hdr, 0x5C)

        f.seek(0x3000 + h_sz)
        tables_sz = max(p_off + p_cnt * p_esz, e_off + e_cnt * e_esz)
        meta = f.read(tables_sz)

    raw_exts = [
        struct.unpack_from("<QIQI", meta, e_off + i * e_esz) for i in range(e_cnt)
    ]
    parts: Dict[str, List[Tuple[int, int, int]]] = {}
    for i in range(p_cnt):
        base = p_off + i * p_esz
        name = meta[base : base + 36].split(b"\x00", 1)[0].decode("ascii", "ignore")
        _, first_ext, num_ext, _ = struct.unpack_from("<IIII", meta, base + 36)
        if num_ext == 0:
            continue
        cur_log = 0
        ext_list: List[Tuple[int, int, int]] = []
        for j in range(num_ext):
            num_sec, target_type, target_data, _ = raw_exts[first_ext + j]
            if target_type != 0:  # LP_TARGET_TYPE_LINEAR == 0
                raise RuntimeError(f"Unsupported LP target_type={target_type} in {name}")
            byte_len = num_sec * 512
            ext_list.append((cur_log, byte_len, target_data * 512))
            cur_log += byte_len
        parts[name] = ext_list

    for cand in candidates:
        if cand in parts:
            reader = LinearExtentReader(super_path, parts[cand])
            sb_magic = struct.unpack_from("<H", reader.pread(0x438, 2), 0)[0]
            if sb_magic == EXT4_SUPER_MAGIC:
                return cand, reader
            reader.close()
    raise RuntimeError(f"None of {candidates} found as ext4 in {super_path} (found: {sorted(parts)})")


class Ext4ReadOnly:
    """Minimal read-only ext4 directory & file reader supporting 32/64-bit groups & extents."""

    def __init__(self, reader: LinearExtentReader):
        self.r = reader
        sb = self.r.pread(1024, 1024)
        magic = struct.unpack_from("<H", sb, 0x38)[0]
        if magic != EXT4_SUPER_MAGIC:
            raise RuntimeError(f"Invalid ext4 superblock magic 0x{magic:04x}")
        log_block_size = struct.unpack_from("<I", sb, 0x18)[0]
        self.block_size = 1024 << log_block_size
        self.inodes_per_group = struct.unpack_from("<I", sb, 0x28)[0]
        self.inode_size = struct.unpack_from("<H", sb, 0x58)[0]
        incompat = struct.unpack_from("<I", sb, 0x60)[0]
        self.is_64bit = bool(incompat & 0x80)
        desc_size = struct.unpack_from("<H", sb, 0xFE)[0]
        self.desc_size = desc_size if (self.is_64bit and desc_size >= 64) else 32
        self.gdt_offset = self.block_size if self.block_size > 1024 else 2048

    def _read_inode(self, ino: int) -> bytes:
        grp = (ino - 1) // self.inodes_per_group
        idx = (ino - 1) % self.inodes_per_group
        gd = self.r.pread(self.gdt_offset + grp * self.desc_size, self.desc_size)
        it_lo = struct.unpack_from("<I", gd, 0x08)[0]
        it_hi = struct.unpack_from("<I", gd, 0x28)[0] if self.desc_size >= 64 else 0
        it_block = it_lo | (it_hi << 32)
        return self.r.pread(it_block * self.block_size + idx * self.inode_size, self.inode_size)

    def _collect_extent_leaves(self, node_buf: bytes, out: List[Tuple[int, int, int]]) -> None:
        eh_magic, eh_entries, _, eh_depth, _ = struct.unpack_from("<HHHHI", node_buf, 0)
        if eh_magic != EXT4_EXTENT_MAGIC:
            raise RuntimeError(f"Bad ext4 extent magic 0x{eh_magic:04x}")
        if eh_depth == 0:
            for i in range(eh_entries):
                ee_block, ee_len, ee_start_hi, ee_start_lo = struct.unpack_from("<IHHI", node_buf, 12 + i * 12)
                uninit = ee_len > 32768
                real_len = ee_len - 32768 if uninit else ee_len
                phys = ee_start_lo | (ee_start_hi << 32)
                out.append((ee_block, real_len, 0 if uninit else phys))
        else:
            for i in range(eh_entries):
                _, ei_leaf_lo, ei_leaf_hi, _ = struct.unpack_from("<IIHH", node_buf, 12 + i * 12)
                child_blk = ei_leaf_lo | (ei_leaf_hi << 32)
                child_buf = self.r.pread(child_blk * self.block_size, self.block_size)
                self._collect_extent_leaves(child_buf, out)

    def read_inode_data(self, ino: int) -> Tuple[int, bytes]:
        inode = self._read_inode(ino)
        mode = struct.unpack_from("<H", inode, 0x00)[0]
        size_lo = struct.unpack_from("<I", inode, 0x04)[0]
        flags = struct.unpack_from("<I", inode, 0x20)[0]
        size_hi = struct.unpack_from("<I", inode, 0x6C)[0] if (mode & 0xF000) == 0x8000 else 0
        size = size_lo | (size_hi << 32)

        # Fast symlink stored inline in i_block[60]
        if (mode & 0xF000) == 0xA000 and size < 60 and not (flags & EXT4_EXTENTS_FL):
            return mode, inode[0x28 : 0x28 + size]

        if not (flags & EXT4_EXTENTS_FL):
            # Direct/indirect block fallback for small legacy files
            out = bytearray()
            blocks = struct.unpack_from("<15I", inode, 0x28)
            for blk in blocks[:12]:
                if len(out) >= size:
                    break
                if blk == 0:
                    out.extend(b"\x00" * self.block_size)
                else:
                    out.extend(self.r.pread(blk * self.block_size, self.block_size))
            return mode, bytes(out[:size])

        leaves: List[Tuple[int, int, int]] = []
        self._collect_extent_leaves(inode[0x28 : 0x28 + 60], leaves)
        leaves.sort(key=lambda x: x[0])

        out = bytearray(size)
        bs = self.block_size
        for l_blk, count, p_blk in leaves:
            start = l_blk * bs
            if start >= size or p_blk == 0:
                continue
            to_read = min(count * bs, size - start)
            out[start : start + to_read] = self.r.pread(p_blk * bs, to_read)
        return mode, bytes(out)

    def listdir_ino(self, dir_ino: int) -> List[Tuple[str, int, int]]:
        mode, raw = self.read_inode_data(dir_ino)
        if (mode & 0xF000) != 0x4000:
            raise RuntimeError(f"Inode {dir_ino} is not a directory (mode=0o{mode:o})")
        entries: List[Tuple[str, int, int]] = []
        bs = self.block_size
        for blk_off in range(0, len(raw), bs):
            blk = raw[blk_off : blk_off + bs]
            pos = 0
            while pos + 8 <= len(blk):
                ino, rec_len, name_len, ftype = struct.unpack_from("<IHBB", blk, pos)
                if rec_len < 8:
                    break
                if ino != 0 and name_len > 0:
                    name = blk[pos + 8 : pos + 8 + name_len].decode("utf-8", "surrogateescape")
                    if name not in (".", ".."):
                        entries.append((name, ino, ftype))
                pos += rec_len
        return entries

    def resolve_path(self, path: str) -> int:
        ino = 2
        for part in [p for p in path.strip("/").split("/") if p]:
            found = None
            for name, child_ino, _ in self.listdir_ino(ino):
                if name == part:
                    found = child_ino
                    break
            if found is None:
                raise FileNotFoundError(f"Path '{path}' missing component '{part}'")
            ino = found
        return ino

    def read_file(self, path: str) -> bytes:
        ino = self.resolve_path(path)
        mode, data = self.read_inode_data(ino)
        if (mode & 0xF000) != 0x8000:
            raise RuntimeError(f"'{path}' is not a regular file (mode=0o{mode:o})")
        return data

    def read_flat_dir(self, path: str) -> Dict[str, bytes]:
        dir_ino = self.resolve_path(path)
        out: Dict[str, bytes] = {}
        for name, ino, _ in sorted(self.listdir_ino(dir_ino)):
            mode, data = self.read_inode_data(ino)
            if (mode & 0xF000) == 0x8000:
                out[name] = data
        return out


def atomic_write(path: str, data: bytes, mode: int = 0o644) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = f"{path}.tmp.{os.getpid()}"
    with open(tmp, "wb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.chmod(tmp, mode)
    os.replace(tmp, path)


def read_sysfs_or_default(name: str, default: str) -> str:
    p = f"/sys/devices/soc0/{name}"
    if os.path.exists(p):
        try:
            val = open(p, "r", encoding="utf-8").read().strip()
            if val:
                return val + "\n"
        except OSError:
            pass
    return default


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage NX769J ADSP Sensor PD firmware & registry")
    ap.add_argument("--out-dir", default=DEFAULT_OUT_DIR, help="Target HexagonFS root directory")
    ap.add_argument("--super-dev", default="/dev/disk/by-partlabel/super")
    ap.add_argument("--dsp-dev", default="/dev/disk/by-partlabel/dsp_b")
    ap.add_argument("--persist-dev", default="/dev/disk/by-partlabel/persist")
    ap.add_argument("--no-droid-juicer-link", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="Validate & summarize read-only without writing")
    args = ap.parse_args()

    # 1. Read vendor_a (fallback vendor_b) sensor configs & sns_reg_config from super
    part_name, vendor_lp = open_lp_partition(args.super_dev, ("vendor_a", "vendor_b"))
    try:
        vendor_fs = Ext4ReadOnly(vendor_lp)
        raw_reg_cfg = vendor_fs.read_file("etc/sensors/sns_reg_config").decode("utf-8")
        sensor_configs = vendor_fs.read_flat_dir("etc/sensors/config")
    finally:
        vendor_lp.close()

    # Strip Android property= lines (hexagonrpcd does not implement libsns_registry_skel property RPC)
    cleaned_lines = [
        ln for ln in raw_reg_cfg.splitlines() if not ln.strip().startswith("property=")
    ]
    cleaned_reg_cfg = ("\n".join(cleaned_lines) + "\n").encode("utf-8")

    # Validate json.lst and every .json config
    if "json.lst" not in sensor_configs or "lanai_lsm6dso_0.json" not in sensor_configs:
        raise RuntimeError("Missing json.lst or lanai_lsm6dso_0.json in vendor sensors/config")
    json_count = 0
    for name, payload in sensor_configs.items():
        if name.endswith(".json"):
            json.loads(payload.decode("utf-8"))
            json_count += 1

    # 2. Read dsp_b (fallback dsp_a) /adsp/* shared libraries (incl. sns_tppe.so)
    dsp_path = args.dsp_dev if os.path.exists(args.dsp_dev) else "/dev/disk/by-partlabel/dsp_a"
    dsp_reader = LinearExtentReader(dsp_path)
    try:
        dsp_fs = Ext4ReadOnly(dsp_reader)
        dsp_adsp_files = dsp_fs.read_flat_dir("adsp")
    finally:
        dsp_reader.close()

    if "sns_tppe.so" not in dsp_adsp_files:
        raise RuntimeError(f"sns_tppe.so missing from {dsp_path}:/adsp (found {len(dsp_adsp_files)} files)")

    # 3. Read factory calibrated binary protobuf registry from persist:/sensors/registry/registry
    persist_reader = LinearExtentReader(args.persist_dev)
    try:
        persist_fs = Ext4ReadOnly(persist_reader)
        registry_files = persist_fs.read_flat_dir("sensors/registry/registry")
    finally:
        persist_reader.close()

    required_cal = (
        "lanai_lsm6dso_0.json.lsm6dso_0_platform.accel.fac_cal.bias",
        "lanai_lsm6dso_0.json.lsm6dso_0_platform.gyro.fac_cal.bias",
        "lanai_lsm6dso_0.json.lsm6dso_0_platform.orient",
    )
    for req in required_cal:
        if req not in registry_files or len(registry_files[req]) == 0:
            raise RuntimeError(f"Missing/empty factory IMU calibration file '{req}' in persist")

    # 4. Prepare synthetic socinfo files (mainline Linux omits hw_platform/platform_subtype*)
    socinfo_files: Dict[str, bytes] = {
        "soc_id": read_sysfs_or_default("soc_id", "557\n").encode("utf-8"),
        "revision": read_sysfs_or_default("revision", "2.0\n").encode("utf-8"),
        "hw_platform": read_sysfs_or_default("hw_platform", "MTP\n").encode("utf-8"),
        "platform_subtype": read_sysfs_or_default("platform_subtype", "0\n").encode("utf-8"),
        "platform_subtype_id": read_sysfs_or_default("platform_subtype_id", "0\n").encode("utf-8"),
        "platform_version": read_sysfs_or_default("platform_version", "65536\n").encode("utf-8"),
    }

    total_bytes = (
        len(cleaned_reg_cfg)
        + sum(len(v) for v in sensor_configs.values())
        + sum(len(v) for v in dsp_adsp_files.values())
        + sum(len(v) for v in registry_files.values())
        + sum(len(v) for v in socinfo_files.values())
    )
    accel_cal_sha = hashlib.sha256(registry_files[required_cal[0]]).hexdigest()[:16]
    gyro_cal_sha = hashlib.sha256(registry_files[required_cal[1]]).hexdigest()[:16]

    print(
        f"[stage_gyro_fw] Validated sources (vendor={part_name}, dsp={dsp_path}, persist={args.persist_dev}):\n"
        f"  sensors/config/   : {len(sensor_configs)} files ({json_count} validated JSONs + json.lst)\n"
        f"  sensors/sns_reg.conf: {len(cleaned_reg_cfg)} bytes (property= lines stripped)\n"
        f"  sensors/registry/ : {len(registry_files)} binary protobuf files "
        f"(accel_bias_sha={accel_cal_sha}, gyro_bias_sha={gyro_cal_sha})\n"
        f"  dsp/adsp/         : {len(dsp_adsp_files)} files (sns_tppe.so={len(dsp_adsp_files['sns_tppe.so'])} B)\n"
        f"  socinfo/          : { {k: v.decode().strip() for k, v in socinfo_files.items()} }\n"
        f"  acdb/             : directory present (required by hexagonrpcd ADSP_AVS_CFG_PATH)\n"
        f"  total payload     : {total_bytes:,} bytes"
    )

    if args.dry_run:
        print("[stage_gyro_fw] --dry-run complete; zero files written.")
        return 0

    out = args.out_dir
    for sub in ("acdb", "dsp/adsp", "sensors/config", "sensors/registry", "socinfo"):
        os.makedirs(os.path.join(out, sub), mode=0o755, exist_ok=True)
        os.chmod(os.path.join(out, sub), 0o755)
    os.chmod(out, 0o755)
    os.chmod(os.path.join(out, "dsp"), 0o755)
    os.chmod(os.path.join(out, "sensors"), 0o755)

    atomic_write(os.path.join(out, "sensors", "sns_reg.conf"), cleaned_reg_cfg, 0o644)
    for name, data in sensor_configs.items():
        atomic_write(os.path.join(out, "sensors", "config", name), data, 0o644)
    for name, data in dsp_adsp_files.items():
        atomic_write(os.path.join(out, "dsp", "adsp", name), data, 0o644)
    for name, data in registry_files.items():
        atomic_write(os.path.join(out, "sensors", "registry", name), data, 0o644)
    for name, data in socinfo_files.items():
        atomic_write(os.path.join(out, "socinfo", name), data, 0o644)

    if not args.no_droid_juicer_link:
        os.makedirs(os.path.dirname(DROID_JUICER_LINK), mode=0o755, exist_ok=True)
        if os.path.islink(DROID_JUICER_LINK) or os.path.isfile(DROID_JUICER_LINK):
            os.unlink(DROID_JUICER_LINK)
        if not os.path.exists(DROID_JUICER_LINK):
            os.symlink(out, DROID_JUICER_LINK)

    print(f"[stage_gyro_fw] Staged all files cleanly to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
