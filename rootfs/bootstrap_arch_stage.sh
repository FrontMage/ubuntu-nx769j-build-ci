#!/usr/bin/env bash
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
#
# Stage an Arch Linux ARM + NX769J hardware enablement rootfs into `/arch-stage`
# (or custom `--stage-root`) directly on the running Linux phone/chroot host.
#
# Designed for non-destructive migration via `atomic_pivot_to_arch.py`.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

STAGE_ROOT="/arch-stage"
SOURCE_ROOT="/"
MODULES_TAR=""
FIRMWARE_DIR=""
TARBALL=""
MIRROR_PRESET="global"
SSH_PUBKEY_FILE=""
DEFAULT_PASS="omarchy"

usage() {
  cat << 'EOF'
Usage: sudo bash rootfs/bootstrap_arch_stage.sh [OPTIONS]

Options:
  --stage-root DIR       Target staging directory (default: /arch-stage)
  --source-root DIR      Live root to copy existing /lib/modules & extracted fw (default: /)
  --modules-tar FILE     Optional modules.tar.zst path (instead of copying from --source-root)
  --firmware-dir DIR     Optional unpacked firmware root (instead of copying from --source-root)
  --tarball FILE         Optional local ArchLinuxARM-aarch64-latest.tar.gz path
  --mirror-preset NAME   'global' (default) or 'cn' (TUNA/USTC Mainland China mirrors)
  --ssh-pubkey-file FILE Optional SSH public key to authorize for root & developer
  --password PASS        Password for 'developer' and 'root' (default: omarchy)
  -h, --help             Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage-root)      STAGE_ROOT="$2"; shift 2 ;;
    --source-root)     SOURCE_ROOT="$2"; shift 2 ;;
    --modules-tar)     MODULES_TAR="$2"; shift 2 ;;
    --firmware-dir)    FIRMWARE_DIR="$2"; shift 2 ;;
    --tarball)         TARBALL="$2"; shift 2 ;;
    --mirror-preset)   MIRROR_PRESET="$2"; shift 2 ;;
    --ssh-pubkey-file) SSH_PUBKEY_FILE="$2"; shift 2 ;;
    --password)        DEFAULT_PASS="$2"; shift 2 ;;
    -h|--help)         usage; exit 0 ;;
    *)                 echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ "$(id -u)" != "0" ]]; then
  echo "ERROR: Must run as root (inside running phone Linux or chroot host)." >&2
  exit 1
fi

CHROOT_MOUNTS_ACTIVE=0
cleanup() {
  if [[ "$CHROOT_MOUNTS_ACTIVE" == "1" ]]; then
    for mp in dev/pts dev proc sys run; do
      umount -lf "${STAGE_ROOT}/${mp}" 2>/dev/null || true
    done
  fi
}
trap cleanup EXIT

mkdir -p "$STAGE_ROOT"

# 1. Extract Arch Linux ARM tarball if /usr/bin/pacman not already staged
if [[ ! -x "${STAGE_ROOT}/usr/bin/pacman" ]]; then
  if [[ -z "$TARBALL" ]]; then
    TARBALL="/tmp/ArchLinuxARM-aarch64-latest.tar.gz"
    if [[ ! -f "$TARBALL" ]]; then
      ALARM_URL="http://os.archlinuxarm.org/os/ArchLinuxARM-aarch64-latest.tar.gz"
      if [[ "$MIRROR_PRESET" == "cn" ]]; then
        ALARM_URL="https://mirrors.tuna.tsinghua.edu.cn/archlinuxarm/os/ArchLinuxARM-aarch64-latest.tar.gz"
      fi
      echo "[stage] Downloading $ALARM_URL ..."
      curl -fL --retry 3 -o "$TARBALL" "$ALARM_URL"
    fi
  fi
  echo "[stage] Extracting ArchLinuxARM rootfs into $STAGE_ROOT ..."
  bsdtar -xpf "$TARBALL" -C "$STAGE_ROOT" || tar --numeric-owner -xzpf "$TARBALL" -C "$STAGE_ROOT"
