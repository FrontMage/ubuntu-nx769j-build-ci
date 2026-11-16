# RedMagic 9 Pro (NX769J / SM8650) — Omarchy Mobile Linux

Full mainline **Linux 7.1.1** + **Arch Linux ARM** + **Omarchy Mobile** (`Hyprland` + `omamobile-shell`) hardware enablement, dual-boot tooling, fixed userspace/DT drivers, and zero-brick installation workflow for the **Nubia RedMagic 9 Pro (`NX769J`, Qualcomm Snapdragon 8 Gen 3 / `SM8650`)**.

Runs completely alongside stock Android using **F2FS-pinned `dm-linear` extents on `/data/nxlinux/`** + **Slot A (`vbmeta_a` + `boot_a`)** while leaving Android on **Slot B (`*_b`)** untouched.

---

## Hardware Support Matrix

| Subsystem | Status | Implementation Highlights |
|---|---|---|
| **Bootloader / Early Bringup** | Working | 8 KiB AArch64 BootShim + patched mainline `u-boot-qcom` (`v2026.10-rc5`, `1116x2480` simplefb HUD, stubbed serial relocs, `bootefi`) |
| **Display & Touch** | Working | Zero-copy/low-overhead `simple-framebuffer` -> Wayland compositor bridge (`nx769j_fb_wl`), `aquamarine 0.16.0` software Wayland touch patch, Goodix `gt9916` 10-slot multitouch, `oled-backlight` PWM |
| **Adreno 750 GPU** | Working | Mainline `msm.ko` (`separate_gpu_kms=1`) + Mesa Turnip (`vulkan-freedreno`) + Zink + extracted `gen70900` SQE/AQE/ZAP/GMU firmware (`stage_gpu_fw.py`) |
| **Physical Dual Speakers** | Working | Dual Awinic `AW88261` (`chip_id=0x2113` @ `0x34`/`0x35`, `i2c@98c000`) + `gpio128` (`sd1-pins`, `output-low`) TX I2S fix + patched `SM8650-Nubia-NX769J-tplg.bin` (`SD_LINE_IDX=2`) + zero-CPU `/dev/snd/timer` TREAD power daemon (`aw88261_init.py`) |
| **Battery & Charging** | Working | `qcom_battmgr` over ADSP ChargerPD (`remoteproc0`), percentage/current/temp/voltage reporting + `80%` hardware charge-limit daemon (`nx769j_charge_limit.py`) |
| **Wi-Fi 7 & Bluetooth** | Working | Qualcomm `WCN7850` (`ath12k` + `pwrseq-qcom-wcn` + `btqca`) with DT `local-bd-address` injection & 8 KiB WCN regulator fix |
| **Built-in Turbo Fan & Perf Modes** | Working | `pwm-fan` + DT thermal fan trip curve (`nx769j-fan.c`) + Eco / Balanced / Diablo-Turbo governor & CPU/fan controller (`nx769j_perf.c`) + QuickSettings QML toggle |
| **6-Axis IMU / Auto-Rotate** | Working | ADSP Sensors PD reverse-tunnel (`nx769j-hexagonrpcd` + `libhexagonrpc`) + FastRPC QRTR QSH daemon (`nx769j_imu_daemon.c`) -> CUSE evdev + `nx769j-sensor-proxy` |
| **Buttons & Input** | Working | `pmk8350` Power key short-press screen blank/standby & long-press shutdown (`nx769j_powerkey.c`), VolUp/VolDown hardware OSD (`sway` / `omamobile`), `nx769j_vkbd` |
| **Storage & Dual-Boot** | Working | Non-destructive F2FS `FS_IOC_SET_PIN_FILE` + `dm-linear` (`nx_dm_tool.c`, expandable to `256 GiB`) + UFS Boot-LUN A/B switcher (`nx769j_slotctl.c`, `reboot-to-android`) |

---

## Repository Layout

