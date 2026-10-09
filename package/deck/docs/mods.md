# EP122 mods — developer cheatsheet

Feature mods in `package/deck/mods/`, built into `ep122_shim.so` and
`LD_PRELOAD`ed into EP122 (the deck's application). They install only inside an
EP122 binary the shim can fully resolve; anywhere else they do nothing.

## Layout

Includes are root-relative (`"kit/mod.h"`). `docs/` and `tools/` sit beside
`mods/`, outside the source wildcards.

| Dir | Contents |
| --- | --- |
| `core/` | safe memory access, slot patching, patch journal, logging, symbol resolution, settings record, `cdj3k_mods.h` (host-shim interface) |
| `juce/` | constructing/calling JUCE objects, component-tree walks, Label-as-button, `Graphics`; `call.hh`/`juce.hh` are the C++ half |
| `kit/` | feature building blocks: mod descriptor, settings rows, message popup |
| `browse/` | playlist reorder, sort, drag |
| `cue/` | hot-cue interception layer and the behaviours on it |
| `db/` | library back end (see `djdb-schema.md`) |
| `grid/` | beat-grid panel |
| `lamp/` | pad lamp appearance and ownership |
| `menu/` | MOD SETTINGS overlay on the DJ SETTING list |
| `stem/`, `stem/ui/` | stem playback, groove circuit, STEMS panel |
| `theme/` | theme model, palette transform, sprite recolour |
| `wave/` | waveform following the stem faders |
| `xpad/` | X-PAD strip and sampler |

## Symbols

EP122 is stripped. Mod sources never name an address.

- `core/ep122_syms.spec` describes each symbol; `tools/gen-syms.py` resolves it
  against the stock binary at build time and emits `core/ep122_syms.h`;
  `core/resolve.c` re-resolves it on the deck at startup.
- **vtable**: exact mangled RTTI class name (`base=` for a secondary vtable).
- **slot**: offset into a known vtable (`VT_SLOT_READ 0x10`); slot offsets are
  ABI and stay inline.
- **signature**: masked aarch64 instruction window for free functions.
- **call**: the BL at a known instruction of a known caller, for functions whose
  prologue matches too much (template instantiations).
- **typeinfo**: for classes whose `juce::Component` is a virtual base, so
  neither the primary vtable nor `base=` reaches it.

## Adding a mod

```c
KIT_MOD(k_mod_cue_gate,
        .name = "cue_gate", .prio = 10, .install = gate_install,
        .what = "momentary gate cue");
```

- Descriptors land in the `ep122_mods` section; `mods_init` (`core/common.c`)
  walks it. A new mod is a new file, with no central table.
- Install order is (`prio`, `name`) ascending. Priorities are 3–80.
- `install` is `static`, returns 0 (installed) or -1 (that feature stays stock).
- **All or nothing**: if any spec symbol fails to resolve, no mod installs and
  the log names the missing symbols.
- Every slot patch is journaled against its mod and used to unwind a mod that
  fails part-way. There is no process-wide uninstall (EP122 stops on SIGTERM).
- `EP122_NO_MODS=1` keeps the shim loaded with no mods. There is no per-mod
  env switch: users toggle in MOD SETTINGS, developers in the build.

## Logging

| `EP122_MOD_LOGLEVEL` | Adds |
| --- | --- |
| `error` (0, default) | only what is broken |
| `warn` (1) | refused installs, unsaved values |
| `info` (2) | what installed, track-load decisions |
| `debug` (3) | everything a user action produces |
| `trace` (4) | everything a frame produces (~17k lines / 30 min) |

- Macros: `MERR` / `MWARN` / `MINFO` / `MDBG` / `MTRACE`. Wrap log-only work in
  `MLOG_AT(level)`.
- Unset/empty is ERROR. An unknown name is reported at ERROR, once the shim
  knows it is inside EP122 (the constructor also runs in every shell helper).
- `stemd_client` reads `STEMD_LOGLEVEL` (same names, `SERR`/`SWARN`/`SINFO`/`SDBG`).
  It is set in `stemd-client.service` (journal) or on the `payload.sh` launch
  line (`/tmp/stemd_client.log`). Server loss is WARN, recovery INFO, periodic
  HELLO detail DEBUG.
- Both parse through `shared/loglevel.h`; `tests/test_loglevel.c` is the contract.

## Threads

Every declaration callable from more than one place is tagged; a hook inherits
its caller's thread.

| Tag | Thread | Must not |
| --- | --- | --- |
| `[message]` | JUCE UI thread (`menu/`, `stem/ui/`) | block (it is also the repaint tick) |
| `[audio]` | realtime callback | allocate, lock, log, touch JUCE |
| `[worker]` | our detached threads (stem job, waveform analysis) | touch JUCE |
| `[filler]` | EP122 page-filler / track-load threads | stall (the loader waits on it) |
| `[deck]` | EP122 per-deck task thread (pad `AsyncTask`) | |
| `[init]` | the constructor | (nothing else runs yet) |

`juce::Value::getValue` allocates; the audio side reads plain floats via
`stem/stem.h`.

## Naming

All mods share one flat symbol namespace in the `.so`.

```
functions   <feature>_<name>       menu_row(), stems_wedge()
variables   <feature>_g_<name>     menu_g_view, stems_g_wedge
contracts   g_<name>               g_stems_on, g_theme_id, g_gate_on
```

File-local symbols are `static` with no prefix. `-Wmissing-prototypes` fails
the build on a non-static function with no header declaration.

## Settings

- File: `/home/root/settings/CDJ3K_MODSETTINGS.DAT` (eMMC, survives reboot,
  available before media mounts).
- One fixed binary struct; checks are magic, size, version and CRC-32. On any
  failure all compiled defaults (all off) stay.
- New setting: take bytes from the **front** of `reserved`; `size` is
  unchanged. 0 must mean the default (hence `preview_off` is inverted).
- Moving or resizing a field bumps `version`.
- Themes are stored by index: only append to `k_mod_themes`.

## Build

- `mods.mk` holds sources, flags and gates; every build that links the mods
  includes it. Sources are wildcarded (`mods/*.c` … `mods/*/*/*.c`, same for
  `.cc`).
- `.cc` is minimal C++: `-fno-exceptions -fno-rtti -fno-threadsafe-statics`, no
  STL, no libstdc++.
- `-fvisibility=hidden`: an exported symbol would interpose EP122's.
- Build against **glibc**, not musl (a musl `.so` breaks every process
  `apl_start.sh` starts). Use the `ubuntu:18.04` stage of
  `package/docker/Dockerfile` via `package/build.sh`.
- `make abi-check`: glibc ≤ 2.17, `NEEDED` has `libc.so.6` and nothing the deck
  lacks.
- `make check`: host tests in `tests/`.

## Themes

- A theme is a row of numbers in `theme/presets.c`; one transform
  (`theme/palette.c`) applies it to every pixel: `setFill` for vectors and
  text, `drawImage` for sprites and the waveform.
- ORIGINAL has no palette: every hook chains straight through.
- Mod controls paint with `struct theme_ui` (`juce/draw.h`) from
  `theme/roles.c`. Under ORIGINAL these reproduce the stock colours; any visible
  change there is a bug.

## Cues

`cue/pad.c` owns every cue-related slot (pad press/release, PLAY, CUE, the
preview needle), turns each into a `cue_event` and dispatches it. Nothing else
touches those slots.

**Handlers**: a `struct cue_handler` in the `ep122_cue` section, ordered by
(`prio`, `name`).

| Hook | When |
| --- | --- |
| `CUE_PAD_DOWN` | before the deck's press runs |
| `pad_claim` | after DOWN; non-zero means the deck's press never runs |
| `CUE_PAD_PRESSED` | after the deck's press, status known |
| `CUE_PAD_UP` | after the release |
| `play_while_held` | may consume a PLAY press |
| `release_op` | op word written into the release closure (back-cue) |

- A claimed pad is the claimer's alone: other handlers get no PRESSED/UP for it,
  and the claimer gets PRESSED/UP only for pads it claimed.
- Claim only what you can honour; check prerequisites (stems, a cue) and decline
  otherwise.
- `cue_event`: pad index (0-based), `CueKind` (index + 1), press status,
  `assigned`, preview needle (held, normalised fraction). Pass the fraction on
  unchanged; the deck scales and snaps it.
- **Link before reading**: `cue/pad.c` calls `link()` on the handler's
  `IUsecaseDeck` (`+0x30`) and the CueController's `ICueLoopSetter` (`+0x18`)
  first. Both `MappedObjPtr`s are empty until first linked.
- `pad.c` checks the CueKind/QuantizeSetUnit a CUE press uses and logs if a
  firmware differs.

**Back-cue**: the deck's release passes `&closure[0x28]` (pad index `+0`, op
`+4`; `1` = return and pause). A handler sets `release_op` and the deck's own
release task performs it. Never issue the op separately (it would run twice and
skip teardown).

