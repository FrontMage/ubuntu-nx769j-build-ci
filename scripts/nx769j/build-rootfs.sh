#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
kernel_dir=${1:?usage: build-rootfs.sh KERNEL_ARTIFACT_DIRECTORY [OUTPUT_DIRECTORY]}
output_dir=${2:-out/nx769j-rootfs}
kernel_dir=$(cd -- "$kernel_dir" && pwd -P)
mkdir -p "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd -P)

for command in blkid curl debootstrap e2fsck mount patch python3 readelf rsync sha256sum tar uuidgen zstd; do
  command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 1; }
done
[ "$(uname -m)" = aarch64 ] || { echo 'this rootfs builder requires a native arm64 Linux runner' >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo 'root is required for loop mount and chroot on the isolated builder' >&2; exit 1; }
for name in Image nx769j.dtb modules.tar.zst perf kernel-manifest.json; do
  [ -s "$kernel_dir/$name" ] || { echo "missing kernel input: $name" >&2; exit 1; }
done
readelf -h "$kernel_dir/perf" | grep -q 'AArch64' || { echo 'perf is not an AArch64 ELF' >&2; exit 1; }
kernel_release=$(python3 - "$kernel_dir/kernel-manifest.json" <<'PY'
import json, re, sys
with open(sys.argv[1], encoding='utf-8') as f:
    value = json.load(f).get('kernel_release', '')
if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]*', value):
    raise SystemExit('invalid kernel_release in kernel manifest')
print(value)
PY
)

# The upstream builder deletes its work directory. Keep everything we prepare
# under our own unique temporary directory, and refuse to overwrite output.
if find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
  echo "refusing nonempty output directory: $output_dir" >&2
  exit 1
fi
temp_dir=$(mktemp -d "${RUNNER_TEMP:-/tmp}/nx769j-rootfs.XXXXXX")
cleanup() { rm -rf -- "$temp_dir"; }
trap cleanup EXIT
upstream="$temp_dir/upstream"
overlay="$temp_dir/overlay"
modules="$temp_dir/modules"
mkdir -p "$overlay/usr/lib/modules" "$overlay/usr/local/bin" \
  "$overlay/usr/share/nx769j" "$overlay/etc/ssh/sshd_config.d" "$modules"

# Reject traversal, other releases, symlinks and special files before extract.
zstd -dc "$kernel_dir/modules.tar.zst" | python3 "$repo_dir/scripts/nx769j/check-module-tar.py" "$kernel_release"
zstd -dc "$kernel_dir/modules.tar.zst" | tar --no-same-owner -xf - -C "$modules"
[ -d "$modules/lib/modules/$kernel_release" ] || { echo 'modules release directory missing' >&2; exit 1; }
if find "$modules" -type l -print -quit | grep -q .; then
  echo 'modules archive contains symlinks; build/source links must be removed at packaging' >&2
  exit 1
fi
cp -a "$modules/lib/modules/$kernel_release" "$overlay/usr/lib/modules/"
install -m 0755 "$kernel_dir/perf" "$overlay/usr/local/bin/perf"
install -m 0644 "$kernel_dir/kernel-manifest.json" "$overlay/usr/share/nx769j/kernel-manifest.json"

rootfs_uuid=$(uuidgen | tr '[:upper:]' '[:lower:]')
cat > "$overlay/etc/fstab" <<EOF
UUID=$rootfs_uuid / ext4 defaults 0 1
EOF
cat > "$overlay/etc/ssh/sshd_config.d/10-nx769j-candidate.conf" <<'EOF'
PermitRootLogin no
PasswordAuthentication no
KbdInteractiveAuthentication no
EOF
cat > "$overlay/usr/share/nx769j/BUILD-STATUS.txt" <<EOF
DEVICE=NX769J
STATUS=OFFLINE_CANDIDATE_NOT_TESTED_ON_DEVICE
KERNEL_RELEASE=$kernel_release
ROOTFS_UUID=$rootfs_uuid
FEX_SOURCE=https://github.com/FEX-Emu/FEX.git
FEX_SOURCE_COMMIT=0fe06483496f1d29fe190c44df6c7c5b881b15d0
FEX_BINARIES_INSTALLED=no
HARDWARE_VERIFICATION=not_performed
EOF

"$repo_dir/scripts/nx769j/prepare-y700-rootfs.sh" "$upstream"
packages='systemd systemd-sysv dbus sudo locales tzdata ca-certificates gnupg curl wget network-manager openssh-server nano vim rsync kmod initramfs-tools build-essential clang lld cmake ninja-build git pkg-config python3 gdb libbabeltrace-dev libcap-dev libelf-dev libdw-dev libtraceevent-dev libtracefs-dev liblzma-dev libslang2-dev libunwind-dev libfmt-dev libssl-dev zlib1g-dev libzstd-dev libvulkan-dev mesa-vulkan-drivers mesa-utils vulkan-tools'

env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root LANG=C.UTF-8 \
  OUTPUT_DIR="$output_dir" OUTPUT_PREFIX=nx769j-kubuntu-arm64 \
  DISTRO=resolute ARCH=arm64 MIRROR=https://ports.ubuntu.com/ubuntu-ports \
  DEBOOTSTRAP_VARIANT=minbase ROOTFS_IMAGE_SIZE=20G \
  ROOTFS_LABEL=NX769J_ROOT ROOTFS_PARTLABEL=NX769J_ROOT ROOTFS_UUID="$rootfs_uuid" \
  ROOTFS_DEVICE_PROFILE=generic HOSTNAME_NAME=nx769j \
  DEFAULT_USER_NAME=developer DEFAULT_USER_PASSWORD=unused LOCK_DEFAULT_USER=1 \
  ROOT_PASSWORD_MODE=locked USER_SUDO_MODE=password SDDM_AUTOLOGIN=0 \
  DESKTOP_ENV=kubuntu-desktop PACKAGE_LIST="$packages" \
  OVERLAY_DIR="$overlay" BUILD_TB321FU_GPU_SENSOR=0 \
  APPLY_Y700_FIRMWARE_FIXES=0 APPLY_Y700_AUDIO_POLICY_FIXES=0 \
  INSTALL_GNOME_SNAPSHOT=0 INSTALL_FIREFOX=0 INSTALL_FCITX5_CHINESE=1 \
  DISABLE_SNAPD=1 CLEAN_APT_CACHE=1 COMPRESS=zstd KEEP_RAW_IMAGE=1 \
  "$upstream/scripts/ci/build-rootfs-image.sh"

image="$output_dir/nx769j-kubuntu-arm64-rootfs.img"
actual_uuid=$(blkid -s UUID -o value "$image")
[ "$actual_uuid" = "$rootfs_uuid" ] || { echo 'rootfs UUID mismatch' >&2; exit 1; }
e2fsck -fn "$image"
printf '%s\n' "$rootfs_uuid" > "$output_dir/ROOTFS-UUID.txt"
cp "$overlay/usr/share/nx769j/BUILD-STATUS.txt" "$output_dir/BUILD-STATUS.txt"
(
  cd "$output_dir"
  sha256sum -c nx769j-kubuntu-arm64-rootfs.SHA256SUMS
  sha256sum ROOTFS-UUID.txt BUILD-STATUS.txt >> nx769j-kubuntu-arm64-rootfs.SHA256SUMS
)
printf 'NX769J rootfs image built: UUID=%s; FEX binaries remain unverified/not installed.\n' "$rootfs_uuid"
