#!/usr/bin/env bash
# build.sh - build the two deck binaries and package the CDJ3Kv000.UPD.
#
#   1. docker: ep122_shim.so (glibc) + stemd_client_aarch64 (musl-static), aarch64
#   2. pack:   assemble the .UPD package (updater/pack.sh, privileged)
#
# Everything runs in containers, so a macOS host with Docker/OrbStack is enough;
# no aarch64 toolchain, genisoimage or cryptsetup on the host is required.
# Outputs land in build/ (git-ignored).
#
# Usage:
#   ./build.sh --key /path/to/packaging.key      build + pack a flashable .UPD
#   ./build.sh --no-pack                          build the binaries only
#   ./build.sh --no-build --key KEY               re-pack using existing binaries
#   ./build.sh --version 0.2.0 --key KEY          stamp a version into the shim
#   ./build.sh --remove --key KEY                 removal .UPD (build/uninstall/)
#
# A package has to be built with the packaging key the deck expects. That key is
# not in this repo; pass it with --key. See updater/pack.sh for details.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

KEY=""
DO_BUILD=1
DO_PACK=1
OUT=""
VERSION=""
REMOVE=0

# Value-taking options validate their argument, so `--key --no-build` is an
# error rather than KEY="--no-build".
need_val() {
    case "${2-}" in
        ''|-*) echo "build.sh: $1 needs a value" >&2; exit 2 ;;
    esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --key)      need_val "$1" "${2-}"; KEY="$2"; shift 2 ;;
        --no-build) DO_BUILD=0; shift ;;
        --no-pack)  DO_PACK=0; shift ;;
        --out)      need_val "$1" "${2-}"; OUT="$2"; shift 2 ;;
        --version)  need_val "$1" "${2-}"; VERSION="$2"; shift 2 ;;
        --remove)   REMOVE=1; shift ;;
        -h|--help)  sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "build.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

# A removal package carries no binaries (it only clears the overlay), so there is
# nothing to build. It lands in its own directory so it cannot overwrite or be
# mistaken for an install package.
if [[ "$REMOVE" == 1 ]]; then
    DO_BUILD=0
    [[ -n "$OUT" ]] || OUT="$HERE/build/uninstall"
else
    [[ -n "$OUT" ]] || OUT="$HERE/build"
fi

# Version stamp for the shim. The mods repo may have no tags yet, in which case
# git describe is empty and the shim reports "unknown"; override with --version.
GIT_DESCRIBE="$(git -C "$HERE" describe --tags --always --dirty 2>/dev/null || true)"
GIT_TAG="$(git -C "$HERE" describe --tags --abbrev=0 2>/dev/null || true)"
MOD_BUILD="${VERSION:-${GIT_DESCRIBE:-unknown}}"
if [[ -n "$VERSION" ]]; then
    MOD_VERSION="$VERSION"
elif [[ -n "$GIT_TAG" ]]; then
    MOD_VERSION="$GIT_TAG"; [[ "$GIT_TAG" == "$GIT_DESCRIBE" ]] || MOD_VERSION="$GIT_TAG+"
else
    MOD_VERSION="unknown"
fi

# Check the key before building.
if [[ "$DO_PACK" == 1 && -n "$KEY" && ! -f "$KEY" ]]; then
    echo "build.sh: key not found: $KEY" >&2; exit 1
fi

