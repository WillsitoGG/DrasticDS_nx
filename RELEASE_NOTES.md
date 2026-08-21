# DrasticDS_nx 1.0.9 – Direct Forwarder Fix

WillsitoGG tuning based on NaGaa95 DrasticDS_nx 1.0.9 (`b587105edd831bb149c0aef681363cbb3c12e9b3`).

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
- Official DrasticDS_nx 1.0.9 NRO SHA-256: `d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679`.
- Official 1.0.9 RomFS/core/resources are reused from the upstream release.
- The current Release is a fresh build from the exact source above and records its own SHA-256 in `RELEASE_STATUS.md` and `SHA256SUMS.txt`.
- Historical hardware-tested reference NRO SHA-256: `91ebec3eab02001609d12fe267fdb190fdffa1ad690ccb48ba63a03e3d321542`.
- The source and historical reference build were manually reviewed/tested by WillsitoGG on real Nintendo Switch hardware. A fresh rebuild is not described as independently hardware-tested unless that exact binary is later tested.

## Installation

Copy the Release asset:

`DrasticDS.nro`

to:

`/switch/DrasticDS.nro`

All BIOS, firmware, resource and legal requirements from upstream DrasticDS_nx continue to apply.