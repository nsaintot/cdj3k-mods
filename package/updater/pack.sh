#!/usr/bin/env bash
# pack.sh - assemble the deck-flashable .UPD from the built binaries and the
# installer scripts in src/. The filename carries the release version
# (CDJ3KvMODS010.UPD); --upd-name sets it, and the scripts inside are rewritten
# to match, since they locate their media by that exact name.
#
# Packaging needs genisoimage, cryptsetup and losetup, none of which exist on
# macOS, and the image step needs a loop device and a running udevd -- so the
# whole step runs inside a privileged Linux container. Nothing is written
# outside build/.
#
# The deck only accepts a package built with its packaging key. The key is not
# in this repo; supply it with --key. Without it the pipeline still produces a
# structurally valid file, but no deck will accept it.
#
# Usage: pack.sh --key PATH [--bins DIR] [--out DIR] [--image NAME] [--remove]
#                [--upd-name CDJ3KvMODS010.UPD]
#   --remove    build a REMOVAL package (no binaries) into build/uninstall/
#   --upd-name  filename to ship under (default CDJ3Kv000.UPD)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PKG="$(cd "$HERE/.." && pwd)"

KEY=""
BINS="$PKG/build/out"
OUT=""
IMAGE="alpine:3.19"
MODE="install"   # install (shim + agent) | remove (clears the overlay)
UPD_NAME_IN=""   # --upd-name: version-stamped filename, e.g. CDJ3KvMODS010.UPD

while [[ $# -gt 0 ]]; do
    case "$1" in
        --key)    KEY="$2"; shift 2 ;;
        --bins)   BINS="$2"; shift 2 ;;
        --out)    OUT="$2"; shift 2 ;;
        --image)  IMAGE="$2"; shift 2 ;;
        --remove) MODE="remove"; shift ;;
        --upd-name) UPD_NAME_IN="$2"; shift 2 ;;
        *) echo "pack.sh: unknown option: $1" >&2; exit 2 ;;
    esac
done

# Separate directories per mode, so an install and a removal image can coexist
# even when they end up sharing a name.
if [[ -z "$OUT" ]]; then
    [[ "$MODE" == remove ]] && OUT="$PKG/build/uninstall" || OUT="$PKG/build"
fi

# Absolute paths for every -v: Docker treats a single-segment relative path
# (e.g. --out dist) as a named volume, so the image would be written into the
# volume, the host directory would stay empty, and the script would still
# report success.
OUT="$(mkdir -p "$OUT" && cd "$OUT" && pwd)"
[[ -d "$BINS" ]] && BINS="$(cd "$BINS" && pwd)"

SHIM="$BINS/ep122_shim.so"
AGENT="$BINS/stemd_client_aarch64"

# A removal .UPD carries no binaries (it only deletes the overlay), so the
# binary check applies to an install package only.
if [[ "$MODE" == install ]]; then
    for f in "$SHIM" "$AGENT"; do
        if [[ ! -f "$f" ]]; then
            echo "pack.sh: missing binary: $f" >&2
            echo "         build it first:  ./build.sh --no-pack   (or run pack via build.sh)" >&2
            exit 1
        fi
    done
fi

