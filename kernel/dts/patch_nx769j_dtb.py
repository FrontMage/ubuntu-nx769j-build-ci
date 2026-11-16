#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""
patch_nx769j_dtb.py — Hardware-Validated NX769J (SM8650) Linux DTB Patcher (v23)

Applies all verified hardware enablement & pinmux fixes onto the base
`nx769j.dtb` compiled from `sm8650-nubia-nx769j-base.dts` (7.1.1-nx769j+):
  - Full 13.8 GiB memory map & non-overlapping rmtfs/ramoops carveouts
  - smmu-splash-keepalive for efifb (0xe3940000, 1116x2480) + disabled dispcc/videocc/camcc
  - USB2 High-Speed ConfigFS peripheral gadget (usb@a600000)
  - Goodix GT9916R 10-slot touchscreen (spi@a90000 + gpi dma + ldo14b + gpio28)
  - Qualcomm WCN7850 Wi-Fi 7 (pcie@1c00000 + wcn7850-pmu) & Bluetooth 5.4 (serial0 @ 898000)
  - SM8650 ADSP PAS + PMIC GLINK (qcom_battmgr) + FastRPC (/dev/fastrpc-adsp)
  - 25 kHz PWM Turbo Cooling Fan (gpio173 boost + PM8550 LPG ch3 + gpio174 tach + cpuss0 trips)
  - Headless Adreno 750 GPU (gpu@3d00000 + ZTE-signed gen70900_zap.mbn)
  - Audio: Dual AW88261 SmartPA I2C (i2c@98c000), Primary MI2S (gpio127 DIN / gpio128 DOUT
    output-low), SoundWire RX/TX WCD9395 3.5mm jack & analog mics
  - PM8550 Volume Up gpio-keys-polled (GPIO6) + PON pwrkey/resin
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile


def find_dtc(explicit: str | None = None) -> str:
    if explicit:
        return explicit
    found = shutil.which("dtc")
    if found:
        return found
    for cand in ("/opt/homebrew/bin/dtc", "/usr/bin/dtc", "/usr/local/bin/dtc"):
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    raise FileNotFoundError("Could not find `dtc` device-tree compiler in PATH")


def format_dt_bd_addr(mac_str: str) -> str:
    """Convert AA:BB:CC:DD:EE:FF to little-endian DT byte list `ff ee dd cc bb aa`."""
    cleaned = mac_str.strip()
    if not re.fullmatch(r"[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}", cleaned):
        raise ValueError(f"Invalid Bluetooth MAC address: {mac_str!r} (expected AA:BB:CC:DD:EE:FF)")
    octets = [int(x, 16) for x in cleaned.split(":")]
    if octets[0] & 0x01:
        raise ValueError(f"Bluetooth MAC {mac_str!r} has multicast bit set in first octet")
    return " ".join(f"{b:02x}" for b in reversed(octets))


