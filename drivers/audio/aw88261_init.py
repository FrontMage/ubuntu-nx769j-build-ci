#!/usr/bin/env python3
"""
aw88261_init.py — Standalone Userspace SmartPA Controller for Nubia RedMagic 9 Pro (NX769J)

Hardware topology (verified from live-fdt.dts, vendor aw882xx_dlkm.ko disassembly,
factory aw882xx_acf.bin project "A1901", vendor ACDB MTP_acdb_cal.acdb, and mainline
snd-q6apm.ko / snd-soc-sc8280xp.ko):
  - Bus: geniqup@9c0000 / i2c@98c000 (400 kHz), auto-discovered via sysfs OF node name
    IMPORTANT: Shared with Fan RGB LED aw22xxx_led@6a (@fan). This tool ONLY touches
    I2C addresses 0x35 and 0x34 — NEVER 0x6a or any other address.
  - 0x35: Bottom Speaker (dev_index=0, Left / Ch0, I2S RX slot 0, silicon PID 0x2113)
  - 0x34: Top Speaker / Earpiece (dev_index=1, Right / Ch1, I2S RX slot 1, silicon PID 0x2113)
  - Audio DAI: PRIMARY_MI2S_RX (id=16, LPAIF, I2S_SD1 = TLMM gpio128 i2s0_data1 DOUT,
    gpio126 i2s0_sck 1.536 MHz BCLK, gpio129 i2s0_ws 48 kHz WS, gpio127 i2s0_data0 DIN).
    Fixed by snd-soc-sc8280xp.ko (sc8280xp_be_hw_params_fixup) to:
      48,000 Hz, 2 channels, S16_LE (16-bit slot, 32*fs = 1.536 MHz BCLK), Philips I2S.
  - Reset GPIOs: None on NX769J (soft reset via 0x00=0x55aa + PID 0x2113 0x73=0x1d40).
  - Register endianness: 8-bit register address, BIG-ENDIAN 16-bit register value (I2C_RDWR).
  - Chip ID (0x00): 0x2113 on NX769J production silicon (0x2013 also supported):
      * Volume register 0x05 bits [9:0] on PID 0x2113 uses coarse/fine encoding
        `((steps // 48) << 6) | (steps % 48)` (aw_pid_2113_set_volume @ 0x12d7c),
        where 1 step = -0.125 dB and mute (720 steps = -90.0 dB) is 0x03C0.
      * I2STXEN in I2SCTRL3 (0x08) on PID 0x2113 is bit 6 (0x0040), NOT bit 0 (0x0001)
        (aw882xx_init @ 0x108a4..0x10a4c). Clearing 0x0040 cleanly disables SmartPA SDO TX.
"""

from __future__ import annotations

import argparse
import ctypes
import struct
import errno
import fcntl
import glob
import json
import os
import re
import select
import signal
import sys
import time
from typing import Callable, Dict, List, Optional, Tuple

# Linux I2C_RDWR ioctl constants (<linux/i2c-dev.h>, <linux/i2c.h>)
I2C_RDWR = 0x0707
I2C_M_RD = 0x0001

# Strictly permitted AW88261 I2C slave addresses on i2c@98c000 (NEVER touch 0x6a!)
ADDR_BOTTOM_LEFT = 0x35
ADDR_TOP_RIGHT = 0x34
ALLOWED_I2C_ADDRS = frozenset({ADDR_BOTTOM_LEFT, ADDR_TOP_RIGHT})

# AW88261 Register Map & Bitfields (verified in vendor aw882xx_dlkm.ko for PID 0x2113)
REG_ID = 0x00
REG_SYSST = 0x01
REG_SYSINT = 0x02
REG_SYSINTM = 0x03
REG_SYSCTRL = 0x04
REG_SYSCTRL2 = 0x05
REG_I2SCTRL1 = 0x06
REG_I2SCTRL3 = 0x08
REG_DBGCTRL = 0x14
REG_VSNTM1 = 0x50
REG_PLLCTRL1 = 0x54
REG_INIT_73 = 0x73
REG_EFRH4 = 0x78
REG_EFRH3 = 0x79
REG_EFRL4 = 0x7C
REG_EFRL3 = 0x7D

SOFT_RESET_VALUE = 0x55AA
PID_2113_POST_RESET_73 = 0x1D40
PID_2013 = 0x2013
PID_2113 = 0x2113
VALID_CHIP_IDS = frozenset({PID_2013, PID_2113})

# SYSCTRL (0x04) bits
BIT_PWDN = 1 << 0        # 1 = power down, 0 = power on
BIT_AMPPD = 1 << 1       # 1 = Class-D & Boost off, 0 = Class-D & Boost working
BIT_I2SEN = 1 << 6       # 1 = I2S RX enabled
BIT_RCV_MODE = 1 << 7    # 1 = Receiver (earpiece) mode, 0 = Speaker boost mode
BIT_HMUTE = 1 << 8       # 1 = hardware mute, 0 = unmuted
BIT_ULS_HMUTE = 1 << 14  # 1 = ultrasonic mute enabled, 0 = disabled

# PLLCTRL1 (0x54) CCO_MUX bit (aw882xx_init @ 0x10a9c..0x10ac8)
BIT_CCO_MUX = 1 << 6

# SYSST (0x01) status bits (st_check = 0x0311, pll_check = 0x0011)
BIT_PLLS = 1 << 0        # PLL locked
BIT_OTHS = 1 << 1        # Over-temperature fault
BIT_OCDS = 1 << 3        # Over-current fault
BIT_CLKS = 1 << 4        # Clock stable
BIT_NOCLKS = 1 << 5      # No I2S clock fault
BIT_SWS = 1 << 8         # Class-D switching active
BIT_BSTS = 1 << 9        # Boost converter ready
BIT_BSTOCS = 1 << 11     # Boost over-current fault
BIT_UVLS = 1 << 14       # Under-voltage fault

PLL_LOCK_MASK = BIT_PLLS | BIT_CLKS
SYSST_ACTIVE_CHECK = BIT_BSTS | BIT_SWS | BIT_CLKS | BIT_PLLS  # 0x0311
SYSST_FAULT_MASK = BIT_UVLS | BIT_BSTOCS | BIT_NOCLKS | BIT_OCDS | BIT_OTHS

# Volume encoding in SYSCTRL2 (0x05) bits [9:0]: 0.125 dB/step attenuation
VOL_MASK = 0x03FF
MAX_VOL_STEPS = 0x02D0  # 720 steps = -90.0 dB mute (0x03C0 encoded on PID 0x2113)

# I2SCTRL1 (0x06) values for 48 kHz Philips I2S:
#   bits [13:12] = 0b11 (RXTHRS)
#   bits [11:10] = CHSEL (0b10 = Left/Ch0, 0b01 = Right/Ch1, 0b11 = (L+R)/2 mono)
#   bits [9:8]   = 0b00 (Philips standard I2S)
#   bits [7:6]   = I2SFS (0b00 = 16-bit, 0b11 = 32-bit)
#   bits [5:4]   = I2SBCK (0b00 = 32*fs [16*2], 0b10 = 64*fs [32*2])
#   bits [3:0]   = I2SSR (0x8 = 48 kHz)
I2SCTRL1_TABLE = {
    16: {
        "left": 0x3808,   # Ch0 Left,  16-bit, 32*fs (1.536 MHz BCLK), 48 kHz
        "right": 0x3408,  # Ch1 Right, 16-bit, 32*fs (1.536 MHz BCLK), 48 kHz
        "mono": 0x3C08,   # (L+R)/2,   16-bit, 32*fs (1.536 MHz BCLK), 48 kHz
    },
    32: {
        "left": 0x38E8,   # Ch0 Left,  32-bit, 64*fs (3.072 MHz BCLK), 48 kHz (ACF raw)
        "right": 0x34E8,  # Ch1 Right, 32-bit, 64*fs (3.072 MHz BCLK), 48 kHz (ACF raw)
        "mono": 0x3CE8,   # (L+R)/2,   32-bit, 64*fs (3.072 MHz BCLK), 48 kHz
    },
}

