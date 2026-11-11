#!/usr/bin/env bash
# Fetch only the two public upstream scripts required for an NX rootfs build.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
upstream_commit=4f87bbc51c6e8ab4434d421bd16fac3f7a8cb1d3
dest=${1:?usage: prepare-y700-rootfs.sh EMPTY_OUTPUT_DIRECTORY}

if [ -e "$dest" ]; then
  printf 'error: output already exists: %s\n' "$dest" >&2
  exit 1
fi
mkdir -p "$dest/scripts/ci"
for name in build-rootfs-image.sh common.sh; do
  curl -fsSL --retry 3 \
    "https://raw.githubusercontent.com/GUF296/ubuntu-y700-build-ci/${upstream_commit}/scripts/ci/${name}" \
    -o "$dest/scripts/ci/$name"
done
(
  cd "$dest"
  sha256sum -c - <<'SUMS'
d007c9d7aab1e80aabd9cd53d705196ceefc5737d774e895f3724f0d539a7aee  scripts/ci/build-rootfs-image.sh
6adf1735df7040c23eac217a987178e3139b269a3e93035e6df4302385913893  scripts/ci/common.sh
SUMS
  patch -p1 --batch --forward < "$repo_dir/patches/nx769j/y700-rootfs-generic.patch"
  chmod +x scripts/ci/build-rootfs-image.sh
)
printf '%s\n' "$upstream_commit" > "$dest/UPSTREAM-COMMIT"
