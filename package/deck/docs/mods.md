# EP122 mods

Feature mods that live inside the shim (`guest/ep122_shim/cdj3k-mods/package/deck/mods/`) and change how
the deck's own application behaves: momentary hot cues, a settings overlay, UI
themes, stem playback, a stem-aware waveform.

They are not part of the emulator. The shim is `LD_PRELOAD`ed into EP122, and the
mods install themselves when the shim finds itself inside an EP122 binary it can
fully resolve. In anything else — a shell helper, an unrecognised firmware — they
do nothing.

Related documents:

| Topic | Document |
| --- | --- |
| Stem pipeline, threading, module layout, mix point | [stem-engine.md](stem-engine.md) |
| 3-band waveform format and injection | [waveform-3band-re.md](waveform-3band-re.md) |
| Audio path the stem mix sits in | [audio-stack.md](audio-stack.md) |

## Naming EP122's internals

EP122 ships stripped. Nothing in the mods names an address: `ep122_syms.spec`
names an RTTI class, a vtable slot or a masked instruction signature, and
`resolve.c` resolves those against whatever binary the deck is running.
`scripts/gen-ep122-syms.py` turns the spec into `ep122_syms.h`, resolving every
entry against the reference build so a bad spec line fails on the build machine.

Three mechanisms:

- **vtables** — walk RTTI for an exact mangled class name. A class name survives a
  relink, a reorder and any amount of inlining.
- **slots** — read the slot. Once the vtable is known there is nothing to guess:
  whatever slot `+0xd0` of `juce::Label` holds *is* its `paint`.
- **signatures** — for free functions. An aarch64 instruction is a fixed 4 bytes,
  so masking the branch and ADRP-pair immediates is exact rather than heuristic.

There are no literal EP122 addresses in the mod sources. Slot *offsets*
(`VT_SLOT_READ 0x10`) are ABI and stay inline; addresses do not.

## Install

One constructor (`common.c`), and nothing that lists the mods. A feature declares
itself with `KIT_MOD` (`kit/mod.h`) next to its own install function:

```c
KIT_MOD(k_mod_cue_gate,
        .name = "cue_gate", .prio = 10, .install = gate_install,
        .what = "momentary gate cue");
```

The descriptors land in one linker section (`ep122_mods`) that the constructor
walks, so a new mod is a new file and nothing else — no central table, and no
file that includes every feature's header. The install function itself is
`static`; the descriptor is its only caller.

`name` tags every slot the mod patches. Install order is (`prio`, `name`)
ascending, sorted at init because link order is the Makefile's wildcard, i.e.
whatever order the filesystem hands back. The mods sit at 10..80 in tens, which
leaves nine numbers between any two of them.

Install is **all or nothing**: if any symbol in the spec fails to resolve, no mod
installs and the refusal names the missing symbols. One symbol used by one
feature takes every feature down — the intended trade, since a partly-hooked deck
on an uncharacterised firmware is not a supportable state. A firmware this
refuses is one to add support for.

There is no per-mod environment switch. What a DJ turns off lives in MOD SETTINGS;
what a developer turns off lives in a build. `EP122_NO_MODS=1` takes the mods out
without taking the shim out.

Logging has five levels and **ships at ERROR**, because a deck in a booth should
say nothing while it works and anything it does say should be worth reading. One
variable moves the whole thing, by name or by digit:

    EP122_MOD_LOGLEVEL=error   (0)  the default — only what is broken
    EP122_MOD_LOGLEVEL=warn    (1)  + a feature that refused to install, a value not saved
    EP122_MOD_LOGLEVEL=info    (2)  + what installed, what a track load decided
    EP122_MOD_LOGLEVEL=debug   (3)  + everything a DJ action produces
    EP122_MOD_LOGLEVEL=trace   (4)  + everything a FRAME produces

Unset or empty leaves ERROR. A value that names no level also leaves ERROR but says
so at ERROR — silence would give someone who typed `verbose` exactly what an unset
variable gives, with no way to tell the two apart. That complaint waits until the
shim knows it is inside the deck: the constructor also runs in `apl_start.sh` and
every shell helper it spawns, and complaining before that gate says it ~180 times.

**The sidecar has its own.** `stemd_client` is a separate PROCESS and reads
`STEMD_LOGLEVEL`, same grammar, same five names, same ERROR default, macros
`SERR` / `SWARN` / `SINFO` / `SDBG`. Two variables rather than one so the sidecar
can be turned up while the deck stays quiet, which is what a misbehaving separation
actually calls for. Where you set it depends on how the deck was built: the QEMU
rootfs runs it from `stemd-client.service`, so it goes in that unit and comes out in
the journal; an updater-installed deck has no unit for it — `payload.sh` backgrounds
the binary directly, so the variable goes on that launch line and the output lands in
`/tmp/stemd_client.log`. Both parse through `shared/loglevel.h`, so a level means the
same thing in both and cannot drift; `tests/test_loglevel.c` is the contract.