DEFAULT_STATE_DIR = "/run/nx769j-smartpa"
DEFAULT_MODE_FILE = "/run/nx769j-smartpa/mode"

# Linux ALSA /dev/snd/timer UAPI (<sound/asound.h>, verified against 7.1.1-nx769j+ snd-timer.ko)
SNDRV_TIMER_CLASS_PCM = 3
SNDRV_TIMER_SCLASS_NONE = 0
SNDRV_TIMER_PSFLG_AUTO = 1 << 0

SNDRV_TIMER_EVENT_RESOLUTION = 0
SNDRV_TIMER_EVENT_TICK = 1
SNDRV_TIMER_EVENT_START = 2
SNDRV_TIMER_EVENT_STOP = 3
SNDRV_TIMER_EVENT_CONTINUE = 4
SNDRV_TIMER_EVENT_PAUSE = 5
SNDRV_TIMER_EVENT_EARLY = 6
SNDRV_TIMER_EVENT_SUSPEND = 7
SNDRV_TIMER_EVENT_RESUME = 8
SNDRV_TIMER_EVENT_MSTART = 12
SNDRV_TIMER_EVENT_MSTOP = 13
SNDRV_TIMER_EVENT_MCONTINUE = 14
SNDRV_TIMER_EVENT_MPAUSE = 15
SNDRV_TIMER_EVENT_MSUSPEND = 17
SNDRV_TIMER_EVENT_MRESUME = 18

# Receive ONLY PCM state transitions; exclude RESOLUTION (0) and TICK (1) so
# snd_timer_user_tinterrupt() never wakes userspace on DMA period interrupts.
PCM_TIMER_EVENT_FILTER = (
    (1 << SNDRV_TIMER_EVENT_START)
    | (1 << SNDRV_TIMER_EVENT_STOP)
    | (1 << SNDRV_TIMER_EVENT_CONTINUE)
    | (1 << SNDRV_TIMER_EVENT_PAUSE)
    | (1 << SNDRV_TIMER_EVENT_SUSPEND)
    | (1 << SNDRV_TIMER_EVENT_RESUME)
    | (1 << SNDRV_TIMER_EVENT_MSTART)
    | (1 << SNDRV_TIMER_EVENT_MSTOP)
    | (1 << SNDRV_TIMER_EVENT_MCONTINUE)
    | (1 << SNDRV_TIMER_EVENT_MPAUSE)
    | (1 << SNDRV_TIMER_EVENT_MSUSPEND)
    | (1 << SNDRV_TIMER_EVENT_MRESUME)
)  # 0x0006F1BC

SNDRV_TIMER_IOCTL_TREAD_OLD = 0x40045402  # _IOW('T', 0x02, int)
SNDRV_TIMER_IOCTL_TREAD64 = 0x400454A4    # _IOW('T', 0xa4, int) -> 32-byte snd_timer_tread64
SNDRV_TIMER_IOCTL_SELECT = 0x40345410     # _IOW('T', 0x10, struct snd_timer_select [52B])
SNDRV_TIMER_IOCTL_PARAMS = 0x40505412     # _IOW('T', 0x12, struct snd_timer_params [80B])
SNDRV_TIMER_IOCTL_START = 0x000054A0      # _IO('T', 0xa0)
SNDRV_TIMER_IOCTL_STOP = 0x000054A1       # _IO('T', 0xa1)

TREAD64_STRUCT = struct.Struct("<iiqqii")  # 32 bytes: event, pad1, sec, nsec, val, pad2
MAX_BCLK_RETRIES = 4
BCLK_RETRY_INTERVAL_SEC = 0.025
STOP_DEBOUNCE_SEC = 0.030

# Embedded Factory ACF Register Tables (extracted verbatim from NX769J vendor
# /vendor/firmware/aw882xx_acf.bin, project "A1901", checksum 0x0a15f908).
EMBEDDED_PROFILES: Dict[int, Dict[str, List[Tuple[int, int]]]] = {
    ADDR_BOTTOM_LEFT: {
        "Music": [
            (0x03, 0xFFFF), (0x04, 0x3240), (0x05, 0x0011), (0x06, 0x38E8),
            (0x07, 0x0010), (0x08, 0x0077), (0x09, 0x626D), (0x0A, 0x0964),
            (0x0B, 0x2590), (0x0C, 0x1C64), (0x0D, 0x005B), (0x0E, 0x4A9B),
            (0x0F, 0x5264), (0x10, 0x5008), (0x11, 0x1F20), (0x12, 0x002B),
            (0x13, 0x2102), (0x14, 0xC003), (0x15, 0x0643), (0x16, 0x000A),
            (0x30, 0x460C), (0x31, 0xC989), (0x32, 0x3541), (0x33, 0x4DB8),
            (0x34, 0xD6F9), (0x35, 0x7ACE), (0x36, 0xEC7C), (0x37, 0xF18C),
            (0x38, 0x0800), (0x3A, 0x8884), (0x3B, 0x0666), (0x3C, 0x0ED2),
            (0x3D, 0x0663), (0x3E, 0x01AE), (0x3F, 0x0505), (0x40, 0xFFA0),
            (0x51, 0x00F3), (0x52, 0x3EE8), (0x53, 0x8005), (0x54, 0x1708),
            (0x55, 0x0202), (0x56, 0x3800), (0x57, 0x84C9), (0x58, 0x41D6),
            (0x59, 0x0000), (0x5A, 0x0000), (0x5B, 0x01C0), (0x5C, 0x0049),
            (0x60, 0x0FCD), (0x61, 0x996C), (0x62, 0x4A40), (0x63, 0x6077),
            (0x64, 0x0053), (0x65, 0x5CA1), (0x66, 0x40F9), (0x67, 0x508E),
            (0x68, 0x0048),
        ],
        "Receiver": [
            (0x03, 0xFFFF), (0x04, 0x32C0), (0x05, 0x008C), (0x06, 0x34E8),
            (0x07, 0x0010), (0x08, 0x0076), (0x09, 0x7380), (0x0A, 0x0964),
            (0x0B, 0x2590), (0x0C, 0x1C64), (0x0D, 0x005B), (0x0E, 0x4A9B),
            (0x0F, 0x5064), (0x10, 0x5008), (0x11, 0x1F20), (0x12, 0x002B),
            (0x13, 0x2102), (0x14, 0xE003), (0x15, 0x0643), (0x16, 0x000A),
            (0x30, 0x460C), (0x31, 0xC989), (0x32, 0x3541), (0x33, 0x4DB8),
            (0x34, 0xD6F9), (0x35, 0x7ACE), (0x36, 0xEC7C), (0x37, 0xF18C),
            (0x38, 0x0800), (0x3A, 0x8884), (0x3B, 0x0666), (0x3C, 0x0ED2),
            (0x3D, 0x0663), (0x3E, 0x01AE), (0x3F, 0x0505), (0x40, 0xFFA0),
            (0x51, 0x00F3), (0x52, 0x3EE8), (0x53, 0x8005), (0x54, 0x1708),
            (0x55, 0x0202), (0x56, 0x3800), (0x57, 0x8489), (0x58, 0x41D6),
            (0x59, 0x0000), (0x5A, 0x0000), (0x5B, 0x01C0), (0x5C, 0x0049),
            (0x60, 0x0F0D), (0x61, 0x996C), (0x62, 0x4A40), (0x63, 0x6077),
            (0x64, 0x0053), (0x65, 0x5CA1), (0x66, 0x40F9), (0x67, 0x508E),
            (0x68, 0x0048),
        ],
    },
    ADDR_TOP_RIGHT: {
        "Music": [
            (0x03, 0xFFFF), (0x04, 0x3240), (0x05, 0x0011), (0x06, 0x34E8),
            (0x07, 0x0010), (0x08, 0x0076), (0x09, 0x626D), (0x0A, 0x0964),
            (0x0B, 0x2590), (0x0C, 0x1C64), (0x0D, 0x005B), (0x0E, 0x4A9B),
            (0x0F, 0x5264), (0x10, 0x5008), (0x11, 0x1F20), (0x12, 0x002B),
            (0x13, 0x2102), (0x14, 0xC003), (0x15, 0x0643), (0x16, 0x000A),
            (0x30, 0x460C), (0x31, 0xC989), (0x32, 0x3541), (0x33, 0x4DB8),
            (0x34, 0xD6F9), (0x35, 0x7ACE), (0x36, 0xEC7C), (0x37, 0xF18C),
            (0x38, 0x0800), (0x3A, 0x8884), (0x3B, 0x0666), (0x3C, 0x0ED2),
            (0x3D, 0x0663), (0x3E, 0x01AE), (0x3F, 0x0505), (0x40, 0xFFA0),
            (0x51, 0x00F3), (0x52, 0x3EE8), (0x53, 0x8005), (0x54, 0x1708),
            (0x55, 0x0202), (0x56, 0x3800), (0x57, 0x84C9), (0x58, 0x41D6),
            (0x59, 0x0000), (0x5A, 0x0000), (0x5B, 0x01C0), (0x5C, 0x0049),
            (0x60, 0x0FCD), (0x61, 0x996C), (0x62, 0x4A40), (0x63, 0x6077),
            (0x64, 0x0053), (0x65, 0x5CA1), (0x66, 0x40F9), (0x67, 0x508E),
            (0x68, 0x0048),
        ],
        "Receiver": [
            (0x03, 0xFFFF), (0x04, 0x32C0), (0x05, 0x008C), (0x06, 0x34E8),
            (0x07, 0x0010), (0x08, 0x0076), (0x09, 0x7380), (0x0A, 0x0964),
            (0x0B, 0x2590), (0x0C, 0x1C64), (0x0D, 0x005B), (0x0E, 0x4A9B),
            (0x0F, 0x5064), (0x10, 0x5008), (0x11, 0x1F20), (0x12, 0x002B),
            (0x13, 0x2102), (0x14, 0xE003), (0x15, 0x0643), (0x16, 0x000A),
            (0x30, 0x460C), (0x31, 0xC989), (0x32, 0x3541), (0x33, 0x4DB8),
            (0x34, 0xD6F9), (0x35, 0x7ACE), (0x36, 0xEC7C), (0x37, 0xF18C),
            (0x38, 0x0800), (0x3A, 0x8884), (0x3B, 0x0666), (0x3C, 0x0ED2),
            (0x3D, 0x0663), (0x3E, 0x01AE), (0x3F, 0x0505), (0x40, 0xFFA0),
            (0x51, 0x00F3), (0x52, 0x3EE8), (0x53, 0x8005), (0x54, 0x1708),
            (0x55, 0x0202), (0x56, 0x3800), (0x57, 0x8489), (0x58, 0x41D6),
            (0x59, 0x0000), (0x5A, 0x0000), (0x5B, 0x01C0), (0x5C, 0x0049),
            (0x60, 0x0F0D), (0x61, 0x996C), (0x62, 0x4A40), (0x63, 0x6077),
            (0x64, 0x0053), (0x65, 0x5CA1), (0x66, 0x40F9), (0x67, 0x508E),
            (0x68, 0x0048),
        ],
    },
}


