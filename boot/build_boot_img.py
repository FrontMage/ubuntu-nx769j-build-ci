#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Build a self-contained Android `boot.img` (header v4) containing:
  1. 8 KiB ARM64 BootShim (initializes RAM console + HUD, clears bottom panel,
     stages Image + patched Linux DTB + initrd into DRAM, jumps to U-Boot)
  2. Patched mainline `u-boot-qcom` (1116x2480 simple-framebuffer vidconsole HUD,
     stubbed serial polling relocs for zero-hang boot, BootMenu + fastboot gadget)
  3. External cpio.gz initramfs overlay:
     - USB2 High-Speed ConfigFS NCM early shell networking (10.42.0.2/24)
     - Out-of-tree/early module load (`dm-mod.ko`, `pwrseq-qcom-wcn.ko`)
     - Static `nx_dm_tool` mapping `/dev/nxroot` over F2FS-pinned extents on `/dev/sda12`
     - Zero-handle atomic rootfs pivot / rollback + offline `e2fsck`/`resize2fs` grow
     - Boot-time injection of fixed `aw88261-init`, `nx769j_fb_wl.c`, `nx769j_vkbd.c`
  4. Patched mainline `nx769j-linux.dtb` via `kernel/dts/patch_nx769j_dtb.py`
  5. Mainline ARM64 Linux kernel `Image` (loaded at `0xb0000000`)

All outputs are built cleanly from inputs without proprietary device identifiers.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "kernel" / "dts"))
import patch_nx769j_dtb  # noqa: E402

EXPECTED_UBOOT_BIN_LEN = 0x15CD50
EXPECTED_UBOOT_BIN_SHA256 = "af23d4d522bfbee7276f750d9ed635fbccf00c916247833ccd34a421d93a8874"

INITRD_PAYLOAD_OFF = 0x200000
DTB_PAYLOAD_OFF = 0x380000
LNX_PAYLOAD_OFF = 0x400000
TOTAL_IMG_SIZE = 0x4000000  # 64 MiB

INITRD_STAGE_ADDR = 0xAF800000
DTB_STAGE_ADDR = 0xAFE00000
LNX_STAGE_ADDR = 0xB0000000

# Standard 8x8 ASCII bitmap font for 0x20..0x7f (96 glyphs * 8 bytes = 768 bytes, MSB = leftmost pixel)
FONT_8X8_HEX = (
    "0000000000000000" "183c3c1818001800" "6c6c240000000000" "6c6cfe6cfe6c6c00"  #  !"#
    "183e603c067c1800" "00c6cc183066c600" "386c3876dcce7600" "1818300000000000"  # $%&'
    "0c18303030180c00" "30180c0c0c183000" "00663cff3c660000" "0018187e18180000"  # ()*+
    "0000000000181830" "0000007e00000000" "0000000000181800" "060c183060c08000"  # ,-./
    "7cc6cedef6e67c00" "1838181818187e00" "7cc6061c3066fe00" "7cc6063c06c67c00"  # 0123
    "1c3c6cccfe0c1e00" "fec0fc0606c67c00" "3860c0fcc6c67c00" "fec60c1830303000"  # 4567
    "7cc6c67cc6c67c00" "7cc6c67e060c7800" "0018180000181800" "0018180000181830"  # 89:;
    "0c18306030180c00" "00007e00007e0000" "6030180c18306000" "7cc60c1818001800"  # <=>?
    "7cc6dededec07c00" "386cc6fec6c6c600" "fcc6c6fcc6c6fc00" "3c66c0c0c0663c00"  # @ABC
    "f8ccc6c6c6ccf800" "fec2c8f8c8c2fe00" "fec2c8f8c8c0c000" "3c66c0c0ce663e00"  # DEFG
    "c6c6c6fec6c6c600" "3c18181818183c00" "1e0c0c0cccce7800" "e6666c786c66e600"  # HIJK
    "f06060606266fe00" "c6eefefed6c6c600" "c6e6f6decec6c600" "7cc6c6c6c6c67c00"  # LMNO
    "fcc6c6fcc0c0c000" "7cc6c6c6d6de7c06" "fcc6c6fccac6c600" "7cc6c07c06c67c00"  # PQRS
    "7e5a181818183c00" "c6c6c6c6c6c67c00" "c6c6c6c6c66c3800" "c6c6c6d6feeec600"  # TUVW
    "c6c66c386cc6c600" "6666663c18183c00" "fec68c183266fe00" "3c30303030303c00"  # XYZ[
    "c06030180c060200" "3c0c0c0c0c0c3c00" "10386cc600000000" "00000000000000ff"  # \]^_
    "30180c0000000000" "0000780c7ccc7600" "e0607c666666dc00" "00007cc6c0c67c00"  # `abc
    "1c0c7ccc66667600" "00007cc6fec07c00" "3c6660f86060f000" "000076cccc7c0cf8"  # defg
    "e0606c766666e600" "1800381818183c00" "060006060666663c" "e060666c786ce600"  # hijk
    "3818181818183c00" "0000ecfed6d6c600" "0000dc6666666600" "00007cc6c6c67c00"  # lmno
    "0000dc66667c60f0" "000076cc663c0c1e" "0000dc766060f000" "00007cc07c06fc00"  # pqrs
    "3030fc3030361c00" "0000ccce66667600" "0000c6c6c66c3800" "0000c6d6fefec600"  # tuvw
    "0000c66c386cc600" "0000c6c6ce76067c" "0000fe8c1832fe00" "0e18187018180e00"  # xyz{
    "1818180018181800" "7018180e18187000" "76dc000000000000" "ffffffffffffffff"  # |}~DEL
)
FONT_BYTES = bytes.fromhex(FONT_8X8_HEX)
assert len(FONT_BYTES) == 768

UBOOT_DTS = """/dts-v1/;

/ {
\tmodel = "nubia REDMAGIC 9 Pro (NX769J)";
\tcompatible = "nubia,nx769j", "qcom,sm8650";
\t#address-cells = <2>;
\t#size-cells = <2>;

\taliases {
\t\tserial0 = "/ramcon-uart";
\t};

\tchosen {
\t\tstdout-path = "serial0:115200n8";
\t};

\tmemory@a2000000 {
\t\tdevice_type = "memory";
\t\treg = <0x00 0xa2000000 0x00 0x35c00000>,
\t\t      <0x08 0xc0000000 0x02 0x3e000000>,
\t\t      <0x0b 0x00000000 0x01 0x00000000>;
\t};

\tpsci {
\t\tcompatible = "arm,psci-1.0";
\t\tmethod = "smc";
\t};

\tramcon-uart {
\t\tbootph-all;
\t\tcompatible = "qcom,geni-debug-uart";
\t\tstatus = "okay";
\t};

\tframebuffer@e3940000 {
\t\tbootph-all;
\t\tcompatible = "simple-framebuffer";
\t\treg = <0x00 0xe3940000 0x00 0x00a8fc00>;
\t\twidth = <1116>;
\t\theight = <2480>;
\t\tstride = <4464>;
\t\trot = <0>;
\t\tformat = "a8r8g8b8";
\t\tstatus = "okay";
\t};

\tgpio-keys {
\t\tcompatible = "gpio-keys";

\t\tkey-vol-up {
\t\t\tlabel = "Volume Up";
\t\t\tgpios = <&pm8550_gpios 6 1>;
\t\t\tlinux,code = <103>;
\t\t\tdebounce-interval = <15>;
\t\t};
\t};

\tsoc: soc@0 {
\t\tcompatible = "simple-bus";
\t\t#address-cells = <2>;
\t\t#size-cells = <2>;
\t\tranges;

\t\tspmi@c400000 {
\t\t\tcompatible = "qcom,spmi-pmic-arb";
\t\t\treg = <0x00 0x0c400000 0x00 0x00003000>,
\t\t\t      <0x00 0x0c500000 0x00 0x00400000>,
\t\t\t      <0x00 0x0c440000 0x00 0x00080000>,
\t\t\t      <0x00 0x0c4c0000 0x00 0x00020000>,
\t\t\t      <0x00 0x0c42d000 0x00 0x00004000>;
\t\t\treg-names = "core", "chnls", "obsrvr", "intr", "cnfg";
\t\t\tqcom,ee = <0>;
\t\t\tqcom,channel = <0>;
\t\t\t#address-cells = <2>;
\t\t\t#size-cells = <0>;

\t\t\tpmic@0 {
\t\t\t\tcompatible = "qcom,pmk8550", "qcom,spmi-pmic";
\t\t\t\treg = <0x0 0x0>;
\t\t\t\t#address-cells = <1>;
\t\t\t\t#size-cells = <0>;

\t\t\t\tpon@1300 {
\t\t\t\t\tcompatible = "qcom,pmk8350-pon";
\t\t\t\t\treg = <0x1300>;

\t\t\t\t\tpwrkey {
\t\t\t\t\t\tcompatible = "qcom,pmk8350-pwrkey";
\t\t\t\t\t\tlinux,code = <28>;
\t\t\t\t\t};

\t\t\t\t\tresin {
\t\t\t\t\t\tcompatible = "qcom,pmk8350-resin";
\t\t\t\t\t\tlinux,code = <114>;
\t\t\t\t\t};
\t\t\t\t};
\t\t\t};

\t\t\tpmic@1 {
\t\t\t\tcompatible = "qcom,pm8550", "qcom,spmi-pmic";
\t\t\t\treg = <0x1 0x0>;
\t\t\t\t#address-cells = <1>;
\t\t\t\t#size-cells = <0>;

\t\t\t\tpm8550_gpios: gpio@8800 {
\t\t\t\t\tcompatible = "qcom,pm8550-gpio";
\t\t\t\t\treg = <0x8800>;
\t\t\t\t\tgpio-controller;
\t\t\t\t\t#gpio-cells = <2>;
\t\t\t\t\tgpio-ranges = <&pm8550_gpios 0 0 12>;
\t\t\t\t};
\t\t\t};
\t\t};
\t};
};
"""


