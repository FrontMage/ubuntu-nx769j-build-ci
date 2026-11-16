#!/usr/bin/env bash
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
#
# Install Omarchy Mobile desktop (`omamobile-shell` + Hyprland + Adreno Turnip
# + patched `aquamarine` 0.16.0 with software Wayland touch routing) inside the
# staged Arch Linux ARM rootfs (`/arch-stage` by default).
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

STAGE_ROOT="/arch-stage"
CACHE_DIR=""
AQUAMARINE_TARBALL=""

usage() {
  cat << 'EOF'
Usage: sudo bash rootfs/install_omarchy_ui.sh [OPTIONS]

Options:
  --stage-root DIR          Staged Arch rootfs directory (default: /arch-stage)
  --cache-dir DIR           Optional local pacman pkg cache directory
  --aquamarine-tarball FILE Optional local aquamarine-0.16.0.tar.gz path
  -h, --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage-root)         STAGE_ROOT="$2"; shift 2 ;;
    --cache-dir)          CACHE_DIR="$2"; shift 2 ;;
    --aquamarine-tarball) AQUAMARINE_TARBALL="$2"; shift 2 ;;
    -h|--help)            usage; exit 0 ;;
    *)                    echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ "$(id -u)" != "0" ]]; then
  echo "ERROR: Must run as root." >&2
  exit 1
fi

CHROOT_MOUNTS_ACTIVE=0
cleanup() {
  if [[ "$CHROOT_MOUNTS_ACTIVE" == "1" ]]; then
    for mp in var/cache/pacman/pkg dev/pts dev proc sys run; do
      umount -lf "${STAGE_ROOT}/${mp}" 2>/dev/null || true
    done
  fi
}
trap cleanup EXIT

# Ensure [omamobile] and [omarchy] pacman repos are present in pacman.conf
PACCONF="${STAGE_ROOT}/etc/pacman.conf"
if ! grep -q '^\[omamobile\]' "$PACCONF"; then
  cat >> "$PACCONF" << 'REPOEOF'

[omamobile]
SigLevel = Optional TrustAll
Server = https://pkg.omamobile.org/$arch

[omarchy]
SigLevel = Optional TrustAll
Server = https://pkgs.omarchy.org/$arch
REPOEOF
fi

for d in proc sys dev dev/pts run var/cache/pacman/pkg; do
  mkdir -p "${STAGE_ROOT}/${d}"
done
mount --bind /proc "${STAGE_ROOT}/proc"
mount --bind /sys "${STAGE_ROOT}/sys"
mount --bind /dev "${STAGE_ROOT}/dev"
mount --bind /dev/pts "${STAGE_ROOT}/dev/pts"
mount --bind /run "${STAGE_ROOT}/run" 2>/dev/null || true
if [[ -n "$CACHE_DIR" && -d "$CACHE_DIR" ]]; then
  mount --bind "$CACHE_DIR" "${STAGE_ROOT}/var/cache/pacman/pkg"
fi
CHROOT_MOUNTS_ACTIVE=1

echo "[ui] Installing Omarchy Mobile, Hyprland, Mesa/Vulkan, PipeWire & browser stack..."
chroot "$STAGE_ROOT" /bin/bash -euo pipefail -c '
export PATH="/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin"
pacman -Syy --noconfirm
pacman -S --needed --noconfirm \
  hyprland aquamarine hyprutils hyprlang hyprgraphics hyprwayland-scanner \
  omamobile-shell omamobile-theme power-profiles-daemon \
  mesa vulkan-freedreno vulkan-icd-loader qt6-base qt6-wayland qt6-declarative \
  pipewire pipewire-alsa pipewire-pulse wireplumber gst-plugins-good \
  xdg-desktop-portal-hyprland polkit seatd dbus ttf-dejavu noto-fonts \
  foot wofi grim slurp wl-clipboard chromium jq python-gobject
'

echo "[ui] Building patched aquamarine 0.16.0 (Wayland touch slot forwarding)..."
AQUA_SRC="${AQUAMARINE_TARBALL:-}"
if [[ -z "$AQUA_SRC" ]]; then
  AQUA_SRC="/tmp/aquamarine-0.16.0.tar.gz"
  if [[ ! -f "$AQUA_SRC" ]]; then
    curl -fL --retry 3 -o "$AQUA_SRC" \
      "https://github.com/hyprwm/aquamarine/archive/refs/tags/v0.16.0.tar.gz"
  fi
fi

mkdir -p "${STAGE_ROOT}/tmp/aqua-build"
cp -f "$AQUA_SRC" "${STAGE_ROOT}/tmp/aqua-build/aquamarine-0.16.0.tar.gz"
cp -f "${REPO_ROOT}/drivers/display/patches/aquamarine-0.16.0-wayland-touch.patch" \
  "${STAGE_ROOT}/tmp/aqua-build/wayland-touch.patch"

chroot "$STAGE_ROOT" /bin/bash -euo pipefail -c '
cd /tmp/aqua-build
tar -xzf aquamarine-0.16.0.tar.gz
cd aquamarine-0.16.0
patch -p1 < /tmp/aqua-build/wayland-touch.patch
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build -j"$(nproc)"
cmake --install build
ldconfig
rm -rf /tmp/aqua-build
'

echo "[ui] Applying NX769J Omarchy UI, perf widget & audio UI overlays..."
cp -a "${REPO_ROOT}/rootfs/overlay/." "${STAGE_ROOT}/"
chmod 0440 "${STAGE_ROOT}/etc/sudoers.d/90-nx769j-wheel" "${STAGE_ROOT}/etc/sudoers.d/10-nx769j-perf"
chmod +x "${STAGE_ROOT}/usr/local/bin/"* "${STAGE_ROOT}/usr/local/sbin/"* 2>/dev/null || true
chown -R 1000:1000 "${STAGE_ROOT}/home/developer"

chroot "$STAGE_ROOT" /bin/bash -euo pipefail -c '
/usr/local/sbin/nx769j-apply-omamobile-ui
python3 /usr/local/share/nx769j-perf-ui/apply-perf-ui.py
python3 /usr/local/share/nx769j-audio-ui/apply-audio-ui.py
if [ -x /usr/bin/systemctl ]; then
  systemctl enable nx769j-fb-wl.service nx769j-omamobile-session.service nx769j-perf.service || true
fi
'

# Enable multi-user.target UI launch units explicitly for chroot compatibility
WANTS_DIR="${STAGE_ROOT}/etc/systemd/system/multi-user.target.wants"
mkdir -p "$WANTS_DIR"
for unit in nx769j-fb-wl.service nx769j-omamobile-session.service nx769j-perf.service; do
  ln -sfn "/etc/systemd/system/${unit}" "${WANTS_DIR}/${unit}"
done

# Disable stock Display Manager (SimpleFB card0 headless bridge handles KMS/FB presentation)
rm -f "${STAGE_ROOT}/etc/systemd/system/display-manager.service"

echo "[ui] Omarchy Mobile UI staging complete inside $STAGE_ROOT"