class I2cMsg(ctypes.Structure):
    _fields_ = [
        ("addr", ctypes.c_uint16),
        ("flags", ctypes.c_uint16),
        ("len", ctypes.c_uint16),
        ("buf", ctypes.c_void_p),
    ]


class I2cRdwrIoctlData(ctypes.Structure):
    _fields_ = [
        ("msgs", ctypes.POINTER(I2cMsg)),
        ("nmsgs", ctypes.c_uint32),
    ]


class AW88261Bus:
    """Safe big-endian 16-bit I2C_RDWR accessor restricted to 0x35 and 0x34."""

    def __init__(
        self,
        dev_path: str,
        dry_run: bool = False,
        verbose: bool = False,
        sim_chip_id: int = PID_2113,
    ):
        self.dev_path = dev_path
        self.dry_run = dry_run
        self.verbose = verbose
        self.sim_chip_id = sim_chip_id
        self.fd: Optional[int] = None
        self._sim_regs: Dict[Tuple[int, int], int] = {}
        if not dry_run:
            self.fd = os.open(dev_path, os.O_RDWR)

    def close(self) -> None:
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def __enter__(self) -> "AW88261Bus":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    @staticmethod
    def _check_addr(addr: int) -> None:
        if addr not in ALLOWED_I2C_ADDRS:
            raise PermissionError(
                f"Refusing I2C access to 0x{addr:02x}: only AW88261 addresses "
                f"0x35 and 0x34 are permitted (0x6a belongs to @fan)"
            )

    def read16(self, addr: int, reg: int) -> int:
        self._check_addr(addr)
        reg &= 0xFF
        if self.dry_run:
            if reg == REG_ID:
                val = self._sim_regs.get((addr, REG_ID), self.sim_chip_id)
            elif reg == REG_SYSST:
                sysctrl = self._sim_regs.get((addr, REG_SYSCTRL), 0x7343)
                if sysctrl & BIT_PWDN:
                    val = 0x0000
                elif sysctrl & BIT_AMPPD:
                    val = BIT_CLKS | BIT_PLLS
                else:
                    val = SYSST_ACTIVE_CHECK
            elif reg in (REG_EFRH3, REG_EFRL3):
                # Default to production NX769J trimmed EFUSE value (0x5bf6 on 0x35, 0x5bf5 on 0x34)
                default_ef = 0x5BF6 if addr == ADDR_BOTTOM_LEFT else 0x5BF5
                val = self._sim_regs.get((addr, reg), default_ef)
            elif reg in (REG_EFRH4, REG_EFRL4):
                default_ef4 = 0x01E0 if addr == ADDR_BOTTOM_LEFT else 0x0050
                val = self._sim_regs.get((addr, reg), default_ef4)
            else:
                val = self._sim_regs.get((addr, reg), 0x0000)
            if self.verbose:
                print(f"  [DRY-RUN] READ  0x{addr:02x} reg 0x{reg:02x} -> 0x{val:04x}")
            return val

        assert self.fd is not None
        tx_buf = (ctypes.c_uint8 * 1)(reg)
        rx_buf = (ctypes.c_uint8 * 2)(0, 0)
        msgs = (I2cMsg * 2)(
            I2cMsg(addr=addr, flags=0, len=1, buf=ctypes.cast(tx_buf, ctypes.c_void_p)),
            I2cMsg(addr=addr, flags=I2C_M_RD, len=2, buf=ctypes.cast(rx_buf, ctypes.c_void_p)),
        )
        req = I2cRdwrIoctlData(msgs=msgs, nmsgs=2)
        fcntl.ioctl(self.fd, I2C_RDWR, req)
        val = (rx_buf[0] << 8) | rx_buf[1]
        if self.verbose:
            print(f"  [I2C] READ  0x{addr:02x} reg 0x{reg:02x} -> 0x{val:04x}")
        return val

    def write16(self, addr: int, reg: int, val: int) -> None:
        self._check_addr(addr)
        reg &= 0xFF
        val &= 0xFFFF
        if self.dry_run:
            if reg == REG_ID and val == SOFT_RESET_VALUE:
                self._sim_regs = {
                    k: v for k, v in self._sim_regs.items() if k[0] != addr
                }
            else:
                self._sim_regs[(addr, reg)] = val
            if self.verbose:
                print(f"  [DRY-RUN] WRITE 0x{addr:02x} reg 0x{reg:02x} <- 0x{val:04x}")
            return

        assert self.fd is not None
        tx_buf = (ctypes.c_uint8 * 3)(reg, (val >> 8) & 0xFF, val & 0xFF)
        msgs = (I2cMsg * 1)(
            I2cMsg(addr=addr, flags=0, len=3, buf=ctypes.cast(tx_buf, ctypes.c_void_p)),
        )
        req = I2cRdwrIoctlData(msgs=msgs, nmsgs=1)
        fcntl.ioctl(self.fd, I2C_RDWR, req)
        if self.verbose:
            print(f"  [I2C] WRITE 0x{addr:02x} reg 0x{reg:02x} <- 0x{val:04x}")

    def update_bits(self, addr: int, reg: int, clear_mask: int, set_bits: int) -> int:
        cur = self.read16(addr, reg)
        nxt = (cur & ~clear_mask) | (set_bits & clear_mask)
        if nxt != cur:
            self.write16(addr, reg, nxt)
        return nxt


