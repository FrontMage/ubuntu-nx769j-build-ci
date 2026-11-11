#!/usr/bin/env bash
# Build the offline NX769J kernel bundle: Image, DTB, modules, matching perf.
# This never packs a boot image and never touches a device.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
output_dir=${1:-out/nx769j-kernel}
kernel_repo=${KERNEL_REPO:-https://github.com/GUF296/linux.git}
kernel_commit=${KERNEL_COMMIT:-5df8e852ea722929f5359a5ef28ebcec0c4443fd}
kernel_src=${KERNEL_SRC:-}
jobs=${JOBS:-$(nproc 2>/dev/null || echo 4)}
device_dir="$repo_dir/devices/nx769j/kernel"
dtb_name=sm8650-nubia-nx769j.dtb

for command in git make python3 tar zstd; do
  command -v "$command" >/dev/null || { echo "missing command: $command" >&2; exit 1; }
done
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] || {
  echo 'this kernel builder requires a native arm64 Linux runner' >&2; exit 1; }
for name in sm8650-nubia-nx769j.dts nx769j.config \
    qcom-nx769j-binding.patch qcom-nx769j-makefile.patch; do
  [ -s "$device_dir/$name" ] || { echo "missing device input: $name" >&2; exit 1; }
done

mkdir -p "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd -P)
if find "$output_dir" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
  echo "refusing nonempty output directory: $output_dir" >&2
  exit 1
fi

temp_dir=$(mktemp -d "${RUNNER_TEMP:-/tmp}/nx769j-kernel.XXXXXX")
kernel_dir="$temp_dir/kernel"
worktree_from=
cleanup() {
  if [ -n "$worktree_from" ]; then
    git -C "$worktree_from" worktree remove --force "$kernel_dir" >/dev/null 2>&1 || true
    git -C "$worktree_from" worktree prune >/dev/null 2>&1 || true
  fi
  rm -rf -- "$temp_dir"
}
trap cleanup EXIT

# Build in a throwaway checkout so a caller-provided tree is never patched.
if [ -n "$kernel_src" ]; then
  kernel_src=$(cd -- "$kernel_src" && pwd -P)
  git -C "$kernel_src" cat-file -e "${kernel_commit}^{commit}" || {
    echo "KERNEL_SRC does not contain pinned commit $kernel_commit" >&2; exit 1; }
  git -C "$kernel_src" worktree add --detach "$kernel_dir" "$kernel_commit"
  worktree_from=$kernel_src
else
  git clone --filter=blob:none --no-checkout "$kernel_repo" "$kernel_dir"
  git -C "$kernel_dir" checkout --detach "$kernel_commit"
fi
actual_commit=$(git -C "$kernel_dir" rev-parse HEAD)
[ "$actual_commit" = "$kernel_commit" ] || {
  echo "kernel commit mismatch: $actual_commit != $kernel_commit" >&2; exit 1; }
df -h "$temp_dir" || true

for patch in qcom-nx769j-binding.patch qcom-nx769j-makefile.patch; do
  git -C "$kernel_dir" apply --check "$device_dir/$patch"
  git -C "$kernel_dir" apply "$device_dir/$patch"
done
install -m 0644 "$device_dir/sm8650-nubia-nx769j.dts" \
  "$kernel_dir/arch/arm64/boot/dts/qcom/sm8650-nubia-nx769j.dts"

make -C "$kernel_dir" ARCH=arm64 defconfig
"$kernel_dir/scripts/kconfig/merge_config.sh" -O "$kernel_dir" -m \
  "$kernel_dir/.config" "$device_dir/nx769j.config"
make -C "$kernel_dir" ARCH=arm64 olddefconfig

# Fail loudly if olddefconfig dropped anything the fragment requested; a
# silently missing UFS/PHY/perf symbol would only surface on the device.
config_missing=0
while IFS= read -r line; do
  grep -qxF "$line" "$kernel_dir/.config" || {
    echo "kernel config not in effect after olddefconfig: $line" >&2
    config_missing=1
  }
