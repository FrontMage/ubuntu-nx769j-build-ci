#!/usr/bin/env bash
set -euo pipefail

image=${1:?usage: verify-rootfs.sh ROOTFS_IMAGE KERNEL_RELEASE EXPECTED_UUID}
kernel_release=${2:?missing kernel release}
expected_uuid=${3:?missing expected filesystem UUID}
[ "$(id -u)" = 0 ] || { echo 'root required for read-only image verification' >&2; exit 1; }
[ -f "$image" ] || { echo "missing image: $image" >&2; exit 1; }
[ "$(blkid -s UUID -o value "$image")" = "$expected_uuid" ] || { echo 'UUID mismatch' >&2; exit 1; }
e2fsck -fn "$image"
mount_dir=$(mktemp -d "${RUNNER_TEMP:-/tmp}/nx769j-verify.XXXXXX")
mounted=0
cleanup() {
  if [ "$mounted" = 1 ]; then umount "$mount_dir"; fi
  rmdir "$mount_dir"
}
trap cleanup EXIT
mount -o loop,ro,noload "$image" "$mount_dir"
mounted=1

for path in \
  "usr/lib/modules/$kernel_release" \
  usr/local/bin/perf usr/bin/cmake usr/bin/ninja usr/bin/git usr/bin/gdb \
  usr/bin/vulkaninfo usr/share/nx769j/BUILD-STATUS.txt; do
  [ -e "$mount_dir/$path" ] || { echo "missing expected rootfs path: $path" >&2; exit 1; }
done
for package in plasma-workspace sddm kwin-wayland build-essential cmake ninja-build git gdb vulkan-tools; do
  dpkg-query --admindir="$mount_dir/var/lib/dpkg" -W -f='${Status}\n' "$package" 2>/dev/null | \
    grep -Fx 'install ok installed' >/dev/null || { echo "required package is not installed: $package" >&2; exit 1; }
done
grep -Fx "UUID=$expected_uuid / ext4 defaults 0 1" "$mount_dir/etc/fstab" >/dev/null
grep -Fx 'PermitRootLogin no' "$mount_dir/etc/ssh/sshd_config.d/10-nx769j-candidate.conf" >/dev/null
grep -Fx 'PasswordAuthentication no' "$mount_dir/etc/ssh/sshd_config.d/10-nx769j-candidate.conf" >/dev/null
grep -Fx 'FEX_BINARIES_INSTALLED=no' "$mount_dir/usr/share/nx769j/BUILD-STATUS.txt" >/dev/null
for user in root developer; do
  awk -F: -v user="$user" '$1 == user && ($2 ~ /^!/ || $2 == "*") { found=1 } END { exit !found }' \
    "$mount_dir/etc/shadow" || { echo "password not locked: $user" >&2; exit 1; }
done
if [ -e "$mount_dir/etc/skel/.config/kwinoutputconfig.json" ] || \
   [ -e "$mount_dir/home/developer/.config/kwinoutputconfig.json" ]; then
  echo 'unexpected Y700 display-specific KWin output configuration' >&2
  exit 1
fi
for path in \
  etc/wireplumber/wireplumber.conf.d/51-y700-alsa-auto.conf \
  usr/lib/systemd/system/tb321fu-haptics.service \
  usr/share/tb321fu-ksystemstats-gpu opt/libcamera-y700; do
  [ ! -e "$mount_dir/$path" ] || { echo "unexpected Y700 device payload: $path" >&2; exit 1; }
done
# The CI runner is native arm64. This confirms the exact installed perf starts
# with the rootfs libraries; it does not sample or prove hardware support.
chroot "$mount_dir" /usr/local/bin/perf --version
echo "rootfs verification passed: UUID=$expected_uuid, modules=$kernel_release, hardware untested"