def discover_i2c_bus(explicit_bus: Optional[str] = None, dry_run: bool = False) -> str:
    """Locate /dev/i2c-N backed by geniqup@9c0000 / 98c000.i2c."""
    if explicit_bus:
        if explicit_bus.isdigit():
            return f"/dev/i2c-{explicit_bus}"
        return explicit_bus

    for p in sorted(glob.glob("/sys/bus/platform/devices/98c000.i2c/i2c-*")):
        base = os.path.basename(p)
        if base.startswith("i2c-") and base[4:].isdigit():
            return f"/dev/{base}"

    for p in sorted(glob.glob("/sys/class/i2c-dev/i2c-*")):
        base = os.path.basename(p)
        dev_link = os.path.realpath(os.path.join(p, "device"))
        if "98c000.i2c" in dev_link:
            return f"/dev/{base}"
        uevent = os.path.join(p, "device", "of_node", "name")
        if os.path.exists(uevent):
            of_real = os.path.realpath(os.path.join(p, "device", "of_node"))
            if "i2c@98c000" in of_real:
                return f"/dev/{base}"

    if dry_run:
        return "/dev/i2c-98c000-dryrun"

    raise FileNotFoundError(
        "Could not find /dev/i2c-* for 98c000.i2c (ensure i2c@98c000 status=\"okay\" "
        "and i2c-dev / i2c-qcom-geni are loaded)"
    )


