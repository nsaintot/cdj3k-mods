#!/usr/bin/env bash
# release.sh - build a shippable release: both .UPD packages, checksums, and a
# manifest describing exactly what is in them.
#
#   ./release.sh --version 0.1.0 --key /path/to/packaging.key
#
# Produces build/release/v<VERSION>/
#   CDJ3Kv<ver>_mods.UPD          the install package  <- what users flash
#   uninstall/CDJ3Kv000_remove.UPD the removal package
#   SHA256SUMS                 checksums for both
#   MANIFEST.txt               versions, sizes, and the shim's measured ABI
#
# The version appears in exactly two places a user can see, because this package
# reflashes nothing and the deck's own firmware version is left alone:
#   - the filename    (CDJ3Kv010_mods.UPD for 0.1.0)
#   - MOD SETTINGS    (stamped into the shim as EP122_MOD_VERSION)
# The scripts inside locate their media by the filename, so pack.sh rewrites
# them to match whatever the package ships as.
#
# Requires the packaging key. Without it there is nothing to ship - no deck will
# accept the result - so this refuses rather than producing something that looks
# releasable and is not.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

VERSION=""
KEY=""
OUT_ROOT="$HERE/build/release"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) VERSION="${2:-}"; shift 2 ;;
        --key)     KEY="${2:-}";     shift 2 ;;
        --out)     OUT_ROOT="${2:-}"; shift 2 ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "release.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$VERSION" ]]; then
    echo "release.sh: --version is required (e.g. --version 0.1.0)" >&2
    exit 2
fi
# Reject a leading v so the directory name (v0.1.0) and the stamped version
# (0.1.0) cannot drift apart.
if [[ "$VERSION" == v* ]]; then
    echo "release.sh: pass the version without the leading 'v' (got: $VERSION)" >&2
    exit 2
fi
if [[ -z "$KEY" || ! -f "$KEY" ]]; then
    cat >&2 <<EOF
release.sh: the packaging key is required to build a release.

    ./release.sh --version $VERSION --key /path/to/packaging.key

A package built without it is not accepted by any deck, so there is no useful
artefact to publish. The key is a secret and is not in this repository. To test
the build WITHOUT shipping it, use:

    ./build.sh --key <any-32-byte-file> --version $VERSION
EOF
    exit 1
fi

# Same derivation build.sh uses, so the manifest names what actually shipped.
# Same shape build.sh enforces: the deck only matches CDJ3Kv<3 digits>.UPD.
VDIGITS="$(printf '%s' "$VERSION" | tr -d '.')"
INSTALL_UPD="CDJ3Kv${VDIGITS}_mods.UPD"
# The removal image cannot also be CDJ3Kv<ver>.UPD (one stick, one name the
# deck will pick up), so it gets a fixed name and lives in its own directory.
# Flash it on its own when removing.
REMOVE_UPD="CDJ3Kv000_remove.UPD"

OUT="$OUT_ROOT/v$VERSION"
if [[ -e "$OUT" ]]; then
    echo "release.sh: $OUT already exists - remove it or pick another version" >&2
    exit 1
fi
mkdir -p "$OUT"

echo "==> building release v$VERSION"

# 1) Install package. Builds the binaries with the version stamped in, runs the
#    ABI gate in the Docker stage, then packs.
./build.sh --version "$VERSION" --key "$KEY" --out "$OUT"

# 2) Removal package. No binaries, so nothing to build; reuses the same key.
#    build.sh names it CDJ3Kv000_remove.UPD whatever the version.
./build.sh --remove --key "$KEY" --out "$OUT/uninstall"   # CDJ3Kv000_remove.UPD

# 3) Checksums, over exactly what ships.
( cd "$OUT" && \
  sha256sum "$INSTALL_UPD" "uninstall/$REMOVE_UPD" > SHA256SUMS 2>/dev/null || \
  shasum -a 256 "$INSTALL_UPD" "uninstall/$REMOVE_UPD" > SHA256SUMS )

# 4) Manifest. What a user (or a future bisect) needs to identify this build:
#    the version stamped into the shim, the git description, and the shim's
#    measured ABI, which decides whether it loads on the deck.
SHIM="$OUT/out/ep122_shim.so"
GIT_DESC="$(git -C "$HERE" describe --tags --always --dirty 2>/dev/null || echo unknown)"
ABI="$(docker run --rm -v "$OUT/out:/o:ro" arm64v8/alpine:3.19 sh -uc '
    apk add --no-cache binutils >/dev/null 2>&1
    echo "  NEEDED:        $(readelf -d /o/ep122_shim.so | sed -n "s/.*NEEDED.*\[\(.*\)\]/\1/p" | tr "\n" " ")"
    echo "  glibc symbols: $(readelf --dyn-syms -W /o/ep122_shim.so | grep -oE "GLIBC_[0-9]+\.[0-9]+" | sort -uV | tr "\n" " ")"
' 2>/dev/null || echo "  (ABI probe unavailable)")"

{
    echo "cdj3k-mods release v$VERSION"
    echo
    echo "version stamped into the shim : $VERSION"
    echo "git description               : $GIT_DESC"
    echo
    echo "install package : $INSTALL_UPD           ($(wc -c < "$OUT/$INSTALL_UPD" | tr -d ' ') bytes)"
    echo "removal package : uninstall/$REMOVE_UPD ($(wc -c < "$OUT/uninstall/$REMOVE_UPD" | tr -d ' ') bytes)"
    echo
    echo "shim ABI (must stay loadable on the deck's glibc 2.17):"
    echo "$ABI"
    echo
    echo "installs to /home/root/mods/{ep122_shim.so,stemd_client}"
    echo "preloaded on the application line only - TestMode and the launcher's"
    echo "helper tools run stock. Removal clears the overlay; the rootfs is never"
    echo "written."
} > "$OUT/MANIFEST.txt"

# The intermediate binaries are build output, not release artefacts.
rm -rf "$OUT/out" "$OUT/uninstall/out"

echo
echo "==> release v$VERSION ready: $OUT"
echo
( cd "$OUT" && find . -type f | sort | sed 's|^\./|    |' )
echo
echo "    Publish $INSTALL_UPD as the download; keep uninstall/ alongside it."
echo "    Users flash it with the deck's normal firmware update procedure."