What the sidecar's level buys is larger than it looks. The deck re-HELLOs every
30 s to refresh its status line, and answering each one prints the discovery detail
whether or not anything changed — about 2,900 lines a day on a deck with no stem
server on the network, which is most of them. So steady state is DEBUG, and what gets
reported is the **transition**, once, rather than every half minute: losing a server
is a WARN, gaining one is an INFO — so a deck sitting at the default hears about the
loss and not the recovery, which is the right way round for something you only look
at when STEMS stopped working. Measured on the deck, five minutes at WARN with no
server is exactly one line.

The macros are `MERR` / `MWARN` / `MINFO` / `MDBG` / `MTRACE`, and `MLOG_AT(level)`
guards work that only exists to be logged, so a census or a dump costs nothing at
the levels that would not print it. ERROR and WARN carry their level in the text
because they are the two that reach a deck nobody is debugging; the rest keep the
bare `[ep122_mod]` prefix every existing grep already matches.

Trace is a level of its own rather than more debug because volume is not verbosity.
Per-draw tracing alone measured 17k lines in half an hour of ordinary use, against a
few dozen that anyone reads; a grid decision arrives once and has to be findable
next to it.

Every mod's install returns 0 (in) or -1 (deck runs stock for that feature), and
the constructor prints one line naming each, with `-` in front of the ones that
skipped.

### Patch journal

Every successful slot patch is recorded against the mod installing it, so an
uninstall needs no second copy of that mod's vtable-and-offset list. A patch that
cannot be journaled is refused rather than made. The install walk uses this to
unwind a mod that gets part-way in and refuses.

There is no process-wide uninstall: the only caller could be a destructor, which
does not run when the process is signalled — and EP122 is stopped with SIGTERM.
Restoring slots in an address space about to be unmapped is unobservable anyway.

## Threads

The mods run on EP122's threads. Every declaration callable from more than one
place is tagged with the thread it expects; a hook inherits the thread of whatever
calls it.

| Tag | Thread | Must not |
| --- | --- | --- |
| `[message]` | JUCE's UI thread — `menu/`, `stem/ui/` | block: it is also the only repaint tick |
| `[audio]` | the realtime callback | allocate, lock, log, or touch juce |
| `[worker]` | our detached threads (stem job, waveform analysis) | touch juce |
| `[filler]` | EP122's page-filler / track-load threads | be stalled — the loader blocks on it |
| `[deck]` | EP122's per-deck task thread (pad `AsyncTask`) | |
| `[init]` | the constructor | — nothing else exists yet, so nothing locks |

`juce::Value::getValue` allocates twice, which is why the audio side reads plain
floats through the accessors in `stem/stem.h` rather than touching juce.

## Naming, for symbols that leave a translation unit

Shared symbols land in one flat namespace with every other mod in the `.so`. Two
directories both calling their captured view `g_view` link cleanly until both are
built.

```
functions   <feature>_<name>       menu_row(), stems_wedge()
variables   <feature>_g_<name>     menu_g_view, stems_g_view
```

Two spellings because a directory usually has both a `stems_wedge()` and a
`g_wedge`. A symbol used in one file stays `static` and needs no prefix —
`-Wmissing-prototypes` is enabled for `mods/` only and fails the build on a
function that is neither static nor declared in a header.

Bare `g_*` is reserved for the cross-feature contracts declared in feature
headers: `g_stems_on`, `g_theme_id`, `g_gate_on`, the stem level array.

## Settings

One fixed binary record, `/home/root/settings/CDJ3K_MODSETTINGS.DAT`, on the eMMC
settings partition — the only writable mount that survives a reboot, alongside the
device's own `CDJ3K_*.DAT` files. Not on inserted media: EP122 starts before
anything is mounted, and removing the stick mid-set would change the UI.

There is no parser. Reading is `sizeof(struct)` plus four header checks (magic,
size, version, CRC-32); writing is the same bytes back. Any check failing leaves
every compiled default in place — a record that does not validate as a whole says
nothing about its individual fields.

Changing the layout:

- A new setting takes bytes from `reserved`, **from the front**. `size` does not
  change, so older builds still read and validate the record and ignore the new
  field. The four flag bytes before `theme_id` are full; that block is not a
  place to grow into.
- A byte taken from `reserved` reads **0** out of every record written before it
  existed, so 0 has to mean whatever the setting's default is. A default-ON
  setting therefore stores its INVERSE — `preview_off` is the first of those.
- Moving or resizing an existing field bumps `version`; older builds then reject
  the record rather than misread it.

Themes are stored by index, so the order of `k_mod_themes` is part of the on-disk
format: append themes, never insert or reorder.

## Layout

Every directory is a feature or a layer under one; nothing sits loose at the
top. An include is written root-relative to the tree — `"kit/mod.h"` reads the
same from anywhere in it.