done < <(grep -E '^CONFIG_[A-Za-z0-9_]+=(y|m|".*")$' "$device_dir/nx769j.config")
[ "$config_missing" = 0 ] || exit 1

make -C "$kernel_dir" ARCH=arm64 -j"$jobs" Image modules
make -C "$kernel_dir" ARCH=arm64 -j"$jobs" "qcom/$dtb_name" || \
  make -C "$kernel_dir" ARCH=arm64 -j"$jobs" dtbs
dtb_path="$kernel_dir/arch/arm64/boot/dts/qcom/$dtb_name"
[ -s "$dtb_path" ] || { echo "DTB was not produced: $dtb_path" >&2; exit 1; }
kernel_release=$(make -s -C "$kernel_dir" ARCH=arm64 kernelrelease)
printf '%s\n' "$kernel_release" | grep -qE '^[A-Za-z0-9][A-Za-z0-9._+-]*$' || {
  echo "unexpected kernel release: $kernel_release" >&2; exit 1; }
df -h "$temp_dir" || true

# Stage modules exactly as the rootfs side validates them: only regular
# files and directories under lib/modules/<release>, no build/source links.
staging="$temp_dir/modules-staging"
mkdir -p "$staging"
make -C "$kernel_dir" ARCH=arm64 INSTALL_MOD_PATH="$staging" modules_install
rm -f "$staging/lib/modules/$kernel_release/build" \
      "$staging/lib/modules/$kernel_release/source"
if find "$staging" -type l -print -quit | grep -q .; then
  echo 'staged modules still contain symlinks' >&2
  exit 1
fi
tar --sort=name --owner=0 --group=0 --numeric-owner -C "$staging" -cf - lib | \
  zstd -q -o "$temp_dir/modules.tar.zst"
zstd -dc "$temp_dir/modules.tar.zst" | \
  python3 "$repo_dir/scripts/nx769j/check-module-tar.py" "$kernel_release"

make -C "$kernel_dir/tools/perf" -j"$jobs" WERROR=0
perf_path="$kernel_dir/tools/perf/perf"
[ -x "$perf_path" ] || { echo "perf was not produced: $perf_path" >&2; exit 1; }
if command -v readelf >/dev/null; then
  readelf -h "$perf_path" | grep -q 'AArch64' || {
    echo 'perf is not an AArch64 ELF' >&2; exit 1; }
fi
"$perf_path" --version

build_repo_commit=unknown
if git -C "$repo_dir" rev-parse HEAD >/dev/null 2>&1; then
  build_repo_commit=$(git -C "$repo_dir" rev-parse HEAD)
fi
python3 - "$temp_dir/kernel-manifest.json" "$kernel_release" "$kernel_commit" \
  "$build_repo_commit" <<'PY'
import json, sys
manifest = {
    "device": "NX769J",
    "status": "OFFLINE_CANDIDATE_NOT_TESTED_ON_DEVICE",
    "kernel_release": sys.argv[2],
    "kernel_source": "https://github.com/GUF296/linux.git",
    "kernel_branch": "TB321FU-7.1.1",
    "kernel_commit": sys.argv[3],
    "build_repo_commit": sys.argv[4],
    "dtb_source": "devices/nx769j/kernel/sm8650-nubia-nx769j.dts",
    "dtb_output": "nx769j.dtb",
    "boot_image_included": False,
    "hardware_verification": "not_performed",
}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, sort_keys=True)
    f.write("\n")
PY

install -m 0644 "$kernel_dir/arch/arm64/boot/Image" "$output_dir/Image"
install -m 0644 "$dtb_path" "$output_dir/nx769j.dtb"
install -m 0644 "$temp_dir/modules.tar.zst" "$output_dir/modules.tar.zst"
install -m 0755 "$perf_path" "$output_dir/perf"
install -m 0644 "$temp_dir/kernel-manifest.json" "$output_dir/kernel-manifest.json"
printf 'NX769J kernel bundle built: release=%s commit=%s; hardware untested, no boot image.\n' \
  "$kernel_release" "$kernel_commit"
