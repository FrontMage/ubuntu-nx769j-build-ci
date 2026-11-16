#!/usr/bin/env python3
"""
nx769j-charge-limit — Smart Battery Charge-Control CLI & Boot Helper for NX769J (SM8650)

Hardware & Kernel Contract (verified by disassembling 7.1.1-nx769j+ qcom_battmgr.ko
and inspecting NX769J factory ADSP firmware adsp.mdt / BattHealthCfg):
  - Power supply node : /sys/class/power_supply/qcom-battmgr-bat (qcom,sm8650-pmic-glink)
  - PMIC-GLINK request: owner=0x800a (BATTMGR), type=1, opcode=0x00000048
                        (BATTMGR_CHG_CTRL_LIMIT_EN -> ADSP battmngr_set_charging_limit)
                        payload: enable=1, target_soc=end, delta_soc=(end - start)
  - Kernel clamping in qcom_battmgr_bat_set_property (0xc44..0xe44):
      * charge_control_end_threshold   : clamped to [55, 100]
      * charge_control_start_threshold : clamped to [50, 95]
      * Minimum recommended hysteresis : end - start >= 5 (%)
  - Write-order & silent-error verification:
      1. Always write `charge_control_end_threshold` FIRST, then
         `charge_control_start_threshold` SECOND (guarantees deterministic convergence
         from any prior (start, end) pair including boot default (0, 0)).
      2. Because qcom_battmgr_bat_set_property() returns 0 even if the 250-jiffy
         PMIC-GLINK ACK times out (skipping cache update at [x19+0x8c..0x90]),
         this tool ALWAYS reads back both sysfs attributes to confirm ADSP battman
         accepted opcode 0x48!
"""

from __future__ import annotations

import argparse
import errno
import json
import os
import re
import sys
import time
from typing import Dict, Optional, Tuple

DEFAULT_SYSFS_ROOT = "/sys/class/power_supply"
BAT_NODE_NAME = "qcom-battmgr-bat"
USB_NODE_NAME = "qcom-battmgr-usb"
DEFAULT_CONFIG_PATH = "/etc/default/nx769j-charge-limit"

MIN_END_SOC = 55
MAX_END_SOC = 100
MIN_START_SOC = 50
MAX_START_SOC = 95
MIN_HYSTERESIS = 5

PRESETS: Dict[str, Tuple[int, int, str]] = {
    "80": (75, 80, "Conservative desk/storage limit (75%..80%)"),
    "85": (80, 85, "Recommended continuous USB/desk limit (80%..85%, zero float heat)"),
    "90": (85, 90, "Balanced daily limit (85%..90%)"),
    "95": (90, 95, "High-capacity limit with top-voltage protection (90%..95%)"),
    "100": (95, 100, "Full charge with 5% anti-trickle recharge hysteresis (95%..100%)"),
}


def _read_text(path: str) -> Optional[str]:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return None


def _read_int(path: str) -> Optional[int]:
    raw = _read_text(path)
    if raw is None or raw == "":
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def parse_active_usb_type(raw: Optional[str]) -> str:
    if not raw:
        return "Unknown"
    m = re.search(r"\[([^\]]+)\]", raw)
    return m.group(1) if m else raw.strip()


def validate_thresholds(start_soc: int, end_soc: int) -> Tuple[int, int]:
    if not (MIN_END_SOC <= end_soc <= MAX_END_SOC):
        raise ValueError(
            f"end threshold {end_soc}% out of kernel range [{MIN_END_SOC}..{MAX_END_SOC}]"
        )
    if not (MIN_START_SOC <= start_soc <= MAX_START_SOC):
        raise ValueError(
            f"start threshold {start_soc}% out of kernel range [{MIN_START_SOC}..{MAX_START_SOC}]"
        )
    if end_soc - start_soc < MIN_HYSTERESIS:
        raise ValueError(
            f"hysteresis (end={end_soc}% - start={start_soc}% = {end_soc - start_soc}%) "
            f"must be at least {MIN_HYSTERESIS}% to prevent PMIC rapid charge cycling"
        )
    return start_soc, end_soc


