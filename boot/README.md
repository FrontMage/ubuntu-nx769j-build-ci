# Boot Chain: BootShim + Patched Mainline U-Boot + Initramfs Overlay

Because Qualcomm's stock ABL on RedMagic 9 Pro (`NX769J`) restricts direct Linux EFI/DTB parameter passing and leaves early serial/PMU quirks when booting mainline kernels directly via Android `boot.img`, this project boots Mainline Linux cleanly through a self-contained **BootShim + U-Boot (`v2026.10-rc5`, `qcm6490`)** stage packed into a standard Android header v4 `boot.img`.

## Memory & Payload Layout (64 MiB Image)

| Payload Offset | DRAM Stage Address | Contents |
|---|---|---|
| `0x000000` .. `0x001fff` | In-place | 8 KiB AArch64 BootShim + `VBAR_EL1` early exception handler + framebuffer HUD |
| `0x002000` .. `0x1fffff` | In-place | Patched `u-boot-qcom` (`0x15cd50` B) + `nx769j-uboot.dtb` (`1116x2480` simplefb) |
| `0x200000` .. `0x37ffff` | `0xaf800000` | `initrd.cpio.gz` (USB NCM, `nx_dm_tool`, `dm-mod.ko`, `pwrseq-qcom-wcn.ko`, `.extents`) |
| `0x380000` .. `0x3fffff` | `0xafe00000` | Patched mainline `nx769j-linux.dtb` (160 KiB padded) |
| `0x400000` .. `0x3ffffff` | `0xb0000000` | Mainline Linux `Image` (`bootefi 0xb0000000:... 0xaf800000:... 0xafe00000`) |

## Quick Usage

1. Fetch & verify the pinned Debian `u-boot-qcom` binary:

```bash
python3 boot/fetch_uboot.py
```

2. Build a bootable Android `boot.img`:

```bash
python3 boot/build_boot_img.py \
  --image /path/to/Image-7.1.1 \
  --dtb /path/to/nx769j.dtb \
  --modules /path/to/modules.tar.zst \
  --extents /path/to/rootfs_256g.extents \
  --ssh-pubkey-file ~/.ssh/id_ed25519.pub \
  -o uboot-omarchy-nx769j-boot.img
```

3. Test in RAM first without modifying any flash partition:

```bash
fastboot -s <SERIAL> boot uboot-omarchy-nx769j-boot.img
```

Inside the U-Boot bootmenu (`1s` delay):
- `Volume Up` / `Power` navigates U-Boot menu entries.
- Entry 0 boots Omarchy Mobile automatically via EFI stub (`bootefi`).
- Entry 2 launches U-Boot fastboot gadget on USB.
