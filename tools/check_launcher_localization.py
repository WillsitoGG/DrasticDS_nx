#!/usr/bin/env python3
"""Reject foreign-product strings and incomplete DraStic setting catalogs."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "launcher/source/main.cpp").read_text(encoding="utf-8")
CATALOG = (ROOT / "launcher/source/localization.cpp").read_text(encoding="utf-8")
forbidden = re.compile(r"\b(?:Cemu|Dolphin|RPCS3|Vita3K|NetherSX2?|GameCube|Wii)\b", re.I)
hits = forbidden.findall(CATALOG)
if hits: raise SystemExit(f"foreign emulator vocabulary in DraStic catalog: {hits}")
if "Changes this launcher or emulator option" in MAIN or 'std::string("Maps ")' in MAIN or 'std::string("Assigns ")' in MAIN:
    raise SystemExit("phrase-generated setting help is forbidden")
entries = set(re.findall(r'^\s*\{"((?:[^"\\]|\\.)*)"\s*,', CATALOG, re.M))
section = MAIN[MAIN.index("static const SettingHelpEntry SETTING_HELP"):MAIN.index("struct SettingHelpInfo")]
help_pairs = re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,', section)
help_keys = {key for key, _ in help_pairs}
missing_kinds = sorted({kind for _, kind in help_pairs if kind not in entries})
help_texts = set(re.findall(r'\{\s*"[^"]+"\s*,\s*"[^"]+"\s*,\s*\n?\s*"([^"]+)"\s*\}', section))
missing_descriptions = sorted(help_texts - entries)
option_pairs = re.findall(r'O_[A-Z_]+\(\s*"([^"]+)"\s*,\s*"([^"]+)"', MAIN)
missing_help = sorted({key for _, key in option_pairs if key not in help_keys})
missing_labels = sorted({label for label, _ in option_pairs if label not in entries})
technical_labels = {"Flow resolution", "Performance mode"}
missing_labels = sorted(set(missing_labels) - technical_labels)
if missing_help or missing_labels or missing_kinds or missing_descriptions:
    raise SystemExit(f"localization coverage failed: help={missing_help}, labels={missing_labels}, kinds={missing_kinds}, descriptions={len(missing_descriptions)}")
print(f"DraStic localization gate passed: {len(entries)} catalog entries")