```text
nx769j-omarchy-linux/
├── boot/
│   ├── fetch_uboot.py              # Fetch & verify Debian u-boot-qcom (v2026.10-rc5)
│   ├── build_boot_img.py           # Assemble BootShim + U-Boot + initrd + DTB + Image -> boot.img
│   └── tools/                      # Apache-2.0 mkbootimg / unpack_bootimg
├── kernel/
│   ├── nx769j.config               # Kernel config fragment (Linux 7.1.1)
│   ├── dts/                        # Base DTS, v23 full DTS, hardware delta patch & DTB patcher
│   ├── patches/                    # Binding & Makefile patches for GUF296/linux
│   └── scripts/                    # Offline kernel bundle build & verification scripts
├── drivers/
│   ├── audio/                      # AW88261 zero-CPU daemon, AudioReach tplg builder, UCM2 profiles
│   ├── display/                    # nx769j_fb_wl, nx769j_vkbd, aquamarine touch patch
│   ├── sensors/                    # libhexagonrpc, hexagonrpcd, ADSP IMU daemon, sensor proxy
│   ├── thermal-fan/                # Cooling fan controller & 3-mode performance daemon
│   └── power/                      # Power key standby daemon & charge limiter
├── firmware-extractors/
│   ├── stage_adsp_fw.py            # Read-only FAT16 modem_b extractor (ADSP/ChargerPD)
│   ├── stage_gpu_fw.py             # Read-only super/vendor extractor (Adreno 750)
│   └── stage_gyro_fw.py            # Read-only super/dsp/persist extractor (IMU/Sensors PD)
├── storage-dualboot/
│   ├── nx_dm_tool.c                # Static nostdlib ARM64 F2FS pin-alloc + FIEMAP + dm-linear helper
│   ├── nx769j_slotctl.c            # Static nostdlib ARM64 GPT A/B + UFS boot LUN controller
│   ├── expand_nxroot_256g.py       # Zero-loss 100 GiB -> 256 GiB allocator & Android switcher installer
│   ├── flash_slot_a.py             # Strict Slot-A-only dual-boot fastboot flasher
│   ├── stage_dualboot_tools.py     # Compile & install static switchers over SSH
│   └── ssh_nx769j.sh               # USB RNDIS/NCM IPv6/IPv4 auto-discovery SSH wrapper
├── rootfs/
│   ├── bootstrap_arch_stage.sh     # Stage Arch Linux ARM + NX769J hardware stack cleanly
│   ├── install_omarchy_ui.sh       # Install Hyprland, omamobile-shell & patched aquamarine
│   ├── verify_arch_stage.py        # Pre-pivot integrity verifier
│   ├── atomic_pivot_to_arch.py     # Zero-handle initrd atomic rootfs swap & rollback controller
│   └── overlay/                    # Complete /etc, /usr/local, systemd, udev, WirePlumber, QML overlays
├── docs/
│   ├── FLASHING-AND-DUALBOOT.md    # End-to-end build, dual-boot, flashing & rollback guide
│   ├── HARDWARE-FIXES.md           # Deep-dive on every NX769J bug & root cause fixed
│   └── ARCHITECTURE.md             # Boot chain, F2FS dm-linear, headless GPU + SimpleFB design
└── tests/                          # Offline sanitization, compilation, and boot.img build tests
```

---

## Quick Start

See **[docs/FLASHING-AND-DUALBOOT.md](docs/FLASHING-AND-DUALBOOT.md)** for the full step-by-step guide covering:
1. Backing up stock partitions (`boot_a/b`, `vbmeta_a/b`, `init_boot`, `persist`, `modem`, `fsg/fsc`).
2. Building the kernel ([kernel/README.md](kernel/README.md)) and compiling static `nx_dm_tool` / `nx769j-slotctl`.
3. Allocating F2FS-pinned `/data/nxlinux/rootfs.img` without wiping Android userdata.
4. Building and RAM-boot testing (`fastboot -s <SERIAL> boot ...`) via [boot/build_boot_img.py](boot/build_boot_img.py).
5. Extracting proprietary firmware dynamically from the phone via [firmware-extractors/](firmware-extractors/).
6. Staging Arch Linux ARM + Omarchy Mobile (`bootstrap_arch_stage.sh` -> `install_omarchy_ui.sh` -> `atomic_pivot_to_arch.py`).
7. Flashing Slot A permanently (`vbmeta_a` + `boot_a`) with zero changes to Android Slot B.

---

## Credits & Upstream Projects

- Kernel base: [`GUF296/linux`](https://github.com/GUF296/linux) branch `TB321FU-7.1.1` (SM8650 mainline tree)
- Initial CI bringup heritage: [`GUF296/ubuntu-y700-build-ci`](https://github.com/GUF296/ubuntu-y700-build-ci) & [`FrontMage/ubuntu-nx769j-build-ci`](https://github.com/FrontMage/ubuntu-nx769j-build-ci) (Actions run `43067542050` kernel artifact)
- U-Boot: Mainline [`u-boot-qcom`](https://packages.debian.org/sid/u-boot-qcom) (`2026.10~rc5-1`)
- Desktop environment: [Omarchy Mobile](https://omamobile.org/) & [Hyprland / Aquamarine](https://hyprland.org/)
- FastRPC daemon library: [`libhexagonrpc`](drivers/sensors/libhexagonrpc/COPYING) (`GPL-3.0`)