fi

# 2. Seed overlay configuration first (including pacman.conf with DisableSandbox!)
echo "[stage] Installing rootfs/overlay files into $STAGE_ROOT ..."
cp -a "${REPO_ROOT}/rootfs/overlay/." "${STAGE_ROOT}/"

if [[ "$MIRROR_PRESET" == "global" ]]; then
  cat > "${STAGE_ROOT}/etc/pacman.d/mirrorlist" << 'MIREOF'
# Global GeoIP Arch Linux ARM Mirror
Server = http://mirror.archlinuxarm.org/$arch/$repo
MIREOF
fi

mkdir -p "${STAGE_ROOT}/etc"
if [[ -f /etc/resolv.conf ]]; then
  cp -L /etc/resolv.conf "${STAGE_ROOT}/etc/resolv.conf"
fi

# 3. Bind mount chroot filesystems
for d in proc sys dev dev/pts run; do
  mkdir -p "${STAGE_ROOT}/${d}"
done
mount --bind /proc "${STAGE_ROOT}/proc"
mount --bind /sys "${STAGE_ROOT}/sys"
mount --bind /dev "${STAGE_ROOT}/dev"
mount --bind /dev/pts "${STAGE_ROOT}/dev/pts"
mount --bind /run "${STAGE_ROOT}/run" 2>/dev/null || true
CHROOT_MOUNTS_ACTIVE=1

# 4. Initialize pacman keyring + base packages
echo "[stage] Initializing pacman keys & installing base Arch packages..."
chroot "$STAGE_ROOT" /bin/bash -euo pipefail -c '
export PATH="/usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/bin"
pacman-key --init
pacman-key --populate archlinuxarm
pacman -Syyu --noconfirm
pacman -S --needed --noconfirm \
  base base-devel cmake ninja meson git rsync python python-pip \
  gcc clang lld linux-api-headers \
  networkmanager bluez bluez-utils upower iio-sensor-proxy \
  pipewire pipewire-alsa pipewire-pulse wireplumber alsa-utils alsa-ucm-conf \
  openssh sudo vim unzip zstd kmod usbutils i2c-tools \
  libinput wayland wayland-protocols libxkbcommon dmidecode
'

# 5. Users & groups (`developer` uid=1000, `fastrpc` system user)
echo "[stage] Configuring developer & fastrpc accounts..."
chroot "$STAGE_ROOT" /bin/bash -euo pipefail -c "
if id alarm >/dev/null 2>&1; then
  userdel -r alarm 2>/dev/null || true
fi
groupadd -f wheel
groupadd -f render
groupadd -f video
groupadd -f audio
groupadd -f input
if ! id developer >/dev/null 2>&1; then
  useradd -m -u 1000 -U -G wheel,render,video,audio,input -s /bin/bash developer
else
  usermod -aG wheel,render,video,audio,input developer
fi
echo 'developer:${DEFAULT_PASS}' | chpasswd
echo 'root:${DEFAULT_PASS}' | chpasswd
systemd-sysusers /etc/sysusers.d/nx769j-fastrpc.conf 2>/dev/null || true
if ! id fastrpc >/dev/null 2>&1; then
  useradd -r -M -s /usr/bin/nologin -d / fastrpc || true
fi
"

# 6. Install kernel modules (`7.1.1-nx769j+` or active kernel)
KVER="7.1.1-nx769j+"
if [[ -n "$MODULES_TAR" && -f "$MODULES_TAR" ]]; then
  echo "[stage] Extracting kernel modules from $MODULES_TAR ..."
  mkdir -p "${STAGE_ROOT}/usr/lib"
  tar --use-compress-program=zstd -xf "$MODULES_TAR" -C "${STAGE_ROOT}/usr/lib"
