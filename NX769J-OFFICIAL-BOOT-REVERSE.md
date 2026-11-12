# NX769J official boot handoff reverse-engineering notes

Status: **read-only offline/device audit, 2026-11-11**. This document
compares the device tree Android actually received with the current
mainline candidate. It is not permission to flash anything.

## Inputs

- Private local device backup (`device-backup-20261111`, SHA-256 manifest,
  not in this repository).
- Runtime final FDT read from stock slot B (`/sys/firmware/fdt`):
  854,781 bytes, SHA-256
  `d7fc2009ede33667d87e0f292e26b25ad345982f5d7acb5323d1648a48ce7d43`.
- Current candidate kernel artifact and 125,672-byte DTB from CI run
  43017582194.

## Confirmed handoff differences

### Root identity and bootloader selection

| Field | Stock runtime FDT | Candidate |
|---|---|---|
| Model | `Qualcomm Technologies, Inc. Pineapple MTP` | `nubia REDMAGIC 9 Pro (NX769J, experimental)` |
| Compatible | `qcom,pineapple-mtp`, `qcom,pineapple`, `qcom,pineapplep-mtp`, `qcom,pineapplep`, `qcom,mtp` | `nubia,nx769j`, `qcom,sm8650` |
| `qcom,msm-id` | `<0x22d 0x20000 0x241 0x20000>` | absent |
| `qcom,board-id` | `<0x08 0x00>` | absent |
| `/chosen` | Full stock + vendor + ABL args, `linux,initrd-start/end`, zero `kaslr-seed`, UART stdout | no `bootargs`, no stdout path |

Stock `vendor_boot` contains six concatenated base FDTs and `dtbo`
contains 26 overlays. The runtime tree's root identity proves that the
bootloader's Qualcomm selection/overlay stage is part of the contract.
The exact behavior when those selector properties are absent is not yet
known. Stock overlays also fail a host-side `fdtoverlay` sanity check
against the candidate because the candidate has no `/__symbols__` node;
this is not by itself proof of what Qualcomm ufdt does on-device.

### Memory

- ABL rewrites stock `/memory` into many usable ranges and appends
  `mem=12089696256B`; kernel code is resident at `0xa8010000` in the
  running stock system.
- Candidate inherits mainline's placeholder
  `memory@a0000000` with length 0 and relies on this same bootloader
  fixup happening for a tree it did not select in the normal way.
- Almost every static firmware carveout in the candidate matches the
  runtime tree by physical range. The clear candidate omission is the
  43 MiB continuous-splash region at `0xe3940000`; dynamic vendor pools
  are also absent. This makes “completely wrong memory map” unlikely,
  while making the DT selection/fixup path more suspicious.

### Early console and persistent logs

- Stock final FDT points stdout to
  `/soc/qcom,qupv3_2_geni_se@8c0000/qcom,qup_uart@89c000`.
- The candidate has GENI UART nodes but no alias or `stdout-path`.
- Stock has `ramoops@0xd7e00000` (2 MiB, whole region used for
  console). Candidate-05 now mirrors that region and enables built-in
  pstore RAM/console. Its failure still left no candidate text in
  `RAMCON.BIN`, bounding the crash to before ramoops registration (or
  its probe) rather than localizing a normal device driver.

### Kernel image

| Property | Stock | Candidate-05 |
|---|---|---|
| Version | 6.1.68 Android GKI | 7.1.1-nx769j+ |
| File size | 34,699,776 | about 60 MB boot image |
| Header image_size | 0x21c0000 | 0x3960000 |
| Header flags/text offset | 0xa / 0 | 0xa / 0 |
| VA/PA bits | 39 / 48 | 52 / 52 + LPA2 |

The same 52-bit choices are present in a known-good SteamOS SM8650
kernel, so VA width alone is not a lead suspect. Candidate size and
the exact ABL load-window calculation remain worth one isolated test
only after the handoff identity is corrected.

### Bootconfig

Stock `vendor_boot` bootconfig is only Android metadata plus
`androidboot.hypervisor.protected_vm.supported=false` and
`androidboot.console=0`. No load-bearing mainline parameter was found
there; the important command-line differences are in final
`chosen/bootargs`, not this 198-byte blob.

## Single-variable experiment order

Do not combine these. Each requires a fresh write approval, an inactive
slot only, hash read-back, and dumps captured before any recovery reset.

1. **Identity-only DTB:** keep candidate kernel/initramfs unchanged;
   only set the DTB root model/compatible and
   `qcom,msm-id = <0x22d 0x20000 0x241 0x20000>`,
   `qcom,board-id = <8 0>`.
2. **Chosen-only DTB:** from the better of step 1/current, only add the
   stock UART alias/stdout and `earlycon` arguments so an early serial
   path exists if the pins or debug cable are usable.
3. **DTBO isolation:** only neutralize slot A's DTBO selection/input
   (backed-up partition) to determine whether a stock overlay is being
   applied to the candidate tree.
4. **Framebuffer-only:** only add the stock continuous-splash reserved
   region and a matching simple-framebuffer/earlycon configuration.
5. **Known-good kernel substitution:** only swap in the already
   extracted SteamOS SM8650 kernel while keeping the NX769J DTB and
   our initramfs, separating kernel code from DT/handoff data.
