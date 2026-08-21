# DrasticDS_nx 1.0.9 – Direct Forwarder Fix

Validated WillsitoGG tuning based on NaGaa95 DrasticDS_nx 1.0.9 (`b587105edd831bb149c0aef681363cbb3c12e9b3`).

## Direct forwarder

- Supports standard NSP forwarders that pass the selected ROM through `argv[1]`.
- Builds the selected game's identity directly before the normal library/UI path.
- Avoids displaying the game grid for positional direct launches.
- Avoids the visible USB-forwarder wait screen for positional direct launches.
- Still initializes USB when the requested ROM itself is on USB.
- Preserves the normal launcher when DrasticDS_nx is opened normally.
- Preserves NaGaa95's existing `-g <gameKey>` behavior.

## Validation

- Tuned `launcher/source/main.cpp` Git blob: `eda8278a5bdb98d847a16070b1ed6bdb5ec474bb`.
- Final `DrasticDS.nro` SHA-256: `91ebec3eab02001609d12fe267fdb190fdffa1ad690ccb48ba63a03e3d321542`.
- Official DrasticDS_nx 1.0.9 RomFS/core/resources are reused from the upstream release.
- The final build was manually reviewed and tested by WillsitoGG on real Nintendo Switch hardware using NSP forwarders.

## Installation

Copy the Release asset:

`DrasticDS.nro`

to:

`/switch/DrasticDS.nro`

All BIOS, firmware, core/resource and legal requirements from upstream DrasticDS_nx continue to apply.