```
core/     the runtime every mod sits on: safe memory access, slot patching, the
          patch journal, logging, symbol resolution, the persisted settings
          record, and cdj3k_mods.h — the interface a host shim links against
juce/     the toolkit: constructing and calling JUCE objects, walking the
          component tree, juce::Label as a flat button, juce::Graphics and the
          deck's own painted surfaces. call.hh and juce.hh are the C++ half
kit/      what a feature is built out of, knowing no feature: the mod
          descriptor, settings rows, the message popup
browse/   list reorder, sort, drag
cue/      hot-cue interception and the behaviours on it: the pad layer, gate
          cue, smart cue, preview hot cue, the GATE CUE shortcut
db/       the library back end — see djdb-schema.md
grid/     the beat-grid panel
lamp/     what a pad should look like, and whose it is
menu/     the MOD SETTINGS overlay on the DJ SETTING list
stem/     stem playback, and stem/ui/ its panel — see stem-engine.md
theme/    the theme model, palette transform, sprite recolour
wave/     the waveform following the stem faders — see waveform-3band-re.md
xpad/     the X-PAD strip and its sampler
docs/     prose and host harnesses. Excluded from the build: the harnesses
          #include a mod's own .c to measure the shipped code
```

### Themes

A theme is **data, not code**: numbers describing what to do to a colour. One
generic transform (`theme/palette.c`) reads them, and every pixel the UI produces
goes through it — vector fills and text via `setFill`, sprites and the waveform via
`drawImage`. Adding a theme is a row in `theme/presets.c` and nothing else.

ORIGINAL is not special-cased: it is the theme with no palette, which every hook
reads as "chain straight through", so the stock look is bit-identical rather than
close, and the sprite bookkeeping never wakes up.

`struct theme_ui` in `draw.h` holds the roles the mods' own controls paint with —
in the drawing kit, since every mod that owns a paint slot asks for one, and
`theme/roles.c` is what answers. ORIGINAL's set reproduces the deck's measured
colours, so anything that moves on screen under ORIGINAL is a bug.

### Cues

Everything the deck does with a hot cue goes through one interception layer,
`cue/`. `cue/pad.c` owns *every* slot involved — the pad's own press and release,
PLAY, the CUE button, and the preview needle — decodes each into a `cue_event`,
and hands it round. No behaviour holds an address or repeats the ABI, and nothing
else in the tree touches those slots.

A behaviour is a `struct cue_handler` in the `ep122_cue` section next to the code
that implements it, ordered by `(prio, name)` exactly like `KIT_MOD`. Adding one
is a new file. That is what makes per-slot work — a colour rule, a blink, a pad
that means something else — an addition rather than another edit to a function
three features already share.

Three phases, each with a real user: `CUE_PAD_DOWN` before the deck's own press
run (arm something), `CUE_PAD_PRESSED` after it with the status known (act, or
correct what the deck just did), `CUE_PAD_UP` after the release. Plus
`play_while_held`, which may CONSUME a PLAY press.

A behaviour may also **take the pad outright**. `pad_claim` is asked after DOWN
and before the deck's press run; returning non-zero means the deck never runs, so
the pad does not jump, does not play, and does not set a cue on an empty slot —
the press means whatever the claimer says, for as long as it is held. A claimed
pad is the claimer's alone: nobody else gets its PRESSED or UP, because the deck
press they would have been reacting to did not happen and gate cue must not
back-cue a jump that was never made.

The converse is what keeps a claimer honest, and it is enforced in `cue_dispatch`
rather than left to each handler: declaring `pad_claim` means `pad` is called for
DOWN on every pad and for the later phases **only on the pads it actually took**.
Without that rule a claimer's own PRESSED runs for pads it *declined*, on
whatever state its last claim left behind — measured, before the rule existed, as
an empty pad being set by the deck and simultaneously starting a loop over the
previous pad's span.

Claim only what can be honoured. A press taken and not acted on is a dead pad,
which is worse than the stock behaviour it replaced, so a behaviour needing stems
or a second cue checks first and declines — and the fallback is then the deck's
own press, not silence.

**Link before you read.** Two `meow::MappedObjPtr`s stand between the pad closure
and the cue table — the handler's `IUsecaseDeck` at `+0x30` and the
CueController's `ICueLoopSetter` at `+0x18` — and both start EMPTY, caching their
pointer from an object-id map on first use. Reading either slot raw found nothing
until the deck's own press path had run once, so the **first pad press of a
process fell through every behaviour that needs the controller**: no claim, no
cue table, stock press instead. `cue/pad.c` now calls each one's `link()` first,
the same way the deck's own back-cue and `setPoint` do. Both are idempotent — an
already-cached pointer is a load and a branch.

Neither link function can be named by signature: they are template instantiations
with a stock prologue, matching hundreds of functions, more than either scan will
hold. They are `call` entries instead — the BL at a known instruction of a known
caller — which is the same argument as the `data` kind's ADRP pair: read the
address the deck's own code computes rather than one we recognise.