| Behaviour | File | Notes |
| --- | --- | --- |
| GATE CUE | `cue/gate.c` | paused-only; no back-cue for a press that created a cue (`assigned` on PRESSED, recorded per pad); PLAY during hold latches |
| SMART CUE | `cue/smart.c` | on PRESSED, `setPoint` queues behind the jump, so it already sees the hot cue |
| PREVIEW HOTCUE | `cue/preview.c` | claims the press, re-issues it via `cue_stock_press()` with `setHere` redirected to `setAt` at the needle |
| GROOVE CIRCUIT | `stem/gc.c` | see below |
| GATE CUE shortcut | `cue/shortcut.c` | play-screen button; shares state with the MOD SETTINGS row via its `changed` callback |

**Short holds**: `cue_pad_tick()` repeats the back-cue at 120, 280, 560 and
1000 ms (idempotent). Without it a hold under ~200 ms can lose the back-cue. Any
new pad or PLAY press cancels the repeats. A tap of a few ms may never reach the
layer: look for `cue: pad N down` in the log.

**Preview needle**: CueKind 9. Each cue slot embeds a
`pcmbuf::PositionWithSourceInfo` at `+0x28`; arming copies 0x30 bytes plus the
beat-grid byte (a copy, because slot 9 moves while the finger is down). Only
the position is redirected. `desc` (colour, slot `+0x10..+0x12`) stays the
caller's, since `setPoint` would paint the pad orange (`255,113,0`).

