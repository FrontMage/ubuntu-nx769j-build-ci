#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Idempotent QuickSettings touch Volume Slider & OSD enabler for NX769J.

Invoked both during staging (`stage_audio_vol.py`) and automatically by ALPM
PostTransaction hook `/etc/pacman.d/hooks/99-nx769j-zz-audio-ui.hook` whenever
`omamobile-shell` is installed or upgraded.

Guarantees:
  - Makes the QuickSettings Volume `SliderRow` always visible (`visible: true`
    instead of `visible: root.expanded`) immediately on first pull-down, right
    below Brightness and `PerfModeRow`.
  - Adds `setVolumePercent(pct)` and `toggleAudioMute()` shell RPC helpers.
  - 100% commutative and conflict-free with `99-nx769j-perf-ui.hook`
    (`apply-perf-ui.py` + `PerfModeRow.qml` + CJK labels) and `99-nx769j-omamobile-ui.hook`.
  - Ensures `omarchy.osd` is never listed in `disabledPlugins` in `shell.json`.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import tempfile
from typing import List, Set, Tuple

REL_QS_DIRS = (
    "usr/share/omamobile/shell/plugins/omamobile.quicksettings",
    "home/developer/.config/omarchy/plugins/omamobile.quicksettings",
)
REL_SHELL_JSON = "home/developer/.config/omarchy/shell.json"

AUDIO_IPC_BLOCK = """/* BEGIN NX769J AUDIO IPC */
  function setVolumePercent(pct) {
    const n = Number(pct)
    if (!isFinite(n)) return "invalid"
    audio.setVolume(Math.max(0, Math.min(100, n)) / 100.0)
    return state()
  }

  function toggleAudioMute() {
    audio.toggleMute()
    return state()
  }
  /* END NX769J AUDIO IPC */"""

UPSTREAM_VOLUME_SLIDER = """            SliderRow {
              visible: root.expanded
              width: parent.width
              icon: audio.icon
              value: audio.volume
              available: audio.available
              unavailableText: "No audio output"
              onMoved: function(value) { audio.setVolume(value) }
              onIconClicked: audio.toggleMute()
            }"""

PATCHED_VOLUME_SLIDER = """/* BEGIN NX769J AUDIO SLIDER */
            SliderRow {
              visible: true
              width: parent.width
              icon: audio.icon
              value: audio.volume
              available: audio.available
              unavailableText: "No audio output"
              onMoved: function(value) { audio.setVolume(value) }
              onIconClicked: audio.toggleMute()
            }
            /* END NX769J AUDIO SLIDER */"""


def _can_start_regex(prev_non_ws: str, prev_word: str) -> bool:
    if not prev_non_ws:
        return True
    if prev_non_ws in ("(", ",", "=", ":", "[", "!", "&", "|", "?", "{", "}", ";", "~", "+", "-", "*", "%", "<", ">", "^"):
        return True
    if prev_word in ("return", "case", "throw", "delete", "typeof", "void", "in", "of"):
        return True
    return False


def strip_comments_and_strings(src: str) -> str:
    out: List[str] = []
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
    stack: List[Tuple[str, int]] = []
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


def atomic_write(path: str, content: str, mode: int = 0o644) -> bool:
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as fh:
            if fh.read() == content:
                os.chmod(path, mode)
                return False
    parent = os.path.dirname(path)
    fd, tmp_path = tempfile.mkstemp(prefix=".nx769j_audio_", dir=parent)
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


