#!/usr/bin/env python3
"""Reject foreign-product strings and incomplete DraStic setting catalogs."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "launcher/source/main.cpp").read_text(encoding="utf-8")
CATALOG = (ROOT / "launcher/source/localization.cpp").read_text(encoding="utf-8")
CHINESE = (ROOT / "launcher/source/localization_zh.h").read_text(encoding="utf-8")
forbidden = re.compile(r"\b(?:Cemu|Dolphin|RPCS3|Vita3K|NetherSX2?|GameCube|Wii)\b", re.I)
hits = forbidden.findall(CATALOG)
if hits: raise SystemExit(f"foreign emulator vocabulary in DraStic catalog: {hits}")
if "Changes this launcher or emulator option" in MAIN or 'std::string("Maps ")' in MAIN or 'std::string("Assigns ")' in MAIN:
    raise SystemExit("phrase-generated setting help is forbidden")
catalog_entries = CATALOG[CATALOG.index("constexpr Entry ENTRIES[]"):
                          CATALOG.index("std::string systemLanguage")]
entries = set(re.findall(r'^\s*\{"((?:[^"\\]|\\.)*)"\s*,', catalog_entries, re.M))
chinese_rows = re.findall(
    r'^\s*\{"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\},',
    CHINESE,
    re.M,
)
chinese_keys = {key for key, _, _ in chinese_rows}
missing_chinese = sorted(entries - chinese_keys)
extra_chinese = sorted(chinese_keys - entries)
empty_chinese = [key for key, simplified, traditional in chinese_rows
                 if not simplified.strip() or not traditional.strip()]
if missing_chinese or extra_chinese or empty_chinese or len(chinese_rows) != len(chinese_keys):
    raise SystemExit(
        f"Chinese localization coverage failed: missing={missing_chinese}, "
        f"extra={extra_chinese}, empty={empty_chinese}, rows={len(chinese_rows)}"
    )
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
print(f"DraStic localization gate passed: {len(entries)} catalog entries, both Chinese variants complete")