def build_init_script(gadget_serial: str, ssh_pubkey: str) -> bytes:
    safe_serial = gadget_serial.replace('"', "")
    if ssh_pubkey.strip():
        quoted_key = ssh_pubkey.strip().replace("'", "'\"'\"'")
        ssh_inject_block = f"""        PUBKEY='{quoted_key}'
        grep -Fq "$PUBKEY" /newroot/root/.ssh/authorized_keys 2>/dev/null || echo "$PUBKEY" >> /newroot/root/.ssh/authorized_keys
        chmod 0700 /newroot/root/.ssh
        chmod 0600 /newroot/root/.ssh/authorized_keys
        for uhome in /newroot/home/arch /newroot/home/developer; do
          if [ -d "$uhome" ]; then
            mkdir -p "$uhome/.ssh"
            grep -Fq "$PUBKEY" "$uhome/.ssh/authorized_keys" 2>/dev/null || echo "$PUBKEY" >> "$uhome/.ssh/authorized_keys"
            chmod 0700 "$uhome/.ssh"
            chmod 0600 "$uhome/.ssh/authorized_keys"
            chown -R 1000:1000 "$uhome/.ssh" 2>/dev/null || true
          fi
        done"""
    else:
        ssh_inject_block = """        chmod 0700 /newroot/root/.ssh 2>/dev/null || true"""

    script = f"""#!/bin/sh
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mkdir -p /dev/pts /dev/shm /proc /sys /newroot /run
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys

echo "========================================================="
echo " NX769J Mainline Linux 7.1.1 -> Omarchy Mobile"
echo "========================================================="

# 1) Bring up USB2 High-Speed ConfigFS NCM (10.42.0.2/24) early
mount -t configfs none /sys/kernel/config 2>/dev/null
G=/sys/kernel/config/usb_gadget/g1
if mkdir -p "$G/strings/0x409" "$G/configs/c.1/strings/0x409" "$G/functions/ncm.usb0" 2>/dev/null; then
  echo 0x1d6b > "$G/idVendor"
  echo 0x0104 > "$G/idProduct"
  echo "RedMagic" > "$G/strings/0x409/manufacturer"
  echo "NX769J Omarchy Mobile" > "$G/strings/0x409/product"
  echo "{safe_serial}" > "$G/strings/0x409/serialnumber"
  echo "USB NCM" > "$G/configs/c.1/strings/0x409/configuration"
  echo "4e:d9:6d:8d:1b:8a" > "$G/functions/ncm.usb0/host_addr" 2>/dev/null || true
  echo "26:91:a3:2c:1c:76" > "$G/functions/ncm.usb0/dev_addr" 2>/dev/null || true
  ln -s "$G/functions/ncm.usb0" "$G/configs/c.1/" 2>/dev/null
  for u in /sys/class/udc/*; do
    [ -e "$u" ] && basename "$u" > "$G/UDC" && break
  done
  for i in $(seq 1 25); do
    [ -e /sys/class/net/usb0 ] && break
    sleep 0.1
  done
  ifconfig usb0 10.42.0.2 netmask 255.255.255.0 up 2>/dev/null
  echo "[initrd] USB NCM active on usb0 (10.42.0.2)"
fi

# 2) Load dm-mod.ko (provides dm-linear) + pwrseq-qcom-wcn.ko (early WCN7850 PMU sequencing)
echo "[initrd] Loading dm-mod.ko & pwrseq-qcom-wcn.ko..."
insmod /lib/modules/dm-mod.ko
insmod /lib/modules/pwrseq-qcom-wcn.ko 2>/dev/null || true

# 3) Wait for UFS userdata partition (/dev/sda12)
echo "[initrd] Waiting for UFS /dev/sda12..."
for i in $(seq 1 60); do
  mdev -s 2>/dev/null
  [ -b /dev/sda12 ] && break
  sleep 0.2
done
chmod 0666 /dev/null /dev/zero /dev/full /dev/random /dev/urandom /dev/tty /dev/ptmx 2>/dev/null
chmod 0755 /dev 2>/dev/null

if [ -b /dev/sda12 ] && [ -s /etc/rootfs.extents ]; then
  echo "[initrd] Mapping Omarchy Mobile dm-linear (/dev/nxroot) over /dev/sda12..."
  if /bin/nx_dm_tool dm-up nxroot /dev/sda12 /etc/rootfs.extents; then
    if /bin/nx_dm_tool check-ext4 /dev/nxroot; then
      echo "[initrd] Mounting /dev/nxroot (ext4) rw..."
      if mount -t ext4 -o rw,noatime /dev/nxroot /newroot; then
        # Zero-handle O(1) atomic rootfs swap & emergency rollback BEFORE PID 1
        if [ -f /newroot/.rollback-old-kubuntu ] && [ -d /newroot/old-kubuntu/usr ]; then
          echo "[initrd] Rolling back /newroot from /newroot/old-kubuntu..."
          mkdir -p /newroot/arch-rolled-back
          for d in .config bin boot etc home lib lib64 media mnt opt root sbin srv usr var; do
            if [ -e "/newroot/$d" ] || [ -L "/newroot/$d" ]; then
              [ ! -e "/newroot/arch-rolled-back/$d" ] && [ ! -L "/newroot/arch-rolled-back/$d" ] && mv "/newroot/$d" "/newroot/arch-rolled-back/$d"
            fi
          done
          for d in .config bin boot etc home lib lib64 media mnt opt root sbin srv usr var; do
            if [ -e "/newroot/old-kubuntu/$d" ] || [ -L "/newroot/old-kubuntu/$d" ]; then
              mv "/newroot/old-kubuntu/$d" "/newroot/$d"
            fi
          done
          mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/run /newroot/tmp /newroot/mnt /newroot/media
          chmod 1777 /newroot/tmp
          rm -f /newroot/.rollback-old-kubuntu /newroot/old-kubuntu/.kubuntu-moved
          sync
        elif [ -f /newroot/.pivot-arch-stage ]; then
          if ( [ -f /newroot/arch-stage/.ready ] && [ -L /newroot/arch-stage/lib ] && [ -x /newroot/arch-stage/usr/lib/systemd/systemd ] ) || ( [ -f /newroot/old-kubuntu/.kubuntu-moved ] && [ -d /newroot/arch-stage ] ); then
            echo "[initrd] Performing zero-handle atomic swap: /arch-stage -> / ..."
            mkdir -p /newroot/old-kubuntu
            if [ ! -f /newroot/old-kubuntu/.kubuntu-moved ]; then
              for d in .config bin boot etc home lib lib64 media mnt opt root sbin srv usr var; do
                if [ -e "/newroot/$d" ] || [ -L "/newroot/$d" ]; then
                  mv "/newroot/$d" "/newroot/old-kubuntu/$d"
                fi
              done
              echo "pivoted" > /newroot/old-kubuntu/.kubuntu-moved
            fi
            for d in bin boot etc home lib lib64 mnt opt root sbin srv usr var; do
              if [ -e "/newroot/arch-stage/$d" ] || [ -L "/newroot/arch-stage/$d" ]; then
                if [ ! -e "/newroot/$d" ] && [ ! -L "/newroot/$d" ]; then
                  mv "/newroot/arch-stage/$d" "/newroot/$d"
                fi
              fi
            done
            mkdir -p /newroot/dev /newroot/proc /newroot/sys /newroot/run /newroot/tmp /newroot/mnt /newroot/media
            chmod 1777 /newroot/tmp
            chmod 0755 /newroot/dev /newroot/proc /newroot/sys /newroot/run /newroot/mnt
            if [ -L /newroot/lib ] && [ -x /newroot/usr/lib/systemd/systemd ] && ( [ -L /newroot/sbin/init ] || [ -x /newroot/sbin/init ] ); then
              rm -rf /newroot/arch-stage /newroot/.pivot-arch-stage
              echo "OK" > /newroot/.omarchy-pivoted-ok
              sync
            else
              echo "[initrd] ERROR: Post-pivot sanity check failed; auto-restoring /old-kubuntu!"
              mkdir -p /newroot/arch-stage
              for d in bin boot etc home lib lib64 mnt opt root sbin srv usr var; do
                if [ -e "/newroot/$d" ] || [ -L "/newroot/$d" ]; then
                  mv "/newroot/$d" "/newroot/arch-stage/$d"
                fi
              done
              for d in .config bin boot etc home lib lib64 media mnt opt root sbin srv usr var; do
                if [ -e "/newroot/old-kubuntu/$d" ] || [ -L "/newroot/old-kubuntu/$d" ]; then
                  mv "/newroot/old-kubuntu/$d" "/newroot/$d"
                fi
              done
              rm -f /newroot/.pivot-arch-stage /newroot/old-kubuntu/.kubuntu-moved
              sync
            fi
          else
            echo "[initrd] WARNING: /.pivot-arch-stage present without valid /arch-stage/.ready; skipping!"
            rm -f /newroot/.pivot-arch-stage
          fi
        fi

        # Lossless ext4 check & grow when dm-linear /dev/nxroot > ext4 block count
        NX_DM_DEV=$(readlink -f /dev/nxroot 2>/dev/null || echo /dev/dm-0)
        NX_DM_BN=$(basename "$NX_DM_DEV")
        NX_BDEV_SEC=0
        if [ -r "/sys/class/block/$NX_DM_BN/size" ]; then
          NX_BDEV_SEC=$(cat "/sys/class/block/$NX_DM_BN/size" 2>/dev/null || echo 0)
        elif [ -r "/sys/class/block/dm-0/size" ]; then
          NX_BDEV_SEC=$(cat "/sys/class/block/dm-0/size" 2>/dev/null || echo 0)
        fi
        if [ "${{NX_BDEV_SEC:-0}}" -le 0 ] && command -v blockdev >/dev/null 2>&1; then
          NX_BDEV_SEC=$(( $(blockdev --getsize64 /dev/nxroot 2>/dev/null || echo 0) / 512 ))
        fi
        NX_BDEV_BLK=$(( NX_BDEV_SEC / 8 ))
        NX_FS_BLK=0
        if [ -x /newroot/usr/lib/ld-linux-aarch64.so.1 ] && [ -x /newroot/usr/bin/dumpe2fs ]; then
          NX_FS_BLK=$(/newroot/usr/lib/ld-linux-aarch64.so.1 --library-path /newroot/usr/lib /newroot/usr/bin/dumpe2fs -h /dev/nxroot 2>/dev/null | awk '/^Block count:/ {{print $3; exit}}')
        fi
        if [ -n "$NX_FS_BLK" ] && [ "${{NX_FS_BLK:-0}}" -gt 0 ] && [ "$NX_BDEV_BLK" -gt "$NX_FS_BLK" ]; then
          echo "[initrd] Growing /dev/nxroot ext4 from ${{NX_FS_BLK}} to ${{NX_BDEV_BLK}} 4K blocks..."
          NX_GREW=0
          if [ -x /newroot/usr/bin/e2fsck ] && [ -x /newroot/usr/bin/resize2fs ]; then
            rm -rf /run/nx-growbin
            mkdir -p /run/nx-growbin
            cp -Lf /newroot/usr/bin/e2fsck /newroot/usr/bin/resize2fs /run/nx-growbin/ 2>/dev/null || true
            for so in ld-linux-aarch64.so.1 libc.so.6 libext2fs.so.2 libcom_err.so.2 libblkid.so.1 libuuid.so.1 libe2p.so.2; do
              [ -e "/newroot/usr/lib/$so" ] && cp -Lf "/newroot/usr/lib/$so" "/run/nx-growbin/$so"
            done
            chmod 0755 /run/nx-growbin/ld-linux-aarch64.so.1 /run/nx-growbin/e2fsck /run/nx-growbin/resize2fs 2>/dev/null || true
            sync
            if [ -x /run/nx-growbin/ld-linux-aarch64.so.1 ] && [ -x /run/nx-growbin/e2fsck ] && [ -x /run/nx-growbin/resize2fs ] && umount /newroot 2>/dev/null; then
              printf '[options]\\n  broken_system_clock = 1\\n' > /run/nx-growbin/e2fsck.conf
              E2_RC=0
              E2FSCK_CONFIG=/run/nx-growbin/e2fsck.conf /run/nx-growbin/ld-linux-aarch64.so.1 --library-path /run/nx-growbin /run/nx-growbin/e2fsck -fy /dev/nxroot || E2_RC=$?
              if [ "$E2_RC" -le 2 ]; then
                if /run/nx-growbin/ld-linux-aarch64.so.1 --library-path /run/nx-growbin /run/nx-growbin/resize2fs /dev/nxroot; then
                  NX_GREW=1
                  echo "[initrd] Offline e2fsck + resize2fs /dev/nxroot SUCCEEDED!"
                fi
              fi
              mount -t ext4 -o rw,noatime /dev/nxroot /newroot
            fi
            rm -rf /run/nx-growbin
          fi
          if [ "$NX_GREW" != "1" ] && [ -x /newroot/usr/lib/ld-linux-aarch64.so.1 ] && [ -x /newroot/usr/bin/resize2fs ]; then
            /newroot/usr/lib/ld-linux-aarch64.so.1 --library-path /newroot/usr/lib /newroot/usr/bin/resize2fs /dev/nxroot && echo "[initrd] Online resize2fs /dev/nxroot SUCCEEDED!"
          fi
          sync
        fi

        mkdir -p /newroot/etc/ssh/sshd_config.d /newroot/root/.ssh
        if [ -f /newroot/etc/ssh/sshd_config ] && ! grep -Fq "sshd_config.d/*.conf" /newroot/etc/ssh/sshd_config; then
          sed -i '1i Include /etc/ssh/sshd_config.d/*.conf' /newroot/etc/ssh/sshd_config
        fi
        cat > /newroot/etc/ssh/sshd_config.d/00-nx769j.conf << 'SSHEOF'
PasswordAuthentication yes
KbdInteractiveAuthentication yes
PermitRootLogin yes
PubkeyAuthentication yes
SSHEOF
{ssh_inject_block}
        if [ -f /newroot/usr/local/sbin/nx769j-usb-gadget.sh ] && ! grep -q "169.254.72.2" /newroot/usr/local/sbin/nx769j-usb-gadget.sh; then
          sed -i 's|ip addr replace 10.42.0.2/24 dev usb0|ip addr replace 10.42.0.2/24 dev usb0\\n    ip addr replace 169.254.72.2/16 dev usb0|' /newroot/usr/local/sbin/nx769j-usb-gadget.sh
        fi
        mkdir -p /newroot/etc/modprobe.d /newroot/usr/local/src /newroot/usr/local/bin /newroot/usr/local/sbin
        if [ -f /etc/aw88261-init ]; then
          cp -f /etc/aw88261-init /newroot/usr/local/sbin/aw88261-init.new && chmod 0755 /newroot/usr/local/sbin/aw88261-init.new && mv -f /newroot/usr/local/sbin/aw88261-init.new /newroot/usr/local/sbin/aw88261-init
          sync
        fi
        if [ ! -f /newroot/usr/local/src/nx769j_fb_wl.c ] || grep -q "DMA_BUF_IOCTL_SYNC" /etc/nx769j_fb_wl.c; then
          cmp -s /etc/nx769j_fb_wl.c /newroot/usr/local/src/nx769j_fb_wl.c 2>/dev/null || cp -f /etc/nx769j_fb_wl.c /newroot/usr/local/src/nx769j_fb_wl.c
        fi
        cmp -s /etc/nx769j_vkbd.c /newroot/usr/local/src/nx769j_vkbd.c 2>/dev/null || cp -f /etc/nx769j_vkbd.c /newroot/usr/local/src/nx769j_vkbd.c
        if [ -f /newroot/etc/systemd/system/nx769j-fb-wl.service ] && ! grep -q "nx769j_fb_wl.c" /newroot/etc/systemd/system/nx769j-fb-wl.service; then
          sed -i '/^ExecStart=/i ExecStartPre=/bin/sh -c '\\''/usr/bin/gcc -O3 /usr/local/src/nx769j_fb_wl.c -o /usr/local/bin/nx769j-fb-wl.new && mv -f /usr/local/bin/nx769j-fb-wl.new /usr/local/bin/nx769j-fb-wl && chmod 0755 /usr/local/bin/nx769j-fb-wl'\\''' /newroot/etc/systemd/system/nx769j-fb-wl.service
        fi
        cat > /newroot/etc/modprobe.d/blacklist-nx769j-splash.conf << 'MODEOF'
options msm separate_gpu_kms=1
blacklist dispcc_sm8550
blacklist videocc_sm8550
blacklist camcc_sm8650
blacklist icc_bwmon
MODEOF
        ln -sf /usr/lib/systemd/system/multi-user.target /newroot/etc/systemd/system/default.target
        for tgt in sleep.target suspend.target hibernate.target hybrid-sleep.target; do
          ln -sf /dev/null "/newroot/etc/systemd/system/$tgt"
        done
        umount /sys/kernel/config 2>/dev/null
        umount /dev/pts 2>/dev/null
        mount --move /dev /newroot/dev 2>/dev/null || umount /dev 2>/dev/null
        umount /proc 2>/dev/null
        umount /sys 2>/dev/null
        echo "[initrd] Handing off to Omarchy Mobile systemd (/sbin/init)!"
        exec switch_root /newroot /sbin/init
      fi
    fi
  fi
fi

echo "[initrd] ERROR: rootfs mount failed! Starting rescue telnetd on 10.42.0.2:23 + console sh"
telnetd -p 23 -l /bin/sh 2>/dev/null
exec /bin/sh
"""
    return script.encode("utf-8")


