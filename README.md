# DrasticDS_nx — WillsitoGG Direct Forwarder Tuning

This fork keeps NaGaa95's DrasticDS_nx upstream history intact while maintaining a separate Nintendo Switch direct-forwarder tuning.

## Branch model

- `main`: upstream-clean and synchronizable with `NaGaa95/DrasticDS_nx:main`.
- `willsito-tuning`: permanent WillsitoGG tuning branch with source, build scripts, validation and history.
- `fix/direct-forwarder`: minimal source-only upstream contribution branch used by Pull Request #33.

## Current tuned release

**DrasticDS_nx 1.0.9 – Direct Forwarder Fix**

| Item | Value |
| --- | --- |
| Upstream repository | `NaGaa95/DrasticDS_nx` |
| Upstream version | `1.0.9` |
| Upstream source commit | `b587105edd831bb149c0aef681363cbb3c12e9b3` |
| Tuned source commit used for upstream PR | `c983d2f6291bbf9dac03b14dc8e7720ba516d05c` |
| Tuned `launcher/source/main.cpp` Git blob | `eda8278a5bdb98d847a16070b1ed6bdb5ec474bb` |
| Release tag | `drasticds-nx-1.0.9-direct-forwarder-fix` |
| SD-ready asset | `DrasticDS.nro` |
| Current rebuilt NRO SHA-256 | See `RELEASE_STATUS.md` / `SHA256SUMS.txt` |
| Hardware-tested historical NRO SHA-256 | `91ebec3eab02001609d12fe267fdb190fdffa1ad690ccb48ba63a03e3d321542` |
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

The normal launcher remains unchanged when DrasticDS_nx is opened normally. NaGaa95's existing `-g <gameKey>` mechanism is intentionally preserved.

## Installation

1. Download `DrasticDS.nro` from this fork's current Release.
2. Copy it to `/switch/DrasticDS.nro` on the SD card.
3. Keep the upstream-required legally obtained BIOS, firmware, resources and game backups.
4. Standard NSP forwarders may pass the ROM path directly through `argv[1]`.

## Validation and provenance

The **source identity is the primary invariant** for rebuilds of this tuning. The build pipeline pins the upstream commit, applies the exact Direct Forwarder source delta and requires the resulting `main.cpp` blob to be:

```text
eda8278a5bdb98d847a16070b1ed6bdb5ec474bb
```

The official DrasticDS_nx 1.0.9 NRO is also verified before its RomFS/core/resources are reused.

A fresh rebuild may have a different SHA-256 from an earlier build even when it is produced from the same validated source. Therefore:

- `91ebec3e...` is retained as the **historical NRO that WillsitoGG actually tested on a real Nintendo Switch**;
- the current Release records its own build-specific SHA-256 in `RELEASE_STATUS.md`, `SHA256SUMS.txt` and `Validation/`;
- hardware validation of the historical NRO is not silently transferred to a newly rebuilt binary.

## Rebuilding

Use:

```text
scripts/build-direct-forwarder-fix.sh
```

The script:

1. fetches upstream commit `b587105...`;
2. applies the exact Direct Forwarder patch;
3. verifies the tuned source blob;
4. downloads and verifies the official 1.0.9 NRO;
5. reuses its official RomFS/core/resources;
6. builds the launcher;
7. validates `NRO0`, DisplayVersion `1.0.9`, structural metadata and SHA-256;
8. records whether the result happens to be byte-identical to the historical hardware-tested build, without requiring that identity.

Reference build environment: `devkitpro/devkita64:20260219`.

## Historical archive

Superseded final revisions live under `Archive/`.

For historical builds where the exact old binary is not moved into this fork, an archived NRO may be a **fresh rebuild of the preserved historical build logic**. In that case the archive must record both:

- the SHA-256 of the originally published historical binary, when known;
- the SHA-256 of the archived rebuild.

The archive must never describe a rebuilt binary as byte-identical unless the hashes actually match.

## Upstream contribution

The source-only fix is submitted upstream as:

- `NaGaa95/DrasticDS_nx` Pull Request **#33 — Add direct ROM launch support for NSP forwarders**.

The PR contains only the upstream-facing `launcher/source/main.cpp` change. Tuning documentation, validation, release infrastructure and history remain on `willsito-tuning`.

## AI disclosure

The Direct Forwarder work was developed and reviewed together with ChatGPT. WillsitoGG personally reviewed the source and validated the historical reference build on a real Nintendo Switch using NSP forwarders. Fresh rebuilds are identified separately unless they are also tested on hardware.

## Legal / upstream

This fork does not redistribute Nintendo DS BIOS or firmware and does not change upstream legal requirements. Drastic is proprietary software; use only legally obtained required files and game backups. Upstream licensing and third-party notices remain authoritative.