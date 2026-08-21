#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/launcher/source/main.cpp"
EXPECTED_BLOB="eda8278a5bdb98d847a16070b1ed6bdb5ec474bb"
UPSTREAM_COMMIT="b587105edd831bb149c0aef681363cbb3c12e9b3"

cd "$ROOT"

test -f "$SOURCE"
ACTUAL_BLOB="$(git hash-object launcher/source/main.cpp)"
[[ "$ACTUAL_BLOB" == "$EXPECTED_BLOB" ]] || {
  echo "main.cpp blob mismatch: $ACTUAL_BLOB (expected $EXPECTED_BLOB)" >&2
  exit 1
}

grep -q 'std::string positionalForwarderPath' "$SOURCE"
grep -q 'const bool silentDirectForwarder' "$SOURCE"
grep -q 'prepareDirectForwarderGame' "$SOURCE"
grep -q 'if(!forwarderDirectPath) renderUsbForwarderWait();' "$SOURCE"
grep -q 'strcmp(argv\[argument\],"-g")' "$SOURCE"
! grep -q 'forwarderByPath' "$SOURCE"

CHANGED_SOURCE_FILES="$(git diff --name-only "$UPSTREAM_COMMIT" -- launcher/source)"
[[ "$CHANGED_SOURCE_FILES" == 'launcher/source/main.cpp' ]] || {
  echo "Unexpected launcher/source scope:" >&2
  printf '%s\n' "$CHANGED_SOURCE_FILES" >&2
  exit 1
}

echo "PASS: exact validated direct-forwarder source and scope confirmed"