def load_profiles(acf_json_path: Optional[str]) -> Dict[int, Dict[str, List[Tuple[int, int]]]]:
    if not acf_json_path:
        return EMBEDDED_PROFILES
    with open(acf_json_path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    out: Dict[int, Dict[str, List[Tuple[int, int]]]] = {}
    for dev in doc["devices"].values():
        addr = int(dev["i2c_addr"], 16)
        if addr not in ALLOWED_I2C_ADDRS:
            continue
        out[addr] = {}
        for prof_name, pairs in dev["profiles"].items():
            out[addr][prof_name] = [
                (int(item["reg"], 16), int(item["val"], 16)) for item in pairs
            ]
    return out


def i2stxen_bit(chip_id: int) -> int:
    """
    Return the I2STXEN bit mask in REG_I2SCTRL3 (0x08):
      - PID 0x2113: bit 6 (0x0040) — verified in aw882xx_init @ 0x108a4..0x10a4c
      - PID 0x2013: bit 0 (0x0001)
    """
    return 0x0040 if chip_id == PID_2113 else 0x0001


def db_to_vol_steps(db: float) -> int:
    """Convert attenuation in dB (e.g. -2.125 or 0.0) to 0.125 dB attenuation steps [0..720]."""
    atten_db = -db if db <= 0.0 else 0.0
    steps = int(round(atten_db * 8.0))
    return max(0, min(MAX_VOL_STEPS, steps))


def encode_vol_reg(steps: int, chip_id: int = PID_2113) -> int:
    """
    Encode 0.125 dB attenuation steps into SYSCTRL2 (0x05) bits [9:0]:
      - PID 0x2113 (aw_pid_2113_set_volume @ 0x12d7c):
          bits [9:6] = steps // 48  (-6.0 dB coarse steps, 0..15)
          bits [5:0] = steps % 48   (-0.125 dB fine steps, 0..47)
          e.g. 17 -> 0x0011 (-2.125 dB), 108 -> 0x008C (-13.5 dB), 720 -> 0x03C0 (-90.0 dB)
      - PID 0x2013:
          bits [9:0] = flat steps
    """
    clamped = max(0, min(MAX_VOL_STEPS, int(steps)))
    if chip_id == PID_2113:
        coarse = clamped // 48
        fine = clamped % 48
        return ((coarse << 6) | fine) & VOL_MASK
    return clamped & VOL_MASK


def decode_vol_steps(reg_val: int, chip_id: int = PID_2113) -> int:
    """
    Decode SYSCTRL2 (0x05) bits [9:0] to 0.125 dB attenuation steps:
      - PID 0x2113 (aw_pid_2113_get_volume @ 0x12cf8):
          steps = ((reg_val >> 6) & 0x0F) * 48 + (reg_val & 0x3F)
    """
    field = reg_val & VOL_MASK
    if chip_id == PID_2113:
        return (((field >> 6) & 0x0F) * 48) + (field & 0x3F)
    return field


def vol_steps_to_db(reg_or_steps: int, chip_id: int = PID_2113) -> float:
    return -decode_vol_steps(reg_or_steps, chip_id) * 0.125


def compute_pid2113_vcalb(bus: AW88261Bus, addr: int) -> int:
    """
    Compute PID 0x2113 IV-sense VCALB (reg 0x50) from EFUSE 0x78/0x7C (ical) & 0x79/0x7D (vcal)
    matching aw_dev_reg_fw_update (0xd9e4..0xdc40) and aw882xx_init (0x10a28..0x10a8c).
    """
    ef_h4 = bus.read16(addr, REG_EFRH4)
    ef_l4 = bus.read16(addr, REG_EFRL4)
    ef_h3 = bus.read16(addr, REG_EFRH3)
    ef_l3 = bus.read16(addr, REG_EFRL3)

    # Default efcheck AND-combine (efuse_check == 0 -> bitwise AND of high & low trim words)
    ical_u10 = (ef_h4 & 0x03FF) & (ef_l4 & 0x03FF)
    if ical_u10 & 0x0200:
        ical_s = ical_u10 - 0x0400
    else:
        ical_s = ical_u10

    vcal_u10 = (ef_h3 & 0x03FF) & (ef_l3 & 0x03FF)
    if vcal_u10 & 0x0200:
        vcal_s = vcal_u10 - 0x0400
    else:
        vcal_s = vcal_u10

    ical = 1000 + ical_s
    vcal = 1000 + vcal_s
    if vcal <= 0:
        return 0x2000
    vcalb = int((ical * 0x2000) // vcal) & 0xFFFF
    return vcalb if vcalb != 0 else 0x2000


def apply_pid2113_frcset_if_needed(bus: AW88261Bus, addr: int) -> bool:
    """
    Match aw_pid_2113_frcset_check (0x13120) + aw_pid_2113_reg_force_set (0x12fa4):
    Production NX769J silicon has (0x79 & 0x7D) & 0x7C00 != 0 (0x5800), so frcset is a no-op.
    """
    ef_h3 = bus.read16(addr, REG_EFRH3)
    ef_l3 = bus.read16(addr, REG_EFRL3)
    if ((ef_h3 & ef_l3) & 0x7C00) != 0:
        return False
    bus.update_bits(addr, 0x62, 0x1000, 0x1000)
    bus.update_bits(addr, 0x64, 0x0007, 0x0004)
    bus.update_bits(addr, 0x65, 0x0003, 0x0002)
    bus.update_bits(addr, 0x66, 0xC000, 0x8000)
    bus.update_bits(addr, 0x67, 0x0008, 0x0000)
    bus.update_bits(addr, 0x68, 0x0380, 0x0100)
    return True


def verify_chip_id(bus: AW88261Bus, addr: int) -> int:
    chip_id = bus.read16(addr, REG_ID)
    if chip_id not in VALID_CHIP_IDS:
        raise RuntimeError(
            f"Unexpected chip ID at 0x{addr:02x}: 0x{chip_id:04x} "
            f"(expected one of {[hex(x) for x in sorted(VALID_CHIP_IDS)]})"
        )
    return chip_id


def wait_pll_lock(bus: AW88261Bus, addr: int, timeout_ms: int = 25) -> Tuple[bool, int]:
    deadline = time.monotonic() + (timeout_ms / 1000.0)
    last_st = 0
    while True:
        last_st = bus.read16(addr, REG_SYSST)
        if (last_st & PLL_LOCK_MASK) == PLL_LOCK_MASK and not (last_st & BIT_NOCLKS):
            return True, last_st
        if bus.dry_run or time.monotonic() >= deadline:
            return ((last_st & PLL_LOCK_MASK) == PLL_LOCK_MASK), last_st
        time.sleep(0.002)


def power_down_one(bus: AW88261Bus, addr: int) -> None:
    """Follow aw882xx_device_stop(): clear int, ULS_HMUTE, HMUTE, I2STXEN=0, AMPPD, PWDN."""
    chip_id = verify_chip_id(bus, addr)
    _ = bus.read16(addr, REG_SYSINT)
    bus.update_bits(addr, REG_SYSCTRL, BIT_ULS_HMUTE | BIT_HMUTE, BIT_ULS_HMUTE | BIT_HMUTE)
    bus.update_bits(addr, REG_I2SCTRL3, i2stxen_bit(chip_id), 0x0000)
    if not bus.dry_run:
        time.sleep(0.0015)
    bus.update_bits(addr, REG_SYSCTRL, BIT_AMPPD, BIT_AMPPD)
    bus.update_bits(addr, REG_SYSCTRL, BIT_PWDN, BIT_PWDN)


def configure_and_start_one(
    bus: AW88261Bus,
    addr: int,
    profile_regs: List[Tuple[int, int]],
    channel_role: str,
    bits_mode: str = "auto",
    volume_db_override: Optional[float] = None,
    soft_reset: bool = False,
    allow_no_bclk: bool = False,
) -> Dict[str, object]:
    """
    Program factory ACF profile registers and execute PID 0x2113 / 0x2013 power-up sequence.
    """
    chip_id = verify_chip_id(bus, addr)
    if soft_reset:
        bus.write16(addr, REG_ID, SOFT_RESET_VALUE)
        if not bus.dry_run:
            time.sleep(0.002)
        if chip_id == PID_2113:
            # Exact PID 0x2113 post-reset sequence from aw882xx_init (0x10b60..0x10b98)
            bus.write16(addr, REG_INIT_73, PID_2113_POST_RESET_73)
            if not bus.dry_run:
                time.sleep(0.003)

    prof_map = dict(profile_regs)
    target_sysctrl = prof_map.get(REG_SYSCTRL, 0x3240) | BIT_I2SEN
    target_sysctrl2 = prof_map.get(REG_SYSCTRL2, 0x0011)

    if volume_db_override is not None:
        steps = db_to_vol_steps(volume_db_override)
        target_sysctrl2 = (target_sysctrl2 & ~VOL_MASK) | encode_vol_reg(steps, chip_id)

    # Initial staged SYSCTRL: PWDN=0 (osc/PLL awake), I2SEN=1, AMPPD=1, HMUTE=1, ULS_HMUTE=1
    staged_sysctrl = (
        (target_sysctrl & ~(BIT_PWDN | BIT_AMPPD | BIT_HMUTE))
        | BIT_I2SEN
        | BIT_AMPPD
        | BIT_HMUTE
        | BIT_ULS_HMUTE
    )

    candidate_bits = [16, 32] if bits_mode == "auto" else [int(bits_mode)]
    initial_i2sctrl1 = I2SCTRL1_TABLE[candidate_bits[0]][channel_role]
    txen_mask = i2stxen_bit(chip_id)

    # Mute volume to -90 dB (0x03C0 on PID 0x2113, 0x02D0 on PID 0x2013) while programming regs
    muted_sysctrl2 = (target_sysctrl2 & ~VOL_MASK) | encode_vol_reg(MAX_VOL_STEPS, chip_id)
    bus.write16(addr, REG_SYSCTRL, staged_sysctrl)
    if not bus.dry_run:
        time.sleep(0.002)

    for reg, val in profile_regs:
        if reg == REG_SYSCTRL:
            val = staged_sysctrl
        elif reg == REG_SYSCTRL2:
            val = muted_sysctrl2
        elif reg == REG_I2SCTRL1:
            val = initial_i2sctrl1
        elif reg == REG_I2SCTRL3:
            # Disable I2STXEN (bit 6 = 0x0040 on PID 0x2113, bit 0 = 0x0001 on PID 0x2013)
            # so SmartPA SDO stays high-Z on gpio127 when IV-sense TX graph is unused.
            val &= ~txen_mask
        elif reg == REG_VSNTM1:
            continue
        bus.write16(addr, reg, val)

    if chip_id == PID_2113:
        vcalb = compute_pid2113_vcalb(bus, addr)
        bus.write16(addr, REG_VSNTM1, vcalb)
        apply_pid2113_frcset_if_needed(bus, addr)
    else:
        bus.write16(addr, REG_VSNTM1, 0x2000)

    chosen_bits = candidate_bits[0]
    locked, sysst = wait_pll_lock(bus, addr, timeout_ms=25)
    if not locked and len(candidate_bits) > 1:
        for alt_bits in candidate_bits[1:]:
            bus.write16(addr, REG_I2SCTRL1, I2SCTRL1_TABLE[alt_bits][channel_role])
            locked, sysst = wait_pll_lock(bus, addr, timeout_ms=25)
            if locked:
                chosen_bits = alt_bits
                break
        if not locked:
            bus.write16(addr, REG_I2SCTRL1, initial_i2sctrl1)

    # Optional CCO_MUX divider retry matching aw_dev_pll_check (0x54 bit 6)
    if not locked:
        orig_pll = bus.read16(addr, REG_PLLCTRL1)
        bus.write16(addr, REG_PLLCTRL1, orig_pll ^ BIT_CCO_MUX)
        locked, sysst = wait_pll_lock(bus, addr, timeout_ms=20)
        if not locked:
            bus.write16(addr, REG_PLLCTRL1, orig_pll)

    if not locked and not allow_no_bclk:
        power_down_one(bus, addr)
        return {
            "addr": f"0x{addr:02x}",
            "chip_id": f"0x{chip_id:04x}",
            "locked": False,
            "sysst": f"0x{sysst:04x}",
            "bits": chosen_bits,
            "state": "powered_down_no_bclk",
        }

    # Enable Class-D & Boost (AMPPD = 0)
    bus.update_bits(addr, REG_SYSCTRL, BIT_AMPPD, 0)
    if not bus.dry_run:
        time.sleep(0.0015)

    sysst_after = bus.read16(addr, REG_SYSST)
    faults = sysst_after & (BIT_UVLS | BIT_BSTOCS | BIT_OCDS | BIT_OTHS)
    if faults and not allow_no_bclk:
        power_down_one(bus, addr)
        raise RuntimeError(
            f"AW88261 0x{addr:02x} reported hardware protection fault in SYSST=0x{sysst_after:04x}"
        )

    # Restore target volume, clear ULS_HMUTE and HMUTE, clear latched interrupts
    bus.write16(addr, REG_SYSCTRL2, target_sysctrl2)
    bus.update_bits(addr, REG_SYSCTRL, BIT_ULS_HMUTE, 0)
    bus.update_bits(addr, REG_SYSCTRL, BIT_HMUTE, 0)
    _ = bus.read16(addr, REG_SYSINT)

    return {
        "addr": f"0x{addr:02x}",
        "chip_id": f"0x{chip_id:04x}",
        "locked": locked,
        "sysst": f"0x{sysst_after:04x}",
        "sysctrl": f"0x{bus.read16(addr, REG_SYSCTRL):04x}",
        "sysctrl2": f"0x{target_sysctrl2:04x}",
        "i2sctrl1": f"0x{bus.read16(addr, REG_I2SCTRL1):04x}",
        "i2sctrl3": f"0x{bus.read16(addr, REG_I2SCTRL3):04x}",
        "vol_db": vol_steps_to_db(target_sysctrl2, chip_id),
        "bits": chosen_bits,
        "state": "active" if locked else "unstaged_no_bclk",
    }


def apply_mode(
    bus: AW88261Bus,
    profiles: Dict[int, Dict[str, List[Tuple[int, int]]]],
    mode: str,
    bits_mode: str = "auto",
    volume_db: Optional[float] = None,
    soft_reset: bool = False,
    allow_no_bclk: bool = False,
) -> List[Dict[str, object]]:
    if mode == "off":
        results = []
        for addr in (ADDR_BOTTOM_LEFT, ADDR_TOP_RIGHT):
            power_down_one(bus, addr)
            results.append({"addr": f"0x{addr:02x}", "state": "off"})
        return results

    if mode == "speaker":
        r_left = configure_and_start_one(
            bus,
            ADDR_BOTTOM_LEFT,
            profiles[ADDR_BOTTOM_LEFT]["Music"],
            channel_role="left",
            bits_mode=bits_mode,
            volume_db_override=volume_db,
            soft_reset=soft_reset,
            allow_no_bclk=allow_no_bclk,
        )
        r_right = configure_and_start_one(
            bus,
            ADDR_TOP_RIGHT,
            profiles[ADDR_TOP_RIGHT]["Music"],
            channel_role="right",
            bits_mode=bits_mode,
            volume_db_override=volume_db,
            soft_reset=soft_reset,
            allow_no_bclk=allow_no_bclk,
        )
        return [r_left, r_right]

    if mode == "receiver":
        power_down_one(bus, ADDR_BOTTOM_LEFT)
        r_top = configure_and_start_one(
            bus,
            ADDR_TOP_RIGHT,
            profiles[ADDR_TOP_RIGHT]["Receiver"],
            channel_role="mono",
            bits_mode=bits_mode,
            volume_db_override=volume_db,
            soft_reset=soft_reset,
            allow_no_bclk=allow_no_bclk,
        )
        return [{"addr": "0x35", "state": "off"}, r_top]

    raise ValueError(f"Unsupported mode: {mode}")


def read_status(bus: AW88261Bus) -> List[Dict[str, object]]:
    out = []
    for addr, label in (
        (ADDR_BOTTOM_LEFT, "Bottom/Left"),
        (ADDR_TOP_RIGHT, "Top/Right"),
    ):
        chip_id = bus.read16(addr, REG_ID)
        sysst = bus.read16(addr, REG_SYSST)
        sysint = bus.read16(addr, REG_SYSINT)
        sysctrl = bus.read16(addr, REG_SYSCTRL)
        sysctrl2 = bus.read16(addr, REG_SYSCTRL2)
        i2sctrl1 = bus.read16(addr, REG_I2SCTRL1)
        i2sctrl3 = bus.read16(addr, REG_I2SCTRL3)
        flags = []
        for bit, name in (
            (BIT_PLLS, "PLLS"),
            (BIT_CLKS, "CLKS"),
            (BIT_NOCLKS, "NOCLKS"),
            (BIT_SWS, "SWS"),
            (BIT_BSTS, "BSTS"),
            (BIT_UVLS, "UVLS"),
            (BIT_BSTOCS, "BSTOCS"),
            (BIT_OCDS, "OCDS"),
            (BIT_OTHS, "OTHS"),
        ):
            if sysst & bit:
                flags.append(name)
        out.append(
            {
                "addr": f"0x{addr:02x}",
                "role": label,
                "chip_id": f"0x{chip_id:04x}",
                "sysst": f"0x{sysst:04x}",
                "sysst_flags": flags,
                "sysint": f"0x{sysint:04x}",
                "sysctrl": f"0x{sysctrl:04x}",
                "pwdn": bool(sysctrl & BIT_PWDN),
                "amppd": bool(sysctrl & BIT_AMPPD),
                "hmute": bool(sysctrl & BIT_HMUTE),
                "i2sen": bool(sysctrl & BIT_I2SEN),
                "rcv_mode": bool(sysctrl & BIT_RCV_MODE),
                "sysctrl2": f"0x{sysctrl2:04x}",
                "vol_db": vol_steps_to_db(sysctrl2, chip_id),
                "i2sctrl1": f"0x{i2sctrl1:04x}",
                "i2sctrl3": f"0x{i2sctrl3:04x}",
                "i2stxen": bool(i2sctrl3 & i2stxen_bit(chip_id)),
            }
        )
    return out


def discover_alsa_pcm_card(
    pcm_device_index: int = 1,
    explicit_card: Optional[int] = None,
    asound_root: str = "/proc/asound",
) -> Optional[int]:
    """Locate the ALSA card index hosting NX769J MultiMedia2 playback (pcm<D>p)."""
    if explicit_card is not None:
        status_path = os.path.join(
            asound_root, f"card{explicit_card}", f"pcm{pcm_device_index}p", "sub0", "status"
        )
        return explicit_card if os.path.exists(status_path) else None

    cards_file = os.path.join(asound_root, "cards")
    preferred: List[int] = []
    fallback: List[int] = []
    if os.path.exists(cards_file):
        try:
            with open(cards_file, "r", encoding="utf-8") as f:
                for line in f:
                    m = re.match(r"^\s*(\d+)\s+\[([^\]]+)\]:\s*(.*)$", line)
                    if not m:
                        continue
                    idx = int(m.group(1))
                    desc = f"{m.group(2)} {m.group(3)}".lower()
                    status_path = os.path.join(
                        asound_root, f"card{idx}", f"pcm{pcm_device_index}p", "sub0", "status"
                    )
                    if not os.path.exists(status_path):
                        continue
                    if any(tok in desc for tok in ("nx769j", "sm8650", "qcom", "snd-soc")):
                        preferred.append(idx)
                    else:
                        fallback.append(idx)
        except OSError:
            pass

    if preferred:
        return preferred[0]
    if fallback:
        return fallback[0]

    for path in sorted(glob.glob(os.path.join(asound_root, f"card*/pcm{pcm_device_index}p/sub0/status"))):
        m = re.search(r"card(\d+)", path)
        if m:
            return int(m.group(1))
    return None


def is_mm2_pcm_running(
    pcm_device_index: int = 1,
    card_index: Optional[int] = None,
    asound_root: str = "/proc/asound",
) -> bool:
    """Check whether ALSA MultiMedia2 playback substream (pcm<D>p/sub0) is in RUNNING state."""
    if card_index is not None:
        paths = [
            os.path.join(asound_root, f"card{card_index}", f"pcm{pcm_device_index}p", "sub0", "status")
        ]
    else:
        paths = glob.glob(os.path.join(asound_root, f"card*/pcm{pcm_device_index}p/sub0/status"))
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8") as f:
                head = f.read(128)
            if "state: RUNNING" in head:
                return True
        except OSError:
            continue
    return False


class AlsaPcmTimerMonitor:
    """
    Zero-CPU kernel-blocking ALSA PCM state monitor via /dev/snd/timer.

    Attaches as an application slave instance to SNDRV_TIMER_CLASS_PCM for
    (card, pcm_device, playback substream 0). Because PCM_TIMER_EVENT_FILTER
    clears RESOLUTION (bit 0) and TICK (bit 1), snd_timer_user_tinterrupt()
    returns without waking select()/poll() on DMA period interrupts — waking
    ONLY on snd_pcm_post_{start,stop,pause,suspend,resume,drain_init} transitions!
    """

    def __init__(
        self,
        card_index: int,
        pcm_device_index: int = 1,
        substream_index: int = 0,
        timer_dev_path: str = "/dev/snd/timer",
    ):
        self.card_index = card_index
        self.pcm_device_index = pcm_device_index
        self.substream_index = substream_index
        self.timer_dev_path = timer_dev_path
        self.fd: Optional[int] = None
        self.record_size: int = TREAD64_STRUCT.size

    def open_and_start(self) -> int:
        self.close()
        flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0)
        fd = os.open(self.timer_dev_path, flags)
        try:
            tread_arg = struct.pack("<i", 1)
            try:
                fcntl.ioctl(fd, SNDRV_TIMER_IOCTL_TREAD64, tread_arg)
                self.record_size = 32
            except OSError as exc:
                if exc.errno != errno.ENOTTY:
                    raise
                fcntl.ioctl(fd, SNDRV_TIMER_IOCTL_TREAD_OLD, tread_arg)
                self.record_size = 32

            # Playback substream N uses subdevice = (N << 1) | 0 (Capture uses | 1)
            subdevice = (self.substream_index << 1) | 0
            sel_buf = struct.pack(
                "<iiiii32x",
                SNDRV_TIMER_CLASS_PCM,
                SNDRV_TIMER_SCLASS_NONE,
                self.card_index,
                self.pcm_device_index,
                subdevice,
            )
            fcntl.ioctl(fd, SNDRV_TIMER_IOCTL_SELECT, sel_buf)

            # CRITICAL (verified in snd_timer_user_start +0x44): ticks MUST be >= 1
            # even for SNDRV_TIMER_CLASS_PCM slave timers or START returns -EINVAL.
            params_buf = struct.pack(
                "<IIIII60x",
                SNDRV_TIMER_PSFLG_AUTO,
                1,                       # ticks (must be >= 1)
                64,                      # queue_size (events)
                0,                       # reserved0
                PCM_TIMER_EVENT_FILTER,  # state-change events ONLY (0x0006F1BC)
            )
            fcntl.ioctl(fd, SNDRV_TIMER_IOCTL_PARAMS, params_buf)
            fcntl.ioctl(fd, SNDRV_TIMER_IOCTL_START, 0)
        except Exception:
            os.close(fd)
            raise

        self.fd = fd
        # Drain the initial SNDRV_TIMER_EVENT_START emitted by SNDRV_TIMER_IOCTL_START
        self.drain_events()
        return fd

    def drain_events(self) -> List[Tuple[int, int]]:
        """Read all queued (event, val) tuples without blocking."""
        if self.fd is None:
            return []
        events: List[Tuple[int, int]] = []
        while True:
            try:
                raw = os.read(self.fd, 2048)
            except BlockingIOError:
                break
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EINTR):
                    break
                self.close()
                break
            if not raw:
                self.close()
                break
            step = self.record_size
            for off in range(0, (len(raw) // step) * step, step):
                ev, _pad1, _sec, _nsec, val, _pad2 = TREAD64_STRUCT.unpack_from(raw, off)
                events.append((ev, val))
        return events

    def fileno(self) -> int:
        if self.fd is None:
            raise RuntimeError("AlsaPcmTimerMonitor is not open")
        return self.fd

    def close(self) -> None:
        if self.fd is not None:
            try:
                fcntl.ioctl(self.fd, SNDRV_TIMER_IOCTL_STOP, 0)
            except OSError:
                pass
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None


def _load_inotify_libc() -> ctypes.CDLL:
    libc = ctypes.CDLL("libc.so.6", use_errno=True)
    libc.inotify_init1.argtypes = [ctypes.c_int]
    libc.inotify_init1.restype = ctypes.c_int
    libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
    libc.inotify_add_watch.restype = ctypes.c_int
    libc.inotify_rm_watch.argtypes = [ctypes.c_int, ctypes.c_int]
    libc.inotify_rm_watch.restype = ctypes.c_int
    return libc


def write_mode_file(mode_file: str, mode: str) -> None:
    if mode not in ("speaker", "receiver", "off"):
        raise ValueError(f"Invalid mode {mode!r}")
    state_dir = os.path.dirname(mode_file)
    if state_dir and not os.path.isdir(state_dir):
        os.makedirs(state_dir, mode=0o777, exist_ok=True)
        try:
            os.chmod(state_dir, 0o777)
        except OSError:
            pass
    tmp = f"{mode_file}.tmp.{os.getpid()}"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(f"{mode}\n")
    try:
        os.chmod(tmp, 0o666)
    except OSError:
        pass
    os.replace(tmp, mode_file)


def read_mode_file(mode_file: str, default: str = "speaker") -> str:
    try:
        with open(mode_file, "r", encoding="utf-8") as f:
            val = f.read().strip().lower()
        if val in ("speaker", "receiver", "off"):
            return val
    except OSError:
        pass
    return default


def run_daemon(
    bus: AW88261Bus,
    profiles: Dict[int, Dict[str, List[Tuple[int, int]]]],
    mode_file: str = DEFAULT_MODE_FILE,
    pcm_device_index: int = 1,
    explicit_card: Optional[int] = None,
    bits_mode: str = "auto",
    volume_db: Optional[float] = None,
    asound_root: str = "/proc/asound",
    snd_dev_dir: str = "/dev/snd",
    pcm_monitor_factory: Optional[Callable[[int, int], object]] = None,
    mode_wakeup_fd: Optional[int] = None,
    stop_check: Optional[Callable[[], bool]] = None,
) -> int:
    """
    Zero-CPU Event-Driven SmartPA Daemon:
      1. Attaches to ALSA slave timer P<card>-<pcm_dev>-0 via /dev/snd/timer
         (wakes ONLY on kernel PCM state transitions; 0 wakeups idle & 0 wakeups playing).
      2. Watches /run/nx769j-smartpa ONCE via inotify (IN_CREATE | IN_MOVED_TO | IN_MODIFY)
         for UCM / CLI mode changes ('speaker' | 'receiver' | 'off').
      3. NEVER watches /dev/snd with IN_OPEN/IN_CLOSE and NEVER scans /dev/snd in the loop.
    """
    state_dir = os.path.dirname(mode_file) or DEFAULT_STATE_DIR
    os.makedirs(state_dir, mode=0o777, exist_ok=True)
    try:
        os.chmod(state_dir, 0o777)
    except OSError:
        pass
    if not os.path.exists(mode_file):
        write_mode_file(mode_file, "speaker")

    apply_mode(bus, profiles, "off")

    IN_NONBLOCK = 0x00000800
    IN_CLOEXEC = 0x00080000
    IN_CREATE = 0x00000100
    IN_MOVED_TO = 0x00000080
    IN_MODIFY = 0x00000002

    libc = None
    ifd: Optional[int] = mode_wakeup_fd
    owns_ifd = False
    snd_create_wd: Optional[int] = None

    if ifd is None:
        libc = _load_inotify_libc()
        ifd = libc.inotify_init1(IN_NONBLOCK | IN_CLOEXEC)
        if ifd < 0:
            err = ctypes.get_errno()
            raise OSError(err, os.strerror(err))
        owns_ifd = True
        wd = libc.inotify_add_watch(
            ifd, state_dir.encode("utf-8"), IN_CREATE | IN_MOVED_TO | IN_MODIFY
        )
        if wd < 0:
            err = ctypes.get_errno()
            os.close(ifd)
            raise OSError(err, os.strerror(err))

    stopping = False
    prev_sigterm = None
    prev_sigint = None

    def _on_sig(signum, frame):
        nonlocal stopping
        stopping = True

    if stop_check is None:
        prev_sigterm = signal.signal(signal.SIGTERM, _on_sig)
        prev_sigint = signal.signal(signal.SIGINT, _on_sig)

    factory = pcm_monitor_factory or (
        lambda c_idx, p_idx: AlsaPcmTimerMonitor(
            card_index=c_idx,
            pcm_device_index=p_idx,
            substream_index=0,
            timer_dev_path=os.path.join(snd_dev_dir, "timer"),
        )
    )

    pcm_mon: Optional[object] = None
    bound_card: Optional[int] = None
    active_hw_mode = "off"
    bclk_retries_left = 0
    bclk_warned = False
    pending_off_deadline: Optional[float] = None

    def _ensure_pcm_monitor() -> None:
        nonlocal pcm_mon, bound_card, snd_create_wd
        if pcm_mon is not None and getattr(pcm_mon, "fd", None) is not None:
            return
        card_idx = discover_alsa_pcm_card(
            pcm_device_index=pcm_device_index,
            explicit_card=explicit_card,
            asound_root=asound_root,
        )
        if card_idx is not None:
            mon = factory(card_idx, pcm_device_index)
            mon.open_and_start()  # type: ignore[attr-defined]
            pcm_mon = mon
            bound_card = card_idx
            if libc is not None and snd_create_wd is not None and ifd is not None:
                libc.inotify_rm_watch(ifd, snd_create_wd)
                snd_create_wd = None
            print(
                f"[aw88261-daemon] bound ALSA PCM slave timer P{card_idx}-{pcm_device_index}-0 "
                f"(zero-CPU event mode)",
                flush=True,
            )
            return

        # Sound card not yet probed at early boot: watch /dev/snd ONLY for IN_CREATE
        # (NEVER IN_OPEN/IN_CLOSE!) and remove watch as soon as card binds.
        if (
            libc is not None
            and snd_create_wd is None
            and ifd is not None
            and os.path.isdir(snd_dev_dir)
        ):
            wd = libc.inotify_add_watch(ifd, snd_dev_dir.encode("utf-8"), IN_CREATE)
            if wd >= 0:
                snd_create_wd = wd

    try:
        while not stopping and not (stop_check is not None and stop_check()):
            _ensure_pcm_monitor()
            now = time.monotonic()

            desired_mode = read_mode_file(mode_file, default="speaker")
            running = (
                is_mm2_pcm_running(
                    pcm_device_index=pcm_device_index,
                    card_index=bound_card,
                    asound_root=asound_root,
                )
                if bound_card is not None
                else False
            )

            if not running:
                bclk_retries_left = 0
                bclk_warned = False

            target_hw_mode = desired_mode if running else "off"

            if target_hw_mode != "off":
                pending_off_deadline = None
                if target_hw_mode != active_hw_mode and not bclk_warned:
                    res = apply_mode(
                        bus,
                        profiles,
                        target_hw_mode,
                        bits_mode=bits_mode,
                        volume_db=volume_db,
                        allow_no_bclk=False,
                    )
                    if any(r.get("locked") for r in res):
                        active_hw_mode = target_hw_mode
                        bclk_retries_left = 0
                        print(
                            f"[aw88261-daemon] hw_mode -> {active_hw_mode}: {json.dumps(res)}",
                            flush=True,
                        )
                    else:
                        if bclk_retries_left == 0:
                            bclk_retries_left = MAX_BCLK_RETRIES
                        else:
                            bclk_retries_left -= 1
                            if bclk_retries_left == 0:
                                bclk_warned = True
                                print(
                                    "[aw88261-daemon] warning: PCM RUNNING without I2S BCLK lock "
                                    f"after {MAX_BCLK_RETRIES} retries; sleeping until next event",
                                    flush=True,
                                )
            else:
                # target_hw_mode == "off"
                if active_hw_mode != "off":
                    # Immediate power-down if user explicitly requested mode="off";
                    # single-shot 30 ms debounce on stream stop to bridge gapless track seeks.
                    if desired_mode == "off":
                        pending_off_deadline = None
                    elif pending_off_deadline is None:
                        pending_off_deadline = now + STOP_DEBOUNCE_SEC

                    if pending_off_deadline is None or now >= pending_off_deadline:
                        pending_off_deadline = None
                        res = apply_mode(bus, profiles, "off")
                        active_hw_mode = "off"
                        print(
                            f"[aw88261-daemon] hw_mode -> off: {json.dumps(res)}",
                            flush=True,
                        )

            # Compute blocking timeout: None in all steady states (both idle AND playing!)
            now = time.monotonic()
            if bclk_retries_left > 0 and not bclk_warned:
                timeout: Optional[float] = BCLK_RETRY_INTERVAL_SEC
            elif pending_off_deadline is not None:
                timeout = max(0.0, pending_off_deadline - now)
            else:
                timeout = None

            wait_fds: List[int] = []
            if ifd is not None:
                wait_fds.append(ifd)
            if pcm_mon is not None and getattr(pcm_mon, "fd", None) is not None:
                wait_fds.append(pcm_mon.fileno())  # type: ignore[attr-defined]

            try:
                rlist, _, _ = select.select(wait_fds, [], [], timeout)
            except InterruptedError:
                continue

            if ifd is not None and ifd in rlist:
                while True:
                    try:
                        chunk = os.read(ifd, 4096)
                        if not chunk:
                            break
                    except BlockingIOError:
                        break
                    except OSError as exc:
                        if exc.errno in (errno.EAGAIN, errno.EINTR):
                            break
                        break
                # Re-allow BCLK lock attempt if user rewrote mode file
                bclk_warned = False

            if (
                pcm_mon is not None
                and getattr(pcm_mon, "fd", None) is not None
                and pcm_mon.fileno() in rlist  # type: ignore[attr-defined]
            ):
                evs = pcm_mon.drain_events()  # type: ignore[attr-defined]
                if evs:
                    bclk_warned = False
    finally:
        if prev_sigterm is not None:
            signal.signal(signal.SIGTERM, prev_sigterm)
        if prev_sigint is not None:
            signal.signal(signal.SIGINT, prev_sigint)
        if pcm_mon is not None:
            try:
                pcm_mon.close()  # type: ignore[attr-defined]
            except Exception:
                pass
        try:
            apply_mode(bus, profiles, "off")
        except Exception:
            pass
        if owns_ifd and ifd is not None:
            try:
                os.close(ifd)
            except OSError:
                pass
    return 0


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="NX769J Dual Awinic AW88261 SmartPA controller (0x35 Left, 0x34 Right)"
    )
    p.add_argument(
        "command",
        choices=["speaker", "receiver", "off", "status", "set-mode", "daemon"],
        help="Action to perform",
    )
    p.add_argument(
        "mode_arg",
        nargs="?",
        choices=["speaker", "receiver", "off"],
        help="Target mode for 'set-mode'",
    )
    p.add_argument("--bus", help="Explicit I2C bus path or number (default: auto 98c000.i2c)")
    p.add_argument(
        "--bits",
        choices=["auto", "16", "32"],
        default="auto",
        help="I2S slot width (default: auto -> 16-bit/32fs for sc8280xp S16_LE, fallback 32-bit/64fs)",
    )
    p.add_argument(
        "--volume-db",
        type=float,
        default=None,
        help="Override factory volume in dB (default uses factory ACF: -2.125 dB Music, -13.5 dB Receiver)",
    )
    p.add_argument("--acf-json", help="Optional path to aw88261_acf_parsed.json override")
    p.add_argument("--mode-file", default=DEFAULT_MODE_FILE, help="Daemon mode state file")
    p.add_argument("--card", type=int, default=None, help="Explicit ALSA card index (default: auto)")
    p.add_argument("--pcm-dev", type=int, default=1, help="ALSA MultiMedia2 PCM index (default: 1)")
    p.add_argument("--reset", action="store_true", help="Issue soft reset (0x00=0x55aa + 0x73=0x1d40) before init")
    p.add_argument(
        "--allow-no-bclk",
        action="store_true",
        help="Unstage amps even if I2S BCLK is not yet running (testing only)",
    )
    p.add_argument("--wait-bclk-ms", type=int, default=0, help="Wait up to N ms for PCM RUNNING before init")
    p.add_argument("--dry-run", action="store_true", help="Simulate I2C transactions without hardware access")
    p.add_argument("-v", "--verbose", action="store_true", help="Log every I2C register read/write")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)

    if args.command == "set-mode":
        if not args.mode_arg:
            raise SystemExit("error: 'set-mode' requires one of: speaker, receiver, off")
        if args.dry_run:
            print(json.dumps({"mode_file": args.mode_file, "set_mode": args.mode_arg, "dry_run": True}))
            return 0
        write_mode_file(args.mode_file, args.mode_arg)
        return 0

    profiles = load_profiles(args.acf_json)
    dev_path = discover_i2c_bus(args.bus, dry_run=args.dry_run)

    if args.wait_bclk_ms > 0 and not args.dry_run and args.command in ("speaker", "receiver"):
        deadline = time.monotonic() + (args.wait_bclk_ms / 1000.0)
        while time.monotonic() < deadline and not is_mm2_pcm_running(args.pcm_dev, args.card):
            time.sleep(0.01)

    with AW88261Bus(dev_path, dry_run=args.dry_run, verbose=args.verbose) as bus:
        if args.command == "status":
            print(json.dumps({"bus": dev_path, "devices": read_status(bus)}, indent=2))
            return 0
        if args.command == "daemon":
            return run_daemon(
                bus,
                profiles,
                mode_file=args.mode_file,
                pcm_device_index=args.pcm_dev,
                explicit_card=args.card,
                bits_mode=args.bits,
                volume_db=args.volume_db,
            )
        res = apply_mode(
            bus,
            profiles,
            args.command,
            bits_mode=args.bits,
            volume_db=args.volume_db,
            soft_reset=args.reset,
            allow_no_bclk=args.allow_no_bclk,
        )
        print(json.dumps({"bus": dev_path, "mode": args.command, "results": res}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