def build_newc_cpio_gz(entries: list[tuple[str, int, bytes]]) -> bytes:
    cpio = bytearray()
    ino = 300000
    for name, mode, data in entries:
        ino += 1
        name_b = name.encode("ascii") + b"\x00"
        nlink = 2 if (mode & 0o170000) == 0o040000 else 1
        hdr = (
            f"070701"
            f"{ino:08x}{mode:08x}{0:08x}{0:08x}{nlink:08x}{0:08x}"
            f"{len(data):08x}{0:08x}{0:08x}{0:08x}{0:08x}"
            f"{len(name_b):08x}{0:08x}"
        ).encode("ascii")
        assert len(hdr) == 110
        cpio.extend(hdr)
        cpio.extend(name_b)
        while len(cpio) % 4:
            cpio.append(0)
        cpio.extend(data)
        while len(cpio) % 4:
            cpio.append(0)

    tr_name = b"TRAILER!!!\x00"
    hdr = (
        f"070701"
        f"{0:08x}{0:08x}{0:08x}{0:08x}{1:08x}{0:08x}"
        f"{0:08x}{0:08x}{0:08x}{0:08x}{0:08x}"
        f"{len(tr_name):08x}{0:08x}"
    ).encode("ascii")
    cpio.extend(hdr)
    cpio.extend(tr_name)
    while len(cpio) % 512:
        cpio.append(0)
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, compresslevel=9, mtime=0) as gz:
        gz.write(cpio)
    return buf.getvalue()


