#!/usr/bin/env python3
"""Correct known-bad cartridge metadata in DraStic 2.6.0.4a build 109.

The APK database incorrectly marks five regional Pokemon Black/White 2 ROMs
as 512-byte EEPROM cartridges.  The games use a 512 KiB Flash save.  Keep the
vendor database as a build input and patch only those records in the temporary
RomFS copy, so no proprietary database is added to this repository.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


# IDs are the little-endian hexadecimal form used by DraStic's XML database.
# They decode to IRDD, IRDI, IREI, IRDS, and IRES respectively.
POKEMON_BW2_GAME_IDS = {
    "44445249",  # Pokemon White 2 (German)
    "49445249",  # Pokemon White 2 (Italian)
    "49455249",  # Pokemon Black 2 (Italian)
    "53445249",  # Pokemon White 2 (Spanish)
    "53455249",  # Pokemon Black 2 (Spanish)
}

CARTRIDGE_RE = re.compile(r"<cartridge\b.*?</cartridge>", re.DOTALL)
ROM_ID_RE = re.compile(r"\bid\s*=\s*(['\"])([0-9a-fA-F]{8})\1")
SAVE_RE = re.compile(r"(?P<indent>^[ \t]*)<save\b[^>]*/>", re.MULTILINE)
CORRECT_SAVE = (
    "<save name='save' size='0x80000' type='Flash' id='0x204013' />"
)

def patch_database(source: str) -> tuple[str, int, dict[str, int]]:
    seen = {game_id: 0 for game_id in POKEMON_BW2_GAME_IDS}
    changed = 0

    def patch_cartridge(match: re.Match[str]) -> str:
        nonlocal changed
        block = match.group(0)
        ids = {item.group(2).upper() for item in ROM_ID_RE.finditer(block)}
        targets = ids & POKEMON_BW2_GAME_IDS
        if not targets:
            return block

        for game_id in targets:
            seen[game_id] += 1

        saves = list(SAVE_RE.finditer(block))
        if len(saves) != 1:
            joined = ", ".join(sorted(targets))
            raise ValueError(
                f"expected one save element for Pokemon game ID(s) {joined}, "
                f"found {len(saves)}"
            )

        save = saves[0]
        replacement = save.group("indent") + CORRECT_SAVE
        if save.group(0) == replacement:
            return block
        changed += 1
        return block[: save.start()] + replacement + block[save.end() :]

    output = CARTRIDGE_RE.sub(patch_cartridge, source)
    return output, changed, seen


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source = args.input.read_text(encoding="utf-8")
    output, changed, seen = patch_database(source)
    missing = sorted(game_id for game_id, count in seen.items() if count == 0)
    if missing:
        raise SystemExit(
            "DraStic database is missing expected build-109 Pokemon records: "
            + ", ".join(missing)
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8", newline="")
    print(
        "Pokemon Black/White 2 save metadata: "
        f"validated {sum(seen.values())} records, corrected {changed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
