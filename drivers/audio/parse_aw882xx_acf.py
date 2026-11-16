#!/usr/bin/env python3
"""
Decode Nubia RedMagic 9 Pro (NX769J / A1901) factory aw882xx_acf.bin (v1.0.0.0)
from ssh_scan_5.txt and export:
  - aw88261_acf.bin (exact 2,016-byte factory binary)
  - aw88261_acf_parsed.json (structured register & monitor tables for 0x35 & 0x34)
"""
import base64
import json
import os
import struct

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
SCAN_PATH = os.path.join(BASE_DIR, "ssh_scan_5.txt")
BIN_OUT = os.path.join(BASE_DIR, "aw88261_acf.bin")
JSON_OUT = os.path.join(BASE_DIR, "aw88261_acf_parsed.json")

PROFILE_NAMES = {
    0: "Music",
    1: "Voice",
    2: "Voip",
    3: "Ringtone",
    4: "Ringtone_hs",
    5: "Lowpower",
    6: "Bypass",
    7: "Mmi",
    8: "Fm",
    9: "Notification",
    10: "Receiver",
}

DATA_TYPE_NAMES = {
    0: "REG",
    1: "DSP_CFG",
    2: "DSP_FW",
    4: "REG_WITH_HDR",
    10: "MONITOR",
}


def cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")