def patch_linux_dts(linux_dts: str, bt_mac: str = "02:00:00:76:9a:01") -> str:
    bt_mac_dt = format_dt_bd_addr(bt_mac)
    LINUX_BOOTARGS = (
        "earlycon=efifb console=tty0 vt.global_cursor_default=0 loglevel=4 "
        "pd_ignore_unused clk_ignore_unused regulator_ignore_unused "
        "plymouth.enable=0 systemd.unit=multi-user.target "
        "arm-smmu.disable_bypass=0 msm.separate_gpu_kms=1 efi=novamap log_buf_len=2M panic=5 rw"
    )

    def replace_exact_once(src: str, old: str, new: str) -> str:
        cnt = src.count(old)
        assert cnt == 1, f"Expected 1 occurrence, got {cnt} for:\n{old[:160]}"
        return src.replace(old, new, 1)

    # 2a) Populate /chosen bootargs + keep qcom-smmu boot-SMR bypass AND mmss_noc/gem_noc/mmcx boot votes active!
    #     Also declare fixed 1.8V VDDIO regulator (GPIO28) for Goodix GT9916R touchscreen.
    linux_dts = replace_exact_once(
        linux_dts,
        "\tchosen {\n\t};",
        f'\tchosen {{\n\t\tbootargs = "{LINUX_BOOTARGS}";\n\t}};\n\n'
        '\tsmmu-splash-keepalive {\n'
        '\t\tcompatible = "nx769j,smmu-splash-keepalive";\n'
        '\t\tinterconnects = <0xcb 0x03 0x07 0x08 0x01 0x07 0x07 0x03 0x03 0x4e 0x0d 0x03>;\n'
        '\t\tinterconnect-names = "mdp0-mem", "cpu-cfg";\n'
        '\t\tpower-domains = <0x4f 0x06>;\n'
        '\t\tclocks = <0x4c 0x0f>;\n'
        '\t\tstatus = "okay";\n'
        '\t};\n\n'
        '\tts-vddio-regulator {\n'
        '\t\tcompatible = "regulator-fixed";\n'
        '\t\tregulator-name = "ts_vddio_1v8";\n'
        '\t\tregulator-min-microvolt = <0x1b7740>;\n'
        '\t\tregulator-max-microvolt = <0x1b7740>;\n'
        '\t\tgpio = <0x99 0x1c 0x00>;\n'
        '\t\tenable-active-high;\n'
        '\t\tregulator-boot-on;\n'
        '\t\tphandle = <0x202>;\n'
        '\t};\n\n'
        '\twcn7850-pmu {\n'
        '\t\tcompatible = "qcom,wcn7850-pmu";\n'
        '\t\tpinctrl-names = "default";\n'
        '\t\tpinctrl-0 = <0x216>;\n'
        '\t\twlan-enable-gpios = <0x99 0x10 0x00>;\n'
        '\t\tbt-enable-gpios = <0x99 0x11 0x00>;\n'
        '\t\tvdd-supply = <0x213>;\n'
        '\t\tvddio-supply = <0xec>;\n'
        '\t\tvddio1p2-supply = <0x212>;\n'
        '\t\tvddaon-supply = <0x211>;\n'
        '\t\tvdddig-supply = <0x104>;\n'
        '\t\tvddrfa1p2-supply = <0x103>;\n'
        '\t\tvddrfa1p8-supply = <0x101>;\n'
        '\t\tclocks = <0x02 0x06>;\n\n'
        '\t\tregulators {\n'
        '\t\t\tldo0 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_rfa_cmn";\n'
        '\t\t\t\tphandle = <0x220>;\n'
        '\t\t\t};\n'
        '\t\t\tldo1 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_aon_0p59";\n'
        '\t\t\t\tphandle = <0x217>;\n'
        '\t\t\t};\n'
        '\t\t\tldo2 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_wlcx_0p8";\n'
        '\t\t\t\tphandle = <0x221>;\n'
        '\t\t\t};\n'
        '\t\t\tldo3 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_wlmx_0p85";\n'
        '\t\t\t\tphandle = <0x222>;\n'
        '\t\t\t};\n'
        '\t\t\tldo4 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_btcmx_0p85";\n'
        '\t\t\t\tphandle = <0x223>;\n'
        '\t\t\t};\n'
        '\t\t\tldo5 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_rfa_0p8";\n'
        '\t\t\t\tphandle = <0x224>;\n'
        '\t\t\t};\n'
        '\t\t\tldo6 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_rfa_1p2";\n'
        '\t\t\t\tphandle = <0x225>;\n'
        '\t\t\t};\n'
        '\t\t\tldo7 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_rfa_1p8";\n'
        '\t\t\t\tphandle = <0x226>;\n'
        '\t\t\t};\n'
        '\t\t\tldo8 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_pcie_0p9";\n'
        '\t\t\t\tphandle = <0x227>;\n'
        '\t\t\t};\n'
        '\t\t\tldo9 {\n'
        '\t\t\t\tregulator-name = "vreg_pmu_pcie_1p8";\n'
        '\t\t\t\tphandle = <0x228>;\n'
        '\t\t\t};\n'
        '\t\t};\n'
        '\t};',
    )

    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tsleep-clk {\n\t\t\tcompatible = "fixed-clock";\n\t\t\t#clock-cells = <0x00>;\n\t\t\tphandle = <0x46>;\n\t\t};',
        '\t\tsleep-clk {\n\t\t\tcompatible = "fixed-clock";\n\t\t\t#clock-cells = <0x00>;\n\t\t\tclock-frequency = <0x7ffc>;\n\t\t\tphandle = <0x46>;\n\t\t};',
    )

    # 2b) Set full ~13.8 GiB verified System RAM banks (860 MiB low + 8.97 GiB + 4.00 GiB upper DDR)
    linux_dts = replace_exact_once(
        linux_dts,
        '\tmemory@a0000000 {\n\t\tdevice_type = "memory";\n\t\treg = <0x00 0xa0000000 0x00 0x00>;\n\t};',
        '\tmemory@a2000000 {\n\t\tdevice_type = "memory";\n'
        '\t\treg = <0x00 0xa2000000 0x00 0x35c00000 0x08 0xc0000000 0x02 0x3e000000 0x0b 0x00000000 0x01 0x00000000>;\n'
        '\t};',
    )

    # 2c) Fix rmtfs@d7c00000 overlap with ramoops@d7e00000 and disable rmtfs XPU assignment
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\trmtfs@d7c00000 {\n\t\t\tcompatible = "qcom,rmtfs-mem";\n\t\t\treg = <0x00 0xd7c00000 0x00 0x400000>;\n\t\t\tno-map;\n\t\t\tqcom,client-id = <0x01>;\n\t\t\tqcom,vmid = <0x0f>;\n\t\t};',
        '\t\trmtfs@d7c00000 {\n\t\t\tcompatible = "qcom,rmtfs-mem";\n\t\t\treg = <0x00 0xd7c00000 0x00 0x200000>;\n\t\t\tno-map;\n\t\t\tqcom,client-id = <0x01>;\n\t\t\tqcom,vmid = <0x0f>;\n\t\t\tstatus = "disabled";\n\t\t};',
    )

    # 2d) Drop qcom,ice phandle from ufshc@1d84000
    linux_dts = replace_exact_once(linux_dts, "\t\t\tqcom,ice = <0x98>;\n", "")

    # 2e) display-subsystem@ae00000 is already status = "disabled" in nx769j.dtb;
    #     MUST ALSO disable dispcc@af00000, videocc@aaf0000, and camcc@ade0000 so udev coldplug
    #     neither reconfigures disp_cc_pll0 nor attaches+suspends devices on the mmcx power rail!
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tclock-controller@af00000 {\n\t\t\tcompatible = "qcom,sm8650-dispcc";\n\t\t\treg = <0x00 0xaf00000 0x00 0x20000>;',
        '\t\tclock-controller@af00000 {\n\t\t\tstatus = "disabled";\n\t\t\tcompatible = "qcom,sm8650-dispcc";\n\t\t\treg = <0x00 0xaf00000 0x00 0x20000>;',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tclock-controller@aaf0000 {\n\t\t\tcompatible = "qcom,sm8650-videocc";\n\t\t\treg = <0x00 0xaaf0000 0x00 0x10000>;',
        '\t\tclock-controller@aaf0000 {\n\t\t\tstatus = "disabled";\n\t\t\tcompatible = "qcom,sm8650-videocc";\n\t\t\treg = <0x00 0xaaf0000 0x00 0x10000>;',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tclock-controller@ade0000 {\n\t\t\tcompatible = "qcom,sm8650-camcc";\n\t\t\treg = <0x00 0xade0000 0x00 0x20000>;',
        '\t\tclock-controller@ade0000 {\n\t\t\tstatus = "disabled";\n\t\t\tcompatible = "qcom,sm8650-camcc";\n\t\t\treg = <0x00 0xade0000 0x00 0x20000>;',
    )

    # 2f) Enable built-in USB2 High-Speed Peripheral Gadget on usb@a600000 (removes =m USB3 QMP PHY dependency!)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\torientation-switch;\n\t\t\tstatus = "okay";\n\t\t\tvdda-phy-supply = <0x96>;\n\t\t\tvdda-pll-supply = <0xc4>;\n\t\t\tphandle = <0x4a>;',
        '\t\t\torientation-switch;\n\t\t\tstatus = "disabled";\n\t\t\tvdda-phy-supply = <0x96>;\n\t\t\tvdda-pll-supply = <0xc4>;\n\t\t\tphandle = <0x4a>;',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tport@1 {\n\t\t\t\t\treg = <0x01>;\n\n\t\t\t\t\tendpoint {\n\t\t\t\t\t\tremote-endpoint = <0xc5>;\n\t\t\t\t\t\tphandle = <0xc8>;\n\t\t\t\t\t};\n\t\t\t\t};',
        '\t\t\t\tport@1 {\n\t\t\t\t\treg = <0x01>;\n\n\t\t\t\t\tendpoint {\n\t\t\t\t\t\tphandle = <0xc8>;\n\t\t\t\t\t};\n\t\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tphys = <0xc7 0x4a 0x00>;\n\t\t\tphy-names = "usb2-phy", "usb3-phy";',
        '\t\t\tphys = <0xc7>;\n\t\t\tphy-names = "usb2-phy";\n\t\t\tmaximum-speed = "high-speed";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tdr_mode = "peripheral";\n\n\t\t\tports {\n\t\t\t\t#address-cells = <0x01>;\n\t\t\t\t#size-cells = <0x00>;\n\n\t\t\t\tport@0 {\n\t\t\t\t\treg = <0x00>;\n\n\t\t\t\t\tendpoint {\n\t\t\t\t\t};\n\t\t\t\t};\n\n\t\t\t\tport@1 {\n\t\t\t\t\treg = <0x01>;\n\n\t\t\t\t\tendpoint {\n\t\t\t\t\t\tremote-endpoint = <0xc8>;\n\t\t\t\t\t\tphandle = <0xc5>;\n\t\t\t\t\t};\n\t\t\t\t};\n\t\t\t};',
        '\t\t\tdr_mode = "peripheral";',
    )

    # 2g) Enable PMK8350 power key + volume-down resin in Linux
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\t\tpwrkey {\n\t\t\t\t\t\tcompatible = "qcom,pmk8350-pwrkey";\n\t\t\t\t\t\tinterrupts = <0x00 0x13 0x07 0x03>;\n\t\t\t\t\t\tlinux,code = <0x74>;\n\t\t\t\t\t\tstatus = "disabled";\n\t\t\t\t\t};',
        '\t\t\t\t\tpwrkey {\n\t\t\t\t\t\tcompatible = "qcom,pmk8350-pwrkey";\n\t\t\t\t\t\tinterrupts = <0x00 0x13 0x07 0x03>;\n\t\t\t\t\t\tlinux,code = <0x1c>;\n\t\t\t\t\t\tstatus = "okay";\n\t\t\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\t\tresin {\n\t\t\t\t\t\tcompatible = "qcom,pmk8350-resin";\n\t\t\t\t\t\tinterrupts = <0x00 0x13 0x06 0x03>;\n\t\t\t\t\t\tstatus = "disabled";\n\t\t\t\t\t};',
        '\t\t\t\t\tresin {\n\t\t\t\t\t\tcompatible = "qcom,pmk8350-resin";\n\t\t\t\t\t\tinterrupts = <0x00 0x13 0x06 0x03>;\n\t\t\t\t\t\tlinux,code = <0x72>;\n\t\t\t\t\t\tstatus = "okay";\n\t\t\t\t\t};',
    )

    # 2h) Enable Goodix GT9916R touchscreen on QUP1 SE4 SPI (spi@a90000 + gpi dma@a00000 + ldo14b 3.2V AVDD + gpio161/162)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tdma-controller@a00000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-gpi-dma", "qcom,sm6350-gpi-dma";\n'
        '\t\t\treg = <0x00 0xa00000 0x00 0x60000>;\n'
        '\t\t\tinterrupts = <0x00 0x117 0x04 0x00 0x00 0x118 0x04 0x00 0x00 0x119 0x04 0x00 0x00 0x11a 0x04 0x00 0x00 0x11b 0x04 0x00 0x00 0x11c 0x04 0x00 0x00 0x125 0x04 0x00 0x00 0x126 0x04 0x00 0x00 0x127 0x04 0x00 0x00 0x128 0x04 0x00 0x00 0x129 0x04 0x00 0x00 0x12a 0x04 0x00>;\n'
        '\t\t\tdma-channels = <0x0c>;\n'
        '\t\t\tdma-channel-mask = <0x0c>;\n'
        '\t\t\t#dma-cells = <0x03>;\n'
        '\t\t\tiommus = <0x4b 0xb6 0x00>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t\tphandle = <0x74>;\n'
        '\t\t};',
        '\t\tdma-controller@a00000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-gpi-dma", "qcom,sm6350-gpi-dma";\n'
        '\t\t\treg = <0x00 0xa00000 0x00 0x60000>;\n'
        '\t\t\tinterrupts = <0x00 0x117 0x04 0x00 0x00 0x118 0x04 0x00 0x00 0x119 0x04 0x00 0x00 0x11a 0x04 0x00 0x00 0x11b 0x04 0x00 0x00 0x11c 0x04 0x00 0x00 0x125 0x04 0x00 0x00 0x126 0x04 0x00 0x00 0x127 0x04 0x00 0x00 0x128 0x04 0x00 0x00 0x129 0x04 0x00 0x00 0x12a 0x04 0x00>;\n'
        '\t\t\tdma-channels = <0x0c>;\n'
        '\t\t\tdma-channel-mask = <0x0c>;\n'
        '\t\t\t#dma-cells = <0x03>;\n'
        '\t\t\tiommus = <0x4b 0xb6 0x00>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\tstatus = "okay";\n'
        '\t\t\tphandle = <0x74>;\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tgeniqup@ac0000 {\n'
        '\t\t\tcompatible = "qcom,geni-se-qup";\n'
        '\t\t\treg = <0x00 0xac0000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x8c 0x4c 0x8d>;\n'
        '\t\t\tclock-names = "m-ahb", "s-ahb";\n'
        '\t\t\tinterconnects = <0x4d 0x01 0x07 0x4d 0x04 0x07>;\n'
        '\t\t\tinterconnect-names = "qup-core";\n'
        '\t\t\tiommus = <0x4b 0xa3 0x00>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x02>;\n'
        '\t\t\tranges;\n'
        '\t\t\tstatus = "disabled";',
        '\t\tgeniqup@ac0000 {\n'
        '\t\t\tcompatible = "qcom,geni-se-qup";\n'
        '\t\t\treg = <0x00 0xac0000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x8c 0x4c 0x8d>;\n'
        '\t\t\tclock-names = "m-ahb", "s-ahb";\n'
        '\t\t\tinterconnects = <0x4d 0x01 0x07 0x4d 0x04 0x07>;\n'
        '\t\t\tinterconnect-names = "qup-core";\n'
        '\t\t\tiommus = <0x4b 0xa3 0x00>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x02>;\n'
        '\t\t\tranges;\n'
        '\t\t\tstatus = "okay";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tspi@a90000 {\n'
        '\t\t\t\tcompatible = "qcom,geni-spi";\n'
        '\t\t\t\treg = <0x00 0xa90000 0x00 0x4000>;\n'
        '\t\t\t\tinterrupts = <0x00 0x165 0x04 0x00>;\n'
        '\t\t\t\tclocks = <0x4c 0x69>;\n'
        '\t\t\t\tclock-names = "se";\n'
        '\t\t\t\tinterconnects = <0x4d 0x01 0x07 0x4d 0x04 0x07 0x07 0x03 0x03 0x4e 0x1e 0x03 0x73 0x01 0x07 0x08 0x01 0x07>;\n'
        '\t\t\t\tinterconnect-names = "qup-core", "qup-config", "qup-memory";\n'
        '\t\t\t\tpower-domains = <0x4f 0x00>;\n'
        '\t\t\t\toperating-points-v2 = <0x50>;\n'
        '\t\t\t\tdmas = <0x74 0x00 0x04 0x01 0x74 0x01 0x04 0x01>;\n'
        '\t\t\t\tdma-names = "tx", "rx";\n'
        '\t\t\t\tpinctrl-0 = <0x83 0x84>;\n'
        '\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\t#address-cells = <0x01>;\n'
        '\t\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t\tstatus = "disabled";\n'
        '\t\t\t};',
        '\t\t\tspi@a90000 {\n'
        '\t\t\t\tcompatible = "qcom,geni-spi";\n'
        '\t\t\t\treg = <0x00 0xa90000 0x00 0x4000>;\n'
        '\t\t\t\tinterrupts = <0x00 0x165 0x04 0x00>;\n'
        '\t\t\t\tclocks = <0x4c 0x69>;\n'
        '\t\t\t\tclock-names = "se";\n'
        '\t\t\t\tinterconnects = <0x4d 0x01 0x07 0x4d 0x04 0x07 0x07 0x03 0x03 0x4e 0x1e 0x03 0x73 0x01 0x07 0x08 0x01 0x07>;\n'
        '\t\t\t\tinterconnect-names = "qup-core", "qup-config", "qup-memory";\n'
        '\t\t\t\tpower-domains = <0x4f 0x00>;\n'
        '\t\t\t\toperating-points-v2 = <0x50>;\n'
        '\t\t\t\tdmas = <0x74 0x00 0x04 0x01 0x74 0x01 0x04 0x01>;\n'
        '\t\t\t\tdma-names = "tx", "rx";\n'
        '\t\t\t\tpinctrl-0 = <0x83 0x84>;\n'
        '\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\t#address-cells = <0x01>;\n'
        '\t\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t\tstatus = "okay";\n\n'
        '\t\t\t\ttouchscreen@0 {\n'
        '\t\t\t\t\tcompatible = "goodix,gt9916";\n'
        '\t\t\t\t\treg = <0x00>;\n'
        '\t\t\t\t\tinterrupts-extended = <0x99 0xa2 0x02>;\n'
        '\t\t\t\t\treset-gpios = <0x99 0xa1 0x01>;\n'
        '\t\t\t\t\tavdd-supply = <0x201>;\n'
        '\t\t\t\t\tvddio-supply = <0x202>;\n'
        '\t\t\t\t\tpinctrl-0 = <0x203>;\n'
        '\t\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\t\ttouchscreen-size-x = <0x45c0>;\n'
        '\t\t\t\t\ttouchscreen-size-y = <0x9b00>;\n'
        '\t\t\t\t\tspi-max-frequency = <0x4c4b40>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\twakeup-parent = <0xae>;\n\t\t\tphandle = <0x99>;\n\n\t\t\tcam0-default-state {',
        '\t\t\twakeup-parent = <0xae>;\n\t\t\tphandle = <0x99>;\n\n'
        '\t\t\tts-active-state {\n'
        '\t\t\t\tpins = "gpio161", "gpio162";\n'
        '\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\tbias-pull-up;\n'
        '\t\t\t\tphandle = <0x203>;\n'
        '\t\t\t};\n\n'
        '\t\t\tcam0-default-state {',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tldo17 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l17b";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x263540>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x263540>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x9a>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
        '\t\t\t\tldo17 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l17b";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x263540>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x263540>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x9a>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tldo14 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l14b";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x30d400>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x30d400>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x201>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )

    # 2i) Enable Qualcomm FastConnect 7800 / WCN7850 Wi-Fi 7 on PCIe0 (pcie@1c00000 + phy@1c06000 + pwrseq-qcom-wcn)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tts-active-state {\n'
        '\t\t\t\tpins = "gpio161", "gpio162";\n'
        '\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\tbias-pull-up;\n'
        '\t\t\t\tphandle = <0x203>;\n'
        '\t\t\t};',
        '\t\t\tts-active-state {\n'
        '\t\t\t\tpins = "gpio161", "gpio162";\n'
        '\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\tbias-pull-up;\n'
        '\t\t\t\tphandle = <0x203>;\n'
        '\t\t\t};\n\n'
        '\t\t\twcn-wlan-bt-active-state {\n'
        '\t\t\t\tphandle = <0x216>;\n\n'
        '\t\t\t\twlan-en-pins {\n'
        '\t\t\t\t\tpins = "gpio16";\n'
        '\t\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\t\tdrive-strength = <0x10>;\n'
        '\t\t\t\t\tbias-pull-down;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tbt-en-pins {\n'
        '\t\t\t\t\tpins = "gpio17";\n'
        '\t\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\t\tdrive-strength = <0x10>;\n'
        '\t\t\t\t\tbias-disable;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tsw-ctrl-pins {\n'
        '\t\t\t\t\tpins = "gpio18", "gpio19";\n'
        '\t\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\t\tbias-pull-down;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tgpio@b800 {\n'
        '\t\t\t\t\tcompatible = "qcom,pmk8550-gpio", "qcom,spmi-gpio";\n'
        '\t\t\t\t\treg = <0xb800>;\n'
        '\t\t\t\t\tgpio-controller;\n'
        '\t\t\t\t\tgpio-ranges = <0xf3 0x00 0x00 0x06>;\n'
        '\t\t\t\t\t#gpio-cells = <0x02>;\n'
        '\t\t\t\t\tinterrupt-controller;\n'
        '\t\t\t\t\t#interrupt-cells = <0x02>;\n'
        '\t\t\t\t\tphandle = <0xf3>;\n'
        '\t\t\t\t};',
        '\t\t\t\tgpio@b800 {\n'
        '\t\t\t\t\tcompatible = "qcom,pmk8550-gpio", "qcom,spmi-gpio";\n'
        '\t\t\t\t\treg = <0xb800>;\n'
        '\t\t\t\t\tgpio-controller;\n'
        '\t\t\t\t\tgpio-ranges = <0xf3 0x00 0x00 0x06>;\n'
        '\t\t\t\t\t#gpio-cells = <0x02>;\n'
        '\t\t\t\t\tinterrupt-controller;\n'
        '\t\t\t\t\t#interrupt-cells = <0x02>;\n'
        '\t\t\t\t\tpinctrl-0 = <0x218>;\n'
        '\t\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\t\tphandle = <0xf3>;\n\n'
        '\t\t\t\t\talt-sleep-clk-state {\n'
        '\t\t\t\t\t\tpins = "gpio3";\n'
        '\t\t\t\t\t\tfunction = "func1";\n'
        '\t\t\t\t\t\tinput-disable;\n'
        '\t\t\t\t\t\toutput-enable;\n'
        '\t\t\t\t\t\tbias-disable;\n'
        '\t\t\t\t\t\tpower-source = <0x00>;\n'
        '\t\t\t\t\t\tphandle = <0x218>;\n'
        '\t\t\t\t\t};\n'
        '\t\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tpcie0-default-state {\n\n\t\t\t\tperst-pins {',
        '\t\t\tpcie0-default-state {\n\t\t\t\tphandle = <0x215>;\n\n\t\t\t\tperst-pins {',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tregulators-nx-c {\n'
        '\t\t\t\tcompatible = "qcom,pm8550vs-rpmh-regulators";\n'
        '\t\t\t\tqcom,pmic-id = "c";\n'
        '\t\t\t\tvdd-s1-supply = <0xff>;\n'
        '\t\t\t\tvdd-s3-supply = <0xff>;\n'
        '\t\t\t\tvdd-s6-supply = <0xff>;\n'
        '\t\t\t\tvdd-l1-supply = <0x103>;',
        '\t\t\tregulators-nx-c {\n'
        '\t\t\t\tcompatible = "qcom,pm8550vs-rpmh-regulators";\n'
        '\t\t\t\tqcom,pmic-id = "c";\n'
        '\t\t\t\tvdd-s1-supply = <0xff>;\n'
        '\t\t\t\tvdd-s2-supply = <0xff>;\n'
        '\t\t\t\tvdd-s3-supply = <0xff>;\n'
        '\t\t\t\tvdd-s6-supply = <0xff>;\n'
        '\t\t\t\tvdd-l1-supply = <0x103>;\n'
        '\t\t\t\tvdd-l3-supply = <0x103>;',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tldo1 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l1c";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x10c8e0>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x9b>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
        '\t\t\t\tldo1 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l1c";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x10c8e0>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x9b>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tsmps2 {\n'
        '\t\t\t\t\tregulator-name = "vreg_s2c";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0xee480>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0xfcee0>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tphandle = <0x211>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tldo3 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l3c";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x212>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tregulators-nx-i {\n'
        '\t\t\t\tcompatible = "qcom,pm8550ve-rpmh-regulators";\n'
        '\t\t\t\tqcom,pmic-id = "i";\n'
        '\t\t\t\tvdd-l1-supply = <0x104>;\n'
        '\t\t\t\tvdd-l3-supply = <0x103>;\n\n'
        '\t\t\t\tldo1 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l1i";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0xd6d80>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0xdea80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0xc2>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tldo3 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l3i";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x96>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
        '\t\t\tregulators-nx-i {\n'
        '\t\t\t\tcompatible = "qcom,pm8550ve-rpmh-regulators";\n'
        '\t\t\t\tqcom,pmic-id = "i";\n'
        '\t\t\t\tvdd-s4-supply = <0xff>;\n'
        '\t\t\t\tvdd-l1-supply = <0x104>;\n'
        '\t\t\t\tvdd-l3-supply = <0x103>;\n\n'
        '\t\t\t\tsmps4 {\n'
        '\t\t\t\t\tregulator-name = "vreg_s4i";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0xe57e0>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0xf51e0>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tphandle = <0x213>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tldo1 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l1i";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0xd6d80>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0xdea80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0xc2>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tldo3 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l3i";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x124f80>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-allow-set-load;\n'
        '\t\t\t\t\tphandle = <0x96>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n\n'
        '\t\t\tregulators-nx-k {\n'
        '\t\t\t\tcompatible = "qcom,pmr735d-rpmh-regulators";\n'
        '\t\t\t\tqcom,pmic-id = "k";\n'
        '\t\t\t\tvdd-l6-supply = <0x101>;\n\n'
        '\t\t\t\tldo6 {\n'
        '\t\t\t\t\tregulator-name = "vreg_l6k";\n'
        '\t\t\t\t\tregulator-min-microvolt = <0x1b7740>;\n'
        '\t\t\t\t\tregulator-max-microvolt = <0x1c61a0>;\n'
        '\t\t\t\t\tregulator-initial-mode = <0x03>;\n'
        '\t\t\t\t\tregulator-always-on;\n'
        '\t\t\t\t\tregulator-boot-on;\n'
        '\t\t\t\t\tphandle = <0x214>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tphy@1c06000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-qmp-gen3x2-pcie-phy";\n'
        '\t\t\treg = <0x00 0x1c06000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x24 0x4c 0x26 0x34 0x00 0x4c 0x28 0x4c 0x2a>;\n'
        '\t\t\tclock-names = "aux", "cfg_ahb", "ref", "rchng", "pipe";\n'
        '\t\t\tassigned-clocks = <0x4c 0x28>;\n'
        '\t\t\tassigned-clock-rates = <0x5f5e100>;\n'
        '\t\t\tresets = <0x4c 0x06>;\n'
        '\t\t\treset-names = "phy";\n'
        '\t\t\tpower-domains = <0x4c 0x01>;\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "pcie0_pipe_clk";\n'
        '\t\t\t#phy-cells = <0x00>;\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t\tphandle = <0x47>;\n'
        '\t\t};',
        '\t\tphy@1c06000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-qmp-gen3x2-pcie-phy";\n'
        '\t\t\treg = <0x00 0x1c06000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x24 0x4c 0x26 0x34 0x00 0x4c 0x28 0x4c 0x2a>;\n'
        '\t\t\tclock-names = "aux", "cfg_ahb", "ref", "rchng", "pipe";\n'
        '\t\t\tassigned-clocks = <0x4c 0x28>;\n'
        '\t\t\tassigned-clock-rates = <0x5f5e100>;\n'
        '\t\t\tresets = <0x4c 0x06>;\n'
        '\t\t\treset-names = "phy";\n'
        '\t\t\tpower-domains = <0x4c 0x01>;\n'
        '\t\t\tvdda-phy-supply = <0xc2>;\n'
        '\t\t\tvdda-pll-supply = <0x96>;\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "pcie0_pipe_clk";\n'
        '\t\t\t#phy-cells = <0x00>;\n'
        '\t\t\tstatus = "okay";\n'
        '\t\t\tphandle = <0x47>;\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tranges = <0x1000000 0x00 0x00 0x00 0x60200000 0x00 0x100000 0x2000000 0x00 0x60300000 0x00 0x60300000 0x00 0x3d00000>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\tstatus = "disabled";',
        '\t\t\tranges = <0x1000000 0x00 0x00 0x00 0x60200000 0x00 0x100000 0x2000000 0x00 0x60300000 0x00 0x60300000 0x00 0x3d00000>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\tperst-gpios = <0x99 0x5e 0x01>;\n'
        '\t\t\twake-gpios = <0x99 0x60 0x01>;\n'
        '\t\t\tpinctrl-0 = <0x215>;\n'
        '\t\t\tpinctrl-names = "default";\n'
        '\t\t\tstatus = "okay";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\topp-16000000-3 {\n'
        '\t\t\t\t\topp-hz = <0x00 0xf42400>;\n'
        '\t\t\t\t\trequired-opps = <0x92>;\n'
        '\t\t\t\t\topp-peak-kBps = <0x1e0b68 0x01>;\n'
        '\t\t\t\t\topp-level = <0x03>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n\n'
        '\t\t\tpcie@0 {\n'
        '\t\t\t\tdevice_type = "pci";\n'
        '\t\t\t\treg = <0x00 0x00 0x00 0x00 0x00>;\n'
        '\t\t\t\tbus-range = <0x01 0xff>;\n'
        '\t\t\t\t#address-cells = <0x03>;\n'
        '\t\t\t\t#size-cells = <0x02>;\n'
        '\t\t\t\tranges;\n'
        '\t\t\t};',
        '\t\t\t\topp-16000000-3 {\n'
        '\t\t\t\t\topp-hz = <0x00 0xf42400>;\n'
        '\t\t\t\t\trequired-opps = <0x92>;\n'
        '\t\t\t\t\topp-peak-kBps = <0x1e0b68 0x01>;\n'
        '\t\t\t\t\topp-level = <0x03>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n\n'
        '\t\t\tpcie@0 {\n'
        '\t\t\t\tdevice_type = "pci";\n'
        '\t\t\t\treg = <0x00 0x00 0x00 0x00 0x00>;\n'
        '\t\t\t\tbus-range = <0x01 0xff>;\n'
        '\t\t\t\t#address-cells = <0x03>;\n'
        '\t\t\t\t#size-cells = <0x02>;\n'
        '\t\t\t\tranges;\n\n'
        '\t\t\t\twifi@0 {\n'
        '\t\t\t\t\tcompatible = "pci17cb,1107";\n'
        '\t\t\t\t\treg = <0x10000 0x00 0x00 0x00 0x00>;\n'
        '\t\t\t\t\tvddrfacmn-supply = <0x220>;\n'
        '\t\t\t\t\tvddaon-supply = <0x217>;\n'
        '\t\t\t\t\tvddwlcx-supply = <0x221>;\n'
        '\t\t\t\t\tvddwlmx-supply = <0x222>;\n'
        '\t\t\t\t\tvddrfa0p8-supply = <0x224>;\n'
        '\t\t\t\t\tvddrfa1p2-supply = <0x225>;\n'
        '\t\t\t\t\tvddrfa1p8-supply = <0x226>;\n'
        '\t\t\t\t\tvddpcie0p9-supply = <0x227>;\n'
        '\t\t\t\t\tvddpcie1p8-supply = <0x228>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )

    # 2j) Enable WCN7850 Bluetooth 5.4 on QUP2 SE6 UART (/aliases/serial0 + geniqup@8c0000 + serial@898000 + qcom,wcn7850-bt)
    linux_dts = replace_exact_once(
        linux_dts,
        '\tchassis-type = "handset";\n\n\tchosen {',
        '\tchassis-type = "handset";\n\n'
        '\taliases {\n'
        '\t\tserial0 = "/soc@0/geniqup@8c0000/serial@898000";\n'
        '\t};\n\n'
        '\tchosen {',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tgeniqup@8c0000 {\n'
        '\t\t\tcompatible = "qcom,geni-se-qup";\n'
        '\t\t\treg = <0x00 0x8c0000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x90 0x4c 0x91>;\n'
        '\t\t\tclock-names = "m-ahb", "s-ahb";\n'
        '\t\t\tiommus = <0x4b 0x423 0x00>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x02>;\n'
        '\t\t\tranges;\n'
        '\t\t\tstatus = "disabled";',
        '\t\tgeniqup@8c0000 {\n'
        '\t\t\tcompatible = "qcom,geni-se-qup";\n'
        '\t\t\treg = <0x00 0x8c0000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x90 0x4c 0x91>;\n'
        '\t\t\tclock-names = "m-ahb", "s-ahb";\n'
        '\t\t\tinterconnects = <0x4d 0x02 0x07 0x4d 0x05 0x07>;\n'
        '\t\t\tinterconnect-names = "qup-core";\n'
        '\t\t\tiommus = <0x4b 0x423 0x00>;\n'
        '\t\t\tdma-coherent;\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x02>;\n'
        '\t\t\tranges;\n'
        '\t\t\tstatus = "okay";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tserial@898000 {\n'
        '\t\t\t\tcompatible = "qcom,geni-uart";\n'
        '\t\t\t\treg = <0x00 0x898000 0x00 0x4000>;\n'
        '\t\t\t\tinterrupts = <0x00 0x1cd 0x04 0x00>;\n'
        '\t\t\t\tclocks = <0x4c 0x82>;\n'
        '\t\t\t\tclock-names = "se";\n'
        '\t\t\t\tinterconnects = <0x4d 0x02 0x07 0x4d 0x05 0x07 0x07 0x03 0x03 0x4e 0x1f 0x03>;\n'
        '\t\t\t\tinterconnect-names = "qup-core", "qup-config";\n'
        '\t\t\t\tpower-domains = <0x4f 0x00>;\n'
        '\t\t\t\toperating-points-v2 = <0x65>;\n'
        '\t\t\t\tpinctrl-0 = <0x66 0x67>;\n'
        '\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\tstatus = "disabled";\n'
        '\t\t\t};',
        '\t\t\tserial@898000 {\n'
        '\t\t\t\tcompatible = "qcom,geni-uart";\n'
        '\t\t\t\treg = <0x00 0x898000 0x00 0x4000>;\n'
        '\t\t\t\tinterrupts = <0x00 0x1cd 0x04 0x00>;\n'
        '\t\t\t\tclocks = <0x4c 0x82>;\n'
        '\t\t\t\tclock-names = "se";\n'
        '\t\t\t\tinterconnects = <0x4d 0x02 0x07 0x4d 0x05 0x07 0x07 0x03 0x03 0x4e 0x1f 0x03>;\n'
        '\t\t\t\tinterconnect-names = "qup-core", "qup-config";\n'
        '\t\t\t\tpower-domains = <0x4f 0x00>;\n'
        '\t\t\t\toperating-points-v2 = <0x65>;\n'
        '\t\t\t\tpinctrl-0 = <0x66 0x67>;\n'
        '\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\tstatus = "okay";\n\n'
        '\t\t\t\tbluetooth {\n'
        '\t\t\t\t\tcompatible = "qcom,wcn7850-bt";\n'
        '\t\t\t\t\tvddaon-supply = <0x217>;\n'
        '\t\t\t\t\tvddbtcmx-supply = <0x223>;\n'
        '\t\t\t\t\tvddwlcx-supply = <0x221>;\n'
        '\t\t\t\t\tvddwlmx-supply = <0x222>;\n'
        '\t\t\t\t\tvddrfacmn-supply = <0x220>;\n'
        '\t\t\t\t\tvddrfa0p8-supply = <0x224>;\n'
        '\t\t\t\t\tvddrfa1p2-supply = <0x225>;\n'
        '\t\t\t\t\tvddrfa1p8-supply = <0x226>;\n'
        '\t\t\t\t\tmax-speed = <0x30d400>;\n'
        f'\t\t\t\t\tlocal-bd-address = [ {bt_mac_dt} ];\n'
        '\t\t\t\t};\n'
        '\t\t\t};',
    )

    # 2k) Enable SM8650 ADSP PAS (remoteproc@6800000) + PMIC GLINK (qcom_battmgr battery & charger telemetry)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tremoteproc@6800000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-adsp-pas";\n'
        '\t\t\treg = <0x00 0x6800000 0x00 0x10000>;\n'
        '\t\t\tinterrupts-extended = <0xae 0x06 0x01 0xaf 0x00 0x01 0xaf 0x01 0x01 0xaf 0x02 0x01 0xaf 0x03 0x01>;\n'
        '\t\t\tinterrupt-names = "wdog", "fatal", "ready", "handover", "stop-ack";\n'
        '\t\t\tclocks = <0x02 0x00>;\n'
        '\t\t\tclock-names = "xo";\n'
        '\t\t\tinterconnects = <0xb0 0x00 0x07 0x08 0x01 0x07>;\n'
        '\t\t\tpower-domains = <0x4f 0x04 0x4f 0x05>;\n'
        '\t\t\tpower-domain-names = "lcx", "lmx";\n'
        '\t\t\tmemory-region = <0xb1 0xb2>;\n'
        '\t\t\tqcom,qmp = <0xa2>;\n'
        '\t\t\tqcom,smem-states = <0xb3 0x00>;\n'
        '\t\t\tqcom,smem-state-names = "stop";\n'
        '\t\t\tstatus = "disabled";\n\n'
        '\t\t\tglink-edge {\n'
        '\t\t\t\tinterrupts-extended = <0x43 0x03 0x00 0x01>;\n'
        '\t\t\t\tmboxes = <0x43 0x03 0x00>;\n'
        '\t\t\t\tqcom,remote-pid = <0x02>;\n'
        '\t\t\t\tlabel = "lpass";\n\n'
        '\t\t\t\tfastrpc {\n'
        '\t\t\t\t\tcompatible = "qcom,fastrpc";\n'
        '\t\t\t\t\tqcom,glink-channels = "fastrpcglink-apps-dsp";\n'
        '\t\t\t\t\tlabel = "adsp";',
        '\t\tremoteproc@6800000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-adsp-pas";\n'
        '\t\t\treg = <0x00 0x6800000 0x00 0x10000>;\n'
        '\t\t\tinterrupts-extended = <0xae 0x06 0x01 0xaf 0x00 0x01 0xaf 0x01 0x01 0xaf 0x02 0x01 0xaf 0x03 0x01>;\n'
        '\t\t\tinterrupt-names = "wdog", "fatal", "ready", "handover", "stop-ack";\n'
        '\t\t\tclocks = <0x02 0x00>;\n'
        '\t\t\tclock-names = "xo";\n'
        '\t\t\tinterconnects = <0xb0 0x00 0x07 0x08 0x01 0x07>;\n'
        '\t\t\tpower-domains = <0x4f 0x04 0x4f 0x05>;\n'
        '\t\t\tpower-domain-names = "lcx", "lmx";\n'
        '\t\t\tmemory-region = <0xb1 0xb2>;\n'
        '\t\t\tqcom,qmp = <0xa2>;\n'
        '\t\t\tqcom,smem-states = <0xb3 0x00>;\n'
        '\t\t\tqcom,smem-state-names = "stop";\n'
        '\t\t\tfirmware-name = "qcom/sm8650/nubia/nx769j/adsp.mdt", "qcom/sm8650/nubia/nx769j/adsp_dtb.mdt";\n'
        '\t\t\tstatus = "okay";\n\n'
        '\t\t\tglink-edge {\n'
        '\t\t\t\tinterrupts-extended = <0x43 0x03 0x00 0x01>;\n'
        '\t\t\t\tmboxes = <0x43 0x03 0x00>;\n'
        '\t\t\t\tqcom,remote-pid = <0x02>;\n'
        '\t\t\t\tlabel = "lpass";\n\n'
        '\t\t\t\tfastrpc {\n'
        '\t\t\t\t\tcompatible = "qcom,fastrpc";\n'
        '\t\t\t\t\tqcom,glink-channels = "fastrpcglink-apps-dsp";\n'
        '\t\t\t\t\tlabel = "adsp";\n'
        '\t\t\t\t\tstatus = "disabled";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\tvph-pwr-regulator {\n'
        '\t\tcompatible = "regulator-fixed";\n'
        '\t\tregulator-name = "vph_pwr";\n'
        '\t\tregulator-min-microvolt = <0x387520>;\n'
        '\t\tregulator-max-microvolt = <0x387520>;\n'
        '\t\tregulator-always-on;\n'
        '\t\tregulator-boot-on;\n'
        '\t\tphandle = <0xff>;\n'
        '\t};\n'
        '};',
        '\tvph-pwr-regulator {\n'
        '\t\tcompatible = "regulator-fixed";\n'
        '\t\tregulator-name = "vph_pwr";\n'
        '\t\tregulator-min-microvolt = <0x387520>;\n'
        '\t\tregulator-max-microvolt = <0x387520>;\n'
        '\t\tregulator-always-on;\n'
        '\t\tregulator-boot-on;\n'
        '\t\tphandle = <0xff>;\n'
        '\t};\n\n'
        '\tpmic-glink {\n'
        '\t\tcompatible = "qcom,sm8650-pmic-glink", "qcom,pmic-glink";\n'
        '\t};\n'
        '};',
    )

    # 2l) Enable Built-in Turbo Cooling Fan (TLMM gpio173 boost regulator-fixed + PM8550 GPIO9 LPG ch3 25kHz PWM + TLMM gpio174 tach + cpuss0-thermal cooling-maps)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tcam0-default-state {\n'
        '\t\t\t\tpins = "gpio100";',
        '\t\t\tfan-en-state {\n'
        '\t\t\t\tpins = "gpio173";\n'
        '\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\tbias-disable;\n'
        '\t\t\t\toutput-low;\n'
        '\t\t\t\tphandle = <0x4602>;\n'
        '\t\t\t};\n\n'
        '\t\t\tfan-tach-state {\n'
        '\t\t\t\tpins = "gpio174";\n'
        '\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\tdrive-strength = <0x10>;\n'
        '\t\t\t\tbias-pull-up;\n'
        '\t\t\t\tinput-enable;\n'
        '\t\t\t\tphandle = <0x4603>;\n'
        '\t\t\t};\n\n'
        '\t\t\tcam0-default-state {\n'
        '\t\t\t\tpins = "gpio100";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tgpio@8800 {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-gpio", "qcom,spmi-gpio";\n'
        '\t\t\t\t\treg = <0x8800>;\n'
        '\t\t\t\t\tgpio-controller;\n'
        '\t\t\t\t\tgpio-ranges = <0xea 0x00 0x00 0x0c>;\n'
        '\t\t\t\t\t#gpio-cells = <0x02>;\n'
        '\t\t\t\t\tinterrupt-controller;\n'
        '\t\t\t\t\t#interrupt-cells = <0x02>;\n'
        '\t\t\t\t\tphandle = <0xea>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tled-controller@ee00 {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-flash-led", "qcom,spmi-flash-led";\n'
        '\t\t\t\t\treg = <0xee00>;\n'
        '\t\t\t\t\tstatus = "disabled";\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tpwm {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-pwm", "qcom,pm8350c-pwm";\n'
        '\t\t\t\t\t#pwm-cells = <0x02>;\n'
        '\t\t\t\t\tstatus = "disabled";\n'
        '\t\t\t\t};',
        '\t\t\t\tgpio@8800 {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-gpio", "qcom,spmi-gpio";\n'
        '\t\t\t\t\treg = <0x8800>;\n'
        '\t\t\t\t\tgpio-controller;\n'
        '\t\t\t\t\tgpio-ranges = <0xea 0x00 0x00 0x0c>;\n'
        '\t\t\t\t\t#gpio-cells = <0x02>;\n'
        '\t\t\t\t\tinterrupt-controller;\n'
        '\t\t\t\t\t#interrupt-cells = <0x02>;\n'
        '\t\t\t\t\tphandle = <0xea>;\n\n'
        '\t\t\t\t\tfan-pwm-state {\n'
        '\t\t\t\t\t\tpins = "gpio9";\n'
        '\t\t\t\t\t\tfunction = "func1";\n'
        '\t\t\t\t\t\tinput-disable;\n'
        '\t\t\t\t\t\toutput-high;\n'
        '\t\t\t\t\t\toutput-enable;\n'
        '\t\t\t\t\t\tbias-disable;\n'
        '\t\t\t\t\t\tdrive-push-pull;\n'
        '\t\t\t\t\t\tpower-source = <0x00>;\n'
        '\t\t\t\t\t\tqcom,drive-strength = <0x03>;\n'
        '\t\t\t\t\t\tphandle = <0x4601>;\n'
        '\t\t\t\t\t};\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tled-controller@ee00 {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-flash-led", "qcom,spmi-flash-led";\n'
        '\t\t\t\t\treg = <0xee00>;\n'
        '\t\t\t\t\tstatus = "disabled";\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tpwm {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-pwm", "qcom,pm8350c-pwm";\n'
        '\t\t\t\t\t#pwm-cells = <0x02>;\n'
        '\t\t\t\t\tpinctrl-0 = <0x4601>;\n'
        '\t\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\t\tstatus = "okay";\n'
        '\t\t\t\t\tphandle = <0x4600>;\n'
        '\t\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tcpuss0-thermal {\n'
        '\t\t\tthermal-sensors = <0x10d 0x01>;\n\n'
        '\t\t\ttrips {\n\n'
        '\t\t\t\tcpuss0-hot {\n'
        '\t\t\t\t\ttemperature = <0x1adb0>;\n'
        '\t\t\t\t\thysteresis = <0x3e8>;\n'
        '\t\t\t\t\ttype = "hot";\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tcpuss0-critical {\n'
        '\t\t\t\t\ttemperature = <0x1c138>;\n'
        '\t\t\t\t\thysteresis = <0x00>;\n'
        '\t\t\t\t\ttype = "critical";\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n'
        '\t\t};',
        '\t\tcpuss0-thermal {\n'
        '\t\t\tpolling-delay = <0x3e8>;\n'
        '\t\t\tpolling-delay-passive = <0xfa>;\n'
        '\t\t\tthermal-sensors = <0x10d 0x01>;\n\n'
        '\t\t\tcooling-maps {\n\n'
        '\t\t\t\tfan-map0 {\n'
        '\t\t\t\t\ttrip = <0x4606>;\n'
        '\t\t\t\t\tcooling-device = <0x4605 0x01 0x01>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-map1 {\n'
        '\t\t\t\t\ttrip = <0x4607>;\n'
        '\t\t\t\t\tcooling-device = <0x4605 0x02 0x02>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-map2 {\n'
        '\t\t\t\t\ttrip = <0x4608>;\n'
        '\t\t\t\t\tcooling-device = <0x4605 0x03 0x03>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-map3 {\n'
        '\t\t\t\t\ttrip = <0x4609>;\n'
        '\t\t\t\t\tcooling-device = <0x4605 0x04 0x04>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-map4 {\n'
        '\t\t\t\t\ttrip = <0x460a>;\n'
        '\t\t\t\t\tcooling-device = <0x4605 0x05 0x05>;\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n\n'
        '\t\t\ttrips {\n\n'
        '\t\t\t\tfan-trip0 {\n'
        '\t\t\t\t\ttemperature = <0xd6d8>;\n'
        '\t\t\t\t\thysteresis = <0x1388>;\n'
        '\t\t\t\t\ttype = "active";\n'
        '\t\t\t\t\tphandle = <0x4606>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-trip1 {\n'
        '\t\t\t\t\ttemperature = <0xfde8>;\n'
        '\t\t\t\t\thysteresis = <0x1388>;\n'
        '\t\t\t\t\ttype = "active";\n'
        '\t\t\t\t\tphandle = <0x4607>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-trip2 {\n'
        '\t\t\t\t\ttemperature = <0x124f8>;\n'
        '\t\t\t\t\thysteresis = <0x1388>;\n'
        '\t\t\t\t\ttype = "active";\n'
        '\t\t\t\t\tphandle = <0x4608>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-trip3 {\n'
        '\t\t\t\t\ttemperature = <0x14c08>;\n'
        '\t\t\t\t\thysteresis = <0x1388>;\n'
        '\t\t\t\t\ttype = "active";\n'
        '\t\t\t\t\tphandle = <0x4609>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tfan-trip4 {\n'
        '\t\t\t\t\ttemperature = <0x16760>;\n'
        '\t\t\t\t\thysteresis = <0x1388>;\n'
        '\t\t\t\t\ttype = "active";\n'
        '\t\t\t\t\tphandle = <0x460a>;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tcpuss0-hot {\n'
        '\t\t\t\t\ttemperature = <0x1adb0>;\n'
        '\t\t\t\t\thysteresis = <0x3e8>;\n'
        '\t\t\t\t\ttype = "hot";\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tcpuss0-critical {\n'
        '\t\t\t\t\ttemperature = <0x1c138>;\n'
        '\t\t\t\t\thysteresis = <0x00>;\n'
        '\t\t\t\t\ttype = "critical";\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\treboot-mode {\n'
        '\t\tcompatible = "nvmem-reboot-mode";\n'
        '\t\tnvmem-cells = <0x120>;\n'
        '\t\tnvmem-cell-names = "reboot-mode";\n'
        '\t\tmode-recovery = <0x01>;\n'
        '\t\tmode-bootloader = <0x02>;\n'
        '\t};',
        '\treboot-mode {\n'
        '\t\tcompatible = "nvmem-reboot-mode";\n'
        '\t\tnvmem-cells = <0x120>;\n'
        '\t\tnvmem-cell-names = "reboot-mode";\n'
        '\t\tmode-recovery = <0x01>;\n'
        '\t\tmode-bootloader = <0x02>;\n'
        '\t};\n\n'
        '\tfan-pwr-regulator {\n'
        '\t\tcompatible = "regulator-fixed";\n'
        '\t\tregulator-name = "vreg_fan_pwr";\n'
        '\t\tregulator-min-microvolt = <0x4c4b40>;\n'
        '\t\tregulator-max-microvolt = <0x4c4b40>;\n'
        '\t\tgpio = <0x99 0xad 0x00>;\n'
        '\t\tenable-active-high;\n'
        '\t\tstartup-delay-us = <0x186a0>;\n'
        '\t\tpinctrl-0 = <0x4602>;\n'
        '\t\tpinctrl-names = "default";\n'
        '\t\tphandle = <0x4604>;\n'
        '\t};\n\n'
        '\tpwm-fan {\n'
        '\t\tcompatible = "pwm-fan";\n'
        '\t\tpwms = <0x4600 0x03 0x9c40>;\n'
        '\t\tfan-supply = <0x4604>;\n'
        '\t\tinterrupts-extended = <0x99 0xae 0x02>;\n'
        '\t\tpulses-per-revolution = <0x03>;\n'
        '\t\tpinctrl-0 = <0x4603>;\n'
        '\t\tpinctrl-names = "default";\n'
        '\t\t#cooling-cells = <0x02>;\n'
        '\t\tcooling-levels = <0x00 0x33 0x66 0x99 0xcc 0xff>;\n'
        '\t\tphandle = <0x4605>;\n'
        '\t};',
    )

    # 2m) Enable Adreno 750 headless 3D GPU (gpu@3d00000 + ZTE-signed PAS_ID=13 zap-shader)
    #     Display-subsystem@ae00000 & dispcc@af00000 remain disabled so efifb (0xe3940000) stays untouched!
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tgpu@3d00000 {\n'
        '\t\t\tcompatible = "qcom,adreno-43051401", "qcom,adreno";\n'
        '\t\t\treg = <0x00 0x3d00000 0x00 0x40000 0x00 0x3d9e000 0x00 0x2000 0x00 0x3d61000 0x00 0x800>;\n'
        '\t\t\treg-names = "kgsl_3d0_reg_memory", "cx_mem", "cx_dbgc";\n'
        '\t\t\tinterrupts = <0x00 0x12c 0x04 0x00>;\n'
        '\t\t\tiommus = <0x9d 0x00 0x00 0x9d 0x01 0x00>;\n'
        '\t\t\toperating-points-v2 = <0x9e>;\n'
        '\t\t\tqcom,gmu = <0x9f>;\n'
        '\t\t\t#cooling-cells = <0x02>;\n'
        '\t\t\tinterconnects = <0x07 0x04 0x07 0x08 0x01 0x07>;\n'
        '\t\t\tinterconnect-names = "gfx-mem";\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t\tphandle = <0x111>;\n\n'
        '\t\t\tzap-shader {\n'
        '\t\t\t\tmemory-region = <0xa0>;\n'
        '\t\t\t};',
        '\t\tgpu@3d00000 {\n'
        '\t\t\tcompatible = "qcom,adreno-43051401", "qcom,adreno";\n'
        '\t\t\treg = <0x00 0x3d00000 0x00 0x40000 0x00 0x3d9e000 0x00 0x2000 0x00 0x3d61000 0x00 0x800>;\n'
        '\t\t\treg-names = "kgsl_3d0_reg_memory", "cx_mem", "cx_dbgc";\n'
        '\t\t\tinterrupts = <0x00 0x12c 0x04 0x00>;\n'
        '\t\t\tiommus = <0x9d 0x00 0x00 0x9d 0x01 0x00>;\n'
        '\t\t\toperating-points-v2 = <0x9e>;\n'
        '\t\t\tqcom,gmu = <0x9f>;\n'
        '\t\t\t#cooling-cells = <0x02>;\n'
        '\t\t\tinterconnects = <0x07 0x04 0x07 0x08 0x01 0x07>;\n'
        '\t\t\tinterconnect-names = "gfx-mem";\n'
        '\t\t\tstatus = "okay";\n'
        '\t\t\tphandle = <0x111>;\n\n'
        '\t\t\tzap-shader {\n'
        '\t\t\t\tmemory-region = <0xa0>;\n'
        '\t\t\t\tfirmware-name = "qcom/sm8650/nubia/nx769j/gen70900_zap.mbn";\n'
        '\t\t\t};',
    )

    # 2n) Enable SM8650 ADSP FastRPC (remoteproc@6800000/glink-edge/fastrpc -> /dev/fastrpc-adsp for HexagonRPCD & QSH 6-axis IMU)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tfastrpc {\n'
        '\t\t\t\t\tcompatible = "qcom,fastrpc";\n'
        '\t\t\t\t\tqcom,glink-channels = "fastrpcglink-apps-dsp";\n'
        '\t\t\t\t\tlabel = "adsp";\n'
        '\t\t\t\t\tstatus = "disabled";',
        '\t\t\t\tfastrpc {\n'
        '\t\t\t\t\tcompatible = "qcom,fastrpc";\n'
        '\t\t\t\t\tqcom,glink-channels = "fastrpcglink-apps-dsp";\n'
        '\t\t\t\t\tlabel = "adsp";',
    )

    # 2o) Enable NX769J Audio: Dual AW88261 SmartPA I2C (geniqup@9c0000/i2c@98c000) + Primary MI2S
    #     + WCD9395 3.5mm Headphone/Headset Jack & Analog Mics (SoundWire RX/TX c0bec4b58b8e fix + va_macro fsgen)
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tgeniqup@9c0000 {\n'
        '\t\t\tcompatible = "qcom,geni-se-i2c-master-hub";\n'
        '\t\t\treg = <0x00 0x9c0000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x5c>;\n'
        '\t\t\tclock-names = "s-ahb";\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x02>;\n'
        '\t\t\tranges;\n'
        '\t\t\tstatus = "disabled";',
        '\t\tgeniqup@9c0000 {\n'
        '\t\t\tcompatible = "qcom,geni-se-i2c-master-hub";\n'
        '\t\t\treg = <0x00 0x9c0000 0x00 0x2000>;\n'
        '\t\t\tclocks = <0x4c 0x5c>;\n'
        '\t\t\tclock-names = "s-ahb";\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x02>;\n'
        '\t\t\tranges;\n'
        '\t\t\tstatus = "okay";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\ti2c@98c000 {\n'
        '\t\t\t\tcompatible = "qcom,geni-i2c-master-hub";\n'
        '\t\t\t\treg = <0x00 0x98c000 0x00 0x4000>;\n'
        '\t\t\t\tinterrupts = <0x00 0x1d3 0x04 0x00>;\n'
        '\t\t\t\tclocks = <0x4c 0x4e 0x4c 0x47>;\n'
        '\t\t\t\tclock-names = "se", "core";\n'
        '\t\t\t\tinterconnects = <0x4d 0x00 0x07 0x4d 0x03 0x07 0x07 0x03 0x03 0x4e 0x0f 0x03>;\n'
        '\t\t\t\tinterconnect-names = "qup-core", "qup-config";\n'
        '\t\t\t\tpower-domains = <0x4f 0x00>;\n'
        '\t\t\t\trequired-opps = <0x37>;\n'
        '\t\t\t\tpinctrl-0 = <0x6c>;\n'
        '\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\t#address-cells = <0x01>;\n'
        '\t\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t\tstatus = "disabled";\n'
        '\t\t\t};',
        '\t\t\ti2c@98c000 {\n'
        '\t\t\t\tcompatible = "qcom,geni-i2c-master-hub";\n'
        '\t\t\t\treg = <0x00 0x98c000 0x00 0x4000>;\n'
        '\t\t\t\tinterrupts = <0x00 0x1d3 0x04 0x00>;\n'
        '\t\t\t\tclocks = <0x4c 0x4e 0x4c 0x47>;\n'
        '\t\t\t\tclock-names = "se", "core";\n'
        '\t\t\t\tinterconnects = <0x4d 0x00 0x07 0x4d 0x03 0x07 0x07 0x03 0x03 0x4e 0x0f 0x03>;\n'
        '\t\t\t\tinterconnect-names = "qup-core", "qup-config";\n'
        '\t\t\t\tpower-domains = <0x4f 0x00>;\n'
        '\t\t\t\trequired-opps = <0x37>;\n'
        '\t\t\t\tpinctrl-0 = <0x6c>;\n'
        '\t\t\t\tpinctrl-names = "default";\n'
        '\t\t\t\tclock-frequency = <0x61a80>;\n'
        '\t\t\t\t#address-cells = <0x01>;\n'
        '\t\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t\tstatus = "okay";\n'
        '\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tcam0-default-state {\n'
        '\t\t\t\tpins = "gpio100";',
        # v22 fix (swarm_speaker_acoustic): gpio127=i2s0_data0 DIN (SDO), gpio128=i2s0_data1 DOUT (SDI)
        '\t\t\tpri-mi2s-active-state {\n'
        '\t\t\t\tphandle = <0x254>;\n\n'
        '\t\t\t\tsck-pins {\n'
        '\t\t\t\t\tpins = "gpio126";\n'
        '\t\t\t\t\tfunction = "i2s0_sck";\n'
        '\t\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\t\tbias-disable;\n'
        '\t\t\t\t\toutput-high;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tsd0-pins {\n'
        '\t\t\t\t\tpins = "gpio127";\n'
        '\t\t\t\t\tfunction = "i2s0_data0";\n'
        '\t\t\t\t\tdrive-strength = <0x02>;\n'
        '\t\t\t\t\tbias-pull-down;\n'
        '\t\t\t\t\tinput-enable;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tsd1-pins {\n'
        '\t\t\t\t\tpins = "gpio128";\n'
        '\t\t\t\t\tfunction = "i2s0_data1";\n'
        '\t\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\t\tbias-disable;\n'
        '\t\t\t\t\toutput-low;\n'
        '\t\t\t\t};\n\n'
        '\t\t\t\tws-pins {\n'
        '\t\t\t\t\tpins = "gpio129";\n'
        '\t\t\t\t\tfunction = "i2s0_ws";\n'
        '\t\t\t\t\tdrive-strength = <0x08>;\n'
        '\t\t\t\t\tbias-disable;\n'
        '\t\t\t\t\toutput-high;\n'
        '\t\t\t\t};\n'
        '\t\t\t};\n\n'
        '\t\t\twcd-default-state {\n'
        '\t\t\t\tpins = "gpio107";\n'
        '\t\t\t\tfunction = "gpio";\n'
        '\t\t\t\tdrive-strength = <0x10>;\n'
        '\t\t\t\tbias-disable;\n'
        '\t\t\t\toutput-low;\n'
        '\t\t\t\tphandle = <0x255>;\n'
        '\t\t\t};\n\n'
        '\t\t\tcam0-default-state {\n'
        '\t\t\t\tpins = "gpio100";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\t\tservice@1 {\n'
        '\t\t\t\t\t\tcompatible = "qcom,q6apm";\n'
        '\t\t\t\t\t\treg = <0x01>;\n'
        '\t\t\t\t\t\t#sound-dai-cells = <0x00>;\n'
        '\t\t\t\t\t\tqcom,protection-domain = "avs/audio", "msm/adsp/audio_pd";\n\n'
        '\t\t\t\t\t\tbedais {\n'
        '\t\t\t\t\t\t\tcompatible = "qcom,q6apm-lpass-dais";\n'
        '\t\t\t\t\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\t\t\t\t};',
        '\t\t\t\t\tservice@1 {\n'
        '\t\t\t\t\t\tcompatible = "qcom,q6apm";\n'
        '\t\t\t\t\t\treg = <0x01>;\n'
        '\t\t\t\t\t\t#sound-dai-cells = <0x00>;\n'
        '\t\t\t\t\t\tqcom,protection-domain = "avs/audio", "msm/adsp/audio_pd";\n'
        '\t\t\t\t\t\tphandle = <0x256>;\n\n'
        '\t\t\t\t\t\tbedais {\n'
        '\t\t\t\t\t\t\tcompatible = "qcom,q6apm-lpass-dais";\n'
        '\t\t\t\t\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\t\t\t\t\tphandle = <0x257>;\n'
        '\t\t\t\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tcodec@6aa0000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-lpass-wsa-macro", "qcom,sm8550-lpass-wsa-macro";\n'
        '\t\t\treg = <0x00 0x6aa0000 0x00 0x1000>;\n'
        '\t\t\tclocks = <0xb4 0x44 0x01 0xb4 0x66 0x01 0xb4 0x67 0x01 0xb5>;\n'
        '\t\t\tclock-names = "mclk", "macro", "dcodec", "fsgen";\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "wsa2-mclk";\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tphandle = <0xb6>;\n'
        '\t\t};',
        '\t\tcodec@6aa0000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-lpass-wsa-macro", "qcom,sm8550-lpass-wsa-macro";\n'
        '\t\t\treg = <0x00 0x6aa0000 0x00 0x1000>;\n'
        '\t\t\tclocks = <0xb4 0x44 0x01 0xb4 0x66 0x01 0xb4 0x67 0x01 0xb5>;\n'
        '\t\t\tclock-names = "mclk", "macro", "dcodec", "fsgen";\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "wsa2-mclk";\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t\tphandle = <0xb6>;\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tcodec@6b00000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-lpass-wsa-macro", "qcom,sm8550-lpass-wsa-macro";\n'
        '\t\t\treg = <0x00 0x6b00000 0x00 0x1000>;\n'
        '\t\t\tclocks = <0xb4 0x42 0x01 0xb4 0x66 0x01 0xb4 0x67 0x01 0xb5>;\n'
        '\t\t\tclock-names = "mclk", "macro", "dcodec", "fsgen";\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "mclk";\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tphandle = <0xba>;\n'
        '\t\t};',
        '\t\tcodec@6b00000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-lpass-wsa-macro", "qcom,sm8550-lpass-wsa-macro";\n'
        '\t\t\treg = <0x00 0x6b00000 0x00 0x1000>;\n'
        '\t\t\tclocks = <0xb4 0x42 0x01 0xb4 0x66 0x01 0xb4 0x67 0x01 0xb5>;\n'
        '\t\t\tclock-names = "mclk", "macro", "dcodec", "fsgen";\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "mclk";\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t\tphandle = <0xba>;\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tsoundwire@6ad0000 {\n'
        '\t\t\tcompatible = "qcom,soundwire-v2.0.0";\n'
        '\t\t\treg = <0x00 0x6ad0000 0x00 0x10000>;\n'
        '\t\t\tinterrupts = <0x00 0x9b 0x04 0x00>;\n'
        '\t\t\tclocks = <0xb8>;\n'
        '\t\t\tclock-names = "iface";\n'
        '\t\t\tlabel = "RX";\n'
        '\t\t\tpinctrl-0 = <0xb9>;\n'
        '\t\t\tpinctrl-names = "default";\n'
        '\t\t\tqcom,din-ports = <0x00>;\n'
        '\t\t\tqcom,dout-ports = <0x0b>;\n'
        '\t\t\tqcom,ports-sinterval = [00 03 00 1f 00 1f 00 07 00 03 00 ff 00 ff 00 31 00 ff 00 ff 00 ff];\n'
        '\t\t\tqcom,ports-offset1 = [00 00 0b 09 01 ff ff 00 ff ff ff];\n'
        '\t\t\tqcom,ports-offset2 = [00 00 0b 00 00 ff ff 00 ff ff ff];\n'
        '\t\t\tqcom,ports-hstart = [ff 03 ff ff ff ff ff 00 ff ff ff];\n'
        '\t\t\tqcom,ports-hstop = [ff 06 ff ff ff ff ff 0f ff ff ff];\n'
        '\t\t\tqcom,ports-word-length = [01 07 04 ff ff ff ff 18 ff ff ff];\n'
        '\t\t\tqcom,ports-block-pack-mode = [ff 00 01 ff ff ff ff 01 ff ff ff];\n'
        '\t\t\tqcom,ports-block-group-count = [ff ff ff 01 03 ff ff 00 ff ff ff];\n'
        '\t\t\tqcom,ports-lane-control = [01 00 00 00 00 ff ff 01 ff ff ff];\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t};',
        '\t\tsoundwire@6ad0000 {\n'
        '\t\t\tcompatible = "qcom,soundwire-v2.0.0";\n'
        '\t\t\treg = <0x00 0x6ad0000 0x00 0x10000>;\n'
        '\t\t\tinterrupts = <0x00 0x9b 0x04 0x00>;\n'
        '\t\t\tclocks = <0xb8>;\n'
        '\t\t\tclock-names = "iface";\n'
        '\t\t\tlabel = "RX";\n'
        '\t\t\tpinctrl-0 = <0xb9>;\n'
        '\t\t\tpinctrl-names = "default";\n'
        '\t\t\tqcom,din-ports = <0x01>;\n'
        '\t\t\tqcom,dout-ports = <0x0b>;\n'
        '\t\t\tqcom,ports-sinterval = [00 03 00 1f 00 1f 00 07 00 03 00 ff 00 ff 00 31 00 ff 00 ff 00 ff 00 ff];\n'
        '\t\t\tqcom,ports-offset1 = [00 00 0b 09 01 ff ff 00 ff ff ff ff];\n'
        '\t\t\tqcom,ports-offset2 = [00 00 0b 00 00 ff ff 00 ff ff ff ff];\n'
        '\t\t\tqcom,ports-hstart = [ff 03 ff ff ff ff ff 00 ff ff ff ff];\n'
        '\t\t\tqcom,ports-hstop = [ff 06 ff ff ff ff ff 0f ff ff ff ff];\n'
        '\t\t\tqcom,ports-word-length = [01 07 04 ff ff ff ff 18 ff ff ff ff];\n'
        '\t\t\tqcom,ports-block-pack-mode = [ff 00 01 ff ff ff ff 01 ff ff ff ff];\n'
        '\t\t\tqcom,ports-block-group-count = [ff ff ff 01 03 ff ff 00 ff ff ff ff];\n'
        '\t\t\tqcom,ports-lane-control = [01 00 00 00 00 ff ff 01 ff ff ff ff];\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tstatus = "okay";\n'
        '\t\t\tphandle = <0x258>;\n\n'
        '\t\t\tcodec@0,4 {\n'
        '\t\t\t\tcompatible = "sdw20217010e00";\n'
        '\t\t\t\treg = <0x00 0x04>;\n'
        '\t\t\t\tqcom,rx-port-mapping = <0x01 0x02 0x03 0x04 0x05 0x09>;\n'
        '\t\t\t\tphandle = <0x251>;\n'
        '\t\t\t};\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tsoundwire@6d30000 {\n'
        '\t\t\tcompatible = "qcom,soundwire-v2.0.0";\n'
        '\t\t\treg = <0x00 0x6d30000 0x00 0x10000>;\n'
        '\t\t\tinterrupts = <0x00 0x1f0 0x04 0x00 0x00 0x208 0x04 0x00>;\n'
        '\t\t\tinterrupt-names = "core", "wakeup";\n'
        '\t\t\tclocks = <0xbc>;\n'
        '\t\t\tclock-names = "iface";\n'
        '\t\t\tlabel = "TX";\n'
        '\t\t\tpinctrl-0 = <0xbd>;\n'
        '\t\t\tpinctrl-names = "default";\n'
        '\t\t\tqcom,din-ports = <0x04>;\n'
        '\t\t\tqcom,dout-ports = <0x00>;\n'
        '\t\t\tqcom,ports-sinterval-low = <0x1010303>;\n'
        '\t\t\tqcom,ports-offset1 = <0x101>;\n'
        '\t\t\tqcom,ports-offset2 = <0x00>;\n'
        '\t\t\tqcom,ports-hstart = <0xffffffff>;\n'
        '\t\t\tqcom,ports-hstop = <0xffffffff>;\n'
        '\t\t\tqcom,ports-word-length = <0xffffffff>;\n'
        '\t\t\tqcom,ports-block-pack-mode = <0xffffffff>;\n'
        '\t\t\tqcom,ports-block-group-count = <0xffffffff>;\n'
        '\t\t\tqcom,ports-lane-control = <0x1020000>;\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tstatus = "disabled";\n'
        '\t\t};',
        '\t\tsoundwire@6d30000 {\n'
        '\t\t\tcompatible = "qcom,soundwire-v2.0.0";\n'
        '\t\t\treg = <0x00 0x6d30000 0x00 0x10000>;\n'
        '\t\t\tinterrupts = <0x00 0x1f0 0x04 0x00 0x00 0x208 0x04 0x00>;\n'
        '\t\t\tinterrupt-names = "core", "wakeup";\n'
        '\t\t\tclocks = <0xbc>;\n'
        '\t\t\tclock-names = "iface";\n'
        '\t\t\tlabel = "TX";\n'
        '\t\t\tpinctrl-0 = <0xbd>;\n'
        '\t\t\tpinctrl-names = "default";\n'
        '\t\t\tqcom,din-ports = <0x04>;\n'
        '\t\t\tqcom,dout-ports = <0x01>;\n'
        '\t\t\tqcom,ports-sinterval-low = [00 01 01 03 03];\n'
        '\t\t\tqcom,ports-offset1 = [00 00 00 01 01];\n'
        '\t\t\tqcom,ports-offset2 = [00 00 00 00 00];\n'
        '\t\t\tqcom,ports-hstart = [ff ff ff ff ff];\n'
        '\t\t\tqcom,ports-hstop = [ff ff ff ff ff];\n'
        '\t\t\tqcom,ports-word-length = [ff ff ff ff ff];\n'
        '\t\t\tqcom,ports-block-pack-mode = [ff ff ff ff ff];\n'
        '\t\t\tqcom,ports-block-group-count = [ff ff ff ff ff];\n'
        '\t\t\tqcom,ports-lane-control = [ff 01 02 00 00];\n'
        '\t\t\t#address-cells = <0x02>;\n'
        '\t\t\t#size-cells = <0x00>;\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tstatus = "okay";\n'
        '\t\t\tphandle = <0x259>;\n\n'
        '\t\t\tcodec@0,3 {\n'
        '\t\t\t\tcompatible = "sdw20217010e00";\n'
        '\t\t\t\treg = <0x00 0x03>;\n'
        '\t\t\t\tqcom,tx-port-mapping = <0x02 0x02 0x04 0x05>;\n'
        '\t\t\t\tphandle = <0x252>;\n'
        '\t\t\t};\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\tcodec@6d44000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-lpass-va-macro", "qcom,sm8550-lpass-va-macro";\n'
        '\t\t\treg = <0x00 0x6d44000 0x00 0x1000>;\n'
        '\t\t\tclocks = <0xb4 0x39 0x01 0xb4 0x66 0x01 0xb4 0x67 0x01>;\n'
        '\t\t\tclock-names = "mclk", "macro", "dcodec";\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "fsgen";\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tphandle = <0xb5>;\n'
        '\t\t};',
        '\t\tcodec@6d44000 {\n'
        '\t\t\tcompatible = "qcom,sm8650-lpass-va-macro", "qcom,sm8550-lpass-va-macro";\n'
        '\t\t\treg = <0x00 0x6d44000 0x00 0x1000>;\n'
        '\t\t\tclocks = <0xb4 0x39 0x01 0xb4 0x66 0x01 0xb4 0x67 0x01>;\n'
        '\t\t\tclock-names = "mclk", "macro", "dcodec";\n'
        '\t\t\t#clock-cells = <0x00>;\n'
        '\t\t\tclock-output-names = "fsgen";\n'
        '\t\t\t#sound-dai-cells = <0x01>;\n'
        '\t\t\tqcom,dmic-sample-rate = <0x249f00>;\n'
        '\t\t\tphandle = <0xb5>;\n'
        '\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\tpmic-glink {\n'
        '\t\tcompatible = "qcom,sm8650-pmic-glink", "qcom,pmic-glink";\n'
        '\t};\n'
        '};',
        '\tpmic-glink {\n'
        '\t\tcompatible = "qcom,sm8650-pmic-glink", "qcom,pmic-glink";\n'
        '\t};\n\n'
        '\tspdif-out {\n'
        '\t\tcompatible = "linux,spdif-dit";\n'
        '\t\t#sound-dai-cells = <0x00>;\n'
        '\t\tphandle = <0x250>;\n'
        '\t};\n\n'
        '\twcd939x-codec {\n'
        '\t\tcompatible = "qcom,wcd9395-codec", "qcom,wcd9390-codec";\n'
        '\t\tpinctrl-names = "default";\n'
        '\t\tpinctrl-0 = <0x255>;\n'
        '\t\treset-gpios = <0x99 0x6b 0x01>;\n'
        '\t\tvdd-rxtx-supply = <0xec>;\n'
        '\t\tvdd-io-supply = <0xec>;\n'
        '\t\tvdd-buck-supply = <0xec>;\n'
        '\t\tvdd-px-supply = <0x212>;\n'
        '\t\tvdd-mic-bias-supply = <0x100>;\n'
        '\t\tqcom,micbias1-microvolt = <0x2625a0>;\n'
        '\t\tqcom,micbias2-microvolt = <0x2932e0>;\n'
        '\t\tqcom,micbias3-microvolt = <0x2625a0>;\n'
        '\t\tqcom,micbias4-microvolt = <0x2625a0>;\n'
        '\t\tqcom,mbhc-buttons-vthreshold-microvolt = <0x124f8 0x249f0 0x39dc8 0x7a120 0x7a120 0x7a120 0x7a120 0x7a120>;\n'
        '\t\tqcom,mbhc-headset-vthreshold-microvolt = <0x19f0a0>;\n'
        '\t\tqcom,mbhc-headphone-vthreshold-microvolt = <0xc350>;\n'
        '\t\tqcom,ground-jack-type-normally-closed;\n'
        '\t\tqcom,rx-device = <0x251>;\n'
        '\t\tqcom,tx-device = <0x252>;\n'
        '\t\t#sound-dai-cells = <0x01>;\n'
        '\t\tphandle = <0x253>;\n'
        '\t};\n\n'
        '\tsound {\n'
        '\t\tcompatible = "qcom,sm8650-sndcard", "qcom,sm8450-sndcard";\n'
        '\t\tmodel = "SM8650-Nubia-NX769J";\n'
        '\t\tpinctrl-names = "default";\n'
        '\t\tpinctrl-0 = <0x254>;\n'
        '\t\taudio-routing = "IN1_HPHL", "HPHL_OUT",\n'
        '\t\t                "IN2_HPHR", "HPHR_OUT",\n'
        '\t\t                "AMIC1", "MIC BIAS1",\n'
        '\t\t                "AMIC2", "MIC BIAS2",\n'
        '\t\t                "AMIC3", "MIC BIAS3",\n'
        '\t\t                "AMIC4", "MIC BIAS3",\n'
        '\t\t                "AMIC5", "MIC BIAS4",\n'
        '\t\t                "TX SWR_INPUT0", "ADC1_OUTPUT",\n'
        '\t\t                "TX SWR_INPUT1", "ADC2_OUTPUT",\n'
        '\t\t                "TX SWR_INPUT2", "ADC3_OUTPUT",\n'
        '\t\t                "TX SWR_INPUT3", "ADC4_OUTPUT";\n\n'
        '\t\tprimary-mi2s-playback-dai-link {\n'
        '\t\t\tlink-name = "Primary MI2S Playback";\n\n'
        '\t\t\tcpu {\n'
        '\t\t\t\tsound-dai = <0x257 0x10>;\n'
        '\t\t\t};\n\n'
        '\t\t\tcodec {\n'
        '\t\t\t\tsound-dai = <0x250>;\n'
        '\t\t\t};\n\n'
        '\t\t\tplatform {\n'
        '\t\t\t\tsound-dai = <0x256>;\n'
        '\t\t\t};\n'
        '\t\t};\n\n'
        '\t\twcd-playback-dai-link {\n'
        '\t\t\tlink-name = "WCD Playback";\n\n'
        '\t\t\tcpu {\n'
        '\t\t\t\tsound-dai = <0x257 0x71>;\n'
        '\t\t\t};\n\n'
        '\t\t\tcodec {\n'
        '\t\t\t\tsound-dai = <0x253 0x00 0x258 0x00 0xb8 0x00>;\n'
        '\t\t\t};\n\n'
        '\t\t\tplatform {\n'
        '\t\t\t\tsound-dai = <0x256>;\n'
        '\t\t\t};\n'
        '\t\t};\n\n'
        '\t\twcd-capture-dai-link {\n'
        '\t\t\tlink-name = "WCD Capture";\n\n'
        '\t\t\tcpu {\n'
        '\t\t\t\tsound-dai = <0x257 0x78>;\n'
        '\t\t\t};\n\n'
        '\t\t\tcodec {\n'
        '\t\t\t\tsound-dai = <0x253 0x01 0x259 0x00 0xbc 0x00>;\n'
        '\t\t\t};\n\n'
        '\t\t\tplatform {\n'
        '\t\t\t\tsound-dai = <0x256>;\n'
        '\t\t\t};\n'
        '\t\t};\n'
        '\t};\n'
        '};',
    )

    # 2p) Enable NX769J Hardware Volume Up Key (PM8550 spmi@1 gpio@8800 gpio6 active-low pull-up -> KEY_VOLUMEUP=115)
    #     Uses gpio-keys-polled (20ms SPMI read_cmd poll via gpio_keys_polled.ko) so KEY_VOLUMEUP fires reliably even
    #     without PDC/SPMI periph_irq, while nx769j-fb-wl auto-skips direct gpiochip0 grab when evdev advertises KEY_VOLUMEUP.
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\tgpio@8800 {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-gpio", "qcom,spmi-gpio";',
        '\t\t\t\tpm8550_gpios: gpio@8800 {\n'
        '\t\t\t\t\tcompatible = "qcom,pm8550-gpio", "qcom,spmi-gpio";',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\t\t\t\tqcom,drive-strength = <0x03>;\n'
        '\t\t\t\t\t\tphandle = <0x4601>;\n'
        '\t\t\t\t\t};\n'
        '\t\t\t\t};',
        '\t\t\t\t\t\tqcom,drive-strength = <0x03>;\n'
        '\t\t\t\t\t\tphandle = <0x4601>;\n'
        '\t\t\t\t\t};\n\n'
        '\t\t\t\t\tkey_vol_up_default: key-vol-up-default {\n'
        '\t\t\t\t\t\tpins = "gpio6";\n'
        '\t\t\t\t\t\tfunction = "normal";\n'
        '\t\t\t\t\t\tinput-enable;\n'
        '\t\t\t\t\t\tbias-pull-up;\n'
        '\t\t\t\t\t\tpower-source = <0x01>;\n'
        '\t\t\t\t\t\tphandle = <0x25a>;\n'
        '\t\t\t\t\t};\n'
        '\t\t\t\t};',
    )
    linux_dts = replace_exact_once(
        linux_dts,
        '\t\t\tplatform {\n'
        '\t\t\t\tsound-dai = <0x256>;\n'
        '\t\t\t};\n'
        '\t\t};\n'
        '\t};\n'
        '};',
        '\t\t\tplatform {\n'
        '\t\t\t\tsound-dai = <0x256>;\n'
        '\t\t\t};\n'
        '\t\t};\n'
        '\t};\n\n'
        '\tgpio-keys {\n'
        '\t\tcompatible = "gpio-keys-polled";\n'
        '\t\tpoll-interval = <0x14>;\n'
        '\t\tpinctrl-names = "default";\n'
        '\t\tpinctrl-0 = <&key_vol_up_default>;\n\n'
        '\t\tvol-up {\n'
        '\t\t\tlabel = "volume_up";\n'
        '\t\t\tgpios = <&pm8550_gpios 0x06 0x01>;\n'
        '\t\t\tlinux,input-type = <0x01>;\n'
        '\t\t\tlinux,code = <0x73>;\n'
        '\t\t\tdebounce-interval = <0x0f>;\n'
        '\t\t};\n'
        '\t};\n'
        '};',
    )
    return linux_dts