The event carries the pad two ways — the deck's 0-based index and the `CueKind`
that is that plus one — the press status, and the preview needle: whether the
zone is held and where, as a normalised fraction. The deck multiplies that by the
source length and snaps it to the beat grid itself, so a behaviour wanting "where
the strip was touched, on the grid" passes the fraction on and does no arithmetic.

| behaviour | file | what |
| --- | --- | --- |
| **GATE CUE** (on) | `cue/gate.c` | momentary pads: press jumps and plays, release back-cues and pauses, PLAY during the hold latches |
| **SMART CUE** (off) | `cue/smart.c` | the memory cue follows the pad you last pressed, so CUE returns to it |
| **PREVIEW HOTCUE** (on) | `cue/preview.c` | hold the preview zone, press an unassigned pad, and the cue lands under the needle instead of at the play head — on the press, and nothing else fires |
| **GROOVE CIRCUIT** | `stem/gc.c` | with the STEMS row open, a hot cue named in a config file toggles that file in place of one stem, looped in time with the track while the rest of it plays on; one slot at a time |

**A release already carries what it means.** Once the deck's own release pops the
last held pad it hands `&closure[0x28]` to the handler's release method, which
reads the pad index at `+0` and dispatches on an op word at `+4`: `1` returns to
the cue and pauses, anything else is the plain release. So a behaviour wanting a
back-cue declares `release_op` and pad.c writes that word before the stock run —
the deck's own task does the work, in the deck's own order. Performing the
operation *alongside* the release instead meant two invocations of it and skipped
the teardown the deck does afterwards.

Gate cue does not back-cue a press that **created** a cue. Setting a hot cue and
recalling one are different instructions: an empty pad puts a cue at the play
head without jumping, so there is nowhere to return from and returning would be a
move the DJ never asked for. `ev->assigned` on PRESSED is the only place that is
known, so gate records it per pad.

**A short hold used to lose its back-cue**, and nothing in this layer decided it:
instrumented, the deck's state at release is identical for a 30 ms and a 400 ms
hold — priority matched, one pad on the handler's stack — so its release reaches
the cue operation and names the same hot cue either way. The difference is below
that, where the press's own jump-and-play is still settling and lands last.

So `cue_pad_tick()` asks again, at 120, 280, 560 and 1000 ms off the display
clock. Returning to a hot cue is **idempotent** — a deck already parked there
parks there again — which is why this can be a plain repeat rather than a
condition: there is nothing to test, and a request already honoured costs a seek
to where the play head is. On a spread because the settle is not a fixed number
and the tail is long: 400 ms holds returned 3/3 and 200 ms 0/3, and two shots
were still not enough for a press of a few milliseconds. Any new pad press and
any PLAY press cancel it — both say the deck is wanted somewhere other than where
the last release left it.

Measured after: 30/60/100/200/400 ms three runs each, 15 of 15; then 1/5/10/20/30
ms five runs each, **21 of 21 of the presses the deck actually saw**. The other
four never reached the layer at all — no `cue: pad N down`, because a pulse of a
few milliseconds can fall between the panel's button scans and there is no press
to gate. Worth knowing when a very short synthetic tap looks like a failure: the
log says which of the two it was.

**Preview hot cue** is its own gesture, not a stock press with a correction on
it. The press is CLAIMED, so nothing else sees it: the pad does not jump, gate
cue does not back-cue on the release, and smart cue does not follow. Assigning a
cue and recalling one are different instructions, and a press meaning the first
must carry none of the second.

What runs instead is the deck's own press, re-issued by the claimer through
`cue_stock_press()` — which looks circular and is not. The claim is what stops
the other behaviours; the deck's press is what makes a cue that is a cue.

The needle's position is already a cue record and already on the grid:
`sub_10d0788`, which turns the normalised touch into a position and snaps it,
stores the result through `sub_10d67d8(table, 9)` — the same per-kind accessor
the hot cues use, at a kind past the eight of them and past the memory cue's 0.
**CueKind 9 is the preview needle**, and every slot embeds its
`pcmbuf::PositionWithSourceInfo` at `+0x28`, so arming is a 0x30-byte snapshot
and the beat-grid byte beside it. A snapshot rather than a pointer: the finger is
still on the strip while the write is in flight, and slot 9 moves under it.

Only the POSITION is intercepted. However a cue is set it ends at one setter
(`sub_10dd0d8`), and the engine has two ways in — `setHere(engine, kind,
quantize, desc)` at the play head and `setAt(engine, kind, pos, onGrid, desc)` at
a given one. So the redirect is one inline hook on `setHere`: when a kind is
armed, call `setAt` with the needle's position instead and forward the caller's
own `desc`.

`desc` **is the colour** — three bytes `sub_10de100` copies into the slot at
`+0x10..+0x12`, and each caller has its own. That is the reason for hooking the
position and nothing else. Setting the cue through `setPoint` instead of the
pad's own press was tried, and carried `setPoint`'s default `255,113,0`: the pad
lit orange against the `0,127,0` of every cue the deck made itself.

