#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-/work/drastic-willsito-direct-forwarder}"
OUT="${OUT:-$ROOT/drastic-willsito-build-output}"
PATCH="$ROOT/Patches/DrasticDS-1.0.9-DirectForwarderFix.patch"

UPSTREAM_REPO="https://github.com/NaGaa95/DrasticDS_nx.git"
UPSTREAM_COMMIT="b587105edd831bb149c0aef681363cbb3c12e9b3"
EXPECTED_SOURCE_BLOB="eda8278a5bdb98d847a16070b1ed6bdb5ec474bb"
EXPECTED_OFFICIAL_NRO_SHA256="d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679"
HARDWARE_TESTED_REFERENCE_SHA256="91ebec3eab02001609d12fe267fdb190fdffa1ad690ccb48ba63a03e3d321542"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export PATH="$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH"
export WORK OUT

for tool in git cmake ninja python3 make sha256sum; do
  command -v "$tool" >/dev/null || { echo "Missing required host tool: $tool" >&2; exit 1; }
done

rm -rf "$WORK" "$OUT"
mkdir -p "$WORK" "$OUT"

echo "==> Checking Switch build dependencies"
dkp-pacman -S --needed --noconfirm \
  switch-tools libnx switch-sdl2 switch-sdl2_ttf switch-sdl2_image \
  switch-curl switch-mesa switch-libdrm_nouveau switch-zlib

echo "==> Fetching exact upstream source $UPSTREAM_COMMIT"
git init "$WORK/DrasticDS_nx"
cd "$WORK/DrasticDS_nx"
git remote add origin "$UPSTREAM_REPO"
git fetch --no-tags --depth 1 origin "$UPSTREAM_COMMIT"
git checkout --detach FETCH_HEAD

echo "==> Applying exact WillsitoGG source delta"
git apply --check "$PATCH"
git apply "$PATCH"
SOURCE_BLOB="$(git hash-object launcher/source/main.cpp)"
if [[ "$SOURCE_BLOB" != "$EXPECTED_SOURCE_BLOB" ]]; then
  echo "Unexpected patched main.cpp blob: $SOURCE_BLOB" >&2
  echo "Expected: $EXPECTED_SOURCE_BLOB" >&2
  exit 1
fi

grep -q 'silentDirectForwarder' launcher/source/main.cpp
grep -q 'prepareDirectForwarderGame' launcher/source/main.cpp
grep -q 'if(!forwarderDirectPath) renderUsbForwarderWait();' launcher/source/main.cpp
! grep -q 'forwarderByPath' launcher/source/main.cpp

echo "==> Downloading and verifying official DrasticDS_nx 1.0.9 release asset"
python3 - <<'PY'
import hashlib
import json
import os
import pathlib
import urllib.request
import zipfile

repo='NaGaa95/DrasticDS_nx'
expected='d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679'
work=pathlib.Path(os.environ['WORK'])
req=urllib.request.Request(
    f'https://api.github.com/repos/{repo}/releases?per_page=100',
    headers={'User-Agent':'willsito-drastic-direct-forwarder-builder'})
with urllib.request.urlopen(req) as r:
    releases=json.load(r)
release=None
for item in releases:
    tag=str(item.get('tag_name',''))
    name=str(item.get('name',''))
    if tag in {'1.0.9','v1.0.9'} or '1.0.9' in name:
        release=item
        break
if release is None:
    raise SystemExit('Official DrasticDS_nx 1.0.9 release not found')
root=work/'official'
root.mkdir(parents=True, exist_ok=True)
for asset in release.get('assets', []):
    target=root/asset['name']
    areq=urllib.request.Request(
        asset['browser_download_url'],
        headers={'User-Agent':'willsito-drastic-direct-forwarder-builder'})
    with urllib.request.urlopen(areq) as src, target.open('wb') as dst:
        while True:
            block=src.read(1024*1024)
            if not block:
                break
            dst.write(block)
    if target.suffix.lower()=='.zip':
        try:
            with zipfile.ZipFile(target) as z:
                z.extractall(root/(target.stem+'_unpacked'))
        except zipfile.BadZipFile:
            pass
