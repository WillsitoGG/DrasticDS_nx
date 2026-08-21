#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="${WORK:-/work/drastic-direct-forwarder-v1}"
OUT="${OUT:-$ROOT/drastic-v1-build-output}"

UPSTREAM_REPO="https://github.com/NaGaa95/DrasticDS_nx.git"
UPSTREAM_COMMIT="b587105edd831bb149c0aef681363cbb3c12e9b3"
EXPECTED_OFFICIAL_NRO_SHA256="d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679"
HISTORICAL_PUBLISHED_SHA256="d58c66b24681098a5fc2fa31f7b511a79176858cbe073b04cb7cc46324a1a7c0"

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

dkp-pacman -S --needed --noconfirm \
  switch-tools libnx switch-sdl2 switch-sdl2_ttf switch-sdl2_image \
  switch-curl switch-mesa switch-libdrm_nouveau switch-zlib

echo "==> Fetching pinned upstream source"
git init "$WORK/DrasticDS_nx"
cd "$WORK/DrasticDS_nx"
git remote add origin "$UPSTREAM_REPO"
git fetch --no-tags --depth 1 origin "$UPSTREAM_COMMIT"
git checkout --detach FETCH_HEAD

echo "==> Applying the original v1 positional-forwarder patch logic"
python3 - <<'PY'
from pathlib import Path
p=Path('/work/drastic-direct-forwarder-v1/DrasticDS_nx/launcher/source/main.cpp')
s=p.read_text()
old='''  bool forwarderRequested=false,forwarderMatched=false;\n  std::string forwarderKey;\n  for(int argument=1;argument+1<argc;argument++) if(!strcmp(argv[argument],"-g")){\n    forwarderRequested=true;\n    forwarderKey=argv[argument+1];\n    if(Game *game=findGameByKey(forwarderKey)){ selectGame(*game); forwarderMatched=true; }\n    break;\n  }\n'''
if old not in s:
    raise SystemExit('Expected upstream forwarder parser not found')
new='''  bool forwarderRequested=false,forwarderMatched=false,forwarderByPath=false;\n  std::string forwarderKey;\n  auto findForwarderGame=[&]() -> Game* {\n    if(!forwarderByPath) return findGameByKey(forwarderKey);\n    const std::string wanted=pathIdentity(forwarderKey);\n    if(wanted.empty()) return nullptr;\n    for(auto &game:g_games)\n      if(pathIdentity(game.path)==wanted) return &game;\n    return nullptr;\n  };\n  for(int argument=1;argument+1<argc;argument++) if(!strcmp(argv[argument],"-g")){\n    forwarderRequested=true;\n    forwarderKey=argv[argument+1];\n    if(Game *game=findForwarderGame()){ selectGame(*game); forwarderMatched=true; }\n    break;\n  }\n  if(!forwarderRequested&&argc>=2&&argv[1]&&argv[1][0]){\n    const std::string directPath=normalizeLocationPath(argv[1]);\n    if(!directPath.empty()&&hasGameExtension(directPath.c_str())){\n      forwarderRequested=true;\n      forwarderByPath=true;\n      forwarderKey=directPath;\n      if(Game *game=findForwarderGame()){ selectGame(*game); forwarderMatched=true; }\n    }\n  }\n'''
s=s.replace(old,new,1)
s=s.replace('if(Game *game=findGameByKey(forwarderKey))','if(Game *game=findForwarderGame())')
p.write_text(s)
PY
SOURCE_BLOB="$(git hash-object launcher/source/main.cpp)"

