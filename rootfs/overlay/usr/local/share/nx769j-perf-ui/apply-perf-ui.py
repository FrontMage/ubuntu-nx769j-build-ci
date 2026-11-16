#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Idempotent structural UI patcher for NX769J 3-Mode Performance in omamobile.quicksettings.

Invoked both during initial staging and automatically by ALPM PostTransaction hook
`/etc/pacman.d/hooks/99-nx769j-perf-ui.hook` whenever `omamobile-shell` upgrades.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

DEFAULT_QS_DIR = "/usr/share/omamobile/shell/plugins/omamobile.quicksettings"
DEFAULT_ASSET_DIR = "/usr/local/share/nx769j-perf-ui"


def _can_start_regex(prev_non_ws: str, prev_word: str) -> bool:
    if not prev_non_ws:
        return True
    if prev_non_ws in ("(", ",", "=", ":", "[", "!", "&", "|", "?", "{", "}", ";", "~", "+", "-", "*", "%", "<", ">", "^"):
        return True
    if prev_word in ("return", "case", "throw", "delete", "typeof", "void", "in", "of"):
        return True
    return False


def strip_comments_and_strings(src: str) -> str:
    out = []
    i = 0
    n = len(src)
    prev_non_ws = ""
    prev_word = ""
    while i < n:
        ch = src[i]
        if ch in ("'", '"', "`"):
            quote = ch
            out.append(" ")
            i += 1
            while i < n and src[i] != quote:
                i += 2 if src[i] == "\\" else 1
            out.append(" ")
            i += 1
            prev_non_ws = quote
            prev_word = ""
        elif src.startswith("//", i):
            end = src.find("\n", i)
            i = n if end < 0 else end
        elif src.startswith("/*", i):
            end = src.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif ch == "/" and _can_start_regex(prev_non_ws, prev_word):
            out.append(" ")
            i += 1
            in_class = False
            while i < n:
                rc = src[i]
                if rc == "\\":
                    i += 2
                    continue
                if rc == "[":
                    in_class = True
                elif rc == "]":
                    in_class = False
                elif rc == "/" and not in_class:
                    i += 1
                    while i < n and src[i].isalpha():
                        i += 1
                    break
                elif rc == "\n":
                    break
                i += 1
            prev_non_ws = "/"
            prev_word = ""
        else:
            out.append(ch)
            if not ch.isspace():
                if ch.isalnum() or ch == "_":
                    prev_word = (prev_word + ch) if (prev_non_ws.isalnum() or prev_non_ws == "_") else ch
                else:
                    prev_word = ""
                prev_non_ws = ch
            i += 1
    return "".join(out)


def verify_balanced_delimiters(src: str, label: str) -> None:
    cleaned = strip_comments_and_strings(src)
    pairs = {"}": "{", ")": "(", "]": "["}
    opens = set(pairs.values())
    stack = []
    for idx, ch in enumerate(cleaned):
        if ch in opens:
            stack.append((ch, idx))
        elif ch in pairs:
            if not stack or stack[-1][0] != pairs[ch]:
                raise ValueError(f"{label}: unbalanced delimiter {ch!r} near offset {idx}")
            stack.pop()
    if stack:
        ch, idx = stack[-1]
        raise ValueError(f"{label}: unclosed delimiter {ch!r} opened near offset {idx}")