Writing the slot by hand is also not enough, and the old copy-on-release was
missing most of it. `sub_10dd0d8` takes a reference on the source, sets the
exists byte at `+0x20` and the on-grid byte at `+0x21`, bumps the version at
`+0x08`, writes the state at `+0x0c`, and resets the second position block at
`+0x70`; the facade above it then tells the lamp and the waveform marker.

Smart cue rides on the press, after the deck has dispatched the jump: `setPoint`
reads the play position when its task runs on the player, behind the jump posted
to that same player, so it already sees the hot cue. No sleep, and measured exact
with the gate OFF where the pad plays on.

**Groove circuit** is the one behaviour that takes the pad rather than following
it. Eight slots, named in a config file on the DJ's own stick:

```
# audio path, slot, stem, bpm
loops/hard-kick.wav,  1, d, 124
Contents/pads/Am.wav, 4, h, 124
```

Slot 1..8 is pad A..H and the letter is the stem that goes — `d` drums, `h`
harmonics, `v` vocals. A pad no entry names is a hot cue exactly as it always
was, so the DJ decides which pads the circuit owns.

**Gated on the STEMS row**, which is the whole of the mode. Eight slots would
otherwise take all eight hot cues away for as long as the stick is in, and a hot
cue is not something to spend without asking. Row closed, every pad is stock — no
claim, no colour, nothing of ours in the way; open it and the pads are the
circuit's. `gc_slot_part()` carries the gate, so the press and the lamp cannot
disagree about it.

**The gate is on the pads, not on the circuit.** Closing the row leaves a running
replacement running, exactly as it leaves the stem levels where the DJ put them:
the row is where the controls live, not what the feature is. A replacement is
therefore not visible on the pads while the row is closed — which is the right
way round, because the pad will not toggle it there either, and a lamp lit for
something the pad will not do is worse than no lamp.

**So the title bar says it instead.** A 9 px dot in the free corner of the STEMS
button, level with the `!` badge in the opposite corner so the two read as a
pair, whenever this track is not coming out as it went in — a level off unity or
a stem being replaced, either one.

**One mark, one colour.** The question a glance asks is a yes or a no, and a mark
that changed colour between the two causes would be asking the DJ to learn a code
for a distinction the row shows them anyway. Blue, because on this deck blue
means on, selected, working — except on the lit plate, where the accent IS the
button and a mark in it would disappear; there it goes white, the same swap the
warn badge makes and the colour the lettering beside it is already wearing.
BYPASS earns no mark: it is the whole feature out of circuit, which is the track
as it came.

The dot is `•` U+2022, 13 px, and getting it there took two findings worth
keeping.

**Label text is bytes, one per codepoint.** `juce::String(const char*)` is
`CharPointer_ASCII` on this build, so a `"•"` literal arrives as the three
Latin-1 characters its UTF-8 encoding is made of — which is what put two stray
marks in this corner. There is no `String::fromUTF8` in the spec, so
`juce_string_utf8` goes round it: a juce::String *stores* UTF-8, so it builds one
from as many ASCII characters as the literal has BYTES and then overwrites those
bytes with the literal's own. The allocation does not move and juce::String
caches no length, so the same buffer simply decodes as what was meant. It checks
the buffer holds exactly the placeholder it asked for before writing, and keeps
the placeholder if not.

**Choose the glyph against the font the deck RESOLVES.** U+00B7 MIDDLE DOT is a
circle in the UCGothic faces and a **square** in TsukuGo, FZLTZH and GB18030 —
and this deck draws the square, which is what a hard-edged 12 px block in the
corner turned out to be. Rendering the candidates out of `/usr/share/fonts/ttf/`
with fontTools is how that got settled; guessing coverage from a codepoint table
would not have caught it, because every one of those fonts *has* U+00B7.

Both the size and the position are measured, not derived: the mark is about a
quarter of the em, the bounds do not constrain it (an 80 pt line in a 48 px box
still drew its full 19 px), and it lands at the box centre — which is *not* true
of U+00B7, whose low position on the line put it on top of the lettering when its
box was centred. `BADGE_AT_X`/`BADGE_AT_Y` name where the mark goes and the box
is derived from that, so swapping the glyph is one line.

A relative path is relative
to the volume root, not to `mods/gc`: the audio is theirs and belongs where they
keep it. The first bank only, because these belong to the deck rather than to a
track and there is no track to break a tie with if two sticks disagreed.

**A file, not a region of the track.** A region can only be a stem the deck
already has, which rules out the drums — the residual `mix - harmonics - vocals`
exists at the play head and nowhere else — and rules out anything the track does
not contain.

The mix generalises to `d·D' + gh·H' + gv·V'` where exactly one term comes from
the file:

