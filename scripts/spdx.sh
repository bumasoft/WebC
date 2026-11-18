#!/bin/sh
# SPDX headers for the C sources.
#
#   sh scripts/spdx.sh           add the header where it is missing
#   sh scripts/spdx.sh --check   exit 1 if any C source lacks it (for CI)
#
# The header is the first line of the file, above the existing `// WebC — …`
# comment. It is a line comment, so it can sit above a feature-test macro
# (#define _GNU_SOURCE) without changing what the preprocessor sees.
set -eu

LICENCE='// SPDX-License-Identifier: Apache-2.0'
cd "$(dirname "$0")/.."

files=$(git ls-files 'src/*.c' 'include/webc/*.h' 'tools/**/*.c')
missing=0
added=0

for f in $files; do
  if head -3 "$f" | grep -q 'SPDX-License-Identifier'; then
    continue
  fi
  if [ "${1:-}" = "--check" ]; then
    echo "missing SPDX header: $f" >&2
    missing=$((missing + 1))
    continue
  fi
  # Write the header plus the original file, avoiding a temp file in the tree.
  { printf '%s\n' "$LICENCE"; cat "$f"; } > "$f.spdx.tmp"
  mv "$f.spdx.tmp" "$f"
  added=$((added + 1))
done

if [ "${1:-}" = "--check" ]; then
  [ "$missing" -eq 0 ] || { echo "$missing file(s) without an SPDX header" >&2; exit 1; }
  echo "spdx: all C sources carry a header"
else
  echo "spdx: added to $added file(s)"
fi