matches=list(root.rglob('DrasticDS.nro')) or list(root.rglob('*.nro'))
if not matches:
    raise SystemExit('No DrasticDS NRO found in official 1.0.9 release')
source=matches[0]
data=source.read_bytes()
actual=hashlib.sha256(data).hexdigest()
if actual != expected:
    raise SystemExit(f'Official NRO SHA mismatch: {actual} != {expected}')
(work/'DrasticDS_official.nro').write_bytes(data)
(work/'OFFICIAL_RELEASE.txt').write_text(
    f"tag={release.get('tag_name')}\nname={release.get('name')}\nasset={source.name}\n")
PY
cp "$WORK/OFFICIAL_RELEASE.txt" "$OUT/OFFICIAL_RELEASE.txt"
printf '%s  %s\n' "$EXPECTED_OFFICIAL_NRO_SHA256" 'DrasticDS_official_1.0.9.nro' > "$OUT/OFFICIAL_NRO_SHA256.txt"

echo "==> Building nstool for official RomFS extraction"
git clone --recursive --depth 1 --branch development-tip \
  https://github.com/jakcron/nstool.git "$WORK/nstool"
cd "$WORK/nstool"
git submodule update --init --recursive
make deps -j2
make -j2
NSTOOL="$(find "$WORK/nstool" -type f -name nstool -perm -111 -print -quit)"
test -x "$NSTOOL"

mkdir -p "$WORK/drastic_romfs"
"$NSTOOL" -x "$WORK/drastic_romfs" "$WORK/DrasticDS_official.nro"

echo "==> Building patched launcher with official 1.0.9 RomFS"
cd "$WORK/DrasticDS_nx"
cmake -S launcher/dependencies -B launcher/dependencies/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
cmake --build launcher/dependencies/build --parallel 2
make -C launcher clean
make -C launcher -j2 ROMFS_DIR="$WORK/drastic_romfs"
cp launcher/DrasticDS.nro "$OUT/DrasticDS.nro"

echo "==> Validating output"
python3 - "$OUT/DrasticDS.nro" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
data=p.read_bytes()[:0x20]
if len(data)<0x14 or data[0x10:0x14] != b'NRO0':
    raise SystemExit('Output is not a valid NRO0 file')
PY

"$NSTOOL" -v "$OUT/DrasticDS.nro" > "$OUT/DrasticDS_nstool.txt"
grep -Eq 'DisplayVersion:[[:space:]]+1\.0\.9' "$OUT/DrasticDS_nstool.txt"

ACTUAL_TUNED_SHA256="$(sha256sum "$OUT/DrasticDS.nro" | awk '{print $1}')"
printf '%s  DrasticDS.nro\n' "$ACTUAL_TUNED_SHA256" > "$OUT/SHA256SUMS.txt"

if [[ "$ACTUAL_TUNED_SHA256" == "$HARDWARE_TESTED_REFERENCE_SHA256" ]]; then
  BUILD_IDENTITY="byte-identical to the original hardware-tested build"
else
  BUILD_IDENTITY="fresh rebuild from the exact hardware-tested source; not byte-identical to the historical binary"
fi

cat > "$OUT/PROVENANCE.txt" <<EOF
DrasticDS_nx 1.0.9 – Direct Forwarder Fix
Upstream source commit: $UPSTREAM_COMMIT
Patched launcher/source/main.cpp blob: $SOURCE_BLOB
Official 1.0.9 NRO SHA-256: $EXPECTED_OFFICIAL_NRO_SHA256
Current rebuilt NRO SHA-256: $ACTUAL_TUNED_SHA256
Hardware-tested historical NRO SHA-256: $HARDWARE_TESTED_REFERENCE_SHA256
Build identity: $BUILD_IDENTITY
Normal launcher: unchanged
NaGaa95 -g <gameKey>: unchanged
Positional argv[1] forwarder: direct game identity, no library-grid frame, no renderUsbForwarderWait frame
Official RomFS/core/resources: reused from upstream DrasticDS_nx 1.0.9
Hardware validation applies to the historical reference build, not automatically to this newly rebuilt binary.
EOF

cat "$OUT/SHA256SUMS.txt"
echo "PASS: DrasticDS_nx Direct Forwarder Fix built and structurally validated from the exact source"