| replacing | output |
| --- | --- |
| nothing | `d·(M − H − V) + gh·H + gv·V`, collapsing to `d·M + (gh−d)·H + (gv−d)·V` |
| drums | `d·F + gh·H + gv·V` |
| harmonics | `d·(M − H − V) + gh·F + gv·V` |
| vocals | `d·(M − H − V) + gh·H + gv·F` |

The residual is written out longhand in the last two because the collapsed form
folds the drums level into the stem terms, and those are exactly what is being
replaced.

**The phase is derived, not carried.** The file's index comes from how far the
track has run since the loop was engaged, wrapped over its length, so there is
nothing to drift: a block boundary cannot slip it and seeking lands the loop
where the track did. That arithmetic is `stem/loop.c`, which is pure and has its
own tests (`tests/test_stem_loop.c`).

**In time and in place.** Two numbers make that work, and both come from the
track's beat grid via `stem/grid.c`.

*Tempo* — the config's `bpm` gives the file's beat length and the track's grid
gives the track's, both linear-interpolated per frame. The tempo fader needs no
term of its own: the pool position already advances with it, so the loop follows.

Which is worth being precise about, because following the fader looks like
following tempo and is not the same thing. **The phase is a function of track
position, never of real time.** The fader changes how fast the timeline is
consumed, so it is followed exactly and for free. A VARIABLE-TEMPO GRID changes
something else — how many samples a beat occupies at different points *within*
that timeline — which no single ratio can express.

So there are two routes, and the mix picks one per block:

- **By beat index**, when the track's beat array is in hand. `stem_beat_at` gives
  the fractional beat at a position, the file index is
  `(beats_now − beats_engaged) × file_spb` wrapped over the loop, and that is
  exact for any grid: a track that speeds up, slows down, or was gridded by hand
  a bar at a time. The loop lands on beat 129 when the track does, however the
  tempo got there.
- **By ratio**, when it is not — no readable grid, or a config line with no
  stated `bpm`. `file_spb / track_spb` against elapsed frames, which is exactly
  right while the tempo holds still. A ratio outside ¼…4× is refused as a wrong
  config line rather than played at forty times speed.

The array is COPIED, never pointed at, and copied at the moment the deck's own
beat-grid reply lands — the one point where the object is certainly alive. It
reaches the mix by an atomic exchange: the reply leaves one behind, the next pad
press takes it, and whichever thread ends up holding one nobody took frees it. It
is keyed on the same TrackID that keys the tempo, so the previous track's beats
are dropped rather than used.

