# package

Builds the CDJ-3000 mods into a **`.UPD` update file** that you flash with the
deck's normal firmware-update procedure. No firmware is replaced and the rootfs
is never written — the mods live in the deck's update overlay, and one step
removes them.

```
deck/       deck-side sources: the mods, the STEMS sidecar, tests.
            mods.mk is the shared source list, included by this build and by
            any other that links the mods
docker/     the aarch64 build
updater/    the .UPD builder and its install/runtime/removal scripts
build.sh    build the binaries and pack a .UPD
release.sh  build a versioned release
devpush.sh  push a new build to an already-modded deck over SSH
```

## What it installs

| File            | Where              | What                                      |
| --------------- | ------------------ | ----------------------------------------- |
| `ep122_shim.so` | `/home/root/mods/` | `LD_PRELOAD`ed into the EP122 application |
| `stemd_client`  | `/home/root/mods/` | the STEMS network sidecar                 |

The preload is scoped to the application line of the deck's own launcher, so
`EP122TestMode` and the launcher's helper tools stay stock. No systemd units, no
writes to the read-only rootfs, no reflash.

## Install, update, remove

1. Copy the `.UPD` to a USB stick, FAT32 or exFAT, and make sure it is the
   **only** `CDJ3Kv….UPD` on it. Use the top USB port; the SD slot is ignored.
2. Run the deck's normal firmware-update procedure. It reboots itself twice.
3. UTILITY shows an `-m` suffix on the version when the mods are in.

The deck mounts the first FAT32/exFAT volume it can — the bare device, then
partitions 1 to 5 — and gives up on the stick if the `.UPD` is not on that one.
A stick formatted on macOS with a **GUID partition map** therefore fails: its EFI
partition is found first and holds no `.UPD`, and the deck stays on "Connect USB
storage device" until you unplug. Format as **MBR with a single FAT32
partition**.

**Updating** is flashing the newer `.UPD`; there is no need to remove first.
The overlay is rebuilt from scratch each time, so nothing from an older version
remains.

**Removing**: the deck's front-panel service-mode key combo, or flash the removal
`.UPD` from `uninstall/`. Either way the deck returns to stock.

## Optional root SSH

Off by default. Put an `authorized_keys` file next to the `.UPD` on the stick and
the installer turns SSH on with that key.

The key comes from the stick every time: flashing without one turns SSH back off,
and removing the mods clears it.

## Logs

Each step writes a log to the USB stick, readable on a computer afterwards:
`install.log`, `phase1.log`, `phase2.log`, `uninstall.log`.

If the stick was pulled between reboots they go to `cdj3k-mods-logs/` on the
deck's overlay partition instead, alongside `runtime.log` — the last boot's
status, and the place to look if the mods stop loading.

## Build

Needs Docker and the packaging key. Everything runs in containers; no aarch64
toolchain on the host.

```bash
./build.sh --key /path/to/packaging.key   # -> build/CDJ3Kv000.UPD
./build.sh --no-pack                      # the two binaries only
./build.sh --version 0.2.0 --key KEY      # stamp a version into the shim
./build.sh --remove --key KEY             # a removal .UPD
```

A package has to be built with the packaging key the deck expects. That key is a
secret and is not in this repository.

## Releasing

```bash
./release.sh --version 0.1.0 --key /path/to/packaging.key
```

Writes `build/release/v0.1.0/`:

```
CDJ3Kv010_mods.UPD              publish this
uninstall/CDJ3Kv000_remove.UPD  the removal package
SHA256SUMS
MANIFEST.txt
```

The deck requires the `CDJ3Kv` prefix followed by exactly three digits, so `0.1.0`
becomes `CDJ3Kv010_mods.UPD`. The version also shows in MOD SETTINGS; nothing else
on the deck reports it, since no firmware is replaced.

## Pushing a build to a modded deck

`devpush.sh` replaces the two binaries in a modded deck's overlay over SSH, with
no firmware update.

```bash
./devpush.sh                    # build and push
./devpush.sh --no-build         # push what is in build/dev/out/
./devpush.sh --from DIR         # push the binaries in DIR
./devpush.sh --version 0.1.0    # stamp a version into the shim
./devpush.sh --host other-deck  # default is cdj-real
```

**Reboot the deck afterwards** — `/home/root` is rebuilt from the overlay at
boot, so nothing changes until then.

```bash
ssh cdj-real systemctl reboot
```

The first install still needs the `.UPD`; `devpush.sh` refuses a deck that is
not already modded. There is no rollback, so push a build you have already run:
a shim that crashes EP122 leaves the deck in a reboot loop, and the only fix is
flashing the `.UPD`.

## Hardware variants

|                         | Rockchip         | Renesas                    |
| ----------------------- | ---------------- | -------------------------- |
| overlay partition       | `/dev/mmcblk1p8` | `/dev/mmcblk0p5`           |
| end of an install phase | reboots itself   | needs a manual power cycle |

## Tests

```bash
updater/test/dryrun.sh  # the whole install/runtime/removal lifecycle, in a container
make -C deck abi-check  # the shim can load on the deck's glibc
make -C deck check      # host unit tests
make -C deck purity     # the pure sources pull in nothing else
```

Run `dryrun.sh` before flashing any change to the updater scripts.

## Licence

MIT OR Apache-2.0.
