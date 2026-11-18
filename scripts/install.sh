#!/bin/sh
# The one-command install:
# a prebuilt CLI and framework tree from a GitHub release, into ~/.local.
#
#   curl -fsSL https://raw.githubusercontent.com/bumasoft/WebC/main/scripts/install.sh | sh
#
# With flags (after `sh -s --`):
#   --ref v0.2.0     pin a release (default: the latest)
#   --dest ~/.local  where bin/ and share/ live
#   --wasi           also fetch wasi-sdk (a browser half needs it)
#   --version        just print the release that would be installed
#
# WEBC_BASE_URL points the download somewhere other than GitHub releases (a mirror, an
# air-gapped share, `file:///tmp/fake-release` in scripts/test-install.sh). It still gets
# the same checksum treatment: there is no path through this script that unpacks a tarball
# nobody vouched for.
#
# The tree it lays out is the one the CLI already knows how to find: share/webc/<ver>/
# with bin/, build/libwebc.a, include/, src/, assets/, skills/, docs/, examples/board/,
# tools/client/ and tools/learn/ — and bin/webc symlinked into it, so `webc doctor` resolves the
# framework through its own binary (no WEBC_HOME, no PATH games beyond bin/ itself).
set -eu

REPO=bumasoft/WebC
REF=""
DEST="${WEBC_DEST:-$HOME/.local}"
WASI=0
JUST_PRINT=0

while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF="${2:-}"; shift 2 ;;
    --dest) DEST="${2:-}"; shift 2 ;;
    --wasi) WASI=1; shift ;;
    --version) JUST_PRINT=1; shift ;;
    *) echo "install.sh: unknown flag $1" >&2; exit 1 ;;
  esac
done

# The whole script is curl and tar; a box without curl should hear that, not "no prebuilt
# webc for this platform".
command -v curl >/dev/null 2>&1 || { echo "install.sh: curl is required" >&2; exit 1; }
command -v tar  >/dev/null 2>&1 || { echo "install.sh: tar is required" >&2; exit 1; }

OS=$(uname -s); ARCH=$(uname -m)
case "$OS" in Darwin) OS=darwin ;; Linux) OS=linux ;; esac
case "$ARCH" in
  arm64|aarch64) ARCH=aarch64 ;;
  x86_64|amd64) ARCH=x86_64 ;;
esac
PLATFORM="$OS-$ARCH"

# the tag, when the caller did not pin one: GitHub redirects latest/download to it,
# and the Location header is the answer without parsing the API
PINNED=${REF:+yes}
if [ -z "$REF" ]; then
  REF=$(curl -fsSI -o /dev/null -w '%{url_effective}' \
        "https://github.com/$REPO/releases/latest/download/webc-$PLATFORM.tar.gz" \
        2>/dev/null | sed 's|.*/tag/||' || true)
fi
case "$REF" in ""|https*) REF=latest; UNRESOLVED=1 ;; esac # a redirect that never happened named no tag
# When the reply was not a tag, say why in the words of the person who just hit it: a private
# repository, no network, or an air-gapped mirror all land here, and the release they want is one
# flag away. The "build from source" advice further down is for a platform with no artefacts,
# which is a different problem.
if [ -n "${UNRESOLVED:-}" ] && [ -z "${PINNED:-}" ]; then
  echo "install.sh: could not resolve the latest release tag (private repository, no network," >&2
  echo "  or a mirror without a 'latest' entry). Pin the release you want:" >&2
  echo "    sh install.sh --ref v0.1.0   # the tag, as /releases lists it" >&2
fi
if [ "$JUST_PRINT" = 1 ]; then echo "$REF ($PLATFORM)"; exit 0; fi

echo "installing webc $REF for $PLATFORM into $DEST"

BASE="${WEBC_BASE_URL:-https://github.com/$REPO/releases/download}"
URL="$BASE/$REF/webc-$PLATFORM.tar.gz"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if ! curl -fsSL -o "$TMP/webc.tar.gz" "$URL"; then
  cat >&2 <<EOF
no prebuilt webc for $PLATFORM at $REF.

From source (a checkout and a C23 compiler are the whole requirement):
  git clone https://github.com/$REPO.git && cd WebC && make
  export PATH="\$PWD/build:\$PATH"        # or: cp build/webc somewhere on the PATH
EOF
  exit 1
fi