def assemble_snippet(asm_src: str, work_dir: pathlib.Path, asm_tool: tuple[str, list[str]]) -> bytes:
    s_path = work_dir / "tmp_snip.S"
    o_path = work_dir / "tmp_snip.o"
    s_path.write_text(".text\n" + asm_src + "\n", encoding="utf-8")
    bin_path, base_flags = asm_tool
    subprocess.check_call([bin_path] + base_flags + ["-o", str(o_path), str(s_path)])
    elf = o_path.read_bytes()
    e_shoff = struct.unpack_from("<Q", elf, 40)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", elf, 58)

    def sh(i: int) -> tuple[int, ...]:
        return struct.unpack_from("<IIQQQQIIQQ", elf, e_shoff + i * e_shentsize)

    shstr = elf[sh(e_shstrndx)[4] : sh(e_shstrndx)[4] + sh(e_shstrndx)[5]]
    for i in range(e_shnum):
        h = sh(i)
        nm = shstr[h[0] :].split(b"\x00", 1)[0]
        if h[1] in (4, 9) and b".text" in nm:
            raise RuntimeError(f"Unexpected relocation in snippet: {nm!r}")
    for i in range(e_shnum):
        h = sh(i)
        if shstr[h[0] :].split(b"\x00", 1)[0] == b".text":
            return elf[h[4] : h[4] + h[5]]
    raise RuntimeError("No .text section in assembled object")


def find_aarch64_assembler() -> tuple[str, list[str]]:
    for cand in ["/usr/bin/clang", shutil.which("clang")]:
        if cand and pathlib.Path(cand).exists():
            # Verify this clang supports aarch64-none-elf target
            probe = subprocess.run(
                [cand, "--target=aarch64-none-elf", "-x", "assembler", "-c", "-", "-o", "/dev/null"],
                input=b"nop\n",
                capture_output=True,
            )
            if probe.returncode == 0:
                return cand, ["--target=aarch64-none-elf", "-c"]
    gnu_as = shutil.which("aarch64-linux-gnu-as")
    if gnu_as:
        return gnu_as, []
    raise FileNotFoundError(
        "Need `clang` (with aarch64 target) or `aarch64-linux-gnu-as` to assemble BootShim/HUD snippets"
    )


def patch_at(buf: bytearray, off: int, code: bytes) -> None:
    buf[off : off + len(code)] = code


def encode_adrp(rd: int, pc_off: int, target_page: int = 0) -> bytes:
    imm = ((target_page & ~0xFFF) - (pc_off & ~0xFFF)) >> 12
    immlo = imm & 0x3
    immhi = (imm >> 2) & 0x7FFFF
    insn = (1 << 31) | (immlo << 29) | (0x10 << 24) | (immhi << 5) | (rd & 0x1F)
    return struct.pack("<I", insn)