echo "==> Downloading and verifying official DrasticDS_nx 1.0.9 NRO"
python3 - <<'PY'
import hashlib, json, os, pathlib, urllib.request, zipfile
work=pathlib.Path(os.environ['WORK'])
expected='d540714fc33c8c41ee33678df52369e426a6b09262a963fdee18c02fc5653679'
req=urllib.request.Request('https://api.github.com/repos/NaGaa95/DrasticDS_nx/releases?per_page=100',headers={'User-Agent':'willsito-drastic-v1-builder'})
with urllib.request.urlopen(req) as r: releases=json.load(r)
release=next((x for x in releases if str(x.get('tag_name','')) in {'1.0.9','v1.0.9'} or '1.0.9' in str(x.get('name',''))),None)
if release is None: raise SystemExit('Official 1.0.9 release not found')
root=work/'official'; root.mkdir(parents=True,exist_ok=True)
for asset in release.get('assets',[]):
    target=root/asset['name']
    areq=urllib.request.Request(asset['browser_download_url'],headers={'User-Agent':'willsito-drastic-v1-builder'})
    with urllib.request.urlopen(areq) as src, target.open('wb') as dst:
        while True:
            block=src.read(1024*1024)
            if not block: break
            dst.write(block)
    if target.suffix.lower()=='.zip':
        try:
            with zipfile.ZipFile(target) as z: z.extractall(root/(target.stem+'_unpacked'))
        except zipfile.BadZipFile: pass
matches=list(root.rglob('DrasticDS.nro')) or list(root.rglob('*.nro'))
if not matches: raise SystemExit('No official NRO found')
data=matches[0].read_bytes(); actual=hashlib.sha256(data).hexdigest()
if actual != expected: raise SystemExit(f'Official NRO SHA mismatch: {actual}')
(work/'DrasticDS_official.nro').write_bytes(data)
PY

echo "==> Building nstool and extracting official RomFS"
git clone --recursive --depth 1 --branch development-tip https://github.com/jakcron/nstool.git "$WORK/nstool"
cd "$WORK/nstool"
git submodule update --init --recursive
make deps -j2
make -j2
NSTOOL="$(find "$WORK/nstool" -type f -name nstool -perm -111 -print -quit)"
test -x "$NSTOOL"
mkdir -p "$WORK/drastic_romfs"
"$NSTOOL" -x "$WORK/drastic_romfs" "$WORK/DrasticDS_official.nro"

echo "==> Building v1 launcher"
cd "$WORK/DrasticDS_nx"
cmake -S launcher/dependencies -B launcher/dependencies/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON
cmake --build launcher/dependencies/build --parallel 2
make -C launcher clean
make -C launcher -j2 ROMFS_DIR="$WORK/drastic_romfs"
cp launcher/DrasticDS.nro "$OUT/DrasticDS.nro"

python3 - "$OUT/DrasticDS.nro" <<'PY'
from pathlib import Path
import sys
data=Path(sys.argv[1]).read_bytes()[:0x20]
if len(data)<0x14 or data[0x10:0x14] != b'NRO0': raise SystemExit('Output is not NRO0')
PY
"$NSTOOL" -v "$OUT/DrasticDS.nro" > "$OUT/NRO_METADATA.txt"
grep -Eq 'DisplayVersion:[[:space:]]+1\.0\.9' "$OUT/NRO_METADATA.txt"
REBUILT_SHA256="$(sha256sum "$OUT/DrasticDS.nro" | awk '{print $1}')"
printf '%s  DrasticDS.nro\n' "$REBUILT_SHA256" > "$OUT/SHA256SUMS.txt"
cat > "$OUT/PROVENANCE.txt" <<EOF
DrasticDS_nx 1.0.9 – Direct Forwarder Fix_v1 (archived rebuild)
Pinned upstream commit: $UPSTREAM_COMMIT
Rebuilt launcher/source/main.cpp blob: $SOURCE_BLOB
Historical published NRO SHA-256: $HISTORICAL_PUBLISHED_SHA256
Archived rebuilt NRO SHA-256: $REBUILT_SHA256
This archived NRO is a fresh rebuild of the historical v1 patch logic. It is not claimed to be byte-identical to the originally published v1 binary and is not independently hardware-tested.
EOF
cat "$OUT/SHA256SUMS.txt"
echo "PASS: archived v1 rebuilt and structurally validated"