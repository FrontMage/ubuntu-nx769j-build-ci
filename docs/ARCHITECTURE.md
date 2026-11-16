# System Architecture

## 1. Two-Stage Boot Pipeline

```text
Qualcomm XBL/ABL (Slot A)
  │  Loads Android boot_a (header v4, pagesize 4096, 64 MiB kernel payload)
  ▼
BootShim (8 KiB @ payload+0x0000)
  │  - Initializes RAMCON + framebuffer cursor state at 0xd7e00000
  │  - Installs early VBAR_EL1 exception vector trap at +0x1000
  │  - Clears bottom HUD banner on UEFI simple-framebuffer (0xe3940000, 1116x2480)
  │  - Stages Linux Image (0xb0000000), patched DTB (0xafe00000), initrd (0xaf800000)
  │  - Jumps in-place to U-Boot (+0x2000) with x0 = nx769j-uboot.dtb
  ▼
Patched U-Boot (v2026.10-rc5, qcm6490)
  │  - Relocation table patched so blocked serial UART writes return immediately
  │  - vidconsole hooks mirror output onto bottom 1116x2480 framebuffer HUD
  │  - 1s bootmenu -> `bootefi 0xb0000000:... 0xaf800000:... 0xafe00000`
  ▼
Linux 7.1.1-nx769j+ & External Initrd Overlay
  │  - Starts USB ConfigFS NCM gadget (usb0 = 10.42.0.2) for early rescue SSH/telnet
  │  - Loads dm-mod.ko + pwrseq-qcom-wcn.ko
  │  - Waits for /dev/sda12 (UFS userdata), runs static `nx_dm_tool dm-up nxroot`
  │  - Performs atomic /arch-stage pivot or /old-kubuntu rollback if armed
  │  - Grows ext4 automatically (`e2fsck` + `resize2fs`) when dm-linear exceeds fs blocks
  │  - `exec switch_root /newroot /sbin/init` -> Arch Linux ARM systemd
```

---

## 2. Zero-Touch F2FS `dm-linear` Storage Design

Rather than resizing UFS GPT partitions (which breaks OTA / risks userdata loss):
1. Rooted Android calls `nx_dm_tool pin-alloc` (`FS_IOC_SET_PIN_FILE = 1` + `fallocate`) on `/data/nxlinux/rootfs.img` (and optional `rootfs_ext2.img` for `+156 GiB -> 256 GiB`).
2. Because F2FS pins those extents in-place (`no_gc` / no block migration), `FS_IOC_FIEMAP` returns a fixed table of `(logical_sector, length_sectors, physical_sector_on_sda12)`.
3. The initrd passes `/etc/rootfs.extents` to `nx_dm_tool dm-up nxroot /dev/sda12 /etc/rootfs.extents`, which constructs an identical `dm-linear` target via `/dev/mapper/control`.
4. Android F2FS never sees writes to `/dev/nxroot` when booted in Linux, and never touches pinned blocks when booted in Android.

---

## 3. Split Rendering Architecture (Adreno 750 + SimpleFB)

Because `msm.separate_gpu_kms=1` decouples GPU rendering (`/dev/dri/renderD128`) from display scanout (`/dev/fb0` SimpleFB):
- **Hyprland** (`omamobile-shell`) renders hardware-accelerated UI and Chromium apps on Adreno 750 (`Turnip` Vulkan / `Freedreno` GL) into a headless Wayland backend (`nx769j-fb-wl` socket `/run/nx769j-wl/wayland-fb0`).
- **`nx769j-fb-wl`** receives committed Wayland client buffers (DMA-BUF synced with `DMA_BUF_IOCTL_SYNC`), blits them cleanly to `1116x2480` ARGB8888 `/dev/fb0`, and feeds real-time 10-slot multitouch + keyboard + powerkey blanking events back into the compositor.
- **Volume/Power hardware integration** bypasses slow daemon chains cleanly so hardware keys work seamlessly in UI without hanging suspend states.
