# Flashing, Installation & Zero-Brick Dual-Boot Guide

> **Safety warning:** Read every safety invariant before running any command on your phone.
> Never modify `*_b` partitions until you have full stock partition backups safely stored off-device.

---

## Safety & Architecture Invariants

1. **Always pass `-s <SERIAL>`** to `adb` and `fastboot` if more than one device is connected.
2. **Keep Android intact on Slot B (`*_b`)** — dual-boot Linux only ever flashes `vbmeta_a` and `boot_a`. Never flash `super`, `userdata`, `persist`, `abl_*`, `xbl_*`, or any `*_b` partition.
3. **Never force-format `/data`**: Omarchy Mobile lives in F2FS-pinned extent files inside `/data/nxlinux/` (`rootfs.img` + optional `rootfs_ext2.img`) mounted via `dm-linear` (`/dev/nxroot`).
4. **Always test new boot images in RAM first** using:
   ```bash
   fastboot -s <SERIAL> boot uboot-omarchy-nx769j-boot.img
   ```
5. **Do not probe `i2c@98c000` address `0x6a`**: addresses `0x34` / `0x35` are the `AW88261` SmartPAs, whereas `0x6a` is the fan RGB LED controller.

---

## Step 0 — Prerequisites & Backups

### Unlock Bootloader & Root Stock Android
- Enable OEM Unlocking & USB Debugging in Android Developer Options (`adb` unlocked bootloader required).
- Obtain Magisk root (`su`) on stock Android (Slot B).

### Back Up Critical Stock Partitions (Mandatory!)
Using root ADB or Qualcomm EDL (`edl.py` / QFIL), back up at minimum:
- `boot_a`, `boot_b`, `init_boot_a`, `init_boot_b`, `vendor_boot_a`, `vendor_boot_b`, `dtbo_a`, `dtbo_b`, `vbmeta_a`, `vbmeta_b`, `abl_a`, `abl_b`
- Device-unique calibration partitions: `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `devinfo`

Save at least `vbmeta_a.img` and `boot_a.img` safely on your computer for instant one-command rollback.

---

## Step 1 — Host Tooling & Artifacts

On your macOS or Linux host:

```bash
# Toolchain requirements: python3, dtc (device-tree-compiler), zstd, clang or aarch64-linux-gnu-gcc/as, adb, fastboot

# 1. Fetch & SHA-256 verify mainline u-boot-qcom (v2026.10-rc5)
python3 boot/fetch_uboot.py

# 2. Cross-compile static AArch64 initrd/dual-boot helpers (Linux or macOS with aarch64-linux-gnu-gcc)
python3 storage-dualboot/stage_dualboot_tools.py --compile-only --out-dir storage-dualboot/build
```

Obtain or build the Linux `7.1.1-nx769j+` kernel bundle (`Image`, `nx769j.dtb`, `modules.tar.zst`) using `kernel/scripts/build-kernel.sh` (see [kernel/README.md](../kernel/README.md)).

---

## Step 2 — Allocate Pinned Linux Storage on `/data/nxlinux/`

With the phone booted into **rooted stock Android** (`adb`):

```bash
# Preview allocation (offline --dry-run by default)
python3 storage-dualboot/expand_nxroot_256g.py \
  --mode create \
  --size-gib 100 \
  --min-free-gib 45 \
  --nx-dm-tool storage-dualboot/build/nx_dm_tool \
  --slotctl storage-dualboot/build/nx769j-slotctl \
  --out-extents rootfs.extents

# Execute live allocation on device over ADB
python3 storage-dualboot/expand_nxroot_256g.py \
  --mode create \
  --size-gib 100 \
  --min-free-gib 45 \
  --nx-dm-tool storage-dualboot/build/nx_dm_tool \
  --slotctl storage-dualboot/build/nx769j-slotctl \
  --out-extents rootfs.extents \
  --execute
```

Format `/data/nxlinux/rootfs.img` as `ext4` on the device (`adb shell su -c "mkfs.ext4 -F /data/nxlinux/rootfs.img"`) or stage your initial ext4 image into it before pinning.

*(Optional later upgrade: expand losslessly from `100 GiB` -> `256 GiB` anytime using `--mode expand --base-gib 100 --add-gib 156 --base-extents rootfs.extents --out-extents rootfs_256g.extents --execute`. On next boot, initrd automatically runs `e2fsck` + `resize2fs` offline before pivot.)*

---

## Step 3 — Build & RAM-Boot the U-Boot + Linux Boot Image

```bash
python3 boot/build_boot_img.py \
  --image /path/to/Image \
  --dtb /path/to/nx769j.dtb \
  --modules /path/to/modules.tar.zst \
  --extents rootfs.extents \
  --nx-dm-tool storage-dualboot/build/nx_dm_tool \
  --ssh-pubkey-file ~/.ssh/id_ed25519.pub \
  -o uboot-omarchy-nx769j-boot.img
