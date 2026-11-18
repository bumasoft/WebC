#!/bin/sh
# scripts/test-install.sh — the installer's integrity gate.
#
# install.sh downloads a tarball and unpacks it into ~/.local, which is to say it runs
# code the user never read. The one thing standing between a tampered release and that
# unpack is the sha256 comparison, and a comparison nobody has ever seen fail is a
# comparison that does not work. This script builds a fake release in a temp directory,
# serves it through file://, and drives install.sh through the three cases that matter:
#
#   1. a good tarball with a matching digest   -> installs, and the tree resolves itself
#   2. one byte changed after the digest       -> refuses, exit 1, nothing unpacked
#   3. no digest published at all              -> refuses, exit 1
#   4. the toolchain pins agree                -> --wasi installs a wasi-sdk the framework can
#                                                 actually use (clang 21+), not an older one that
#                                                 cannot compile the browser half it is there for
#
#   sh scripts/test-install.sh        (or: make install-test)
set -eu

for tool in curl tar; do
  command -v $tool >/dev/null 2>&1 || { echo "test-install: $tool is required" >&2; exit 1; }
done
command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1 || {
  echo "test-install: sha256sum or shasum is required" >&2; exit 1; }

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=$(cat "$REPO_ROOT/VERSION")
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

# the platform strings install.sh computes for itself, so the fake artefact has the name
# it asks for
OS=$(uname -s); ARCH=$(uname -m)
case "$OS" in Darwin) OS=darwin ;; Linux) OS=linux ;; esac
case "$ARCH" in arm64|aarch64) ARCH=aarch64 ;; x86_64|amd64) ARCH=x86_64 ;; esac
PLATFORM="$OS-$ARCH"

fail() { echo "test-install: FAIL: $*" >&2; exit 1; }
digest() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# ---------------------------------------------------------------- a fake release tree
REL="$TMP/release/v$VER"
mkdir -p "$REL" "$TMP/stage/webc/bin" "$TMP/stage/webc/build" "$TMP/stage/webc/include/webc"
printf '#!/bin/sh\necho "webc %s"\n' "$VER" > "$TMP/stage/webc/bin/webc"
chmod +x "$TMP/stage/webc/bin/webc"
printf '%s\n' "$VER" > "$TMP/stage/webc/version"
: > "$TMP/stage/webc/build/libwebc.a"
: > "$TMP/stage/webc/include/webc/core.h"   # prefix.c: a framework root has include/webc
tar czf "$REL/webc-$PLATFORM.tar.gz" -C "$TMP/stage" webc
# the shape release.yml publishes: "<digest>  webc-<platform>.tar.gz"
printf '%s  webc-%s.tar.gz\n' "$(digest "$REL/webc-$PLATFORM.tar.gz")" "$PLATFORM" \
  > "$REL/webc-$PLATFORM.tar.gz.sha256"

run_install() { # $1 = dest
  WEBC_BASE_URL="file://$TMP/release" sh "$REPO_ROOT/scripts/install.sh" \
    --ref "v$VER" --dest "$1"
}

# ---------------------------------------------------------------- 1. the good path
echo "== a tarball whose digest matches installs"
DEST="$TMP/dest-good"
if ! OUT=$(run_install "$DEST" 2>&1); then
  printf '%s\n' "$OUT" >&2
  fail "a good tarball was refused"
fi
case "$OUT" in *"checksum ok"*) ;; *) printf '%s\n' "$OUT" >&2; fail "no checksum line in the output" ;; esac
[ -x "$DEST/bin/webc" ] || fail "no bin/webc after a successful install"
[ "$("$DEST/bin/webc" --version)" = "webc $VER" ] \
  || fail "the installed CLI does not report $VER"
[ -f "$DEST/share/webc/$VER/include/webc/core.h" ] || fail "the tree was not laid out"
echo "   ok — installed into $DEST, webc reports $VER"

# ---------------------------------------------------------------- 2. one byte changed
echo "== a tarball that no longer matches its digest is refused"
printf 'x' >> "$REL/webc-$PLATFORM.tar.gz"
DEST2="$TMP/dest-tampered"
if OUT=$(run_install "$DEST2" 2>&1); then
  printf '%s\n' "$OUT" >&2
  fail "a tampered tarball was unpacked anyway"
fi
case "$OUT" in
  *"checksum mismatch"*) ;;
  *) printf '%s\n' "$OUT" >&2; fail "it failed, but not on the checksum" ;;
esac
[ -e "$DEST2/share/webc/$VER" ] && fail "the tampered tree was laid out before the refusal"
echo "   ok — refused, exit 1, nothing unpacked"

# ---------------------------------------------------------------- 3. no digest at all
echo "== a release with no published digest is refused"
rm -f "$REL/webc-$PLATFORM.tar.gz.sha256"
DEST3="$TMP/dest-nodigest"
if OUT=$(run_install "$DEST3" 2>&1); then
  printf '%s\n' "$OUT" >&2
  fail "an unverified tarball was unpacked anyway"
fi
case "$OUT" in
  *"no checksum published"*) ;;
  *) printf '%s\n' "$OUT" >&2; fail "it failed, but not because the digest was missing" ;;
esac
echo "   ok — refused, exit 1"

# ---------------------------------------------------------------- 4. the toolchain pins agree
# install.sh used to fetch wasi-sdk 25.0 (clang 19) while CI installed 34.0 — so `--wasi`,
# the flag whose whole job is the browser half, installed a compiler that cannot build it
# (`#embed` needs clang 21). Two numbers in two files, drifting apart, and nothing compared
# them. This does.
echo "== --wasi asks for the wasi-sdk CI pins"
SH_PIN=$(sed -n 's/.*WASI_VER:-\{0,1\}\([0-9][0-9.]*\).*/\1/p' "$REPO_ROOT/scripts/install.sh" | head -1)
CI_PIN=$(sed -n 's/.*default: "\([0-9][0-9.]*\)".*/\1/p' "$REPO_ROOT/.github/actions/wasi-sdk/action.yml" | head -1)
[ -n "$SH_PIN" ] || fail "scripts/install.sh has no wasi-sdk version to read"
[ -n "$CI_PIN" ] || fail ".github/actions/wasi-sdk/action.yml has no default version"
[ "$SH_PIN" = "$CI_PIN" ] || fail "install.sh wants wasi-sdk $SH_PIN, CI installs $CI_PIN"
echo "   ok — both say wasi-sdk $SH_PIN"

echo "test-install: 4/4"