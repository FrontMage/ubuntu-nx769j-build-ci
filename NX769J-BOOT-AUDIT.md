# NX769J boot-chain audit and first offline boot candidate

Status: **audit + offline assembly only, 2026-11-11**. Nothing in this
document qualifies the candidate for flashing. No partition was written,
no reboot was issued, and no root shell was used. Readings below come from
ordinary ADB `getprop`/`ls` on the intended test device and from the cited
public sources.

## Observed device state (read-only)

- Active slot is `_b`; stock kernel is `6.1.68-android14-11`.
- The bootloader is **already unlocked**: `ro.boot.flash.locked=0`,
  `ro.boot.vbmeta.device_state=unlocked`,
  `ro.boot.verifiedbootstate=orange`, `sys.oem_unlock_allowed=1`.
  Magisk (`com.topjohnwu.magisk`) is installed, so the active boot chain
  (most likely `init_boot_b`, which root guides patch on this device)
  must be assumed patched until its partitions are dumped and hashed.
- Partitions present include `boot_a/b`, `init_boot_a/b`,
  `vendor_boot_a/b`, `dtbo_a/b`, `vbmeta_a/b`, `vbmeta_system_a/b`,
  `recovery_a/b`, `super`, `userdata` and the usual SM8650 firmware set.
  `userdata` reports about 460 GiB total with about 337 GiB free.
- On NX769J the community unlock path writes an engineering ABL over
  EDL/QFIL before `fastboot flashing unlock` is accepted (see sources
  below). Whether this unit carries that engineering ABL, and whether
  its fastboot has further critical-partition restrictions, has **not**
  been verified on the device.

## Recovery paths (existence only, not authenticated)

- A public NX769J "Global EDL ROM" with QFIL XML exists and multiple
  users report unbricking with it; a matching SM8650 firehose ELF
  (`prog_ufs_firehose_sm8650_v1_ddr.elf`) is referenced by the unlock
  guides. No package was downloaded or hash-authenticated in this audit.
- Sources: [XDA bootloader-unlock guide](https://xdaforums.com/t/redmagic-9-pro-bootloader-unlock.4667624/),
  [XDA NX769J Global EDL ROM](https://xdaforums.com/t/redmagic-9-pro-9-pro-plus-nx769j-9-0-13-global-edl-rom.4667740/),
  [DroidWin Nubia unlock guide](https://droidwin.com/how-to-unlock-bootloader-on-any-nubia-device/).

## First offline candidate (built and read back locally)

`scripts/nx769j/pack-boot-candidate.sh` packs the verified CI kernel
bundle (build commit `e803698`) with a minimal busybox initramfs
(`make-initramfs.py`, deterministic; mounts the candidate rootfs by UUID
and `switch_root`s, shell on failure) into header-v4 images using the
stock 4096-byte vendor page size:

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| `boot.img` | 59281408 | `a9697a7a0f07d54557502a9e57bf253c48423238e6f0682b3898457d63ef44e7` |
| `init_boot.img` | 1011712 | `1d702572639866f092cf7a99d6d52799055beffcfa3fbd2c991d5289a6c81c22` |
| `vendor_boot.img` | 1142784 | `352cd84fd4ac9bd3555ae4096e62224ae6fd5afabe1b174cf5ed595ef536ce93` |
| `initramfs.cpio.gz` | 1005031 | `848a7e963650dac5045b07a6210b84071567354b73c525a6214ec90fa5de56f8` |

All three images round-tripped through upstream `unpack_bootimg.py`;
kernel, DTB and ramdisk payloads compare byte-identical to their inputs.
Tools: AOSP `mkbootimg.py`/`unpack_bootimg.py` (android14-release) and
Ubuntu `busybox-static` 1.36.1-6ubuntu3.1 (arm64).

## Remaining gates before any device write

- Dump and hash the active-slot `boot`, `init_boot`, `vendor_boot`,
  `dtbo`, `vbmeta*` (needs root or a proven read path) and derive a
  restorable stock state; download and authenticate an EDL package.
- Decide rootfs placement (no partition is assigned today) and whether
  Android is retained, before any partition table or `super` change.
- Observe how this bootloader merges vendor cmdline, selects DTB
  (vendor_boot DTB vs `dtbo`) and orders init_boot/vendor ramdisks.
- Display, touch, GPU, Wi-Fi, modem and audio are disabled in the
  candidate DTB; UFS wiring and reserved-memory handoff are unproven,
  so a first boot would be blind and could fail before any console.
