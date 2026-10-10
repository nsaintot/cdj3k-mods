#!/usr/bin/env bash
# loader/pack.sh - cdj3k-mods as a mod for a mod loader such as cdj3k-emu: a
# folder and a .tgz of it.
#
#   loader/pack.sh              package build/loader/out/ (build.sh --no-pack --out build/loader)
#   loader/pack.sh --from DIR   package the binaries in DIR
#
# Writes build/loader/cdj3k-mods/ (in cdj3k-emu: add it as a folder used in
# place, or boot it with --mod) and build/loader/cdj3k-mods.tgz (in cdj3k-emu:
# Add mod… or Add from URL…).
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
FROM="$HERE/build/loader/out"
[[ "${1-}" == "--from" ]] && FROM="${2:?--from needs a directory}"

OUT="$HERE/build/loader"
MOD="$OUT/cdj3k-mods"
rm -rf "${MOD:?}"
mkdir -p "$MOD"
VERSION="$(git -C "$HERE" describe --tags --always --dirty 2>/dev/null || echo unknown)"
install -m 0755 "$HERE/loader/loader.sh" "$MOD/loader.sh"
sed "s|@VERSION@|${VERSION#v}|" "$HERE/loader/mod.toml" > "$MOD/mod.toml"
chmod 0644 "$MOD/mod.toml"
install -m 0755 "$FROM/ep122_shim.so" "$MOD/ep122_shim.so"
install -m 0755 "$FROM/stemd_client_aarch64" "$MOD/stemd_client"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$OUT" -czf "$OUT/cdj3k-mods.tgz" cdj3k-mods
echo "==> $MOD"
echo "==> $OUT/cdj3k-mods.tgz"