if [[ -z "$KEY" ]]; then
    # Default location: a single key file in updater/, which is git-ignored.
    # Any *.key name is accepted.
    KEY="$PKG/updater/packaging.key"
    if [[ ! -f "$KEY" ]]; then
        for k in "$PKG"/updater/*.key; do
            [[ -f "$k" ]] && { KEY="$k"; break; }
        done
    fi
fi
[[ -f "$KEY" ]] && KEY="$(cd "$(dirname "$KEY")" && pwd)/$(basename "$KEY")"
if [[ ! -f "$KEY" ]]; then
    cat >&2 <<EOF
pack.sh: packaging key not found: $KEY

A package has to be built with the packaging key the deck expects. It is not in
this repository and is git-ignored. Provide it with:

    ./build.sh --key /path/to/packaging.key
    # or, if the binaries are already built:
    updater/pack.sh --key /path/to/packaging.key

For a structural dry run only (the result will NOT flash on a real deck) you can
point --key at any 32-byte file.
EOF
    exit 1
fi

# UPD_DEFAULT is the literal the scripts carry in the tree, so they run and test
# as they are. A release stamps the version into the filename instead
# (CDJ3KvMODS010.UPD), as other projects on this format do; it is the only place
# a DJ can see which build a stick holds, since the deck's firmware version is
# untouched. The scripts find their media by this exact name in /proc/mounts,
# so the filename is substituted into them when they are copied into the ISO.
# They must match this package's filename exactly, not any CDJ3Kv*.UPD on the
# stick, so the name is substituted rather than globbed.
UPD_DEFAULT="CDJ3Kv000.UPD"
UPD_NAME="${UPD_NAME_IN:-$UPD_DEFAULT}"

NOT_MATCHING=$(grep -L "$UPD_DEFAULT" \
    "$PKG"/updater/src/usb_update.sh "$PKG"/updater/src/phase1.sh \
    "$PKG"/updater/src/phase2.sh    "$PKG"/updater/src/uninstall.sh || true)
if [[ -n "$NOT_MATCHING" ]]; then
    echo "pack.sh: these scripts do not search for $UPD_DEFAULT:" >&2
    printf '  %s\n' $NOT_MATCHING >&2
    echo "         the deck would not find the update media - refusing to pack" >&2
    exit 1
fi

mkdir -p "$OUT"

echo "==> packing $MODE $UPD_NAME (privileged $IMAGE container)"
if [[ "$MODE" == install ]]; then
    echo "    shim : $SHIM"
    echo "    agent: $AGENT"
fi
echo "    key  : $KEY"

# Everything heavy happens on the container's own filesystem (loop devices do
# not work over an OrbStack bind mount); only the finished file is copied to /out.
# The binary mounts are only needed by an install package.
MOUNTS=(-v "$PKG/updater/src:/src:ro" -v "$KEY:/key:ro" -v "$OUT:/out")
if [[ "$MODE" == install ]]; then
    MOUNTS+=(-v "$SHIM:/in/ep122_shim.so:ro" -v "$AGENT:/in/stemd_client:ro")
fi

docker run --rm -i --privileged -e "MODE=$MODE" -e "UPD_NAME=$UPD_NAME" -e "UPD_DEFAULT=$UPD_DEFAULT" "${MOUNTS[@]}" \
    "$IMAGE" sh -eu <<'RECIPE'
set -o pipefail 2>/dev/null || true
apk add --no-cache cdrkit cryptsetup util-linux eudev >/dev/null 2>&1

# cryptsetup's LUKS1 reencrypt opens a temporary dm device and waits on udev to
# create its node; with no udevd the wait fails as "Cannot open temporary LUKS
# device". Start eudev's udevd first.
/sbin/udevd --daemon >/dev/null 2>&1 || true
udevadm trigger >/dev/null 2>&1 || true

WORK="$(mktemp -d)"
cleanup() { [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null || true; }
trap cleanup EXIT

# --- stage the ISO tree. The deck's updater always runs usb_update.sh; the two
# packages differ in what that entry does and what rides along with it.
mkdir -p "$WORK/stage"
if [ "$MODE" = remove ]; then
    # Removal: the entry is the uninstaller. No binaries, no phases, no payload.
    cp /src/uninstall.sh "$WORK/stage/usb_update.sh"
    chmod 0755 "$WORK/stage/usb_update.sh"
else
    # Install: the installer entry + the two-phase chain + the runtime script +
    # the mod binaries. Listed explicitly so uninstall.sh never rides along.
    cp /src/usb_update.sh /src/phase1.sh /src/phase2.sh /src/payload.sh "$WORK/stage/"
    mkdir -p "$WORK/stage/mods"
    cp /in/ep122_shim.so "$WORK/stage/mods/ep122_shim.so"
    cp /in/stemd_client  "$WORK/stage/mods/stemd_client"
    chmod 0755 "$WORK/stage/mods/stemd_client" "$WORK"/stage/*.sh
fi

# Point the staged scripts at the filename this package ships under, then
# verify the rewrite landed.
if [ "$UPD_NAME" != "$UPD_DEFAULT" ]; then
    for f in "$WORK"/stage/*.sh; do
        sed -i "s/$UPD_DEFAULT/$UPD_NAME/g" "$f"
    done
    if grep -l "$UPD_DEFAULT" "$WORK"/stage/*.sh 2>/dev/null; then
        echo "FATAL: a staged script still refers to $UPD_DEFAULT" >&2; exit 1
    fi
    grep -q "$UPD_NAME" "$WORK"/stage/usb_update.sh \
        || { echo "FATAL: staged entry does not search for $UPD_NAME" >&2; exit 1; }
fi

cd "$WORK"

# 1) inner ISO
( cd stage && genisoimage -quiet -R -J -input-charset utf-8 -o "$WORK/CDJ3K-RK3399.iso" . )
# 2) outer ISO: same tree at root + inner ISO grafted under images/
( cd stage && genisoimage -quiet -R -J -input-charset utf-8 --graft-points \
    -o "$WORK/$UPD_NAME" "images/CDJ3K-RK3399.iso=$WORK/CDJ3K-RK3399.iso" . )

# 3+4) build the container explicitly: `cryptsetup reencrypt` shifts the
# payload by the whole --reduce-device-size and lands it at the wrong offset.
# Reserve the header room, luksFormat at the wanted offset, then write the
# payload through the mapper so it lands there.
ISO_BYTES="$(wc -c < "$UPD_NAME")"
mv "$UPD_NAME" payload.iso
dd if=/dev/zero of="$UPD_NAME" bs=512 count=4096 status=none
cat payload.iso >> "$UPD_NAME"

LOOP="$(losetup -f)"
losetup "$LOOP" "$UPD_NAME"
cryptsetup luksFormat --batch-mode --type luks1 --cipher aes-xts-plain64 \
    --key-size 512 --hash sha256 --offset 4096 --key-file /key "$LOOP"
cryptsetup luksOpen --key-file /key "$LOOP" cdj_firmware
dd if=payload.iso of=/dev/mapper/cdj_firmware bs=1M status=none
sync
cryptsetup luksClose cdj_firmware
losetup -d "$LOOP"; LOOP=

# Check the payload landed at the requested offset.
OFFSET="$(cryptsetup luksDump "$UPD_NAME" | awk "/Payload offset/ {print \$3}")"
[ "$OFFSET" = "4096" ] || { echo "FATAL: payload offset is $OFFSET, expected 4096" >&2; exit 1; }
rm -f payload.iso

# 5) CRC-32 of the image, before the trailer is appended. A gzip stream ends in
#    the IEEE CRC-32 of its input (the same value zlib.crc32 gives), little-
#    endian, so take gzip's first 4 trailer bytes and skip a CRC tool entirely.
#    Written to a file, not a shell var: the 4 bytes are binary and may hold NUL.
gzip -1 -c "$UPD_NAME" | tail -c 8 | head -c 4 > crc.le
# The pipeline's exit status comes from `head`, which succeeds even if gzip
# died halfway (full disk, dm error). A 0-byte crc.le would give an 11-byte
# trailer instead of 15 and an image the deck rejects, with the script still
# reporting success.
[ "$(wc -c < crc.le)" -eq 4 ] || { echo "FATAL: CRC is $(wc -c < crc.le) bytes, expected 4" >&2; exit 1; }
# 6) trailer, then those 4 CRC bytes
printf 'XDJ-RR0.00\000' >> "$UPD_NAME"
cat crc.le >> "$UPD_NAME"

# Final structural checks before anything leaves the container: the image must
# start with the LUKS magic and end with the 11-byte trailer + 4 CRC bytes.
[ "$(dd if="$UPD_NAME" bs=1 count=4 2>/dev/null)" = "LUKS" ] \
    || { echo "FATAL: image does not start with the LUKS magic" >&2; exit 1; }
[ "$(tail -c 15 "$UPD_NAME" | head -c 11)" = "XDJ-RR0.00" ] \
    || { echo "FATAL: trailer missing or misplaced" >&2; exit 1; }

cp "$UPD_NAME" "/out/$UPD_NAME"
SZ="$(wc -c < "/out/$UPD_NAME")"
echo "    wrote /out/$UPD_NAME  ($SZ bytes, $MODE)"
RECIPE

echo "==> done ($MODE): $OUT/$UPD_NAME"
