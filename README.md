# DrasticDS_nx — WillsitoGG Direct Forwarder Tuning

This fork keeps NaGaa95's DrasticDS_nx upstream history intact while maintaining a separately validated Nintendo Switch direct-forwarder tuning.

## Branch model

- `main`: upstream-clean branch. Keep it synchronized with `NaGaa95/DrasticDS_nx:main`; do not add WillsitoGG-specific files here.
- `willsito-tuning`: permanent branch for the validated WillsitoGG build, documentation, validation records and archive.
- `fix/direct-forwarder`: minimal upstream contribution branch used by Pull Request #33. Do not add tuning-only documentation or release infrastructure to it.

## Current tuned release

**DrasticDS_nx 1.0.9 – Direct Forwarder Fix**

| Item | Value |
| --- | --- |
| Upstream repository | `NaGaa95/DrasticDS_nx` |
| Upstream version | `1.0.9` |
| Upstream source commit | `b587105edd831bb149c0aef681363cbb3c12e9b3` |
| Tuned source commit | `c983d2f6291bbf9dac03b14dc8e7720ba516d05c` |
| Tuned `main.cpp` Git blob | `eda8278a5bdb98d847a16070b1ed6bdb5ec474bb` |
| Release tag | `drasticds-nx-1.0.9-direct-forwarder-fix` |
| SD-ready asset | `DrasticDS.nro` |
| Tuned NRO SHA-256 | `91ebec3eab02001609d12fe267fdb190fdffa1ad690ccb48ba63a03e3d321542` |
| Official 1.0.9 NRO SHA-256 | `d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679` |

## What this tuning changes

Standard NSP forwarders can pass the selected Nintendo DS ROM as a positional argument:

```text
DrasticDS.nro "sdmc:/roms/nds/Game.nds"
```

The tuned launcher detects that path through `argv[1]` before the normal library/storage UI pipeline and builds the launch identity directly from the requested ROM.

For positional direct forwarders it:

- launches the requested ROM without first showing the game grid;
- avoids the visible USB-connection wait screen;
- skips the normal library scan when it is not needed;
- avoids unrelated SMB auto-mount work;
- still initializes USB when the forwarded ROM itself is stored on USB;
- preserves stable-key/path-key per-game configuration handling.

The normal launcher remains unchanged when DrasticDS_nx is opened normally. NaGaa95's existing `-g <gameKey>` mechanism is also intentionally preserved.

## Installation

1. Download the tuned `DrasticDS.nro` from this fork's current Release.
2. Copy it to `/switch/DrasticDS.nro` on the SD card.
3. Keep using your own legally obtained Nintendo DS BIOS, firmware, core/resources and game backups as required by upstream DrasticDS_nx.
4. Standard NSP forwarders may pass the ROM path directly in `argv[1]`.

For the complete upstream installation, controls, renderer and feature documentation, see the official repository: `NaGaa95/DrasticDS_nx`.

## Validation and provenance

The current release was built from the exact upstream source commit shown above and preserves the official DrasticDS_nx 1.0.9 RomFS/core/resources. The tuned source is the same source manually tested by WillsitoGG on real Nintendo Switch hardware.

Validation records are stored under `Validation/`:

- final and official NRO SHA-256 values;
- exact upstream/release provenance;
- source identity information;
- NRO structural metadata.

The exact source delta is stored under `Patches/` and can be applied to the pinned upstream commit.

## Rebuilding

Use `scripts/build-direct-forwarder-fix.sh` from the `willsito-tuning` branch. The script:

1. fetches the exact upstream commit;
2. applies the exact direct-forwarder patch;
3. verifies the resulting `launcher/source/main.cpp` Git blob;
4. downloads and verifies the official DrasticDS_nx 1.0.9 NRO;
5. reuses its RomFS/core/resources;
6. builds the launcher and validates the resulting NRO and SHA-256.

The reference environment for the validated release is `devkitpro/devkita64:20260219`.

## Historical archive

Superseded final revisions are stored under `Archive/` only after recovering the exact published binary and verifying its SHA-256. Current release binaries are not duplicated in the repository.

The earlier `drasticds-nx-1.0.9-direct-forwarder` revision is recoverable from the historical `NSW_Tunning` tag and is retained in the archive with its exact source, binary, hash and original tag metadata.

## Upstream contribution

The source-only fix has been submitted upstream as:

- `NaGaa95/DrasticDS_nx` Pull Request **#33 — Add direct ROM launch support for NSP forwarders**.

That PR intentionally contains only the `launcher/source/main.cpp` change. The files in `Archive/`, `Validation/`, `Patches/`, `scripts/`, `AGENTS.md` and this README belong only to the tuning branch.

## AI disclosure

The direct-forwarder work was developed and reviewed together with ChatGPT. WillsitoGG personally reviewed the final result and performed the functional validation on a real Nintendo Switch using NSP forwarders.

## Legal / upstream

This fork does not redistribute Nintendo DS BIOS or firmware and does not change the upstream project's legal requirements. Drastic is proprietary software; use only legally obtained required files and game backups. Upstream licensing and third-party notices remain authoritative for the original project.