elif [[ -d "${SOURCE_ROOT}/lib/modules/${KVER}" ]]; then
  echo "[stage] Copying ${SOURCE_ROOT}/lib/modules/${KVER} ..."
  mkdir -p "${STAGE_ROOT}/usr/lib/modules"
  cp -a "${SOURCE_ROOT}/lib/modules/${KVER}" "${STAGE_ROOT}/usr/lib/modules/"
fi

if [[ -d "${STAGE_ROOT}/usr/lib/modules/${KVER}" ]]; then
  chroot "$STAGE_ROOT" /usr/bin/depmod -a "$KVER"
fi

# 7. Copy extracted proprietary firmware + sensor calibration (must remain uncompressed!)
FW_SRC="${FIRMWARE_DIR:-${SOURCE_ROOT}/lib/firmware}"
if [[ -d "$FW_SRC" ]]; then
  echo "[stage] Copying uncompressed device firmware from $FW_SRC ..."
  mkdir -p "${STAGE_ROOT}/usr/lib/firmware"
  for sub in ath12k qca qcom; do
    if [[ -d "${FW_SRC}/${sub}" ]]; then
      cp -a "${FW_SRC}/${sub}" "${STAGE_ROOT}/usr/lib/firmware/"
    fi
  done
  find "${STAGE_ROOT}/usr/lib/firmware" -name '*.zst' -delete 2>/dev/null || true
fi

PERSIST_SRC="${SOURCE_ROOT}/usr/share/qcom/sm8650/nubia/nx769j/persist_sensors"
if [[ ! -d "$PERSIST_SRC" && -d "${SOURCE_ROOT}/persist/sensors" ]]; then
  PERSIST_SRC="${SOURCE_ROOT}/persist/sensors"
fi
if [[ -d "$PERSIST_SRC" ]]; then
  mkdir -p "${STAGE_ROOT}/usr/share/qcom/sm8650/nubia/nx769j/persist_sensors"
  cp -a "${PERSIST_SRC}/." "${STAGE_ROOT}/usr/share/qcom/sm8650/nubia/nx769j/persist_sensors/"
fi
mkdir -p "${STAGE_ROOT}/var/lib/droid-juicer"
ln -sfn /usr/share/qcom/sm8650/nubia/nx769j/persist_sensors "${STAGE_ROOT}/var/lib/droid-juicer/sensors"

# 8. Install open-source NX769J drivers, audio topology/UCM2, and compile C daemons
echo "[stage] Installing NX769J audio topology, UCM2, C/Python drivers..."
mkdir -p \
  "${STAGE_ROOT}/usr/lib/firmware/qcom/sm8650" \
  "${STAGE_ROOT}/usr/share/alsa/ucm2" \
  "${STAGE_ROOT}/usr/local/src" \
  "${STAGE_ROOT}/usr/local/bin" \
  "${STAGE_ROOT}/usr/local/sbin" \
  "${STAGE_ROOT}/usr/local/libexec" \
  "${STAGE_ROOT}/usr/local/src/libhexagonrpc"

install -m 0644 "${REPO_ROOT}/drivers/audio/SM8650-Nubia-NX769J-tplg.bin" \
  "${STAGE_ROOT}/usr/lib/firmware/qcom/sm8650/SM8650-Nubia-NX769J-tplg.bin"
cp -a "${REPO_ROOT}/drivers/audio/ucm2/." "${STAGE_ROOT}/usr/share/alsa/ucm2/"
install -m 0755 "${REPO_ROOT}/drivers/audio/aw88261_init.py" "${STAGE_ROOT}/usr/local/sbin/aw88261-init"

