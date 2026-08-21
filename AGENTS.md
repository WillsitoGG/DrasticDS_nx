# AGENTS.md

Repository-specific working rules for `WillsitoGG/DrasticDS_nx`.

## 1. Repository purpose

This repository is a fork of `NaGaa95/DrasticDS_nx` used for two separate purposes:

1. keep an upstream-clean mirror suitable for syncing and upstream Pull Requests;
2. maintain WillsitoGG's validated Nintendo Switch tuning and release history.

Do not mix those two purposes in the same branch.

## 2. Branch policy

### `main`

- Must remain upstream-clean and synchronizable with `NaGaa95/DrasticDS_nx:main`.
- Do not add WillsitoGG-specific README changes, validation files, release scripts, archives or tuneo-only code to `main`.
- When upstream changes, sync `main` first and review whether the tuning needs rebasing/revalidation.

### `willsito-tuning`

- Permanent branch containing the current validated WillsitoGG tuning.
- May contain `AGENTS.md`, custom README, `Archive/`, `Validation/`, `Patches/` and `scripts/`.
- The current direct-forwarder implementation in `launcher/source/main.cpp` must stay traceable to its exact upstream base and validated release.

### Upstream PR branches

- Create from the exact current upstream `main`, not from `willsito-tuning`.
- Include only the source change required by upstream.
- Do not include tuning documentation, archive files, validation records, custom release files or build-history clutter.
- Search for an existing matching PR before opening another one.

## 3. Current validated tuning

Name: `DrasticDS_nx 1.0.9 – Direct Forwarder Fix`

- Upstream source commit: `b587105edd831bb149c0aef681363cbb3c12e9b3`
- Tuned source commit: `c983d2f6291bbf9dac03b14dc8e7720ba516d05c`
- Tuned `launcher/source/main.cpp` blob: `eda8278a5bdb98d847a16070b1ed6bdb5ec474bb`
- Final NRO SHA-256: `91ebec3eab02001609d12fe267fdb190fdffa1ad690ccb48ba63a03e3d321542`
- Official 1.0.9 NRO SHA-256: `d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679`

Never silently replace these values. If upstream or the tuning changes, update provenance and validation together.

## 4. Direct-forwarder requirements

Target behavior:

`NSP forwarder -> DrasticDS.nro -> requested game`

without displaying avoidable launcher UI in between.

For standard positional forwarders:

- the game path is received through `argv[1]`;
- detect the positional forwarder before the normal library/storage UI pipeline;
- resolve the selected game directly from that path;
- do not require a full library scan just to launch the selected ROM;
- do not show the game grid before launch;
- do not show `renderUsbForwarderWait()` for positional direct forwarders;
- avoid unrelated SMB auto-mount work;
- initialize USB when the requested ROM itself is on USB;
- preserve stable game identity and per-game configuration behavior.

The upstream `-g <gameKey>` mechanism must remain supported and retain its normal behavior.

Normal launcher startup must remain unchanged when no positional direct-forwarder path is supplied.

## 5. Scope discipline

- Prefer minimal, localized source changes.
- Do not change unrelated emulator behavior while working on forwarders.
- Do not alter upstream version numbers unless a real upstream version change requires it.
- Keep the upstream version and add the tuning name/revision externally.
- Do not replace official core/resources with unrelated or rebuilt variants without an explicit reason and a new validation record.

## 6. Build and validation

Compilation alone is not sufficient.

Before publishing a new tuned release, verify at minimum:

- exact upstream base commit/version;
- expected source patch applies cleanly;
- final `launcher/source/main.cpp` identity;
- ARM64/Switch build succeeds;
- output is a valid NRO (`NRO0`);
- NACP/display version is expected;
- SHA-256 of the final NRO;
- no out-of-scope source changes;
- positional `argv[1]` path is still direct and silent;
- `-g <gameKey>` remains present;
- normal launcher path remains present;
- official RomFS/core/resources provenance remains correct when reused.

Hardware-only behavior must never be claimed as tested unless WillsitoGG actually tested that exact resulting build on a Nintendo Switch.

## 7. Releases

- Keep only the current final WillsitoGG tuning visible in this fork's Releases.
- Current SD-ready Release asset should normally be only `DrasticDS.nro`.
- Do not publish validation text, patches, logs or provenance files as Release assets; keep them in the repository.
- Do not store the current `DrasticDS.nro` in the normal source tree.

When replacing a final tuned release:

1. build and validate the new release first;
2. publish and verify it;
3. recover the exact old published NRO before removing the old Release;
4. archive that exact binary under `Archive/<Version>/` with SHA-256 and provenance;
5. preserve historical tag information;
6. remove the superseded Release from the visible Releases list;
7. clean all temporary workflows, outputs and scripts.

Do not reconstruct a historical binary and present it as exact unless its SHA-256 proves identity with the original published binary.

## 8. Archive

`Archive/` contains only final revisions that were actually published and later superseded.

Do not archive:

- failed builds;
- intermediate experiments;
- temporary CI artifacts;
- logs;
- unvalidated reconstructed binaries.

For each archived final revision retain, where recoverable:

- exact NRO;
- SHA-256;
- original release/tag name;
- exact source or source identity;
- a short provenance note.

## 9. Repository cleanliness

Do not leave temporary build/output directories or one-shot workflows in the final branch tree.

Examples that must not remain after work is complete:

- `*-build-output/`;
- `build/`, `dist/`, `out/` created only for local/CI work;
- one-time migration workflows;
- test trigger files;
- discarded patch scripts;
- temporary run-id files;
- logs unrelated to permanent validation.

## 10. Documentation

Document every material WillsitoGG tuning change in the repository.

At minimum update, when applicable:

- `README.md` for user-facing behavior/status;
- `Validation/` for provenance and hashes;
- `Patches/` for the reproducible source delta;
- `Archive/` when a final revision is replaced;
- `AGENTS.md` if the working rules themselves change.

## 11. Upstream Issues and Pull Requests

Use a technical but friendly tone.

Normally:

- greet the maintainer;
- thank them for their work;
- explain the problem, change, preserved behavior, scope and actual testing;
- include an `AI disclosure` explaining that the analysis/development was done together with ChatGPT while WillsitoGG personally reviewed the result and performed the stated real-hardware validation;
- never claim hardware validation that did not occur;
- close with `Thanks in advance!` and `Best regards, Willsito` unless repository policy calls for something else.

If an upstream repository has a specific AI/contribution policy, that policy overrides this default format.

## 12. Legal / proprietary material

Do not commit Nintendo DS BIOS, firmware, game images or other user-supplied proprietary material.

Respect the upstream project's licensing and redistribution requirements. Keep proprietary core/resource handling consistent with upstream and with the exact release provenance recorded for the tuning.