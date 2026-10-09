# Groove circuit

Put one of your loops on a hot cue pad and play it **in place of a stem**,
locked to the track's beat grid, while the rest of the track keeps playing.

Groove circuit needs [stems](stems.md) working.

## What it is for

The three stem faders take a part out. Groove circuit puts a different one in:
your own kick loop in place of the track's drums, or your own chord stab in
place of its harmonics, with everything else still playing.

## Setting it up

Put your loops on your USB stick or SD card, and list them in a plain text file
at:

```
mods/gc/gc-config.txt
```

on the **root of the stick**. One line per pad:

```
# audio path, slot, stem, bpm
loops/hard-kick.wav,  1, d, 124
Contents/pads/Am.wav, 4, h, 124
```

| Field | Means |
| --- | --- |
| **audio path** | Where the file is. Relative paths start from the **root of the stick**, not from `mods/gc`, so your loops can stay where you keep them. An absolute path is used as given. |
| **slot** | `1` to `8`, which is pad **A** to **H**. |
| **stem** | Which part it replaces: `d` drums, `h` harmonics, `v` vocals. |
| **bpm** | The tempo of your file. Optional, but recommended. See below. |

Lines starting with `#` are comments. Fields are comma-separated, so paths can
contain spaces.

Notes on the file:

- **First bank only.** These pads belong to the deck, not to a track, so there
  is no second bank.
- **Up to 30 seconds** per loop. A longer file is cut to its first 30 seconds.
- **A pad with no line stays a normal hot cue.** You decide how many pads
  groove circuit uses; the rest are untouched.

### Give it a BPM

With a `bpm` on the line, the loop is locked to the track's beat grid: it lands
on the track's downbeats, follows tempo changes within the track, and stays in
place after a seek.

Without one, it is stretched by a fixed ratio instead. That is exact while the
tempo is constant, but drifts on a track whose grid speeds up or slows down.

A file whose stated tempo is more than 4× away from the track's is refused.
This is almost always a typo in the config.

## Using it

**Open the STEMS row.** Groove circuit only takes over the pads while the stem
control row is open. With the row closed, all eight pads are ordinary hot cues.

With the row open:

- **Press a pad** to start its loop, replacing the stem named on its line.
- **Press it again** to stop it.
- **One at a time.** Starting a slot stops whichever one was playing.

The tempo fader works normally, and the loop follows it.

### Pad colours

| Colour | Stem it replaces |
| --- | --- |
| Red | Drums |
| Blue | Harmonics |
| Green | Vocals |

**Steady** means the slot is loaded and ready. **Blinking** means it is
playing.

![The hot cue pads with groove circuit slots loaded: red pads replace drums, blue harmonics, green vocals. A still cannot show which one is blinking.](img/gc-pads.png)

![The STEMS row open, with circuit pads firing against the track.](vid/gc-pads.mp4)

A pad only lights when pressing it will do something. If a pad with a config
line is unlit, the row is closed, the file did not load, or the track has no
stems yet.

## What happens to what is playing

A replacement stays on until you press its pad again or **load a different
track**. Nothing else stops it:

- **Pausing does not.**
- **Closing the STEMS row does not.** A running replacement keeps running.

With the row closed, the pads do not show a running replacement. The
[dot on the STEMS button](stems.md#the-dot-on-the-stems-button) shows it
instead.

## Level

Your file plays at the level it was made at. Adding audio that is unrelated to
the track can push the output past full scale; use the three stem faders to
bring it back down.