install -m 0644 "${REPO_ROOT}/drivers/display/nx769j_fb_wl.c" "${STAGE_ROOT}/usr/local/src/nx769j_fb_wl.c"
install -m 0644 "${REPO_ROOT}/drivers/display/nx769j_vkbd.c" "${STAGE_ROOT}/usr/local/src/nx769j_vkbd.c"
install -m 0644 "${REPO_ROOT}/drivers/thermal-fan/nx769j-fan.c" "${STAGE_ROOT}/usr/local/src/nx769j-fan.c"
install -m 0644 "${REPO_ROOT}/drivers/thermal-fan/nx769j_perf.c" "${STAGE_ROOT}/usr/local/src/nx769j_perf.c"
install -m 0644 "${REPO_ROOT}/drivers/power/nx769j_powerkey.c" "${STAGE_ROOT}/usr/local/src/nx769j_powerkey.c"
install -m 0755 "${REPO_ROOT}/drivers/power/nx769j_charge_limit.py" "${STAGE_ROOT}/usr/local/bin/nx769j-charge-limit"

install -m 0644 "${REPO_ROOT}/drivers/sensors/nx769j_hexagonrpcd.c" "${STAGE_ROOT}/usr/local/src/nx769j_hexagonrpcd.c"
install -m 0644 "${REPO_ROOT}/drivers/sensors/nx769j_imu_daemon.c" "${STAGE_ROOT}/usr/local/src/nx769j_imu_daemon.c"
install -m 0755 "${REPO_ROOT}/drivers/sensors/nx769j-sensor-proxy" "${STAGE_ROOT}/usr/local/libexec/nx769j-sensor-proxy"
install -m 0755 "${REPO_ROOT}/drivers/sensors/nx769j-imu" "${STAGE_ROOT}/usr/local/bin/nx769j-imu"
cp -a "${REPO_ROOT}/drivers/sensors/libhexagonrpc/." "${STAGE_ROOT}/usr/local/src/libhexagonrpc/"

install -m 0644 "${REPO_ROOT}/storage-dualboot/nx_dm_tool.c" "${STAGE_ROOT}/usr/local/src/nx_dm_tool.c"
install -m 0644 "${REPO_ROOT}/storage-dualboot/nx769j_slotctl.c" "${STAGE_ROOT}/usr/local/src/nx769j_slotctl.c"

echo "[stage] Compiling native ARM64 drivers inside Arch chroot..."
chroot "$STAGE_ROOT" /bin/bash -euo pipefail -c '
meson setup /tmp/build-libhex /usr/local/src/libhexagonrpc --prefix=/usr --libdir=lib --wipe || \
  meson setup /tmp/build-libhex /usr/local/src/libhexagonrpc --prefix=/usr --libdir=lib
ninja -C /tmp/build-libhex install
ldconfig
rm -rf /tmp/build-libhex

gcc -O2 -Wall -Wextra /usr/local/src/nx769j_hexagonrpcd.c -lhexagonrpc -o /usr/local/libexec/nx769j-hexagonrpcd
gcc -O2 -Wall -Wextra /usr/local/src/nx769j_imu_daemon.c -lhexagonrpc -lpthread -lm -o /usr/local/sbin/nx769j-imu-daemon
gcc -O3 /usr/local/src/nx769j_fb_wl.c -o /usr/local/bin/nx769j-fb-wl
gcc -O2 -Wall -Wextra /usr/local/src/nx769j_vkbd.c -o /usr/local/bin/nx769j-vkbd
gcc -std=c11 -O2 -Wall -Wextra /usr/local/src/nx769j-fan.c -o /usr/local/bin/nx769j-fan
gcc -std=c11 -O2 -Wall -Wextra -Werror /usr/local/src/nx769j_perf.c -o /usr/local/bin/nx769j-perf
gcc -std=c11 -O2 -Wall -Wextra /usr/local/src/nx769j_powerkey.c -o /usr/local/sbin/nx769j-powerkey
gcc -O2 -Wall -Wextra -Werror -nostdlib -static /usr/local/src/nx_dm_tool.c -o /usr/local/sbin/nx_dm_tool
gcc -DNX_NOSTDLIB_ARM64 -O2 -Wall -Wextra -Werror -nostdlib -static /usr/local/src/nx769j_slotctl.c -o /usr/local/sbin/nx769j-slotctl