# The .UPD filename carries the version, in the shape the deck accepts.
#
# An RK3399 deck ignores CDJ3KvMODS010.UPD (the update never starts) but takes
# CDJ3Kv000.UPD. Stock update images are CDJ3Kv319.UPD / CDJ3Kv322.UPD, so the
# deck matches CDJ3Kv<3 digits>.UPD and nothing longer. (The Renesas-only Magic
# Phono loader ships CDJ3KvSDBOOT001.UPD, which that variant accepts; do not
# generalise it to this one.)
#
# So the three digits are the version, with a label after them:
# 0.1.0 -> CDJ3Kv010_mods.UPD. Besides MOD SETTINGS this is the only place the
# version is visible, since nothing is reflashed and the deck's firmware version
# is untouched. A version that does not fit three digits is refused: a mangled
# name may be rejected or read as a different firmware revision.
UPD_NAME=""
if [[ "$REMOVE" == 1 ]]; then
    # The removal image carries no version (it does the same thing whatever is
    # installed), so it gets a fixed, self-describing name.
    UPD_NAME="CDJ3Kv000_remove.UPD"
elif [[ -n "$VERSION" ]]; then
    VDIGITS="$(printf '%s' "$VERSION" | tr -d '.')"
    if [[ ! "$VDIGITS" =~ ^[0-9]{3}$ ]]; then
        echo "build.sh: version '$VERSION' does not fit the deck's three-digit" >&2
        echo "          filename field (got '$VDIGITS'). Use e.g. 0.1.0." >&2
        exit 2
    fi
    UPD_NAME="CDJ3Kv${VDIGITS}_mods.UPD"
fi

mkdir -p "$OUT/out"

if [[ "$DO_BUILD" == 1 ]]; then
    echo "==> building deck binaries (docker, aarch64)  version=$MOD_VERSION build=$MOD_BUILD"
    docker buildx build --platform linux/arm64 --provenance=false \
        --target artifacts \
        --build-arg MOD_VERSION="$MOD_VERSION" \
        --build-arg MOD_BUILD="$MOD_BUILD" \
        --output "type=local,dest=$OUT/out" \
        -f docker/Dockerfile .
    echo "    built: $OUT/out/ep122_shim.so"
    echo "    built: $OUT/out/stemd_client_aarch64"

fi

# Host-side sanity check on the artefact about to be packed, so it also covers
# --no-build (which reuses whatever is in build/out). The authoritative test is
# `make abi-check` in the Docker shim stage, which has a real readelf and asserts
# the glibc 2.17 ceiling and the NEEDED set; this is the last check against
# packing a stale or wrong object.
if [[ "$DO_PACK" == 1 && "$REMOVE" == 0 ]]; then
    if [[ ! -f "$OUT/out/ep122_shim.so" ]]; then
        echo "REFUSING: no shim at $OUT/out/ep122_shim.so - build it first" >&2
        exit 1
    fi
    # Herestring, not a pipe: `grep -q` exits on first match, and the SIGPIPE
    # that follows counts as a failed pipeline under `set -o pipefail`.
    if ! grep -qxF 'libc.so.6' <<<"$(strings -a "$OUT/out/ep122_shim.so")"; then
        echo "REFUSING: shim is not glibc-linked (no libc.so.6) - would brick boot" >&2
        exit 1
    fi
    echo "    ok: shim is glibc-linked (libc.so.6)"
fi

if [[ "$DO_PACK" == 1 ]]; then
    ARGS=(--bins "$OUT/out" --out "$OUT")
    [[ -n "$KEY" ]] && ARGS+=(--key "$KEY")
    [[ "$REMOVE" == 1 ]] && ARGS+=(--remove)
    [[ -n "$UPD_NAME" ]] && ARGS+=(--upd-name "$UPD_NAME")
    updater/pack.sh "${ARGS[@]}"
    echo
    if [[ "$REMOVE" == 1 ]]; then
        echo "==> removal package ready: $OUT/${UPD_NAME:-CDJ3Kv000.UPD}"
        echo "    flash it like any update; it clears the mods and reboots to stock."
        echo "    (the deck's front-panel service mode does the same, no USB needed.)"
    else
        echo "==> package ready: $OUT/${UPD_NAME:-CDJ3Kv000.UPD}"
        echo "    put it on a USB stick and run the deck's firmware update."
    fi
else
    echo "==> skipped packaging (--no-pack); binaries in $OUT/out"
fi