def extract_modules_from_tar_zst(
    modules_tar_zst: pathlib.Path,
) -> tuple[bytes, bytes]:
    """Extract `dm-mod.ko` and `pwrseq-qcom-wcn.ko` from `modules.tar.zst`."""
    zstd = shutil.which("zstd")
    if not zstd:
        raise FileNotFoundError("`zstd` not found in PATH to decompress modules.tar.zst")

    resolved = modules_tar_zst.resolve()
    proc = subprocess.run([zstd, "-dcf", str(resolved)], capture_output=True, check=True)
    dm_mod: bytes | None = None
    pwrseq: bytes | None = None

    def maybe_decompress(name: str, data: bytes) -> bytes:
        if name.endswith(".zst"):
            return subprocess.run([zstd, "-dc"], input=data, capture_output=True, check=True).stdout
        if name.endswith(".xz"):
            import lzma

            return lzma.decompress(data)
        return data

    with tarfile.open(fileobj=io.BytesIO(proc.stdout), mode="r:*") as tf:
        for member in tf.getmembers():
            if not member.isfile():
                continue
            bn = pathlib.PurePosixPath(member.name).name
            if bn in ("dm-mod.ko", "dm-mod.ko.zst", "dm-mod.ko.xz"):
                f = tf.extractfile(member)
                if f:
                    dm_mod = maybe_decompress(bn, f.read())
            elif bn in ("pwrseq-qcom-wcn.ko", "pwrseq-qcom-wcn.ko.zst", "pwrseq-qcom-wcn.ko.xz"):
                f = tf.extractfile(member)
                if f:
                    pwrseq = maybe_decompress(bn, f.read())

    if dm_mod is None or pwrseq is None:
        raise FileNotFoundError(
            f"Missing required modules in {modules_tar_zst}: "
            f"dm-mod.ko={'ok' if dm_mod else 'MISSING'}, "
            f"pwrseq-qcom-wcn.ko={'ok' if pwrseq else 'MISSING'}"
        )
    return dm_mod, pwrseq


def ensure_nx_dm_tool(explicit_bin: str | None, work_dir: pathlib.Path) -> bytes:
    if explicit_bin:
        return pathlib.Path(explicit_bin).read_bytes()
    prebuilt = REPO_ROOT / "storage-dualboot" / "nx_dm_tool"
    if prebuilt.is_file():
        return prebuilt.read_bytes()
    cc = shutil.which("aarch64-linux-gnu-gcc") or shutil.which("gcc")
    if not cc:
        raise FileNotFoundError(
            "Pass --nx-dm-tool <ARM64 binary> or install `aarch64-linux-gnu-gcc` to auto-compile nx_dm_tool.c"
        )
    out_bin = work_dir / "nx_dm_tool"
    subprocess.check_call(
        [
            cc,
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-nostdlib",
            "-static",
            "-o",
            str(out_bin),
            str(REPO_ROOT / "storage-dualboot" / "nx_dm_tool.c"),
        ]
    )
    return out_bin.read_bytes()


def build_hud_asm() -> str:
    font_words_asm = "\n".join(
        f"    .byte 0x{FONT_BYTES[i]:02x},0x{FONT_BYTES[i+1]:02x},0x{FONT_BYTES[i+2]:02x},0x{FONT_BYTES[i+3]:02x},"
        f"0x{FONT_BYTES[i+4]:02x},0x{FONT_BYTES[i+5]:02x},0x{FONT_BYTES[i+6]:02x},0x{FONT_BYTES[i+7]:02x}"
        for i in range(0, 768, 8)
    )
    return f"""
.Lhud_putc:
    movz x2, #0xd7e0, lsl #16
    ldr  w3, [x2, #4]
    and  w3, w3, #0x0fffff
    add  x4, x2, #0x10
    strb w1, [x4, w3, uxtw]
    add  w5, w3, #1
    str  w5, [x2, #4]
    add  x4, x4, w3, uxtw
    dc   civac, x4

    ldr  w3, [x2, #8]           /* col */
    ldr  w4, [x2, #12]          /* row */
    and  w5, w1, #0xff
    cmp  w5, #0x0d
    b.ne 1f
    mov  w3, #0
    b    .Lhud_save_pos
1:
    cmp  w5, #0x0a
    b.ne 2f
    mov  w3, #0
    add  w4, w4, #1
    b    .Lhud_wrap_row
2:
    sub  w6, w5, #0x20
    cmp  w6, #96
    b.lo 3f
    mov  w6, #95
3:
    adr  x7, .Lfont8x8
    add  x7, x7, w6, uxtw #3

    mov  w8, #22
    mul  w8, w4, w8
    add  w8, w8, #1280
    mov  w9, #4464
    umull x8, w8, w9
    lsl  w9, w3, #4
    add  w9, w9, #16
    lsl  w9, w9, #2
    add  x8, x8, x9
    movz x9, #0xe394, lsl #16
    add  x8, x9, x8

    movz w10, #0xff90, lsl #16
    movk w10, #0xd8ff
    movz w11, #0xff08, lsl #16
    movk w11, #0x1424

    mov  w12, #0
.Lglyph_row:
    ldrb w13, [x7, w12, uxtw]
    mov  x14, x8
    mov  x15, #4464
    add  x15, x8, x15
    mov  w9, #7
.Lglyph_bit:
    lsr  w6, w13, w9
    tst  w6, #1
    csel w6, w10, w11, ne
    str  w6, [x14], #4
    str  w6, [x14], #4
    str  w6, [x15], #4
    str  w6, [x15], #4
    subs w9, w9, #1
    b.pl .Lglyph_bit

    dc   civac, x8
    add  x6, x8, #64
    dc   civac, x6
    mov  x6, #4464
    add  x6, x8, x6
    dc   civac, x6
    add  x6, x6, #64
    dc   civac, x6
    mov  x6, #8928
    add  x8, x8, x6
    add  w12, w12, #1
    cmp  w12, #8
    b.lo .Lglyph_row

    add  w3, w3, #1
    cmp  w3, #66
    b.lo .Lhud_save_pos
    mov  w3, #0
    add  w4, w4, #1
.Lhud_wrap_row:
    cmp  w4, #52
    b.lo .Lhud_save_pos
    mov  w4, #0
.Lhud_save_pos:
    str  w3, [x2, #8]
    str  w4, [x2, #12]
    dc   civac, x2
    dsb  sy
    ret

.Lhud_hex64:
    mov  x16, x30
    mov  x17, x1
    mov  w0, #60
8:
    lsr  x1, x17, x0
    and  w1, w1, #0xf
    cmp  w1, #10
    add  w3, w1, #0x30
    add  w4, w1, #0x57
    csel w1, w3, w4, lo
    bl   .Lhud_putc
    tst  w0, #0xf
    b.ne 7f
    cbz  w0, 7f
    mov  w1, #0x20
    bl   .Lhud_putc
7:
    subs w0, w0, #4
    b.pl 8b
    ret  x16

.balign 8
.Lfont8x8:
{font_words_asm}
"""