The per-frame cost went DOWN. Each block bisects the array once and then walks a
compare at a time (`stem_beat_at`'s optional cursor), where the ratio route
divides on every frame. The cursor is a hint that is checked and never believed:
a seek, a reordered block or an uninitialised one all cost the bisection they
were meant to save and none of them can change the answer — `tests/test_stem_loop.c`
holds every cursor state to what `NULL` gives.

*Alignment* — `engage` is the grid's first downbeat, **not** the moment the pad
went down. Anchoring at the play head gives a loop that is in time and a bar
turned by however late the press was; anchoring at the grid reduces the phase to
how many beats into the track we are, so the loop's downbeat lands on the
track's and every press, re-press and takeover drops into the same alignment. The
cost is that a press does not restart the loop from its first frame, which is the
right way round for a part standing in for a stem.

**The loop is whole beats, not the buffer.** A decoder pads, and looping over
what it returned puts that padding at the end of every bar. With a stated BPM the
span is rounded to the nearest whole number of beats, which is always shorter
than the buffer and so needs no bounds test in the mix. Measured against
rekordbox's own factory pack, whose eight samples come out at 4.0015, 4.0014,
4.0014, 16.0000, 32.0000, 32.0000, 32.0000 and 15.9983 beats — the excess is
between 0 and 31 frames, and rounding removes exactly it.

**Reading the grid.** The chain starts at a cue slot, because that is where the
deck keeps a `PositionWithSourceInfo` and its last field is the page source. The
block being played is no use: `read()` takes a *range* — two bare
`pcmbuf::Position`, `from` and an unbounded `to` — and a bare Position carries a
source ID, not the source.

| | |
| --- | --- |
| `pwsi + 0x28` | the page source, a `meow::RefCountedObjEx` |
| `source + 0x68` | the grid holder |
| `holder + 0x28` | `appnd_trk_info::BeatGrid::Content` |
| `holder + 0x30` | origin, added to every beat position |
| `holder + 0x40` | the rate those positions are counted at |
| `content + 0x28` | the beats, 16 bytes each: position, then number in the bar |
| `content + 0x30` | how many |

The deck's own quantiser walks the same links (`sub_10dfac8`, from `setAt`). Any
cue will do — they all name the same source — so `stem_grid_take` takes the first
slot that reads, and a track with no cue at all leaves the tempo unknown. The
first and last beats and the count give the average beat length, which for a
fixed-tempo track is exactly its beat length; the whole array would let a loop
follow a tempo change at the cost of copying it, versioning it and searching it on
the audio thread. Verified against the deck's own display: a track showing 125.0
reads 125.0.

Nothing in the walk is trusted. Every step goes through `mod_safe_read`, the
source is checked for the `'XOCR'` signature the app asserts on at `+0x10`, and
the result is refused unless the rate is positive, the beats are ordered and the
tempo lands between 20 and 400 BPM. The position's own constant tag is
deliberately *not* checked — it differs between a bare Position (`0x01e10c08`)
and the longer form, and a constant read off one build is a thing to get wrong
quietly.

One replacement at a time across the whole feature, and exactly one thing takes
it off: a track change — the phase is measured against that track's timeline and
the replacement against that track's stems. Pausing does not, because the DJ
pausing has thrown no switch and a slot that comes off under the finger has to be
re-armed before the next press of PLAY.

The play head is not a stand-in for a play state. `stem_source_pos()` is the
stretcher's last read position, so it stands still whenever the audio thread
does; deciding "stopped" from it turned every stall — and a resample is most of a
stem load — into a slot switching itself off.

The pads are coloured by the stem each slot takes away — red drums, blue
harmonics, green vocals — static while a slot is only loaded and blinking while
it is the one replacing. `gc_slot_part()` is one predicate asked by both the
press and the lamp, so a pad is never lit for something it will not do: it
answers −1 with the row closed, without a file, and without stems resident, since
every replacement is expressed against the track's harmonics and vocals.

**The lamp index is brightness, and slot 2 is the lit one.** A lamp holds ONE
`(index, colour)` pair — `sub_1917e00` keeps the index at `+0xc0` and the colour
at `+0xc1`, then runs the pair through a transform at `+0xb0` whose result is
what the panel gets — so the index is not a layer that composites, it selects how
the colour is rendered. Measured on 3.19: the app writes slot 2 for a pad holding
a cue and slot 1 for one that does not, and the same 255 comes out as `0x7f`
through slot 2 against `0x0c` through slot 1. Passing the app's index through
therefore gave a groove-circuit pad the right hue at a twentieth of the
brightness, on exactly the pads that hold no cue — which is most of them. A pad
we have taken is written on slot 2 whatever slot the app asked for, and the app's
own write is kept whole, colour *and* index, because putting a pad back is
replaying it verbatim.


The peak counter is what proves the mix ran: replacing a stem at unity levels
measured 1523/1000 against 0 at rest, because with no replacement and unity
levels the mix bypasses and never touches a sample. Substituting uncorrelated
audio can exceed full scale — the file is the DJ's own, at whatever level they
made it, and the stem faders are what brings it down.


The CueKind and QuantizeSetUnit the behaviours pass are read off a real CUE press
— the button's own `cueing(kind=0, slip=0, quantize=0)` — and `pad.c` keeps
saying so: a firmware where that slot names different values prints that the cue
behaviours are wrong on this build, on the first CUE press.

**GATE CUE also has a play-screen shortcut** (`cue/shortcut.c`), in the gap
between the timer and the tempo: white on the accent while armed, an outline like
A.HOT CUE and MT while not. The two thumbs on one flag stay in step — the
shortcut persists what it toggles, and the MOD SETTINGS row repaints it through
its `changed`.

The rack is `gui::NormalPlayerInfoWidget`, and nothing about it is reachable by
name: it and the `gui::PlayerInfo*Widget` classes are `UpdaterComponent` models
whose primary vtable is nine slots of listener, with the `juce::Component` each of
them also IS at a secondary vtable. `base=` cannot name that one either, because
`juce::Component` is a VIRTUAL base there and its RTTI offset is not its layout
offset. So they are found by **typeinfo**, which every vtable in a class's group
carries and which therefore identifies a subobject pointer whichever vtable it
points at. The gap is measured off the live timer and tempo rather than written
down, so a firmware that moves either one moves the button with it.

### Lamps

`cue/led.c` decides what colour the panel's coloured lamps are, **on the app**.
The frame they end up in leaves through an ioctl and hooking that would be
easier, but `mods/` is being separated from the emulation shim it currently
lives in and a syscall hook does not survive that.

`hui::HuiIndicatorAbs` is a lamp. It keeps an UpdateBehavior at `+0x10` and its
own update just tail-calls that behaviour's — `SingleColor` for a lamp that is
only on or off, `MultiColor` for one with a colour, which is what the hot cue
pads are. MultiColor's update asks `*(this+8)` through vtable `+0x18` for a
packed u64 and hands the result to the ONE function every lamp in the app writes
through (exactly two callers, the two behaviours' updates):

```
packed   byte 0     an index
         bytes 4-6  R, G, B
```

So one wrapper on that slot sees every coloured lamp, needing nothing from the
indicator that owns it. It REPLACES the stock body rather than following it: the
write is a change-notifier, so a second one carrying a different colour reads
downstream as the lamp changing twice. Any read that fails falls back to the
stock call.

Two things measured rather than assumed. **The index is scoped to the holder** —
it is a colour slot within a lamp, and the holder is the lamp. Keyed on the index
alone the survey showed two lamps alternating between two colours each, which is
several holders each numbering their own slots from 1. And the holders are one
array, stride `0x470`, with the hot cue pads eight consecutive entries starting
at the fourth: a track whose cues were A, D and F turned entries 3, 6 and 8
green, the pad spacing exactly.

The stride is a finding, not a mechanism: **every write is captured whatever its
index, so the app hands over all eight lamp addresses itself within a second of
startup** — `led: pad A lamp is 0x1802db90` through `pad H … 0x1802faa0`. Nothing
derives an address it has not seen written.

That array position is a heap address and must not be relied on. The stable
identity is the **ordinal**: `sub_18001f0` builds all 35 deck indicators in one
loop, `0..0x22`, constructing the kind as `{vtable, deck, ordinal}` and pushing
each into a vector in that order — so pad `p` is ordinal `p+3`. The catch is that
`HuiIndicatorAbs` does not RETAIN the kind; its ctor uses it for the registry
lookup that picks the behaviour, stores the behaviour, and drops it. So the
ordinal cannot be read back at update time, and closing that gap means either
inline hooking of the ctor (a capability this tree does not have — only vtable
slot patching) or synthesising the kind per pad and calling the app's own
`sub_18001c8` / `sub_17ff860` to get a colour source to compare against.

The survey that measured all this is not in the tree any more. It logged every
lamp write that changed a colour, which even change-only ran to thousands of
lines a minute — several lamps genuinely do alternate, that being what a blinking
lamp is on this panel. What it established is above; keeping a switch in the shim
for a measurement already made was not worth the shipped code.

### Menu

The overlay is reached by tapping the "Ver.X.XX" label and collapses the stock DJ
SETTING list in place rather than opening a screen of its own. Twelve vtable slots
across one list model, one view and one right-pane model; all or nothing, since a
subset is a DJ SETTING screen that half-answers to a mod.

The rows are not the overlay's. A feature declares its own (`kit/menu.h`) and hands
them over from its install, so adding a mod with a setting touches nothing in
`menu/` — which in turn names no feature. A row is a CHOICE (a state index into a
value list; OFF/ON is just `kit_off_on`) or TEXT, and a row with a `parent` appears
only while `*parent->state == show_when`, so a subtree disappears with its root. The
visible list is those rows flattened in `idx` order with children under their parent,
rebuilt on every query — nothing computes an offset, and enabling STEMS moves the
rows below it down rather than renumbering anything.

Both ways that can go wrong are a developer's, since the mods are statically linked:
two siblings claiming one `idx` disables every row and the Ver-tap says so on the
glass instead of arming, and more live rows than the list can show (nine when the
overlay could grow it to ten rows, seven when it could not; `KIT_MENU_MAX_ROWS` and
`kit_menu_set_shown`) drops the surplus with a log line naming the first one. Neither
is silent.

The right pane is the deck's, borrowed. What the mod supplies is the answer to how
many values a row has and which is checked — so most of `menu/pane.c` overrides a
count or a label and hands the drawing back.

Text rows use the deck's software keyboard but **not** its editor: typing into the
view's own editor would rewrite the stock HISTORY NAME setting, so the mod owns the
keyboard's listener slot and commits into its own buffer.

## Build

`mods.mk` carries the sources, the flags and the gates. Both builds that link
the mods include it — this package's, and cdj3k-emu's guest build — so a new
file reaches both and the purity, test and ABI manifests cannot drift apart.

Sources are discovered by wildcard four levels deep, so adding a file or a whole
feature directory needs no Makefile change.

A `.cc` file builds as C++ in a minimal profile: constructors, destructors,
references and templates, with `-fno-exceptions`, `-fno-rtti`,
`-fno-threadsafe-statics` and no STL. Nothing there needs libstdc++, so the
object's `NEEDED` list is the same either way — `make abi-check` fails the build
if that stops holding.

It must be built against glibc, not musl: a musl-linked `.so` records
`NEEDED: libc.so`, the deck has no musl, and glibc's `ld.so` then refuses it — into
`/bin/sh` as well, so `apl_start.sh` dies with status 127 and the deck comes up with
no app. `make docker` builds it in the `ubuntu:18.04` stage. The Makefile guards
against the musl compiler rather than trusting a comment.

Mods are compiled `-fvisibility=hidden`: every exported symbol would interpose
that name in EP122 and every library it loads. `-Wmissing-prototypes`
(`-Wmissing-declarations` in C++) states the same rule in the source — a mod
function that is neither static nor header-declared fails the build.

`make abi-check` asserts the three properties that decide whether the object
will load on the deck: the glibc version ceiling of 2.17, `NEEDED` naming
`libc.so.6` rather than musl's `libc.so`, and nothing in `NEEDED` the deck
lacks. Failing any of them takes down every process `apl_start.sh` starts.