def load_config(config_path: str = DEFAULT_CONFIG_PATH) -> Dict[str, int]:
    cfg = {
        "ENABLED": 1,
        "CHARGE_END_THRESHOLD": 85,
        "CHARGE_START_THRESHOLD": 80,
    }
    if not os.path.exists(config_path):
        return cfg
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k = k.strip()
                v = v.split("#", 1)[0].strip().strip('"').strip("'")
                if k in cfg:
                    cfg[k] = int(v)
    except (OSError, ValueError) as exc:
        raise RuntimeError(f"Failed to parse {config_path}: {exc}") from exc
    return cfg


def save_config(start_soc: int, end_soc: int, config_path: str = DEFAULT_CONFIG_PATH) -> None:
    parent = os.path.dirname(config_path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    content = (
        "# NX769J Smart Battery Charge-Control Configuration (SM8650 ADSP battman opcode 0x48)\n"
        "# Managed by nx769j-charge-limit. Valid ranges:\n"
        "#   CHARGE_END_THRESHOLD   : 55..100 (stop charging at this SoC %)\n"
        "#   CHARGE_START_THRESHOLD : 50..95  (resume charging at/below this SoC %, <= END - 5)\n"
        "ENABLED=1\n"
        f"CHARGE_END_THRESHOLD={end_soc}\n"
        f"CHARGE_START_THRESHOLD={start_soc}\n"
    )
    tmp_path = f"{config_path}.tmp.{os.getpid()}"
    with open(tmp_path, "w", encoding="utf-8") as f:
        f.write(content)
    os.chmod(tmp_path, 0o644)
    os.replace(tmp_path, config_path)


def collect_status(
    sysfs_root: str = DEFAULT_SYSFS_ROOT,
    config_path: str = DEFAULT_CONFIG_PATH,
) -> Dict[str, object]:
    bat_dir = os.path.join(sysfs_root, BAT_NODE_NAME)
    usb_dir = os.path.join(sysfs_root, USB_NODE_NAME)

    if not os.path.isdir(bat_dir):
        raise FileNotFoundError(f"Battery power_supply node not found: {bat_dir}")

    v_bat_uv = _read_int(os.path.join(bat_dir, "voltage_now"))
    i_bat_ua = _read_int(os.path.join(bat_dir, "current_now"))
    p_bat_uw = _read_int(os.path.join(bat_dir, "power_now"))
    temp_deci_c = _read_int(os.path.join(bat_dir, "temp"))
    start_thr = _read_int(os.path.join(bat_dir, "charge_control_start_threshold"))
    end_thr = _read_int(os.path.join(bat_dir, "charge_control_end_threshold"))

    v_usb_uv = _read_int(os.path.join(usb_dir, "voltage_now")) if os.path.isdir(usb_dir) else None
    i_usb_ua = _read_int(os.path.join(usb_dir, "current_now")) if os.path.isdir(usb_dir) else None
    icl_ua = _read_int(os.path.join(usb_dir, "input_current_limit")) if os.path.isdir(usb_dir) else None
    usb_online = _read_int(os.path.join(usb_dir, "online")) if os.path.isdir(usb_dir) else None
    usb_type_raw = _read_text(os.path.join(usb_dir, "usb_type")) if os.path.isdir(usb_dir) else None

    usb_power_w = None
    if v_usb_uv is not None and i_usb_ua is not None:
        usb_power_w = round((v_usb_uv / 1_000_000.0) * (i_usb_ua / 1_000_000.0), 3)

    cfg: Optional[Dict[str, int]] = None
    if os.path.exists(config_path):
        try:
            cfg = load_config(config_path)
        except Exception:
            cfg = None

    limit_active = bool(end_thr and end_thr >= MIN_END_SOC)

    return {
        "battery": {
            "node": bat_dir,
            "status": _read_text(os.path.join(bat_dir, "status")) or "Unknown",
            "charge_type": _read_text(os.path.join(bat_dir, "charge_type")) or "Unknown",
            "health": _read_text(os.path.join(bat_dir, "health")) or "Unknown",
            "capacity_pct": _read_int(os.path.join(bat_dir, "capacity")),
            "temp_c": round(temp_deci_c / 10.0, 1) if temp_deci_c is not None else None,
            "voltage_v": round(v_bat_uv / 1_000_000.0, 3) if v_bat_uv is not None else None,
            "cell_avg_v": round(v_bat_uv / 2_000_000.0, 3) if v_bat_uv is not None else None,
            "current_ma": round(i_bat_ua / 1_000.0, 1) if i_bat_ua is not None else None,
            "power_w": round(p_bat_uw / 1_000_000.0, 3) if p_bat_uw is not None else None,
            "cycle_count": _read_int(os.path.join(bat_dir, "cycle_count")),
            "state_of_health_pct": _read_int(os.path.join(bat_dir, "state_of_health")),
        },
        "charge_control": {
            "active": limit_active,
            "start_threshold_pct": start_thr,
            "end_threshold_pct": end_thr,
            "hysteresis_pct": (end_thr - start_thr) if (start_thr is not None and end_thr is not None and limit_active) else None,
            "configured_default": cfg,
        },
        "usb": {
            "online": bool(usb_online) if usb_online is not None else None,
            "type": parse_active_usb_type(usb_type_raw),
            "voltage_v": round(v_usb_uv / 1_000_000.0, 3) if v_usb_uv is not None else None,
            "current_ma": round(i_usb_ua / 1_000.0, 1) if i_usb_ua is not None else None,
            "input_limit_ma": round(icl_ua / 1_000.0, 1) if icl_ua is not None else None,
            "estimated_power_w": usb_power_w,
        },
    }


def format_status_human(st: Dict[str, object]) -> str:
    bat = st["battery"]  # type: ignore[index]
    cc = st["charge_control"]  # type: ignore[index]
    usb = st["usb"]  # type: ignore[index]

    if cc["active"]:  # type: ignore[index]
        cc_line = (
            f"ACTIVE — stop at {cc['end_threshold_pct']}%, "  # type: ignore[index]
            f"resume <= {cc['start_threshold_pct']}% "  # type: ignore[index]
            f"(delta={cc['hysteresis_pct']}%)"  # type: ignore[index]
        )
    else:
        cc_line = (
            f"UNCONFIGURED (sysfs start={cc['start_threshold_pct']}%, "  # type: ignore[index]
            f"end={cc['end_threshold_pct']}%)"  # type: ignore[index]
        )

    cfg = cc.get("configured_default")  # type: ignore[attr-defined]
    if isinstance(cfg, dict):
        cfg_line = (
            f"ENABLED={cfg.get('ENABLED', 1)} "
            f"({cfg.get('CHARGE_START_THRESHOLD')}%..{cfg.get('CHARGE_END_THRESHOLD')}%)"
        )
    else:
        cfg_line = "none"

    lines = [
        "NX769J Battery & Charge-Control Status",
        f"  Status / Capacity : {bat['status']} ({bat['charge_type']}) | {bat['capacity_pct']}% | Health: {bat['health']} (SoH {bat['state_of_health_pct']}%, {bat['cycle_count']} cycles)",  # type: ignore[index]
        f"  Pack Telemetry    : {bat['voltage_v']} V (2S, ~{bat['cell_avg_v']} V/cell) | {bat['current_ma']} mA | {bat['power_w']} W | Temp: {bat['temp_c']} C",  # type: ignore[index]
        f"  Charge Limit      : {cc_line}",
        f"  Boot Default      : {cfg_line}",
        f"  USB Input         : online={usb['online']} ({usb['type']}) | {usb['voltage_v']} V @ {usb['current_ma']} mA (limit {usb['input_limit_ma']} mA, ~{usb['estimated_power_w']} W)",  # type: ignore[index]
    ]
    return "\n".join(lines)


def program_thresholds(
    start_soc: int,
    end_soc: int,
    sysfs_root: str = DEFAULT_SYSFS_ROOT,
    wait_sec: float = 5.0,
    retry_interval: float = 0.5,
    dry_run: bool = False,
) -> Tuple[int, int]:
    """
    Write end_threshold FIRST then start_threshold SECOND to qcom-battmgr-bat,
    and verify by reading back both sysfs nodes that ADSP opcode 0x48 succeeded.
    """
    validate_thresholds(start_soc, end_soc)
    bat_dir = os.path.join(sysfs_root, BAT_NODE_NAME)
    end_path = os.path.join(bat_dir, "charge_control_end_threshold")
    start_path = os.path.join(bat_dir, "charge_control_start_threshold")

    if dry_run:
        return start_soc, end_soc

    deadline = time.monotonic() + max(0.0, wait_sec)
    last_err = "unknown error"

    while True:
        try:
            if not (os.path.exists(end_path) and os.path.exists(start_path)):
                raise FileNotFoundError(
                    f"{end_path} not yet registered (waiting for qcom_battmgr / pmic_glink)"
                )

            # 1. Write END threshold first (clamped [55..100] in kernel)
            with open(end_path, "w", encoding="utf-8") as f:
                f.write(f"{end_soc}\n")

            # 2. Write START threshold second (clamped [50..95] in kernel, computes delta = end - start)
            with open(start_path, "w", encoding="utf-8") as f:
                f.write(f"{start_soc}\n")

            # 3. Mandatory readback check (catches silent 250-jiffy PMIC-GLINK timeouts in qcom_battmgr.ko)
            rb_end = _read_int(end_path)
            rb_start = _read_int(start_path)
            if rb_end == end_soc and rb_start == start_soc:
                return rb_start, rb_end

            last_err = (
                f"ADSP battman readback mismatch after opcode 0x48 write: "
                f"wrote ({start_soc}%, {end_soc}%), read back ({rb_start}%, {rb_end}%)"
            )
        except PermissionError as exc:
            raise PermissionError(
                f"Permission denied writing {bat_dir} (run with sudo, e.g. `sudo nx769j-charge-limit {end_soc}`)"
            ) from exc
        except OSError as exc:
            if exc.errno == errno.EACCES:
                raise PermissionError(
                    f"Permission denied writing {bat_dir} (run with sudo, e.g. `sudo nx769j-charge-limit {end_soc}`)"
                ) from exc
            last_err = f"{type(exc).__name__}: {exc}"

        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"Failed to apply charge limit ({start_soc}%..{end_soc}%) within {wait_sec:.1f}s: {last_err}"
            )
        time.sleep(retry_interval)


