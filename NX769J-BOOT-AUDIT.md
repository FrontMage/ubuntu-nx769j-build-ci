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
| `vendor_boot.img` | 1142784 | `4aba3237e95c005e947c76a527ef944aa5f76d61470c6bbf9bdbfe50c4882f02` |
| `initramfs.cpio.gz` | 1005031 | `848a7e963650dac5045b07a6210b84071567354b73c525a6214ec90fa5de56f8` |

All three images round-tripped through upstream `unpack_bootimg.py`;
kernel, DTB and ramdisk payloads compare byte-identical to their inputs.
Tools: AOSP `mkbootimg.py`/`unpack_bootimg.py` (android14-release) and
Ubuntu `busybox-static` 1.36.1-6ubuntu3.1 (arm64).

## Device backup taken 2026-11-11 (private, local only)

With the owner's confirmation, 20 boot-chain partitions were streamed
read-only (`dd` over Magisk root) to a private local directory outside
this repository: both slots of `boot`, `init_boot`, `vendor_boot`,
`dtbo`, `vbmeta`, `vbmeta_system`, `recovery`, `abl`, plus `persist`,
`modemst1/2` and `fsg` (about 722 MiB, every size and SHA-256 verified).
`super` and `userdata` were deliberately excluded. Measured against
`NX769J-STOCK-OTA-AUDIT.md`, the active-slot `boot_b` and
`vendor_boot_b` hashes match the stock OTA exactly (unmodified), while
`init_boot_b` carries Magisk markers in its ramdisk — consistent with
the owner's earlier VM/root work. That modification does not affect
this Linux candidate (it replaces `init_boot` wholesale), and the
backup preserves the exact current state for Android restoration.
The candidate's vendor load offsets were also aligned to the dumped
stock `vendor_boot_b` header (base 0; kernel `0x8000`, ramdisk
`0x01000000`, tags `0x100`, DTB `0x1f00000`).

## First device attempt 2026-11-11 (failed safe, fully recovered)

With the owner's explicit confirmation, `boot-candidate-04` was flashed
to the inactive slot A only (`boot_a`, `init_boot_a`, `vendor_boot_a`),
slot A was set active and the device rebooted. The candidate kernel
crashed early into Qualcomm memory-dump mode (`19d2:0112 QUSB_BULK`)
and never reached the initramfs: no USB gadget and no fastboot were
observed, and volume-key fastboot entry did not work from that state.
A host-issued Sahara reset with both volume keys held moved the device
to true EDL (`05c6:9008`). The community SM8650 v1 firehose
(`prog_ufs_firehose_sm8650_v1_ddr.elf`, SHA-256
`01e4f38fd9ffff2056baffde8123ebe966feb6279f2be71bf831729ba66954d9`)
read the GPT, set the active slot back to B, and stock Android booted
(`boot_completed=1`, root intact). Slot A was then restored
byte-identically from the local backup (post-restore hashes match the
backup manifest). The early-crash cause is still unknown: the RAM
console region was listed (52 minidump regions, about 4.4 MiB) but not
captured before recovery, and no pstore records survived.

## OpenABL slot-A attempt and later recovery on 2026-11-11

At the owner's request, the next controlled experiment replaced only the
inactive slot-A bootloader with OpenABL v1.2 for SM8650 and populated
`boot_a` with a FAT/LPS Type-1 boot entry containing the already-built
NX769J initramfs, the candidate DTB and the verified SteamOS SM8650 kernel
payload. All of slot B and the remainder of the boot chain stayed in place.
After switching to slot A, the handset went back to Qualcomm memory-dump
mode and produced neither an OpenABL menu nor its normal fastboot. A
subsequent read of the small Sahara minidump was attempted but lost the
USB session and did not produce usable boot logs. The now-proven recovery
path—Sahara reset with both volume keys held, entry to `05c6:9008`,
firehose `setactiveslot b`, and restoration of slot A from this backup—again
returned the handset to Android. Therefore, the stock ZTE-signed ABL and the
A/B Android images are reinstated. This outcome establishes that a standard
EDL recovery path exists for this unit, but it does not distinguish yet
between OpenABL's test signing being rejected, an NX769J-specific OpenABL
configuration/handoff incompatibility, or another platform-init failure.
The next attempt must capture and inspect `RAMCON.BIN`, `md_XBL_LOG.BIN`,
`md_UEFI_LOG.BIN`, and the reset-status regions before issuing any reset.
Do not repeat an ABL replacement until those records or a device-specific
OpenABL build explain the hand-off failure.

## Remaining gates before any further device write

- Capture and analyze the RAM console on the next failure (the Sahara
  memory-debug listing path is proven; use it before recovery resets).
- Decide rootfs placement (no partition is assigned today) and whether
  Android is retained, before any partition table or `super` change.
- Observe how this bootloader merges vendor cmdline, selects DTB
  (vendor_boot DTB vs `dtbo`) and orders init_boot/vendor ramdisks.
- Display, touch, GPU, Wi-Fi, modem and audio are disabled in the
  candidate DTB; UFS wiring and reserved-memory handoff are unproven,
  so a first boot would be blind and could fail before any console.
