#!/usr/bin/env bash
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
#
# Connect to RedMagic 9 Pro (NX769J) running Omarchy Mobile Linux over USB
# gadget networking (RNDIS / CDC-NCM / CDC-ECM).
#
# Supports macOS (via ioreg + IPv6 link-local discovery / 169.254.72.2 fallback)
# and Linux hosts (via /sys/class/net).
#
# Environment variables:
#   NX769J_USER      SSH username (default: arch)
#   NX769J_SSH_KEY   Optional path to private key (if unset, uses default ~/.ssh config/agent)
#   NX769J_HOST      Override target IP / IPv6 address (skip auto-discovery)
#   NX769J_TIMEOUT   ConnectTimeout seconds (default: 8)
set -euo pipefail

USER_NAME="${NX769J_USER:-arch}"
CONNECT_TIMEOUT="${NX769J_TIMEOUT:-8}"

SSH_ARGS=(
  -o "ConnectTimeout=${CONNECT_TIMEOUT}"
  -o ServerAliveInterval=3
  -o ServerAliveCountMax=2
  -o StrictHostKeyChecking=accept-new
  -o UserKnownHostsFile=/dev/null
  -o LogLevel=ERROR
)

if [[ -n "${NX769J_SSH_KEY:-}" ]]; then
  SSH_ARGS+=(-i "$NX769J_SSH_KEY")
fi

TARGET="${NX769J_HOST:-}"

if [[ -z "$TARGET" ]]; then
  case "$(uname -s)" in
    Darwin)
      IFACES=$(ioreg -r -c IOUserNetworkEthernet -l 2>/dev/null | sed -n 's/.*"BSD Name" = "\([^"]*\)".*/\1/p' || true)
      for iface in $IFACES; do
        addrs=$(ping6 -c 1 -W 1000 "ff02::1%${iface}" 2>/dev/null | sed -n 's/.*from \(fe80::[^ ]*\) .*/\1/p' || true)
        for addr in $addrs; do
          [[ "$addr" == *"%"* ]] || addr="${addr}%${iface}"
          if ssh "${SSH_ARGS[@]}" "${USER_NAME}@${addr}" true 2>/dev/null; then
            TARGET="$addr"
            break 2
          fi
        done
      done
      ;;
    Linux)
      for sysif in /sys/class/net/*; do
        iface=$(basename "$sysif")
        [[ "$iface" == "lo" ]] && continue
        addrs=$(ping -6 -c 1 -W 1 "ff02::1%${iface}" 2>/dev/null | sed -n 's/.*from \([^ %]*\).*/\1/p' | grep '^fe80::' || true)
        for addr in $addrs; do
          if ssh "${SSH_ARGS[@]}" "${USER_NAME}@${addr}%${iface}" true 2>/dev/null; then
            TARGET="${addr}%${iface}"
            break 2
          fi
        done
      done
      ;;
  esac
fi

if [[ -z "$TARGET" ]]; then
  TARGET="169.254.72.2"
fi

exec ssh "${SSH_ARGS[@]}" "${USER_NAME}@${TARGET}" "$@"