def patch_dtb_file(
    in_dtb: str,
    out_dtb: str,
    out_dts: str | None = None,
    orig_dts: str | None = None,
    bt_mac: str = "02:00:00:76:9a:01",
    pad_size: int = 163840,
    dtc_bin: str | None = None,
) -> bytes:
    dtc = find_dtc(dtc_bin)
    with tempfile.TemporaryDirectory(prefix="nx769j_dtb_") as tmpdir:
        orig_path = orig_dts or os.path.join(tmpdir, "nx769j-orig.dts")
        patched_path = out_dts or os.path.join(tmpdir, "nx769j-linux.dts")
        subprocess.check_call([dtc, "-q", "-I", "dtb", "-O", "dts", "-o", orig_path, in_dtb])
        dts_in = open(orig_path, "r", encoding="utf-8").read()
        dts_out = patch_linux_dts(dts_in, bt_mac=bt_mac)
        with open(patched_path, "w", encoding="utf-8") as f:
            f.write(dts_out)
        cmd = [dtc, "-q", "-I", "dts", "-O", "dtb"]
        if pad_size > 0:
            cmd += ["-S", str(pad_size)]
        cmd += ["-o", out_dtb, patched_path]
        subprocess.check_call(cmd)
    return open(out_dtb, "rb").read()


def main() -> int:
    ap = argparse.ArgumentParser(description="Patch base NX769J Linux DTB with full v23 hardware enablement")
    ap.add_argument("--in", dest="in_dtb", required=True, help="Input base nx769j.dtb path")
    ap.add_argument("--out", dest="out_dtb", required=True, help="Output patched nx769j-linux.dtb path")
    ap.add_argument("--out-dts", help="Optional path to save patched DTS")
    ap.add_argument("--bt-mac", default=os.environ.get("NX769J_BT_MAC", "02:00:00:76:9a:01"),
                    help="Bluetooth MAC address AA:BB:CC:DD:EE:FF (default: 02:00:00:76:9a:01)")
    ap.add_argument("--dtc", help="Explicit dtc binary path")
    args = ap.parse_args()
    dtb_bytes = patch_dtb_file(
        args.in_dtb, args.out_dtb, out_dts=args.out_dts, bt_mac=args.bt_mac, dtc_bin=args.dtc
    )
    print(f"Patched DTB written to {args.out_dtb} ({len(dtb_bytes)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