# The tarball must be the one the release vouched for. The published digest names the
# platform file (webc-darwin-aarch64.tar.gz) while this one lands as webc.tar.gz, so
# `shasum -c` cannot be used: compare the digests instead. sha256sum is Linux, shasum is
# what macOS has. `set -eu` means a GOT we failed to compute aborts here rather than
# comparing an empty string.
if ! curl -fsSL -o "$TMP/webc.tar.gz.sha256" "$URL.sha256"; then
  cat >&2 <<EOF
install.sh: no checksum published for $REF ($URL.sha256)

An artefact nobody signed off on is not something this script will unpack. Pin a release
that publishes one (--ref vX.Y.Z), or build from source — see the message above.
EOF
  exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
  GOT=$(sha256sum "$TMP/webc.tar.gz" | cut -d' ' -f1)
else
  GOT=$(shasum -a 256 "$TMP/webc.tar.gz" | cut -d' ' -f1)
fi
# the file is "<digest>  webc-<platform>.tar.gz"; the first field is the digest
WANT=$(cut -d' ' -f1 "$TMP/webc.tar.gz.sha256")
if [ -z "${WANT:-}" ] || [ "$WANT" != "$GOT" ]; then
  echo "install.sh: checksum mismatch — refusing to install" >&2
  echo "  published: ${WANT:-<empty>}" >&2
  echo "  downloaded: $GOT" >&2
  exit 1
fi
echo "  checksum ok     $(printf '%s' "$GOT" | cut -c1-16)…"

tar xzf "$TMP/webc.tar.gz" -C "$TMP"
# the tarball is one webc/ directory; the version file inside names the release
VER=$(cat "$TMP/webc/version" 2>/dev/null || echo "$REF")
TREE="$DEST/share/webc/$VER"
mkdir -p "$DEST/share/webc" "$DEST/bin"
rm -rf "$TREE"
mv "$TMP/webc" "$TREE"
ln -sf "$TREE/bin/webc" "$DEST/bin/webc"

echo "  webc $VER       $DEST/bin/webc"
case ":$PATH:" in
  *":$DEST/bin:"*) ;;
  *) echo "  note: $DEST/bin is not on the PATH — add it:  export PATH=\"$DEST/bin:\$PATH\"" ;;
esac

if [ "$WASI" = 1 ]; then
  # wasi-sdk, where the CLI already looks (~/.local/share/wasi-sdk-*).
  #
  # The version is not decoration: the framework needs clang 21 or newer (the suite uses
  # `#embed`) and this scripts the *browser half*, so a wasi-sdk whose clang is 19 installs a
  # toolchain that cannot build the thing it was installed for. 34.0 ships clang 23; it is the
  # same pin CI uses (.github/actions/wasi-sdk), which is what keeps the two from drifting.
  WASI_VER="${WASI_VER:-34.0}"
  case "$PLATFORM" in
    darwin-aarch64) WASI_ARCH=arm64; WASI_OS=macos ;;
    darwin-x86_64) WASI_ARCH=x86_64; WASI_OS=macos ;;
    linux-aarch64) WASI_ARCH=arm64; WASI_OS=linux ;;
    linux-x86_64) WASI_ARCH=x86_64; WASI_OS=linux ;;
  esac
  WASI_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_VER%%.*}/wasi-sdk-${WASI_VER}-${WASI_ARCH}-${WASI_OS}.tar.gz"
  WASI_DIR="$DEST/share"
  if curl -fsSL -o "$TMP/wasi.tar.gz" "$WASI_URL"; then
    tar xzf "$TMP/wasi.tar.gz" -C "$WASI_DIR"
    WASI_HOME="$WASI_DIR/wasi-sdk-${WASI_VER}-${WASI_ARCH}-${WASI_OS}"
    if [ ! -x "$WASI_HOME/bin/clang" ]; then
      echo "install.sh: wasi-sdk unpacked without bin/clang ($WASI_HOME) — the browser half" >&2
      echo "  will not build; a server-only app still does." >&2
    else
      echo "  wasi-sdk $WASI_VER  $WASI_HOME ($("$WASI_HOME/bin/clang" --version | head -1))"
      echo "  webc doctor finds it there by itself"
    fi
  else
    echo "  wasi-sdk $WASI_VER not fetched ($WASI_URL) — a server-only app does not need it" >&2
  fi
fi

echo "next:  webc new myapp && cd myapp && webc dev"
