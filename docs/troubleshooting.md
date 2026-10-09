# Troubleshooting

Common symptoms and what to check, in order.

## The deck will not start the update

**"Connect USB storage device into top USB port", with the stick already in.**
The deck reads the first FAT32 or exFAT volume it finds on the stick and stops
there if the update is not on it. Two things cause this:

- **The stick was formatted with a GUID Partition Map**, which is the Mac
  default. The deck finds the small hidden EFI partition first, sees no
  update, and gives up. Reformat with a **Master Boot Record** partition map and
  a single FAT32 partition.
- **The update is in a folder.** It has to be at the top level of the stick.

**It stops with an error instead.** There is more than one `.UPD` on the stick.
Leave exactly one.

**Nothing happens at all.** The SD slot is not read for updates. Use the top USB
port.

## The update did not finish

**Two restarts are normal.** The deck restarts itself partway through the
install and continues on its own.

**Leave the stick in until it has finished.** Each step writes a log to the
stick you flashed from, so pulling it early loses the logs. Afterwards, put it
in a computer and look for:

| | |
| --- | --- |
| `install.log` | the update itself |
| `phase1.log`, `phase2.log` | the two restarts that follow |

## MOD SETTINGS is not there

**First check the version at the top right of the UTILITY screen.** If it does
not end in `-m`, the mods did not install, and tapping it does nothing.

On a firmware the mods do not fully recognise, nothing installs. There is no
state where some features work and the settings screen is missing. See
[getting started](getting-started.md).

If it *does* end in `-m` and tapping still does nothing, you are not on the
**DJ SETTING** screen. The label only works there.

## A hot cue pad does nothing

Check in this order:

1. **Is the STEMS row open?** If it is, and the pad has a
   [groove circuit](groove-circuit.md) line, the pad belongs to groove circuit,
   not to your cues. Close the row and the pad is a hot cue again.
2. **Is the pad lit?** An unlit groove circuit pad cannot play: no file
   loaded, or the track has no stems yet.

## Releasing a pad does not return to the cue

**GATE CUE is off.** Turn it on in [MOD SETTINGS](mod-settings.md), or with the
shortcut on the play screen.

An **empty** pad does not return. Pressing one sets a cue at the play head, so
there is nothing to return to. See [hot cues](hot-cues.md).

## The preview hot cue lands at the play head

The preview zone was not held when you pressed the pad. Hold it first, keep
holding, then press.

It only works on **unassigned** pads. A pad that already has a cue recalls that
cue, with or without the preview zone held.

## The stem faders never become live

Check in order:

1. **ENABLE STEMS** is ON in [MOD SETTINGS](mod-settings.md).
2. The **stem server is reachable** from the deck: on the same network, and
   running.
3. If **STEM SERVER LOCATION** is AUTO and the server is not found, switch to
   **MANUAL** and type the address.
4. The server produces what the player needs.

Allow time on the first play of a track: about 25 seconds for an eight-minute
track on the test setup. The track plays normally while it waits.

## A groove circuit pad is not lit

Any of these leaves a pad unlit:

- The **STEMS row is closed**. Groove circuit only uses the pads while it is
  open.
- **No stems for this track yet.** Each replacement needs the track's
  separated parts.
- The **file did not load**: wrong path, or a format the deck cannot decode.
- **No line for that pad** in `mods/gc/gc-config.txt`, in which case it is an
  ordinary hot cue.

## A groove circuit loop drifts out of time

Its config line has **no `bpm`**, so it is stretched by a fixed ratio instead
of locked to the beat grid. That is exact while the tempo is constant, but
drifts on a track whose grid changes. Add your file's tempo to the line.

If the loop is refused, the stated `bpm` is more than 4× away from the
track's, which is almost always a typo.

## A reordered playlist did not stick

Reordering only works in a playlist, and EDIT only appears on playlist views.
See [browsing](browsing.md).

## The waveform does not follow the faders

You are on a waveform style other than the **3-band** detailed waveform. The
others do not follow the faders yet.

## Something looks wrong under a theme

Switch **THEME** to **ORIGINAL**, which draws exactly what the deck would draw.
If the problem is still there, the theme is not the cause.

## Putting the deck back to stock

**Without uninstalling:** turn every row in [MOD SETTINGS](mod-settings.md) off
and set **THEME** to **ORIGINAL**. A feature that is off leaves the deck
running its own code, so the deck behaves as stock and you can switch features
back on later.

**Fully:** see [removing](getting-started.md#removing). Power off, hold
**CUE/LOOP CALL ◀** and **TEMPO ±6/±10/±16/WIDE** while powering on, then reboot.
The deck starts in service mode and removes the mods as it starts. It needs
only the front panel, not the stock firmware.