def patch_quicksettings_qml(src: str) -> str:
    out = src

    # 1. Shell RPC helpers for volume testing & scripting
    pat_ipc = re.compile(r"/\* BEGIN NX769J AUDIO IPC \*/.*?/\* END NX769J AUDIO IPC \*/", re.S)
    if pat_ipc.search(out):
        out = pat_ipc.sub(AUDIO_IPC_BLOCK, out, count=1)
    else:
        anchor = "  PwObjectTracker { objects: audio.sink ? [audio.sink] : [] }"
        if anchor not in out:
            raise RuntimeError("QuickSettings.qml PwObjectTracker anchor not found")
        out = out.replace(anchor, anchor + "\n\n  " + AUDIO_IPC_BLOCK, 1)

    # 2. Always-visible touch Volume SliderRow (preserves `// ---------- Sliders:` anchor for perf-ui!)
    pat_slider = re.compile(
        r"[ \t]*/\* BEGIN NX769J AUDIO SLIDER \*/.*?/\* END NX769J AUDIO SLIDER \*/",
        re.S,
    )
    if pat_slider.search(out):
        out = pat_slider.sub("            " + PATCHED_VOLUME_SLIDER, out, count=1)
    elif UPSTREAM_VOLUME_SLIDER in out:
        out = out.replace(UPSTREAM_VOLUME_SLIDER, "            " + PATCHED_VOLUME_SLIDER, 1)
    else:
        raise RuntimeError("QuickSettings.qml Volume SliderRow anchor not found")

    verify_balanced_delimiters(out, "QuickSettings.qml")
    return out


def ensure_osd_enabled_in_shell_json(shell_json_path: str, check_only: bool = False) -> bool:
    if not os.path.isfile(shell_json_path):
        return False
    try:
        with open(shell_json_path, "r", encoding="utf-8") as fh:
            raw = fh.read()
        cfg = json.loads(raw)
    except Exception:
        return False
    if not isinstance(cfg, dict):
        return False
    disabled = cfg.get("disabledPlugins")
    if isinstance(disabled, list) and "omarchy.osd" in disabled:
        cfg["disabledPlugins"] = [x for x in disabled if x != "omarchy.osd"]
        if not check_only:
            atomic_write(shell_json_path, json.dumps(cfg, indent=2) + "\n", 0o644)
        return True
    return False


def apply_to_dir(qs_dir: str, check_only: bool = False, strict: bool = False) -> int:
    if not os.path.isdir(qs_dir):
        return 1 if strict else 0
    qml_path = os.path.join(qs_dir, "QuickSettings.qml")
    if not os.path.isfile(qml_path):
        sys.stderr.write(f"[nx769j-audio-ui] warning: {qml_path} missing, skipping.\n")
        return 1 if (check_only or strict) else 0

    with open(qml_path, "r", encoding="utf-8") as fh:
        orig = fh.read()
    try:
        patched = patch_quicksettings_qml(orig)
    except Exception as exc:
        sys.stderr.write(f"[nx769j-audio-ui] warning: failed to patch {qml_path}: {exc}\n")
        return 1 if (check_only or strict) else 0

    if check_only:
        return 0

    if atomic_write(qml_path, patched, 0o644):
        print(f"[nx769j-audio-ui] enabled always-visible touch Volume slider in {qml_path}")
    else:
        print(f"[nx769j-audio-ui] {qml_path} already up-to-date")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Apply NX769J QuickSettings touch Volume slider & OSD config")
    ap.add_argument("--root", default="/", help="Target rootfs directory (default: /)")
    ap.add_argument("--target-dir", default="", help="Explicit path to a single omamobile.quicksettings dir")
    ap.add_argument("--check", action="store_true", help="Dry-run syntax & anchor verification only")
    ap.add_argument("--strict", action="store_true", help="Fail if target QuickSettings.qml cannot be patched")
    args = ap.parse_args()

    if args.target_dir:
        return apply_to_dir(args.target_dir, check_only=args.check, strict=args.strict)

    root = os.path.abspath(args.root)
    seen_inodes: Set[Tuple[int, int]] = set()
    patched_any = False

    for rel in REL_QS_DIRS:
        cand = os.path.join(root, rel)
        qml = os.path.join(cand, "QuickSettings.qml")
        if not os.path.isfile(qml):
            continue
        st = os.stat(qml)
        inode_key = (st.st_dev, st.st_ino)
        if inode_key in seen_inodes:
            continue
        seen_inodes.add(inode_key)
        rc = apply_to_dir(cand, check_only=args.check, strict=args.strict)
        if rc != 0:
            return rc
        patched_any = True

    if args.strict and not patched_any:
        sys.stderr.write(f"[nx769j-audio-ui] no QuickSettings.qml found under {root}\n")
        return 1

    ensure_osd_enabled_in_shell_json(os.path.join(root, REL_SHELL_JSON), check_only=args.check)
    return 0


if __name__ == "__main__":
    sys.exit(main())
