#!/usr/bin/env python3
# Copyright (c) 2026 Omarchy Mobile NX769J Contributors
# SPDX-License-Identifier: MIT
"""
Sanitization & privacy verification suite for nx769j-omarchy-linux.

Ensures no personal filesystem paths, device serial numbers, Wi-Fi SSIDs/PSKs,
SSH keys, or personal Bluetooth MAC addresses appear anywhere in tracked files.
"""

from __future__ import annotations

import pathlib
import sys
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Split literal patterns so the test file itself contains zero verbatim PII strings.
FORBIDDEN_PATTERNS: list[tuple[str, str]] = [
    ("personal-home-path", "/" + "Users/" + "xinbi" + "guo"),
    ("personal-user", "xinbi" + "guo"),
    ("phone-serial", "FY2419" + "710171"),
    ("sibling-serial", "HA250" + "68Q"),
    ("personal-wifi-ssid-file", "1201" + ".nmconnection"),
    ("personal-wifi-ssid-prop", "ssid=" + "1201"),
    ("personal-ssh-pubkey", "AAAA" + "C3NzaC1lZDI1NTE5AAAAIP4HUNospkDb"),
    ("personal-ssh-comment", "nx769j-" + "mac"),
    ("personal-bt-mac-dts", "51 " + "95 5b"),
    ("personal-handle-1", "xbg" + "xwh"),
    ("personal-handle-2", "8489" + "1989"),
]

SKIP_DIRS = {".git", "__pycache__", "build", "cache"}
SKIP_SUFFIXES = {".bin", ".png", ".jpg", ".pyc", ".img", ".dtb"}


class TestRepoSanitization(unittest.TestCase):
    def test_no_forbidden_secrets_or_personal_identifiers(self) -> None:
        violations: list[str] = []
        for path in sorted(REPO_ROOT.rglob("*")):
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            if not path.is_file() or path.suffix.lower() in SKIP_SUFFIXES:
                continue
            raw = path.read_bytes()
            if b"\x00" in raw[:4096]:
                continue
            text = raw.decode("utf-8", errors="replace")
            rel = path.relative_to(REPO_ROOT)
            for label, needle in FORBIDDEN_PATTERNS:
                if needle in text:
                    violations.append(f"{rel}: matched {label}")

        self.assertEqual(violations, [], "Sanitization violations found:\n" + "\n".join(violations))

    def test_no_untracked_build_or_key_artifacts(self) -> None:
        bad_files: list[str] = []
        for path in sorted(REPO_ROOT.rglob("*")):
            if any(part in SKIP_DIRS for part in path.parts):
                continue
            if path.is_file() and (
                path.name.endswith(".nmconnection")
                or "id_ed25519" in path.name
                or path.suffix in {".img", ".dtb", ".ko"}
            ):
                bad_files.append(str(path.relative_to(REPO_ROOT)))
        self.assertEqual(bad_files, [], f"Forbidden binary/secret files present: {bad_files}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