Cue slot fields written by the setter: version `+0x08`, state `+0x0c`, exists
`+0x20`, on-grid `+0x21`, second position block `+0x70`. Writing a slot by hand
skips the source reference and the lamp/marker notifications.

## Groove circuit

Config `mods/gc/gc-config.txt` on the first media root:

```
# audio path, slot, stem, bpm
loops/hard-kick.wav,  1, d, 124
Contents/pads/Am.wav, 4, h, 124
```

- Slot 1–8 = pad A–H; stem `d`/`h`/`v`; BPM optional (0 or omitted). Relative
  paths are from the volume root.
- Pads are the circuit's only while the STEMS row is open. `gc_slot_part()` is the
  single predicate for both press and lamp (−1 if row closed, no file or no
  stems).
- Closing the row does not stop a running replacement. One replacement at a
  time; only a track change stops it (pause does not).
- Never infer play state from `stem_source_pos()`: it stalls with the audio
  thread.

Mix, with `F` = file, `M` = mix, `H`/`V` = harmonics/vocals:

| Replacing | Output |
| --- | --- |
| nothing | `d·(M−H−V) + gh·H + gv·V` |
| drums | `d·F + gh·H + gv·V` |
| harmonics | `d·(M−H−V) + gh·F + gv·V` |
| vocals | `d·(M−H−V) + gh·H + gv·F` |