```

Reboot the phone into bootloader (`adb -s <SERIAL> reboot bootloader`) and test **pure RAM boot** (does not alter flash):

```bash
fastboot -s <SERIAL> boot uboot-omarchy-nx769j-boot.img
```

Once booted, connect over USB Ethernet (`usb0`):

```bash
bash storage-dualboot/ssh_nx769j.sh
```

---

## Step 4 — Extract Proprietary Firmware (Read-Only from Phone Partitions)

Inside the running Linux shell on the phone (as `root`), run the zero-mount extractors:

```bash
# Adreno 750 GPU (SQE/AQE/ZAP/GMU) from super (vendor_b)
sudo python3 firmware-extractors/stage_gpu_fw.py

# ADSP / ChargerPD battery manager from modem_b
sudo python3 firmware-extractors/stage_adsp_fw.py

# ADSP Sensors PD / 6-axis IMU calibration & .jsn files from super + dsp_b + persist
sudo python3 firmware-extractors/stage_gyro_fw.py
```

*(Note: Because kernel `CONFIG_FW_LOADER_COMPRESS` is disabled, firmware under `/usr/lib/firmware` must remain **uncompressed**.)*

---

## Step 5 — Stage & Pivot Into Arch Linux ARM + Omarchy Mobile

Run inside the phone's Linux environment as root:

```bash
# 1. Stage Arch Linux ARM base + NX769J C/Python drivers + modules + overlay into /arch-stage
sudo bash rootfs/bootstrap_arch_stage.sh --stage-root /arch-stage --ssh-pubkey-file /root/.ssh/authorized_keys

# 2. Install Omarchy Mobile desktop + Hyprland + Mesa Turnip + patched Aquamarine 0.16.0
sudo bash rootfs/install_omarchy_ui.sh --stage-root /arch-stage

# 3. Verify every binary, symlink, firmware file, and systemd unit before touching /
sudo python3 rootfs/verify_arch_stage.py --stage-root /arch-stage --require-ui

# 4. Arm atomic pivot (moves old rootfs to /old-kubuntu and promotes /arch-stage -> / in initrd!)
sudo python3 rootfs/atomic_pivot_to_arch.py --arm --require-ui
sudo sync && sudo reboot
```

After rebooting into Omarchy Mobile and verifying everything works (optional later cleanup):
- **Reclaim old rootfs space**: `sudo python3 rootfs/atomic_pivot_to_arch.py --cleanup-old-kubuntu`
- **Emergency rollback (if ever needed)**: `sudo python3 rootfs/atomic_pivot_to_arch.py --arm-rollback && sudo reboot`

---

## Step 6 — Permanent Slot-A Dual-Boot Flash & Everyday Switching

Once RAM-booting (`fastboot boot ...`) is 100% verified, flash **Slot A only** while preserving Android on **Slot B**:

```bash
# Dry-run inspection first
python3 storage-dualboot/flash_slot_a.py \
  --serial <SERIAL> \
  --stock-vbmeta-a /path/to/backup/vbmeta_a.img \
  --boot-img uboot-omarchy-nx769j-boot.img

# Flash vbmeta_a (flags=2) + boot_a + set active slot _a (Slot _b untouched!)
python3 storage-dualboot/flash_slot_a.py \
  --serial <SERIAL> \
  --stock-vbmeta-a /path/to/backup/vbmeta_a.img \
  --boot-img uboot-omarchy-nx769j-boot.img \
  --execute --reboot
```

Install the in-OS dual-boot switcher helpers:

```bash
python3 storage-dualboot/stage_dualboot_tools.py --out-dir storage-dualboot/build
```

### Switching Between OSes
- **Omarchy Mobile -> Android**: run `reboot-to-android` (switches GPT/UFS active LUN back to `_b` and reboots).
- **Android -> Omarchy Mobile**: run `su -c /data/nxlinux/reboot-to-omarchy.sh` (installed automatically by `expand_nxroot_256g.py` + Magisk `service.d` helper `/sdcard/Reboot-to-Omarchy.sh`).

### Emergency Rollback to Stock Android Only
If anything goes wrong after flashing Slot A:
```bash
fastboot -s <SERIAL> flash vbmeta_a /path/to/backup/vbmeta_a.img
fastboot -s <SERIAL> flash boot_a /path/to/backup/boot_a.img
fastboot -s <SERIAL> --set-active=b
fastboot -s <SERIAL> reboot
```
*(Or hold VolUp + VolDown on USB cable insert for Qualcomm EDL `9008` recovery.)*