def extract_b64_from_scan(scan_path: str) -> bytes:
    with open(scan_path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()
    b64_lines = []
    in_block = False
    for line in lines:
        s = line.strip()
        if s == "=== AW882XX_ACF.BIN BASE64 ===":
            in_block = True
            continue
        if in_block:
            if s.startswith("==="):
                break
            if s:
                b64_lines.append(s)
    raw = base64.b64decode("".join(b64_lines))
    return raw


def parse_acf_v1(raw: bytes) -> dict:
    # Awinic ACF header v1.0.0.0 (80 bytes = 0x50):
    # +0x00: u32 check_sum
    # +0x04..0x1c: project[24] ("A1901")
    # +0x1c..0x2c: custom[16] ("Awinic")
    # +0x2c: u32 hdr_version (0x02000000 -> v0.0.0.2 / v1.0.0.0)
    # +0x30: u32 author_id (0x000007f3 = 2035)
    # +0x34: u32 ddt_size (0x00000180 = 384 = 6 * 64)
    # +0x38: u32 dde_num (6)
    # +0x3c: u32 ddt_offset (0x00000050 = 80)
    check_sum = struct.unpack_from("<I", raw, 0x00)[0]
    project = cstr(raw[0x04:0x1C])
    custom = cstr(raw[0x1C:0x2C])
    hdr_version, author_id, ddt_size, dde_count, ddt_offset = struct.unpack_from(
        "<IIIII", raw, 0x2C
    )

    entries = []
    devices = {}

    for i in range(dde_count):
        base = ddt_offset + i * 64
        # aw_dde_v_1_0_0_0 (64 bytes):
        # +0x00: u32 type (2 = dev_type_id)
        # +0x04..0x14: customer/model[16] ("aw88261" / "aw882xx")
        # +0x14: u16 dev_index (0 -> 0x35 Bottom Ch0, 1 -> 0x34 Top Ch1)
        # +0x16: u16 dev_bus (0xffff)
        # +0x18: u16 dev_addr (0xffff)
        # +0x1a: u16 dev_profile (0 = Music, 10 = Receiver)
        # +0x1c: u32 data_type (4 = REG_WITH_HDR, 10 = MONITOR)
        # +0x20: u32 data_size
        # +0x24: u32 data_offset
        # +0x28: u32 data_crc
        dev_type = struct.unpack_from("<I", raw, base + 0x00)[0]
        chip_name = cstr(raw[base + 0x04 : base + 0x14])
        dev_index, dev_bus, dev_addr, dev_profile = struct.unpack_from("<HHHH", raw, base + 0x14)
        data_type, data_size, data_offset, data_crc = struct.unpack_from(
            "<IIII", raw, base + 0x1C
        )

        payload = raw[data_offset : data_offset + data_size]
        prof_name = PROFILE_NAMES.get(dev_profile, f"Profile_{dev_profile}")
        dtype_name = DATA_TYPE_NAMES.get(data_type, f"Type_{data_type}")

        entry_info = {
            "dde_index": i,
            "type": dev_type,
            "model": chip_name,
            "dev_index": dev_index,
            "dev_bus": f"0x{dev_bus:04x}",
            "i2c_addr_field": f"0x{dev_addr:04x}",
            "profile_id": dev_profile,
            "profile_name": prof_name,
            "data_type_id": data_type,
            "data_type": dtype_name,
            "data_offset": f"0x{data_offset:04x}",
            "data_size": data_size,
            "data_crc": f"0x{data_crc:08x}",
        }

        # REG_WITH_HDR (data_type == 4): 64-byte aw_bin_hdr + reg_count*(u16 reg, u16 val)
        if data_type == 4 and data_size >= 64:
            inner_chip = cstr(payload[0x18:0x20])
            reg_bytes = payload[64:]
            reg_count = struct.unpack_from("<I", payload, 60)[0]
            regs = []
            if reg_count * 4 == len(reg_bytes):
                for r_idx in range(reg_count):
                    r_addr, r_val = struct.unpack_from("<HH", reg_bytes, r_idx * 4)
                    regs.append(
                        {
                            "reg": f"0x{r_addr:02x}",
                            "val": f"0x{r_val:04x}",
                            "reg_int": r_addr,
                            "val_int": r_val,
                        }
                    )
                entry_info["inner_chip"] = inner_chip
                entry_info["reg_count"] = reg_count
                entry_info["registers"] = regs

                i2c_hw = "0x35" if dev_index == 0 else "0x34"
                ch_role = (
                    "Bottom Speaker (Ch0 / Left)"
                    if dev_index == 0
                    else "Top Speaker / Earpiece (Ch1 / Right)"
                )
                dev_obj = devices.setdefault(
                    str(dev_index),
                    {
                        "dev_index": dev_index,
                        "i2c_addr": i2c_hw,
                        "role": ch_role,
                        "profiles": {},
                    },
                )
                dev_obj["profiles"][prof_name] = [
                    {"reg": r["reg"], "val": r["val"]} for r in regs
                ]
        else:
            entry_info["raw_hex_preview"] = payload[:64].hex()

        entries.append(entry_info)

    return {
        "file_size": len(raw),
        "check_sum": f"0x{check_sum:08x}",
        "project": project,
        "custom": custom,
        "hdr_version": f"0x{hdr_version:08x}",
        "author_id": author_id,
        "ddt_offset": f"0x{ddt_offset:04x}",
        "ddt_size": ddt_size,
        "dde_count": dde_count,
        "devices": devices,
        "entries": entries,
    }


def main():
    raw = extract_b64_from_scan(SCAN_PATH)
    with open(BIN_OUT, "wb") as f:
        f.write(raw)
    parsed = parse_acf_v1(raw)
    with open(JSON_OUT, "w", encoding="utf-8") as f:
        json.dump(parsed, f, indent=2)
    print(f"Wrote {BIN_OUT} ({len(raw)} bytes) and {JSON_OUT}")
    for dev_idx, dev in parsed["devices"].items():
        print(f"\nDevice {dev_idx} ({dev['i2c_addr']} - {dev['role']}):")
        for pname, regs in dev["profiles"].items():
            head_regs = ", ".join(f"{r['reg']}={r['val']}" for r in regs[:16])
            print(f"  Profile {pname} ({len(regs)} regs): {head_regs}")
            # Also print differences or key regs
            by_r = {r["reg"]: r["val"] for r in regs}
            print(
                f"    Key regs: SYSINTM(0x03)={by_r.get('0x03')} SYSCTRL(0x04)={by_r.get('0x04')} "
                f"SYSCTRL2(0x05)={by_r.get('0x05')} I2SCTRL(0x06)={by_r.get('0x06')} "
                f"I2SCFG1(0x07)={by_r.get('0x07')} HAGCCFG4(0x0c)={by_r.get('0x0c')} "
                f"I2SCFG2(0x08)={by_r.get('0x08')} PLLCTRL1(0x54)={by_r.get('0x54')}"
            )


if __name__ == "__main__":
    main()