Phase (`stem/loop.c`, tested by `tests/test_stem_loop.c`):

- Derived from track position, never real time; anchored at the grid's first
  downbeat, so the loop's downbeat matches the track's on every press.
- **By beat index** when the beat array is available:
  `(beats_now − beats_engaged) × file_spb`, wrapped. Exact for variable grids.
  `stem_beat_at` uses a cursor hint that is always checked.
- **By ratio** otherwise: `file_spb / track_spb`; ratios outside ¼–4× are
  refused.
- With a stated BPM the loop is rounded to whole beats (always shorter than the
  buffer), which drops decoder padding.

Beat grid (`stem/grid*.c`), read from any cue slot's `PositionWithSourceInfo`:

| Offset | Field |
| --- | --- |
| `pwsi + 0x28` | page source (`meow::RefCountedObjEx`) |
| `source + 0x68` | grid holder |
| `holder + 0x28` | `appnd_trk_info::BeatGrid::Content` |
| `holder + 0x30` | origin (added to every beat) |
| `holder + 0x40` | position rate |
| `content + 0x28` | beats, 16 B each: int64 position, double BPM |
| `content + 0x30` | count |

- Every read goes through `mod_safe_read`; objects are checked for `'XOCR'` at
  `+0x10`; refused unless rate > 0, beats ordered and BPM in 20–400.
- The array is copied when the deck's grid reply lands and handed to the mix by
  atomic exchange, keyed on TrackID.

## STEMS dot

- A 9 px `•` (U+2022) on the STEMS button whenever the track is not playing as
  loaded (a level off unity or a stem replaced). Blue, white on the lit plate;
  none for BYPASS.
- `juce::String(const char*)` is ASCII here: build UTF-8 labels with
  `juce_string_utf8`.
- Check glyphs against the deck's fonts in `/usr/share/fonts/ttf/`: U+00B7
  renders as a square in the face the deck resolves.
- Position via `BADGE_AT_X`/`BADGE_AT_Y`.

## Lamps

`cue/led.c` writes the lamps; `lamp/lamp.h` decides their look. It works in
the app, without a syscall hook.

- `hui::HuiIndicatorAbs`: UpdateBehavior at `+0x10` (`SingleColor` or
  `MultiColor`). MultiColor queries its colour source (`+0x08`, vtable `+0x18`)
  for a packed u64: byte 0 index, bytes 4–6 RGB.
- One wrapper on the shared lamp-write function sees every coloured lamp. It
  replaces the stock call (a second write would notify twice) and falls back to
  stock on any failed read.
- Lamp setter: index `+0xc0`, colour `+0xc1`, transform `+0xb0`. The index is
  brightness: slot 2 lit (255 → `0x7f`), slot 1 dim (`0x0c`). Groove-circuit
  pads always write slot 2; the app's own write is kept for restoring.
- Pad identity: a source whose vtable is `EP122_IND_DECK_ID` is a deck lamp,
  ordinal at `+0x68`; pads are 8 ordinals from `ORD_HOTCUE_A` (4).

## Menu

- Opened by tapping the "Ver.X.XX" label; collapses the stock DJ SETTING list in
  place. Twelve vtable slots, all or nothing.
- Features declare rows in `kit/menu.h` and register them at install; `menu/`
  names no feature.
- Row types: CHOICE (state index into values; `kit_off_on`) or TEXT. A row with
  `parent` shows only while `*parent->state == show_when`.
- The visible list is rebuilt per query in `idx` order. Duplicate sibling `idx`
  disables all rows (reported on tap). More than `KIT_MENU_MAX_ROWS` (9, or 7;
  see `kit_menu_set_shown`) drops the surplus with a log line.
- The right pane is the deck's; `menu/pane.c` overrides counts and labels.
- TEXT rows use the deck's keyboard but their own buffer (the stock editor would
  overwrite HISTORY NAME).
