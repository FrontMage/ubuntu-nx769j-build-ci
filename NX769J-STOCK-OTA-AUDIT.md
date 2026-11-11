# NX769J stock OTA boot-format audit

Status: **source inspection only, 2026-11-11**. This note does not qualify a
custom boot image or a recovery procedure. No phone partition was written,
and no root access, reboot, flash, or device-unique data export was used.

## Public source and build match

- The reference package is the vendor-hosted full OTA
  `GEN_NEEA_NX769SV1BV1.0.0B14MR3_SD_WO_ERA.zip` at
  `https://rom.download.nubia.com/Europe%26Asia/NX769S/GEN_NEEA_NX769SV1BV1.0.0B14MR3_SD_WO_ERA.zip`.
  The [community OTA index](https://github.com/szescxz/ZTE_NX769J_FOTA/releases/tag/GEN_NEEA_NX769SV1BV1.0.0B14MR3)
  links to this vendor URL. An HTTP HEAD request returned 200 and content length
  5,507,446,499 bytes. The complete archive was not downloaded or independently
  authenticated in this audit.
- HTTP byte ranges were used to read the ZIP central directory and
  `META-INF/com/android/metadata`; no source archive or proprietary images were
  included in the repository. Metadata reports `pre-device=NX769J`, A/B OTA,
  Android 14, and build incremental `20241128.200320`. Its `post-build`
  fingerprint exactly matches the build fingerprint read through ordinary ADB
  on the intended test device. This is a build-version match, not a per-slot
  byte-for-byte match or proof of an EDL recovery image.
- The ZIP stores `payload.bin` uncompressed. Its AOSP update-engine manifest
  reports payload major version 2 and minor version 0 (full payload), with no
  old partition information for `boot` or `vendor_boot`. The vendor OTA is an
  Android update package, not a verified Qualcomm EDL service package.

## Declared boot images

The manifest declares these complete partition image sizes and hashes. They
are **OTA target hashes**, not hashes measured from the phone's active slot.

| Partition | Size (bytes) | SHA-256 from OTA manifest |
| --- | ---: | --- |
| `boot` | 100663296 | `143aa465a55304ed4cb4619b127619050883e2b85de52bf798470db2ace801f5` |
| `vendor_boot` | 100663296 | `6ef08850fe1c19be348ba2ede46d691cecd6ac939ab8f7b17d649005f11367e8` |

Only the XZ-compressed OTA operations covering the first 512 4096-byte
blocks of each image were fetched. Each compressed blob matched its own
SHA-256 in the fetched manifest before decompression. The manifest and complete
OTA image signatures were not independently verified in this audit.

| Field | `boot` | `vendor_boot` |
| --- | --- | --- |
| Magic | `ANDROID!` | `VNDRBOOT` |
| Header version | 4 | 4 |
| Header size | 1584 | 2128 |
| Page size | v4 boot header has no page-size field | 4096 |
| Kernel size | 34699776 | not applicable |
| Ramdisk size | 0 | 8784394 |
| DTB size | not carried in v4 boot header | 2764814 |

These values characterize the public OTA images of the matching software
build. They are not proof that an independently built Linux image is accepted
by this device's Android bootloader or that a vendor DTB works with mainline
Linux.

## Remaining gates

- Ordinary ADB shell could not read the on-device firmware DTB or boot block
  devices. The actual active-slot `boot`, `vendor_boot`, `dtbo`, `vbmeta`, and
  any patched Magisk images have not been hashed or compared with the OTA.
- The observed Android system uses slot B. The other slot's contents and
  bootability, Android userdata recovery, and an EDL service path are unknown.
  The user's Android, shared storage, and calibration data are not build inputs.
- The public [NX769J UniXNU project](https://github.com/Asultop/UniXNU-NX769J#status)
  reports its own UEFI/XNU images have not been booted on hardware. Its v0
  boot-image experiment is not evidence that this NX769J accepts that format
  or that its vendor DTB is a mainline Linux board description.
- A Linux boot candidate requires a separately evidenced NX769J bootloader
  handoff, compatible kernel, board DTB and initramfs arrangement, and private
  handling of any needed device firmware. Do not reuse the Y700 boot image or
  infer a safe partition/slot layout from a successful offline pack/unpack.

Reference: [AOSP update-engine payload schema](https://android.googlesource.com/platform/system/update_engine/+/HEAD/update_metadata.proto).
