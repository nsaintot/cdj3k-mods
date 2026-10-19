# Groove circuit

Put one of your loops on a hot cue pad and play it **in place of a stem**,
locked to the track's beat grid, while the rest of the track keeps playing.

Groove circuit needs [stems](stems.md) working.

## What it is for

The three stem faders take a part out. Groove circuit puts a different one in:
your own kick loop in place of the track's drums, or your own chord stab in
place of its harmonics, with everything else still playing.

## Setting it up

Put your loops on your USB stick or SD card, and list them in a plain text file at:

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
| **audio path** | The file's location: absolute, or relative to the **root of the stick**. |
| **slot** | `1` to `8`, for pads **A** to **H**. |
| **stem** | Which part it replaces: `d` drums, `h` harmonics, `v` vocals. |
| **bpm** | The tempo of your file. Optional, but recommended. See below. |

Lines starting with `#` are comments. Fields are comma-separated.

Each loop can be **up to 30 seconds** long. A longer file is cut to its first 30 seconds.

### Give it a BPM

With a `bpm` on the line, the loop is locked to the track's beat grid: it lands
on the track's downbeats, follows tempo changes within the track, and stays in
place after a seek.

Without one, it is stretched by a fixed ratio, and can drift out of sync on a track
with a dynamic beat grid.

A file whose stated tempo is more than 4× away from the track's is refused.
This is almost always a typo in the config.

## Using it

**Open the STEMS row.** Groove circuit pads only work while the STEMS row is open.

With the row open:

- **Press a pad** to start or stop its loop, which replaces the stem named on its
  line.
- **One at a time.** Only one loop plays at a time.

The tempo fader works normally, and the loop follows it.

### Pad colours

| Colour | Stem it replaces |
| --- | --- |
| Red | Drums |
| Blue | Harmonics |
| Green | Vocals |

A **dim** pad means the slot is loaded and ready. A **blinking** pad means it is
playing.

![The hot cue pads with groove circuit slots loaded and dim: red pads replace drums, blue harmonics, green vocals.](img/gc-pads.png)

![The STEMS row open, with circuit pads firing against the track.](vid/gc-pads.mp4)

With [STEM MUTE PADS](stems.md#mute-from-the-pads) on, pads F, G and H are
stem mutes, so slots 6, 7 and 8 cannot be used for loops.

## What happens to what is playing

- A playing loop stops only when you press its pad again or load another track.
- Closing the **STEMS** row does not stop it. With the row closed, the pads do not
  show a playing loop; the
  [dot on the STEMS button](stems.md#the-dot-on-the-stems-button) does.

## Volume

Loops are not normalised: each plays at the level it was made at. Audio unrelated
to the track can push the output past full scale and clip; use the three stem
faders to bring it back down.
