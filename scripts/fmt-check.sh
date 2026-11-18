#!/bin/sh
# Formatting check, as a ratchet (the same idea as api.txt).
#
#   sh scripts/fmt-check.sh            exit 1 if a file outside the baseline is unformatted
#   sh scripts/fmt-check.sh --write    rewrite tools/fmt-baseline.txt from the current tree
#
# The tree predates .clang-format: most files are laid out by hand, with guard
# clauses on one line and argument lists broken where the author wanted them.
# Reformatting them wholesale would bury every real change in a launch-week
# diff, so formatting is enforced going forward instead of retroactively:
# a C source that is not in the baseline must be clang-format-clean, and entries
# leave the baseline as files get formatted. `make fmt` formats everything and
# `make fmt-baseline` re-cuts the list, for the day the human wants that
# one-time commit.
#
# The formatter's major version matters: clang-format changes its mind between
# releases. tools/fmt-baseline.txt was cut with the version printed by
# `clang-format --version` on the machine that wrote it. Use the same one.
set -eu

CLANG_FORMAT=${CLANG_FORMAT:-clang-format}
BASELINE=tools/fmt-baseline.txt
cd "$(dirname "$0")/.."

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
  echo "fmt-check: $CLANG_FORMAT not found." >&2
  echo "  install it:  brew install clang-format    (macOS)" >&2
  echo "               apt install clang-format     (Debian/Ubuntu)" >&2
  exit 2
fi

# --others so a file the contributor has written but not yet staged is checked too.
sources=$(git ls-files --cached --others --exclude-standard \
  'src/*.c' 'include/webc/*.h' 'tools/**/*.c')

if [ "${1:-}" = "--write" ]; then
  : > "$BASELINE"
  for f in $sources; do
    "$CLANG_FORMAT" --dry-run --Werror "$f" >/dev/null 2>&1 || echo "$f" >> "$BASELINE"
  done
  echo "fmt-baseline: $(wc -l < "$BASELINE" | tr -d ' ') of $(echo "$sources" | wc -l | tr -d ' ') files do not conform"
  exit 0
fi

[ -f "$BASELINE" ] || { echo "fmt-check: $BASELINE is missing; run make fmt-baseline" >&2; exit 2; }

new=0
fixed=0
for f in $sources; do
  if grep -qxF "$f" "$BASELINE"; then
    "$CLANG_FORMAT" --dry-run --Werror "$f" >/dev/null 2>&1 && fixed=$((fixed + 1))
    continue
  fi
  if ! "$CLANG_FORMAT" --dry-run --Werror "$f" >/dev/null 2>&1; then
    echo "unformatted: $f" >&2
    new=$((new + 1))
  fi
done

known=$(wc -l < "$BASELINE" | tr -d ' ')
echo "fmt: $new new offender(s), $known known-unformatted (baseline), $fixed formatted and still listed"

if [ "$fixed" -gt 0 ]; then
  echo "  → $fixed file(s) in the baseline are now clean: run make fmt-baseline to shrink the list"
fi
[ "$new" -eq 0 ] || { echo "run: make fmt   (or clang-format -i <file>)" >&2; exit 1; }