chmod 0755 /usr/local/libexec/nx769j-hexagonrpcd /usr/local/sbin/nx769j-imu-daemon \
  /usr/local/bin/nx769j-fb-wl /usr/local/bin/nx769j-vkbd /usr/local/bin/nx769j-fan \
  /usr/local/bin/nx769j-perf /usr/local/sbin/nx769j-powerkey /usr/local/sbin/nx_dm_tool
chmod 4755 /usr/local/sbin/nx769j-slotctl
'

# 9. Permissions, sudoers, SSH host keys & systemd enables/masks
chmod 0440 "${STAGE_ROOT}/etc/sudoers.d/90-nx769j-wheel" "${STAGE_ROOT}/etc/sudoers.d/10-nx769j-perf"
chmod +x "${STAGE_ROOT}/usr/local/bin/"* "${STAGE_ROOT}/usr/local/sbin/"* 2>/dev/null || true

chroot "$STAGE_ROOT" /usr/bin/ssh-keygen -A
mkdir -p "${STAGE_ROOT}/root/.ssh" "${STAGE_ROOT}/home/developer/.ssh"
chmod 0700 "${STAGE_ROOT}/root/.ssh" "${STAGE_ROOT}/home/developer/.ssh"
touch "${STAGE_ROOT}/root/.ssh/authorized_keys" "${STAGE_ROOT}/home/developer/.ssh/authorized_keys"
chmod 0600 "${STAGE_ROOT}/root/.ssh/authorized_keys" "${STAGE_ROOT}/home/developer/.ssh/authorized_keys"
if [[ -n "$SSH_PUBKEY_FILE" && -f "$SSH_PUBKEY_FILE" ]]; then
  cat "$SSH_PUBKEY_FILE" >> "${STAGE_ROOT}/root/.ssh/authorized_keys"
  cat "$SSH_PUBKEY_FILE" >> "${STAGE_ROOT}/home/developer/.ssh/authorized_keys"
fi
chown -R 1000:1000 "${STAGE_ROOT}/home/developer"

if [[ -f "${STAGE_ROOT}/etc/ssh/sshd_config" ]] && ! grep -Fq "sshd_config.d/*.conf" "${STAGE_ROOT}/etc/ssh/sshd_config"; then
  sed -i '1i Include /etc/ssh/sshd_config.d/*.conf' "${STAGE_ROOT}/etc/ssh/sshd_config"
fi

echo "[stage] Enabling NX769J systemd services & masks..."
WANTS_DIR="${STAGE_ROOT}/etc/systemd/system/multi-user.target.wants"
mkdir -p "$WANTS_DIR"
for unit in \
  sshd.service NetworkManager.service bluetooth.service upower.service \
  nx769j-usb-gadget.service nx769j-fan-init.service nx769j-aw88261.service \
  nx769j-charge-limit.service hexagonrpcd-rootpd.service hexagonrpcd.service \
  nx769j-imu-daemon.service iio-sensor-proxy.service nx769j-powerkey.service \
  nx769j-perf.service nx769j-audio-ready.service; do
  if [[ -f "${STAGE_ROOT}/etc/systemd/system/${unit}" ]]; then
    ln -sfn "/etc/systemd/system/${unit}" "${WANTS_DIR}/${unit}"
  else
    ln -sfn "/usr/lib/systemd/system/${unit}" "${WANTS_DIR}/${unit}"
  fi
done

for masked in qrtr-ns.service sleep.target suspend.target hibernate.target hybrid-sleep.target; do
  ln -sfn /dev/null "${STAGE_ROOT}/etc/systemd/system/${masked}"
done

if [[ ! -f "${STAGE_ROOT}/swapfile" ]]; then
  fallocate -l 4G "${STAGE_ROOT}/swapfile" && chmod 0600 "${STAGE_ROOT}/swapfile" && mkswap "${STAGE_ROOT}/swapfile"
fi

echo "[stage] Base Arch Linux ARM stage complete at $STAGE_ROOT"
