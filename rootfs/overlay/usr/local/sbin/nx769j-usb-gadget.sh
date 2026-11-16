#!/bin/sh
# SPDX-License-Identifier: MIT
# NX769J USB2 High-Speed ConfigFS NCM Gadget (10.42.0.2/24 + 169.254.72.2/16)
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config 2>/dev/null || true
G=/sys/kernel/config/usb_gadget/g1
if [ ! -d "$G" ]; then
    mkdir -p "$G/strings/0x409" "$G/configs/c.1/strings/0x409" "$G/functions/ncm.usb0"
    echo 0x1d6b > "$G/idVendor"
    echo 0x0104 > "$G/idProduct"
    echo "RedMagic" > "$G/strings/0x409/manufacturer"
    echo "NX769J Omarchy Linux" > "$G/strings/0x409/product"
    echo "${NX769J_GADGET_SERIAL:-nx769j-omarchy}" > "$G/strings/0x409/serialnumber"
    echo "USB NCM" > "$G/configs/c.1/strings/0x409/configuration"
    echo "4e:d9:6d:8d:1b:8a" > "$G/functions/ncm.usb0/host_addr" 2>/dev/null || true
    echo "26:91:a3:2c:1c:76" > "$G/functions/ncm.usb0/dev_addr" 2>/dev/null || true
    ln -sf "$G/functions/ncm.usb0" "$G/configs/c.1/"
fi
CUR_UDC=$(cat "$G/UDC" 2>/dev/null || true)
if [ -z "$CUR_UDC" ]; then
    for u in /sys/class/udc/*; do
        [ -e "$u" ] && basename "$u" > "$G/UDC" && break
    done
fi
for _ in $(seq 1 30); do
    [ -e /sys/class/net/usb0 ] && break
    sleep 0.2
done
if [ -e /sys/class/net/usb0 ]; then
    ip link set usb0 up
    ip addr replace 10.42.0.2/24 dev usb0
    ip addr replace 169.254.72.2/16 dev usb0
fi
exit 0
