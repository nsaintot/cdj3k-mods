# Troubleshooting

What the usual symptoms mean, in the order they are worth checking.

## The deck will not start the update

**"Connect USB storage device into top USB port", with the stick already in.**
The deck reads the first FAT32 or exFAT volume it finds on the stick and stops
there if the update is not on it. Two things cause that:

- **The stick was formatted with a GUID Partition Map**, which is what a Mac
  does by default. The deck finds the small hidden EFI partition first, sees no
  update, and gives up. Reformat with a **Master Boot Record** partition map and
  a single FAT32 partition.
- **The update is in a folder.** It has to sit at the top level of the stick.

**It stops and complains instead.** There is more than one `.UPD` on the stick.
Leave exactly one.

**Nothing happens at all.** The SD slot is not read for updates. Use the top USB
port.

## The update did not finish

**Two restarts are normal.** The deck restarts itself partway through the
install and carries on by itself.

**Leave the stick in until it has settled.** Each step writes a log to the same
stick you flashed from, so a stick pulled early takes the explanation with it.
Afterwards, put it back in a computer and look for:

| | |
| --- | --- |
| `install.log` | the update itself |
| `phase1.log`, `phase2.log` | the two restarts that follow |

## MOD SETTINGS is not there

**Check the version at the top right of the UTILITY screen first.** If it does
not end in `-m`, the mods did not install, and tapping it will do nothing no
matter how many times you try.

On a firmware the mods do not fully recognise, nothing installs, there is no
state where some features work and the settings screen is missing. See
[getting started](getting-started.md).

If it *does* read `-m` and tapping still does nothing, you are on a screen other
than **DJ SETTING**. The label is only a toggle there.

## A hot cue pad does nothing

Check in this order:

1. **Is the STEMS row open?** If it is, and the pad has a
   [groove circuit](groove-circuit.md) line, the pad belongs to the circuit and
   not to your cues. Close the row and the pad is a hot cue again.
2. **Is the pad lit?** An unlit groove circuit pad is one that cannot act: no
   file loaded, or the track has no stems yet.

## Releasing a pad does not return to the cue

**GATE CUE is off.** Turn it on in [MOD SETTINGS](mod-settings.md), or with the
shortcut on the play screen.

**The deck was playing when you pressed.** Only a pad pressed **while paused**
gates. Pressed while playing, it is a normal hot cue: the deck jumps and keeps
playing, and releasing it does nothing. Whether a press gates is decided at
the press, so pausing during a hold does not turn it into one.

An **empty** pad does not return. Pressing one sets a cue at the play head
instead of recalling one, so there is nowhere to return to. See
[hot cues](hot-cues.md).

## The preview hot cue lands at the play head

The preview zone was not held at the moment you pressed the pad. Hold it first,
keep holding, then press.

Note that it only applies to **unassigned** pads. A pad that already has a cue
recalls that cue, preview zone or not.

## The stem faders never become live

In order:

1. **ENABLE STEMS** is ON in [MOD SETTINGS](mod-settings.md).
2. The **stem server is reachable** from the deck: same network, and running.
3. If **STEM SERVER LOCATION** is AUTO and the server is not being found,
   switch to **MANUAL** and type the address.
4. The server produces what the player needs.

Give it time on the first play of a track: around 25 seconds for an
eight-minute track on the setup this was measured on. The track plays normally
while it waits. With the STEMS row open, the bar under the caption shows where
the job is. **PREPARING** standing still is normal: the server is writing the
parts and reports no count for that step, and how long it takes depends on the
server. A **!** on the row means the job failed; load another track, then this one
again, to retry.

## A groove circuit pad is not lit

All of these produce an unlit pad:

- The **STEMS row is closed**. The circuit only owns the pads while it is open.
- **No stems for this track yet.** Every replacement is expressed against the
  track's own parts.
- The **file did not load**: wrong path, or a format the deck cannot decode.
- **No line for that pad** in `mods/gc/gc-config.txt`, in which case it is an
  ordinary hot cue.

## A groove circuit loop drifts out of time

Its config line has **no `bpm`**, so it is being stretched by a fixed ratio
rather than locked to the beat grid. That is exact while the tempo holds still
and drifts on a track whose grid changes. Add the tempo of your file to the
line.

If the loop plays absurdly fast or is refused outright, the stated `bpm` is
more than 4× away from the track's, almost always a typo.

## A reordered playlist did not stick

You were **not in a playlist**. EDIT currently appears on any track list, but
the write is refused anywhere the position is not a playlist's own. See
[browsing](browsing.md).

## The waveform does not follow the faders

You are on a waveform style other than the **3-band** detailed waveform. The
others do not follow the faders yet.

## Something looks wrong under a theme

Switch **THEME** to **ORIGINAL**, which draws exactly what the deck would draw.
If the problem is still there, it is not the theme.

## Putting the deck back to stock

**Without uninstalling:** turn every row in [MOD SETTINGS](mod-settings.md) off
and set **THEME** to **ORIGINAL**. Every feature that is off leaves the deck
running its own code, so this behaves as a stock deck and you can switch things
back on afterwards.

**Properly:** see [removing](getting-started.md#removing), power off, hold
**CUE/LOOP CALL ◀** and **TEMPO ±6/±10/±16/WIDE** while powering on, then reboot.
The deck comes up in its service mode and the mods come off as it starts. It
needs nothing but the front panel, and it does not need the stock firmware.