def patch_uboot_bin(
    uboot_raw: bytes,
    work_dir: pathlib.Path,
    asm_tool: tuple[str, list[str]],
    hud_asm: str,
    linux_img_copy_len: int,
    initrd_exact_len: int,
) -> bytearray:
    if len(uboot_raw) < EXPECTED_UBOOT_BIN_LEN:
        raise ValueError(f"u-boot.bin too small ({len(uboot_raw)} < 0x{EXPECTED_UBOOT_BIN_LEN:x})")
    ub = bytearray(uboot_raw[:EXPECTED_UBOOT_BIN_LEN])
    got_sha = hashlib.sha256(ub).hexdigest()
    if got_sha != EXPECTED_UBOOT_BIN_SHA256:
        raise ValueError(
            f"u-boot.bin (first 0x15cd50 bytes) SHA-256 mismatch: {got_sha} != {EXPECTED_UBOOT_BIN_SHA256}"
        )

    ret0_stub = assemble_snippet("mov w0, #0\nret", work_dir, asm_tool)
    eagain_stub = assemble_snippet("mov w0, #-11\nret", work_dir, asm_tool)
    skip_reloc_insn = assemble_snippet("mov x0, #0x800", work_dir, asm_tool)
    early_alloc_insn = assemble_snippet("movn x1, #0x4, lsl #16", work_dir, asm_tool)
    early_limit_insn = assemble_snippet("movz x0, #0x4, lsl #16", work_dir, asm_tool)

    repoint_ret0 = {0x9FC61B44, 0x9FC620C8, 0x9FC61924, 0x9FC61CB4, 0x9FC61C78}
    repoint_eagain = {0x9FC61A34}
    reloc_patched = 0
    for off in range(0x135C90, 0x15CD50, 24):
        _r_off, _r_info, r_add = struct.unpack_from("<QQQ", ub, off)
        if r_add in repoint_ret0:
            struct.pack_into("<Q", ub, off + 16, 0x9FC61924)
            reloc_patched += 1
        elif r_add in repoint_eagain:
            struct.pack_into("<Q", ub, off + 16, 0x9FC6192C)
            reloc_patched += 1
    assert reloc_patched == 6, f"Expected 6 serial relocs, patched {reloc_patched}"

    hud_pack = assemble_snippet(
        f"""
.Lputc_hook:
    mov  x16, x30
    mov  w17, w0
    mov  w1, w0
    bl   .Lhud_putc
    mov  w0, w17
    mov  x30, x16
    cbz  x18, 1f
    b    .Lputc_hook + (0x330d8 - 0x61940)
1:
    ret

.balign 64
.Lputs_hook:
    mov  x16, x30
    mov  x17, x0
2:
    ldrb w1, [x0], #1
    cbz  w1, 3f
    bl   .Lhud_putc
    b    2b
3:
    mov  x0, x17
    mov  x1, x17
    mov  x30, x16
    cbz  x18, 4f
    b    .Lputc_hook + (0x33100 - 0x61940)
4:
    ret

.balign 64
.Lmemmap_hook:
    ubfiz x0, x0, #5, #32
    add   x1, x1, x0
    mov   x4, #0x310
    movz  x2, #0xd7e0, lsl #16
    movz  x3, #0x0020, lsl #16
    stp   x2, x2, [x1], #16
    stp   x3, x4, [x1], #16
    movz  x2, #0xe380, lsl #16
    movz  x3, #0x02c0, lsl #16
    stp   x2, x2, [x1], #16
    stp   x3, x4, [x1], #16
    str   xzr, [x1]
    mov   x0, #-1
    stp   x0, xzr, [x1, #8]
    b     .Lputc_hook + (0x54c4 - 0x61940)

.balign 64
.Lub_exc:
    msr  daifset, #0xf
    mov  x19, x30
    mov  x20, sp
    mov  w1, #0x0a
    bl   .Lhud_putc
    mov  w1, #0x21
    bl   .Lhud_putc
    mov  w1, #0x45
    bl   .Lhud_putc
    mov  w1, #0x3d
    bl   .Lhud_putc
    mrs  x1, esr_el1
    bl   .Lhud_hex64
    mov  w1, #0x0a
    bl   .Lhud_putc
    mov  w1, #0x50
    bl   .Lhud_putc
    mov  w1, #0x3d
    bl   .Lhud_putc
    mrs  x1, elr_el1
    bl   .Lhud_hex64
    mov  w1, #0x0a
    bl   .Lhud_putc
    mov  w1, #0x46
    bl   .Lhud_putc
    mov  w1, #0x3d
    bl   .Lhud_putc
    mrs  x1, far_el1
    bl   .Lhud_hex64
    mov  w1, #0x0a
    bl   .Lhud_putc
    mov  w1, #0x53
    bl   .Lhud_putc
    mov  w1, #0x3d
    bl   .Lhud_putc
    mov  x1, x20
    bl   .Lhud_hex64
    mov  w1, #0x0a
    bl   .Lhud_putc
    mov  w1, #0x4c
    bl   .Lhud_putc
    mov  w1, #0x3d
    bl   .Lhud_putc
    mov  x1, x19
    bl   .Lhud_hex64
    mov  w1, #0x0a
    bl   .Lhud_putc
9:
    wfi
    b    9b
"""
        + hud_asm,
        work_dir,
        asm_tool,
    )
    assert len(hud_pack) <= (0x621B0 - 0x61940), f"hud_pack too big: {len(hud_pack)}"

    patch_at(ub, 0x422C, skip_reloc_insn)
    patch_at(ub, 0x4ED0, ret0_stub)
    patch_at(ub, 0x548, encode_adrp(1, 0x548, 0x0))
    patch_at(ub, 0x2E91C, assemble_snippet("nop", work_dir, asm_tool))
    patch_at(ub, 0x2E920, encode_adrp(0, 0x2E920, 0x0))
    patch_at(ub, 0x99920, encode_adrp(19, 0x99920, 0x0))
    patch_at(ub, 0x2ADF4, early_alloc_insn)
    patch_at(ub, 0x3476C, early_limit_insn)
    patch_at(ub, 0x5514, assemble_snippet(f"b . + {0x5550 - 0x5514}", work_dir, asm_tool))
    patch_at(ub, 0x54B4, assemble_snippet(f"b . + {0x619c0 - 0x54b4}", work_dir, asm_tool))
    patch_at(ub, 0x64AE4, assemble_snippet("nop", work_dir, asm_tool))
    patch_at(ub, 0x61924, ret0_stub)
    patch_at(ub, 0x6192C, eagain_stub)
    patch_at(ub, 0x61940, hud_pack)

    patch_at(ub, 0x330D4, assemble_snippet(f"b . + {0x61940 - 0x330d4}", work_dir, asm_tool))
    patch_at(ub, 0x330F8, assemble_snippet(f"b . + {0x61980 - 0x330f8}", work_dir, asm_tool))
    for vec in (0x3800, 0x3980, 0x3A00, 0x3B80):
        patch_at(ub, vec, assemble_snippet(f"b . + {0x61a00 - vec}", work_dir, asm_tool))

    env_start = ub.find(b"stdin=serial,button-kbd\x00")
    env_end = ub.find(b"\x00\x00", env_start) + 2
    orig_env_len = env_end - env_start
    new_vars = [
        b"stdin=serial,button-kbd",
        b"stdout=serial,vidconsole",
        b"stderr=serial,vidconsole",
        b"bootdelay=0",
        b"bootmenu_delay=1",
        b"m=sleep 6;bootmenu",
        f"bootmenu_0=Omarchy Mobile=bootefi {LNX_STAGE_ADDR:x}:{linux_img_copy_len:x} "
        f"{INITRD_STAGE_ADDR:x}:{initrd_exact_len:x} {DTB_STAGE_ADDR:x};run m".encode(),
        b"bootmenu_1=SysInfo=bdinfo;run m",
        b"bootmenu_2=FB=fastboot -l c0000000 usb 0",
        b"bootmenu_3=Reboot=reset",
        b"bootcmd=bootmenu",
    ]
    new_env = b"\x00".join(new_vars) + b"\x00\x00"
    assert len(new_env) <= orig_env_len, f"Env too big: {len(new_env)} > {orig_env_len}"
    ub[env_start:env_end] = new_env + b"\x00" * (orig_env_len - len(new_env))
    assert len(ub) == EXPECTED_UBOOT_BIN_LEN
    return ub


