#!/usr/bin/env bash
# Pack an OFFLINE NX769J boot candidate (boot/init_boot/vendor_boot, v4).
# This only writes files. It never flashes, and producing these images does
# not prove the device accepts or boots them; see NX769J-BOOT-AUDIT.md.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
kernel_dir=${1:?usage: pack-boot-candidate.sh KERNEL_ARTIFACT_DIR ROOTFS_UUID TOOLS_DIR OUTPUT_DIR}
rootfs_uuid=${2:?missing rootfs UUID}
tools_dir=${3:?missing tools dir (mkbootimg.py, unpack_bootimg.py, busybox)}
output_dir=${4:?missing output dir}
kernel_dir=$(cd -- "$kernel_dir" && pwd -P)
tools_dir=$(cd -- "$tools_dir" && pwd -P)

for name in Image nx769j.dtb kernel-manifest.json; do
  [ -s "$kernel_dir/$name" ] || { echo "missing kernel input: $name" >&2; exit 1; }
done
for name in mkbootimg.py unpack_bootimg.py busybox; do
  [ -s "$tools_dir/$name" ] || { echo "missing tool: $name" >&2; exit 1; }
done
mkdir -p "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd -P)
if find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
  echo "refusing nonempty output directory: $output_dir" >&2
  exit 1
fi
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/nx769j-boot.XXXXXX")
cleanup() { rm -rf -- "$temp_dir"; }
trap cleanup EXIT

python3 "$repo_dir/scripts/nx769j/make-initramfs.py" \
  "$tools_dir/busybox" "$rootfs_uuid" "$temp_dir/initramfs.cpio.gz"

mk() { PYTHONPATH="$tools_dir" python3 "$tools_dir/mkbootimg.py" "$@"; }
cmdline="root=UUID=$rootfs_uuid rw rootwait console=tty0 loglevel=7"

# Stock OTA declares 100663296-byte boot/vendor_boot partitions and a
# 4096-byte vendor_boot page size; mkbootimg defaults to 2048, so pin it.
mk --header_version 4 --pagesize 4096 --kernel "$kernel_dir/Image" -o "$output_dir/boot.img"
mk --header_version 4 --pagesize 4096 --ramdisk "$temp_dir/initramfs.cpio.gz" \
  -o "$output_dir/init_boot.img"
mk --header_version 4 --pagesize 4096 --vendor_cmdline "$cmdline" \
  --dtb "$kernel_dir/nx769j.dtb" --vendor_ramdisk "$temp_dir/initramfs.cpio.gz" \
  --vendor_boot "$output_dir/vendor_boot.img"

check_size() { # file, limit, label
  size=$(wc -c < "$1"); size=${size//[[:space:]]/}
  [ "$size" -le "$2" ] || { echo "$3 too large: $size > $2" >&2; exit 1; }
  printf '%s bytes=%s\n' "$(basename -- "$1")" "$size"
}
check_size "$output_dir/boot.img" 100663296 boot
check_size "$output_dir/vendor_boot.img" 100663296 vendor_boot
# Stock init_boot size is not yet evidenced for this build; warn only.
check_size "$output_dir/init_boot.img" 67108864 init_boot

# Round-trip every image through the upstream unpacker and compare payloads.
same() { cmp -s "$1" "$2" || { echo "payload mismatch: $1" >&2; exit 1; }; }
PYTHONPATH="$tools_dir" python3 "$tools_dir/unpack_bootimg.py" \
  --boot_img "$output_dir/boot.img" --out "$temp_dir/boot" --format=mkbootimg >/dev/null
same "$temp_dir/boot/kernel" "$kernel_dir/Image"
PYTHONPATH="$tools_dir" python3 "$tools_dir/unpack_bootimg.py" \
  --boot_img "$output_dir/init_boot.img" --out "$temp_dir/init_boot" --format=mkbootimg >/dev/null
same "$temp_dir/init_boot/ramdisk" "$temp_dir/initramfs.cpio.gz"
PYTHONPATH="$tools_dir" python3 "$tools_dir/unpack_bootimg.py" \
  --boot_img "$output_dir/vendor_boot.img" --out "$temp_dir/vendor_boot" --format=mkbootimg >/dev/null
same "$temp_dir/vendor_boot/dtb" "$kernel_dir/nx769j.dtb"
same "$temp_dir/vendor_boot/vendor_ramdisk00" "$temp_dir/initramfs.cpio.gz"

cp "$temp_dir/initramfs.cpio.gz" "$output_dir/initramfs.cpio.gz"
python3 - "$output_dir/boot-candidate-manifest.json" "$rootfs_uuid" \
  "$kernel_dir/kernel-manifest.json" <<'PY'
import json, sys
with open(sys.argv[3], encoding="utf-8") as f:
    kernel = json.load(f)
manifest = {
    "device": "NX769J",
    "status": "OFFLINE_CANDIDATE_NOT_TESTED_ON_DEVICE_DO_NOT_FLASH",
    "kernel_release": kernel.get("kernel_release"),
    "kernel_commit": kernel.get("kernel_commit"),
    "build_repo_commit": kernel.get("build_repo_commit"),
    "rootfs_uuid": sys.argv[2],
    "cmdline": f"root=UUID={sys.argv[2]} rw rootwait console=tty0 loglevel=7",
    "images": ["boot.img", "init_boot.img", "vendor_boot.img", "initramfs.cpio.gz"],
    "avb_signed": False,
    "hardware_verification": "not_performed",
    "unresolved_gates": [
        "stock backup of active-slot partitions not yet taken",
        "partition placement for the root filesystem not decided",
        "bootloader DTB selection and cmdline merge not observed",
        "display/touch/GPU/Wi-Fi/audio disabled in the candidate DTB",
        "EDL recovery package not downloaded or authenticated",
    ],
}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, sort_keys=True)
    f.write("\n")
PY
(cd "$output_dir" && shasum -a 256 boot.img init_boot.img vendor_boot.img \
  initramfs.cpio.gz boot-candidate-manifest.json > SHA256SUMS \
  && shasum -a 256 -c SHA256SUMS)
printf 'offline NX769J boot candidate packed in %s; NOT tested on device.\n' "$output_dir"