def build_parser() -> argparse.ArgumentParser:
    epilog = (
        "Examples:\n"
        "  nx769j-charge-limit status           # Show battery, USB power, and threshold state\n"
        "  sudo nx769j-charge-limit 85          # Stop at 85%, resume <= 80% (default desk mode)\n"
        "  sudo nx769j-charge-limit 95          # Stop at 95%, resume <= 90%\n"
        "  sudo nx769j-charge-limit 100         # Full 100% charge with 95% anti-trickle hysteresis\n"
        "  sudo nx769j-charge-limit 88 --start 80\n"
        "  sudo nx769j-charge-limit apply       # Apply /etc/default/nx769j-charge-limit at boot\n"
    )
    p = argparse.ArgumentParser(
        prog="nx769j-charge-limit",
        description="NX769J Smart Battery Charge-Control Threshold CLI (SM8650 ADSP battman)",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "target",
        nargs="?",
        default="status",
        help="Command or end-SoC preset: status | apply | 80 | 85 | 90 | 95 | 100 | <55..100>",
    )
    p.add_argument(
        "--start",
        type=int,
        default=None,
        help="Explicit recharge start SoC %% (50..95, must be <= end - 5; default: end - 5)",
    )
    p.add_argument(
        "--runtime-only",
        action="store_true",
        help="Apply to live kernel/ADSP only without updating /etc/default/nx769j-charge-limit",
    )
    p.add_argument(
        "--persist",
        action="store_true",
        help="Persist threshold to /etc/default/nx769j-charge-limit (default for preset commands)",
    )
    p.add_argument(
        "--wait-sec",
        type=float,
        default=None,
        help="Seconds to wait for ADSP battmgr readiness (default: 45s for apply, 5s otherwise)",
    )
    p.add_argument("--sysfs-root", default=DEFAULT_SYSFS_ROOT, help="Override /sys/class/power_supply path")
    p.add_argument("--config", default=DEFAULT_CONFIG_PATH, help="Override /etc/default/nx769j-charge-limit path")
    p.add_argument("--json", action="store_true", help="Emit machine-readable JSON output")
    p.add_argument("--dry-run", action="store_true", help="Validate without writing sysfs or config")
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    target = str(args.target).strip().lower()

    if target == "status":
        st = collect_status(sysfs_root=args.sysfs_root, config_path=args.config)
        if args.json:
            print(json.dumps(st, indent=2))
        else:
            print(format_status_human(st))
        return 0

    if target == "apply":
        cfg = load_config(args.config)
        if not cfg.get("ENABLED", 1):
            msg = {"applied": False, "reason": "ENABLED=0 in config", "config": args.config}
            print(json.dumps(msg) if args.json else f"nx769j-charge-limit: skipped ({msg['reason']})")
            return 0
        end_soc = int(cfg["CHARGE_END_THRESHOLD"])
        start_soc = int(cfg["CHARGE_START_THRESHOLD"])
        wait_s = args.wait_sec if args.wait_sec is not None else 45.0
        rb_start, rb_end = program_thresholds(
            start_soc=start_soc,
            end_soc=end_soc,
            sysfs_root=args.sysfs_root,
            wait_sec=wait_s,
            dry_run=args.dry_run,
        )
        out = {
            "applied": True,
            "mode": "apply",
            "start_threshold_pct": rb_start,
            "end_threshold_pct": rb_end,
            "dry_run": args.dry_run,
        }
        if args.json:
            print(json.dumps(out, indent=2))
        else:
            print(
                f"Applied NX769J battery charge limit from {args.config}: "
                f"start={rb_start}%, end={rb_end}% (hysteresis={rb_end - rb_start}%)"
            )
        return 0

    # Numeric preset or custom integer 55..100
    if target in PRESETS and args.start is None:
        start_soc, end_soc, desc = PRESETS[target]
    else:
        try:
            end_soc = int(target)
        except ValueError:
            raise SystemExit(
                f"error: unsupported target {args.target!r} "
                f"(expected 'status', 'apply', preset 80|85|90|95|100, or integer 55..100)"
            )
        start_soc = args.start if args.start is not None else max(MIN_START_SOC, end_soc - MIN_HYSTERESIS)
        desc = f"Custom charge limit ({start_soc}%..{end_soc}%)"

    try:
        validate_thresholds(start_soc, end_soc)
    except ValueError as exc:
        raise SystemExit(f"error: {exc}")

    wait_s = args.wait_sec if args.wait_sec is not None else 5.0
    try:
        rb_start, rb_end = program_thresholds(
            start_soc=start_soc,
            end_soc=end_soc,
            sysfs_root=args.sysfs_root,
            wait_sec=wait_s,
            dry_run=args.dry_run,
        )
    except PermissionError as exc:
        raise SystemExit(f"error: {exc}")

    persisted = False
    if not args.runtime_only and not args.dry_run:
        try:
            save_config(rb_start, rb_end, config_path=args.config)
            persisted = True
        except OSError as exc:
            if args.persist:
                raise SystemExit(f"error: failed to persist {args.config}: {exc}")

    out = {
        "applied": True,
        "description": desc,
        "start_threshold_pct": rb_start,
        "end_threshold_pct": rb_end,
        "persisted": persisted,
        "config_path": args.config if persisted else None,
        "dry_run": args.dry_run,
    }
    if args.json:
        print(json.dumps(out, indent=2))
    else:
        persist_note = f" [persisted to {args.config}]" if persisted else (" [dry-run]" if args.dry_run else " [runtime-only]")
        print(
            f"NX769J charge limit set: stop at {rb_end}%, resume <= {rb_start}% "
            f"(delta={rb_end - rb_start}%){persist_note}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