def main() -> int:
    ap = argparse.ArgumentParser(description="Build NX769J Omarchy Mobile boot.img (header v4)")
    ap.add_argument("--image", required=True, help="Path to mainline ARM64 kernel Image")
    ap.add_argument("--dtb", default=None, help="Path to unpatched mainline nx769j.dtb (auto-patched)")
    ap.add_argument("--patched-dtb", default=None, help="Or pass an already-patched nx769j-linux.dtb")
    ap.add_argument("--modules", default=None, help="Path to modules.tar.zst (to extract dm-mod & pwrseq-qcom-wcn)")
    ap.add_argument("--dm-mod-ko", default=None, help="Or explicit path to dm-mod.ko")
    ap.add_argument("--pwrseq-ko", default=None, help="Or explicit path to pwrseq-qcom-wcn.ko")
    ap.add_argument("--uboot-bin", default=None, help="Path to verified u-boot.bin (default: boot/cache/u-boot.bin)")
    ap.add_argument("--extents", default=None, help="Path to rootfs.extents or rootfs_256g.extents")
    ap.add_argument("--allow-empty-extents", action="store_true", help="Build rescue boot image without rootfs.extents")
    ap.add_argument("--nx-dm-tool", default=None, help="Path to static ARM64 nx_dm_tool (auto-compiled if omitted)")
    ap.add_argument("--busybox", default=None, help="Optional static ARM64 busybox to bundle in initrd /bin/busybox")
    ap.add_argument("--bt-mac", default="02:00:00:76:9a:01", help="Linux Bluetooth MAC address injected into DTB")
    ap.add_argument("--ssh-pubkey", default="", help="Optional SSH public key string injected into initrd")
    ap.add_argument("--ssh-pubkey-file", default=None, help="Optional file containing SSH public key")
    ap.add_argument("--gadget-serial", default="nx769j-omarchy", help="USB ConfigFS serial string")
    ap.add_argument("--dtc", default=None, help="Optional explicit path to `dtc`")
    ap.add_argument("--work-dir", default=None, help="Optional directory to keep intermediate artifacts")
    ap.add_argument("-o", "--out", default="uboot-omarchy-nx769j-boot.img", help="Output boot.img path")
    args = ap.parse_args()

    if not args.dtb and not args.patched_dtb:
        ap.error("Pass either --dtb <stock-mainline-nx769j.dtb> or --patched-dtb <nx769j-linux.dtb>")
    if not args.extents and not args.allow_empty_extents:
        ap.error("Pass --extents <rootfs.extents> (or --allow-empty-extents for rescue-only image)")

    ssh_pubkey = args.ssh_pubkey
    if args.ssh_pubkey_file:
        ssh_pubkey = pathlib.Path(args.ssh_pubkey_file).read_text(encoding="utf-8").strip()

    uboot_path = (
        pathlib.Path(args.uboot_bin)
        if args.uboot_bin
        else pathlib.Path(__file__).resolve().parent / "cache" / "u-boot.bin"
    )
    if not uboot_path.is_file():
        raise FileNotFoundError(
            f"Missing u-boot.bin at {uboot_path}. Run `python3 boot/fetch_uboot.py` first or pass --uboot-bin."
        )

    temp_ctx = None
    if args.work_dir:
        work_dir = pathlib.Path(args.work_dir)
        work_dir.mkdir(parents=True, exist_ok=True)
    else:
        temp_ctx = tempfile.TemporaryDirectory(prefix="nx769j_boot_build_")
        work_dir = pathlib.Path(temp_ctx.name)

    try:
        dtc_bin = patch_nx769j_dtb.find_dtc(args.dtc)
        asm_tool = find_aarch64_assembler()

        # 1. Extract modules
        if args.dm_mod_ko and args.pwrseq_ko:
            dm_mod_bytes = pathlib.Path(args.dm_mod_ko).read_bytes()
            pwrseq_bytes = pathlib.Path(args.pwrseq_ko).read_bytes()
        elif args.modules:
            dm_mod_bytes, pwrseq_bytes = extract_modules_from_tar_zst(pathlib.Path(args.modules))
        else:
            raise ValueError("Pass either --modules <modules.tar.zst> or both --dm-mod-ko and --pwrseq-ko")

        # 2. Gather initrd files directly from repo canonical sources
        nx_dm_tool_bytes = ensure_nx_dm_tool(args.nx_dm_tool, work_dir)
        if args.extents:
            extents_bytes = pathlib.Path(args.extents).read_bytes()
            ext_sec = sum(int(ln.split()[1]) for ln in extents_bytes.decode("ascii").splitlines() if ln.strip())
            if ext_sec <= 0 or (ext_sec % 8) != 0:
                raise ValueError(f"Invalid extents total sectors ({ext_sec}) in {args.extents}")
        else:
            extents_bytes = b""

        fb_wl_bytes = (REPO_ROOT / "drivers" / "display" / "nx769j_fb_wl.c").read_bytes()
        vkbd_bytes = (REPO_ROOT / "drivers" / "display" / "nx769j_vkbd.c").read_bytes()
        aw88261_bytes = (REPO_ROOT / "drivers" / "audio" / "aw88261_init.py").read_bytes()
        assert b"SNDRV_TIMER_IOCTL_TREAD" in aw88261_bytes and b"0x2113" in aw88261_bytes

        entries: list[tuple[str, int, bytes]] = [
            ("bin", 0o040755, b""),
            ("etc", 0o040755, b""),
            ("lib", 0o040755, b""),
            ("lib/modules", 0o040755, b""),
            ("init", 0o100755, build_init_script(args.gadget_serial, ssh_pubkey)),
            ("bin/nx_dm_tool", 0o100755, nx_dm_tool_bytes),
            ("lib/modules/dm-mod.ko", 0o100644, dm_mod_bytes),
            ("lib/modules/pwrseq-qcom-wcn.ko", 0o100644, pwrseq_bytes),
            ("etc/rootfs.extents", 0o100644, extents_bytes),
            ("etc/nx769j_fb_wl.c", 0o100644, fb_wl_bytes),
            ("etc/nx769j_vkbd.c", 0o100644, vkbd_bytes),
            ("etc/aw88261-init", 0o100755, aw88261_bytes),
        ]
        if args.busybox:
            entries.append(("bin/busybox", 0o100755, pathlib.Path(args.busybox).read_bytes()))

        initrd_gz = build_newc_cpio_gz(entries)
        initrd_exact_len = len(initrd_gz)
        (work_dir / "initrd.cpio.gz").write_bytes(initrd_gz)
        initrd_padded = initrd_gz + b"\x00" * ((64 - (len(initrd_gz) % 64)) % 64)

        # 3. Compile U-Boot DTB & Linux DTB
        uboot_dts_path = work_dir / "nx769j-uboot.dts"
        uboot_dtb_path = work_dir / "nx769j-uboot.dtb"
        uboot_dts_path.write_text(UBOOT_DTS, encoding="utf-8")
        subprocess.check_call(
            [dtc_bin, "-q", "-I", "dts", "-O", "dtb", "-o", str(uboot_dtb_path), str(uboot_dts_path)]
        )
        uboot_dtb_bytes = uboot_dtb_path.read_bytes()
        uboot_dtb_bytes += b"\x00" * ((8 - (len(uboot_dtb_bytes) % 8)) % 8)

        if args.patched_dtb:
            linux_dtb_bytes = pathlib.Path(args.patched_dtb).read_bytes()
        else:
            linux_dtb_path = work_dir / "nx769j-linux.dtb"
            linux_dts_path = work_dir / "nx769j-linux.dts"
            linux_dtb_bytes = patch_nx769j_dtb.patch_dtb_file(
                in_dtb=args.dtb,
                out_dtb=str(linux_dtb_path),
                out_dts=str(linux_dts_path),
                bt_mac=args.bt_mac,
                pad_size=163840,
                dtc_bin=dtc_bin,
            )
        if len(linux_dtb_bytes) > 0x30000:
            raise ValueError(f"Patched Linux DTB too big: {len(linux_dtb_bytes)} > 0x30000")
        linux_dtb_padded = linux_dtb_bytes + b"\x00" * (0x30000 - len(linux_dtb_bytes))

        # 4. Load Linux kernel Image
        linux_img_bytes = pathlib.Path(args.image).read_bytes()
        if linux_img_bytes[:2] != b"MZ" or linux_img_bytes[56:60] != b"ARMd":
            raise ValueError(f"Invalid ARM64 EFI Linux Image: {args.image}")
        linux_img_bytes += b"\x00" * ((64 - (len(linux_img_bytes) % 64)) % 64)
        linux_img_copy_len = len(linux_img_bytes)

        # 5. Patch U-Boot + assemble 8 KiB BootShim
        hud_asm = build_hud_asm()
        ub = patch_uboot_bin(
            uboot_path.read_bytes(),
            work_dir,
            asm_tool,
            hud_asm,
            linux_img_copy_len=linux_img_copy_len,
            initrd_exact_len=initrd_exact_len,
        )
        uboot_payload = bytes(ub) + uboot_dtb_bytes
        uboot_payload += b"\x00" * ((8 - (len(uboot_payload) % 8)) % 8)

        shim_2pages = assemble_snippet(
            f"""
.globl _start
_start:
.Lstart:
    .inst 0xfa405a4d
    b .Lentry
    .quad 0x0000000000000000
    .quad 0x{TOTAL_IMG_SIZE:016x}
    .quad 0x000000000000000a
    .quad 0
    .quad 0
    .quad 0
    .word 0x644d5241
    .word 0

.balign 64
.Lentry:
    msr daifset, #0xf
    mov x19, x0
    adr x20, .Lstart

    movz x2, #0xd7e0, lsl #16
    movz w4, #0x5542, lsl #16
    movk w4, #0x4c47
    str  w4, [x2]
    str  wzr, [x2, #4]
    str  wzr, [x2, #8]
    str  wzr, [x2, #12]
    dc   civac, x2
    dsb  sy

    add  x0, x20, #0x1000
    msr  vbar_el1, x0
    isb

    /* Clear bottom HUD (y=1260..2479) with dark navy */
    movz x13, #0xe394, lsl #16
    movz x12, #0x0055, lsl #16
    movk x12, #0xdc20
    add  x13, x13, x12
    movz x14, #0xe394, lsl #16
    movz x12, #0x00a8, lsl #16
    movk x12, #0xfc00
    add  x14, x14, x12
    movz w15, #0xff08, lsl #16
    movk w15, #0x1424
4:
    str  w15, [x13], #4
    cmp  x13, x14
    b.lo 4b

    movz x13, #0xe394, lsl #16
    movz x12, #0x0055, lsl #16
    movk x12, #0xdc20
    add  x13, x13, x12
5:
    dc   civac, x13
    add  x13, x13, #64
    cmp  x13, x14
    b.lo 5b
    dsb  sy

    ldr  x6, .Llnx_off
    add  x6, x20, x6
    ldr  x7, .Llnx_dst
    ldr  x8, .Llnx_len
    bl   .Lmemmove_flush

    ldr  x6, .Llxdtb_off
    add  x6, x20, x6
    ldr  x7, .Llxdtb_dst
    ldr  x8, .Llxdtb_len
    bl   .Lmemmove_flush

    ldr  x6, .Linitrd_off
    add  x6, x20, x6
    ldr  x7, .Linitrd_dst
    ldr  x8, .Linitrd_len
    bl   .Lmemmove_flush

    ic   iallu
    dsb  sy
    isb

    mov  w1, #0x4e
    bl   .Lhud_putc
    mov  w1, #0x58
    bl   .Lhud_putc
    mov  w1, #0x20
    bl   .Lhud_putc
    mov  w1, #0x4f
    bl   .Lhud_putc
    mov  w1, #0x4b
    bl   .Lhud_putc
    mov  w1, #0x0a
    bl   .Lhud_putc

    add  x11, x20, #0x2000
    ldr  x0, .Ldtb_off
    add  x0, x11, x0
    mov  x1, #0
    mov  x2, #0
    mov  x3, #0
    br   x11

.Lmemmove_flush:
    cmp  x7, x6
    b.eq .Lflush_only
    b.hi .Lcopy_bwd
    mov  x9, x6
    mov  x10, x7
    add  x11, x6, x8
10:
    ldp  x4, x5, [x9], #16
    stp  x4, x5, [x10], #16
    cmp  x9, x11
    b.lo 10b
    b    .Lflush_only
.Lcopy_bwd:
    add  x9, x6, x8
    add  x10, x7, x8
11:
    ldp  x4, x5, [x9, #-16]!
    stp  x4, x5, [x10, #-16]!
    cmp  x9, x6
    b.hi 11b
.Lflush_only:
    mov  x10, x7
    add  x11, x7, x8
12:
    dc   civac, x10
    add  x10, x10, #64
    cmp  x10, x11
    b.lo 12b
    dsb  sy
    ret
"""
            + hud_asm
            + f"""
.balign 8
.Ldtb_off:
    .quad 0x15cd50
.Linitrd_off:
    .quad 0x{INITRD_PAYLOAD_OFF:x}
.Linitrd_dst:
    .quad 0x{INITRD_STAGE_ADDR:x}
.Linitrd_len:
    .quad 0x{len(initrd_padded):x}
.Llxdtb_off:
    .quad 0x{DTB_PAYLOAD_OFF:x}
.Llxdtb_dst:
    .quad 0x{DTB_STAGE_ADDR:x}
.Llxdtb_len:
    .quad 0x{len(linux_dtb_padded):x}
.Llnx_off:
    .quad 0x{LNX_PAYLOAD_OFF:x}
.Llnx_dst:
    .quad 0x{LNX_STAGE_ADDR:x}
.Llnx_len:
    .quad 0x{linux_img_copy_len:x}

.balign 4096
.Learly_vecs:
    b .Learly_exc
.balign 0x80
    b .Learly_exc
.balign 0x80
    b .Learly_exc
.balign 0x80
    b .Learly_exc
.balign 0x80
    b .Learly_exc
.balign 0x80
    b .Learly_exc
.balign 0x80
    b .Learly_exc
.balign 0x80
    b .Learly_exc

.Learly_exc:
    msr  daifset, #0xf
    mov  w1, #0x3f
    bl   .Lhud_putc
    mov  w1, #0x45
    bl   .Lhud_putc
    mov  w1, #0x3d
    bl   .Lhud_putc
    mrs  x1, esr_el1
    bl   .Lhud_hex64
    mov  w1, #0x0a
    bl   .Lhud_putc
9:
    wfi
    b    9b

.balign 4096
""",
            work_dir,
            asm_tool,
        )
        assert len(shim_2pages) == 8192, f"Expected 8192 bytes for BootShim, got {len(shim_2pages)}"

        # 6. Assemble 64 MiB combined payload & pack Android boot image v4
        part1 = shim_2pages + uboot_payload
        assert len(part1) <= INITRD_PAYLOAD_OFF, f"U-Boot overlaps initrd: 0x{len(part1):x}"
        part2 = part1 + b"\x00" * (INITRD_PAYLOAD_OFF - len(part1)) + initrd_padded
        assert len(part2) <= DTB_PAYLOAD_OFF, f"Initrd overlaps DTB: 0x{len(part2):x}"
        part3 = part2 + b"\x00" * (DTB_PAYLOAD_OFF - len(part2)) + linux_dtb_padded
        assert len(part3) <= LNX_PAYLOAD_OFF, f"DTB overlaps kernel: 0x{len(part3):x}"
        part4 = part3 + b"\x00" * (LNX_PAYLOAD_OFF - len(part3)) + linux_img_bytes
        assert len(part4) <= TOTAL_IMG_SIZE, f"Image exceeds 64 MiB: 0x{len(part4):x}"
        full_image = part4 + b"\x00" * (TOTAL_IMG_SIZE - len(part4))

        img_out = work_dir / "uboot-omarchy-nx769j-Image"
        img_out.write_bytes(full_image)

        out_path = pathlib.Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        tools_dir = REPO_ROOT / "boot" / "tools"
        env = os.environ.copy()
        env["PYTHONPATH"] = str(tools_dir)
        subprocess.check_call(
            [
                sys.executable,
                str(tools_dir / "mkbootimg.py"),
                "--header_version",
                "4",
                "--pagesize",
                "4096",
                "--kernel",
                str(img_out),
                "-o",
                str(out_path),
            ],
            env=env,
        )
        print(f"SUCCESS: Built {out_path} ({out_path.stat().st_size} bytes)")
        return 0
    finally:
        if temp_ctx is not None:
            temp_ctx.cleanup()


if __name__ == "__main__":
    sys.exit(main())