6. **Minimal DT:** only disable candidate-enabled UFS and USB/PHY nodes
   to separate early SoC handoff from storage/USB probing.

### Temporary Cerro-style v2 boot (2026-11-11)

A separate RAM-only experiment then reproduced the Nubia Cerro
`Scheme E` shape: candidate-05 kernel padded to its ARM64
`image_size`, the candidate DTB appended at that offset, the same DTB
also carried by an Android boot-image header v2, and the 96 MiB image
started with `fastboot boot` from the current slot-B ZTE engineering
ABL. No partition or slot was changed. The host reported download and
`Booting OKAY`, the handset stayed on the RedMagic logo, and the
fastboot USB session remained alive; a subsequent `fastboot continue`
returned `Failed to load image from partition: Load Error`, while
slot B still reported `successful=yes`. Reasserting slot B and issuing
a normal reboot returned Android (`boot_completed=1`). This proves
the v2 temporary image was rejected before kernel handoff in this
form; it is not evidence about the flashed Cerro same-slot layout,
which also zeroes DTBO and uses that project's proven reference boot
header/cmdline and embedded initramfs. No new log was found in
`logfs`, `logdump`, pstore or boot-reason properties.

### Cerro-aligned offline rebuild (2026-11-11)

The public Cerro scripts were then diffed against the rejected local
v2 temporary image. Boot-header fields we used already matched:
header v2, 4096-byte pages, base 0, kernel/ramdisk/second/tags/DTB
offsets `0x8000/0x01000000/0/0x100/0x01f00000`, OS 16.0.0 /
2026-07 and a 96 MiB partition-sized result. The substantive deltas
were elsewhere:

| Field | Proven Cerro layout | Rejected local v2 test |
|---|---|---|
| External ramdisk | exactly 127 bytes (stub) | 1,005,755-byte real initramfs |
| Real initramfs | gzip/newc archive embedded in `Image` | only in boot-image ramdisk field |
| Cmdline start | `ignore_loglevel pd_ignore_unused clk_ignore_unused console=ttyMSM0,115200n8 earlycon=qcom_geni,0x0089c000` | `console=tty0 console=ttyMSM0,115200n8 earlycon` |
| DTB | kernel-built board DTB also carried by their wrap object and appended after padded `Image` | appended/header DTB, no wrap object |
| Deployment | written to active boot slot with active DTBO zeroed | RAM-only `fastboot boot` with stock DTBO still present |

The kernel builder now generates the existing NX769J initramfs before
compilation and sets it as `CONFIG_INITRAMFS_SOURCE`;
`pack-boot-v2-aligned.py` refuses artifacts whose embedded archive
does not contain the expected `/init`, uses a valid deterministic
127-byte gzip/newc stub, reproduces Cerro's header values and verifies
its own round-trip. This still does not port Cerro's built-in-DTB wrap
object or its private known-good embedded init, so the result is an
alignment candidate for review, not a claim of equivalence.

The first aligned CI attempt and its rerun both failed before kernel
compilation while apt was resolving archive hosts. Their 3.7 GB job log
ends in repeated `Temporary failure resolving 'ports.ubuntu.com'`
messages for 23 packages; this was runner DNS failure, not a missing
package or a kernel error. Removing the extra apt package and using
the runner's statically linked `/usr/lib/initramfs-tools/bin/busybox`
produced successful run `43067542050` at build-repo commit `469bf00`.
The resulting offline `boot-candidate-07-aligned` passed a second,
packer-independent check: kernel payload = the downloaded `Image`,
zero padding through its declared `image_size` (61,865,984), then the
exact candidate DTB; header DTB also matches, the external ramdisk is
a valid 127-byte gzip/newc stub, and the embedded config records
built-in pstore RAM/console plus the generated initramfs source. The
96 MiB image SHA-256 is
`0f0c87229fce5c6132efd7d7ee51918a6fc512baf724bb13bb80151337a96932`.
It was subsequently flashed only to inactive `boot_a` in a separate
approved attempt. That attempt again reached Qualcomm memory-dump
mode in about 28 seconds, before any initramfs USB gadget appeared.
The mature Sahara client downloaded all requested regions without
error, but the new RAMCON again contains no candidate `7.1.1-nx769j+`
text. Embedded initramfs plus a 127-byte stub and Cerro header/cmdline
values therefore do not move the failure observably later. Slot B
Android was restored through the proven 9008 path, and slot A was
again restored byte-identically from backup.

### Outcome of step 1 (2026-11-11)

`boot-candidate-06-idonly` changed only the two root ID properties
(same kernel, initramfs, ramdisk and images as candidate-05 besides
`vendor_boot.img`). It reproduced the same memory-dump state about
28 seconds after reboot. Root ID properties alone therefore do not
explain the failure, and the new RAMCON still contains no candidate
kernel text. The next single variable is the stock UART `stdout-path`
plus `earlycon`, not another combined DT rewrite.

Stop this device path if step 2 also produces no new observable state and
the XBL/UEFI logs cannot be decoded offline. At that point the missing
evidence requires a hardware UART/JTAG path or a device-specific ABL
build, which is a materially larger project than the remaining budget.