def atomic_write(path: str, content: str, mode: int) -> bool:
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as fh:
            if fh.read() == content:
                os.chmod(path, mode)
                return False
    parent = os.path.dirname(path)
    fd, tmp_path = tempfile.mkstemp(prefix=".nx769j_perf_", dir=parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(content)
        os.chmod(tmp_path, mode)
        os.replace(tmp_path, path)
    except Exception:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
        raise
    return True


def patch_tiles_model(src: str) -> str:
    block = (
        '  /* BEGIN NX769J PERF MODE */\n'
        '  { id: "perf-mode", label: "性能模式" }\n'
        '  /* END NX769J PERF MODE */'
    )
    pattern = re.compile(
        r"[ \t]*/\* BEGIN NX769J PERF MODE \*/.*?/\* END NX769J PERF MODE \*/",
        re.S,
    )
    if pattern.search(src):
        patched = pattern.sub(block, src, count=1)
    else:
        anchor = '  { id: "dictation", label: "Dictation" }\n]'
        if anchor not in src:
            raise RuntimeError("TilesModel.js anchor not found")
        patched = src.replace(
            anchor,
            '  { id: "dictation", label: "Dictation" },\n' + block + "\n]",
            1,
        )
    verify_balanced_delimiters(patched, "TilesModel.js")
    return patched


def patch_tile_state_sh(src: str) -> str:
    block = (
        "# BEGIN NX769J PERF MODE\n"
        "if [ -r /var/lib/nx769j-perf/mode ]; then\n"
        "  case $(head -n1 /var/lib/nx769j-perf/mode 2>/dev/null) in\n"
        "    powersave) profile=power-saver ;;\n"
        "    performance) profile=performance ;;\n"
        "    *) profile=balanced ;;\n"
        "  esac\n"
        "else\n"
        "  profile=$(have powerprofilesctl && t powerprofilesctl get)\n"
        "fi\n"
        'echo "powerprofile=${profile:-none}"\n'
        "# END NX769J PERF MODE"
    )
    pattern = re.compile(
        r"# BEGIN NX769J PERF MODE\n.*?# END NX769J PERF MODE",
        re.S,
    )
    if pattern.search(src):
        return pattern.sub(block, src, count=1)

    anchor = (
        "# powerprofile\n"
        "profile=$(have powerprofilesctl && t powerprofilesctl get)\n"
        'echo "powerprofile=${profile:-none}"'
    )
    if anchor not in src:
        raise RuntimeError("tile-state.sh powerprofile anchor not found")
    return src.replace(anchor, "# powerprofile\n" + block, 1)


def patch_tile_action_sh(src: str) -> str:
    block = (
        "    # BEGIN NX769J PERF MODE\n"
        "    if [ -x /usr/local/bin/nx769j-perf ]; then\n"
        '      if [ "$want" = on ]; then\n'
        "        exec /usr/local/bin/nx769j-perf powersave\n"
        "      else\n"
        "        exec /usr/local/bin/nx769j-perf balanced\n"
        "      fi\n"
        "    fi\n"
        '    if [ "$want" = on ]; then\n'
        "      exec omarchy-powerprofiles-set autodetect power-saver\n"
        "    else\n"
        "      exec omarchy-powerprofiles-set autodetect balanced\n"
        "    fi\n"
        "    # END NX769J PERF MODE"
    )
    pattern = re.compile(
        r"[ \t]*# BEGIN NX769J PERF MODE\n.*?# END NX769J PERF MODE",
        re.S,
    )
    if pattern.search(src):
        return pattern.sub(block, src, count=1)

    anchor = (
        "  battery-saver)\n"
        "    # Through Omarchy's setter, which remembers the choice for AC or battery.\n"
        '    if [ "$want" = on ]; then\n'
        "      exec omarchy-powerprofiles-set autodetect power-saver\n"
        "    else\n"
        "      exec omarchy-powerprofiles-set autodetect balanced\n"
        "    fi\n"
        "    ;;"
    )
    if anchor not in src:
        raise RuntimeError("tile-action.sh battery-saver anchor not found")
    return src.replace(anchor, "  battery-saver)\n" + block + "\n    ;;", 1)


PERF_REFRESH_BLOCK = (
    "/* BEGIN NX769J PERF REFRESH */ "
    "case 3: refreshProbe(); perfMode.refresh(); break "
    "/* END NX769J PERF REFRESH */"
)

PERF_STATE_BLOCK = (
    "/* BEGIN NX769J PERF STATE */\n"
    "      perfMode: perfMode.mode,\n"
    "      perfLabel: perfMode.labelZh,\n"
    "      cpuTempC: perfMode.cpuTempC,\n"
    "      gpuTempC: perfMode.gpuTempC,\n"
    "      fanRpm: perfMode.fanRpm,\n"
    "      fanDutyPercent: perfMode.fanDutyPercent,\n"
    "      /* END NX769J PERF STATE */"
)

PERF_BACKEND_BLOCK = """/* BEGIN NX769J PERF BACKEND */
  function setPerfMode(name) {
    const req = String(name || "").trim().toLowerCase()
    if (req !== "powersave" && req !== "balanced" && req !== "performance") return "invalid"
    perfMode.select(req)
    return state()
  }

  function cyclePerfMode() {
    perfMode.toggle()
    return state()
  }

  QtObject {
    id: perfMode

    property string mode: "balanced"
    property string labelZh: "均衡"
    property real cpuTempC: -1
    property real gpuTempC: -1
    property int fanRpm: 0
    property int fanDutyPercent: 0
    property bool busy: false
    readonly property bool available: true
    readonly property bool enabled: mode !== "balanced"
    readonly property string icon: mode === "powersave" ? "󰌪" : mode === "performance" ? "󰓅" : "󰾅"
    readonly property string status: {
      const tag = mode === "powersave" ? "省电" : mode === "performance" ? "狂暴" : "均衡"
      const tempPart = (isFinite(cpuTempC) && cpuTempC > 0) ? (" · " + Math.round(cpuTempC) + "°C") : ""
      const rpmPart = fanRpm > 0 ? (" · " + fanRpm + " RPM") : ""
      return tag + tempPart + rpmPart
    }

    function applyJson(rawText) {
      try {
        const obj = JSON.parse(String(rawText || "").trim())
        if (!obj || typeof obj !== "object") return
        if (obj.mode === "powersave" || obj.mode === "balanced" || obj.mode === "performance")
          mode = obj.mode
        labelZh = mode === "powersave" ? "省电" : mode === "performance" ? "狂暴" : "均衡"
        const hot = Number(obj.hotspot_c !== undefined ? obj.hotspot_c : obj.peak_cpu_c)
        if (isFinite(hot) && hot > 0) cpuTempC = hot
        const gpu = Number(obj.peak_gpu_c)
        if (isFinite(gpu) && gpu > 0) gpuTempC = gpu
        const rpm = parseInt(obj.fan_rpm, 10)
        fanRpm = (isFinite(rpm) && rpm > 0) ? rpm : 0
        const duty = parseInt(obj.fan_duty_percent, 10)
        fanDutyPercent = (isFinite(duty) && duty >= 0) ? duty : 0
      } catch (e) {}
    }

    function refresh() {
      if (!perfReadProc.running && !perfWriteProc.running)
        perfReadProc.running = true
    }

    function select(nextMode) {
      if (nextMode !== "powersave" && nextMode !== "balanced" && nextMode !== "performance")
        return
      mode = nextMode
      labelZh = nextMode === "powersave" ? "省电" : nextMode === "performance" ? "狂暴" : "均衡"
      busy = true
      perfWriteProc.pendingMode = nextMode
      if (!perfWriteProc.running)
        perfWriteProc.flush()
    }

    function toggle() {
      const next = mode === "powersave" ? "balanced"
        : mode === "balanced" ? "performance"
        : "powersave"
      select(next)
    }
  }

  Process {
    id: perfReadProc
    command: ["/usr/local/bin/nx769j-perf", "status", "--json"]
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: perfMode.applyJson(text)
    }
  }

  Process {
    id: perfWriteProc
    property string pendingMode: ""
    function flush() {
      if (pendingMode === "") {
        perfMode.busy = false
        return
      }
      const target = pendingMode
      pendingMode = ""
      command = ["/usr/local/bin/nx769j-perf", target, "--json"]
      running = true
    }
    stdout: StdioCollector {
      waitForEnd: true
      onStreamFinished: perfMode.applyJson(text)
    }
    onExited: {
      if (pendingMode !== "") {
        flush()
      } else {
        perfMode.busy = false
        root.refreshProbe()
        perfSpinupTimer.restart()
      }
    }
  }

  Timer {
    id: perfSpinupTimer
    interval: 1400
    repeat: false
    onTriggered: perfMode.refresh()
  }

  Timer {
    id: perfPollTimer
    interval: 2500
    repeat: true
    running: root.opened && !root.editing
    onTriggered: perfMode.refresh()
  }
  /* END NX769J PERF BACKEND */"""

PERF_ROW_BLOCK = """/* BEGIN NX769J PERF ROW */
          PerfModeRow {
            visible: !root.editing
            width: parent.width
            mode: perfMode.mode
            labelZh: perfMode.labelZh
            cpuTempC: perfMode.cpuTempC
            gpuTempC: perfMode.gpuTempC
            fanRpm: perfMode.fanRpm
            fanDutyPercent: perfMode.fanDutyPercent
            busy: perfMode.busy
            foreground: root.foreground
            onModeSelected: function(modeId) { perfMode.select(modeId) }
            onCycleRequested: perfMode.toggle()
          }

          PanelSeparator { foreground: root.foreground; visible: !root.editing }
          /* END NX769J PERF ROW */"""


def patch_quicksettings_qml(src: str) -> str:
    out = src

    # 1. Open refresh hook
    pat_ref = re.compile(r"/\* BEGIN NX769J PERF REFRESH \*/.*?/\* END NX769J PERF REFRESH \*/", re.S)
    if pat_ref.search(out):
        out = pat_ref.sub(PERF_REFRESH_BLOCK, out, count=1)
    else:
        anchor = "case 3: refreshProbe(); break"
        if anchor not in out:
            raise RuntimeError("QuickSettings.qml refreshProbe anchor not found")
        out = out.replace(anchor, PERF_REFRESH_BLOCK, 1)

    # 2. state() telemetry fields
    pat_state = re.compile(r"/\* BEGIN NX769J PERF STATE \*/.*?/\* END NX769J PERF STATE \*/", re.S)
    if pat_state.search(out):
        out = pat_state.sub(PERF_STATE_BLOCK, out, count=1)
    else:
        anchor = "      notifications: notifications.length,"
        if anchor not in out:
            raise RuntimeError("QuickSettings.qml state() notifications anchor not found")
        out = out.replace(anchor, "      " + PERF_STATE_BLOCK + "\n" + anchor, 1)

    # 3. backends map entry + perfMode backend block
    pat_bmap = re.compile(r'/\* BEGIN NX769J PERF BMAP \*/.*?/\* END NX769J PERF BMAP \*/', re.S)
    bmap_entry = '/* BEGIN NX769J PERF BMAP */ "perf-mode": perfMode /* END NX769J PERF BMAP */'
    if pat_bmap.search(out):
        out = pat_bmap.sub(bmap_entry, out, count=1)
    else:
        anchor = '"clipboard": clipboard, "dictation": dictation\n  })'
        if anchor not in out:
            raise RuntimeError("QuickSettings.qml backends anchor not found")
        out = out.replace(
            anchor,
            '"clipboard": clipboard, "dictation": dictation,\n    ' + bmap_entry + "\n  })",
            1,
        )

    pat_backend = re.compile(r"/\* BEGIN NX769J PERF BACKEND \*/.*?/\* END NX769J PERF BACKEND \*/", re.S)
    if pat_backend.search(out):
        out = pat_backend.sub(PERF_BACKEND_BLOCK, out, count=1)
    else:
        anchor = bmap_entry + "\n  })"
        if anchor not in out:
            raise RuntimeError("QuickSettings.qml post-backends anchor not found")
        out = out.replace(anchor, anchor + "\n\n  " + PERF_BACKEND_BLOCK, 1)

    # 4. Component.onCompleted initial read
    pat_init = re.compile(r"/\* BEGIN NX769J PERF INIT \*/.*?/\* END NX769J PERF INIT \*/", re.S)
    init_block = "/* BEGIN NX769J PERF INIT */ Component.onCompleted: { refreshProbe(); perfMode.refresh() } /* END NX769J PERF INIT */"
    if pat_init.search(out):
        out = pat_init.sub(init_block, out, count=1)
    else:
        anchor = "Component.onCompleted: refreshProbe()"
        if anchor in out:
            out = out.replace(anchor, init_block, 1)

    # 5. PerfModeRow UI widget above sliders
    pat_row = re.compile(r"/\* BEGIN NX769J PERF ROW \*/.*?/\* END NX769J PERF ROW \*/", re.S)
    if pat_row.search(out):
        out = pat_row.sub(PERF_ROW_BLOCK, out, count=1)
    else:
        anchor = "          // ---------- Sliders: brightness always, volume when expanded"
        if anchor not in out:
            raise RuntimeError("QuickSettings.qml sliders anchor not found")
        out = out.replace(anchor, "          " + PERF_ROW_BLOCK + "\n\n" + anchor, 1)

    verify_balanced_delimiters(out, "QuickSettings.qml")
    return out


def apply_to_dir(qs_dir: str, asset_dir: str, check_only: bool = False, strict: bool = False) -> int:
    if not os.path.isdir(qs_dir):
        sys.stderr.write(f"[nx769j-perf-ui] target dir {qs_dir} not present, skipping.\n")
        return 1 if strict else 0

    perf_row_src = os.path.join(asset_dir, "PerfModeRow.qml")
    if not os.path.isfile(perf_row_src):
        sys.stderr.write(f"[nx769j-perf-ui] missing {perf_row_src}\n")
        return 1

    with open(perf_row_src, "r", encoding="utf-8") as fh:
        perf_row_qml = fh.read()
    verify_balanced_delimiters(perf_row_qml, "PerfModeRow.qml")

    targets = [
        ("TilesModel.js", patch_tiles_model, 0o644),
        ("tile-state.sh", patch_tile_state_sh, 0o755),
        ("tile-action.sh", patch_tile_action_sh, 0o755),
        ("QuickSettings.qml", patch_quicksettings_qml, 0o644),
    ]

    staged = {}
    for rel, fn, mode in targets:
        p = os.path.join(qs_dir, rel)
        if not os.path.isfile(p):
            sys.stderr.write(f"[nx769j-perf-ui] warning: {p} missing, leaving plugin untouched.\n")
            return 1 if (check_only or strict) else 0
        with open(p, "r", encoding="utf-8") as fh:
            orig = fh.read()
        try:
            patched = fn(orig)
        except Exception as exc:
            sys.stderr.write(f"[nx769j-perf-ui] warning: could not patch {rel} ({exc}); leaving upstream intact.\n")
            return 1 if (check_only or strict) else 0
        staged[rel] = (p, patched, mode)

    # Validate shell syntax of patched .sh files before committing any writes
    sh_bin = shutil.which("sh") or "/bin/sh"
    for rel in ("tile-state.sh", "tile-action.sh"):
        _, content, _ = staged[rel]
        res = subprocess.run([sh_bin, "-n"], input=content, text=True, capture_output=True)
        if res.returncode != 0:
            sys.stderr.write(f"[nx769j-perf-ui] sh -n failed on {rel}: {res.stderr}\n")
            return 1

    if check_only:
        return 0

    changed = False
    if atomic_write(os.path.join(qs_dir, "PerfModeRow.qml"), perf_row_qml, 0o644):
        changed = True
    for rel, (p, content, mode) in staged.items():
        if atomic_write(p, content, mode):
            changed = True

    if changed:
        print(f"[nx769j-perf-ui] applied 3-mode performance UI to {qs_dir}")
    else:
        print(f"[nx769j-perf-ui] {qs_dir} already up-to-date")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Apply NX769J 3-Mode Perf UI to omamobile.quicksettings")
    ap.add_argument("--target-dir", default=DEFAULT_QS_DIR, help="Path to omamobile.quicksettings")
    ap.add_argument("--asset-dir", default=DEFAULT_ASSET_DIR, help="Path containing PerfModeRow.qml")
    ap.add_argument("--check", action="store_true", help="Dry-run syntax & anchor verification only")
    ap.add_argument("--strict", action="store_true", help="Return non-zero if any target file cannot be patched")
    args = ap.parse_args()
    return apply_to_dir(args.target_dir, args.asset_dir, check_only=args.check, strict=args.strict)


if __name__ == "__main__":
    sys.exit(main())
