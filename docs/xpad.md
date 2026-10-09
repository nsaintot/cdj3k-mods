# X-PAD

A sampler on a touch strip under the waveform. Moving your finger across the
strip picks a loop length, and moving it up and down bends the pitch. The hot
cue pads play eight samples from your stick.

Off by default. Turn **ENABLE X-PAD** on in [MOD SETTINGS](mod-settings.md), and
an **X-PAD** button appears in the row above the waveform, next to STEMS.

![ENABLE X-PAD, switched on in MOD SETTINGS.](img/mod-settings-xpad.png)

## The panel is the mode

Opening X-PAD takes over the eight hot cue pads and three buttons. Closing it
stops the sound and gives them all back.

| Control | Normally | While X-PAD is open |
| --- | --- | --- |
| The eight pads | Hot cues | The sample banks, A to H |
| MEMORY | Stores a memory cue | **HOLD** |
| CALL / DELETE | Deletes a cue | **OVERDUB** |
| VINYL SPEED ADJUST | Sets the brake | **VOL** |

With the panel closed, every control does what its label says.

## The strip

Six bricks, left to right, each a loop length in beats.

![The strip: six bricks from 1/16 to 2 beats, with the readout, VOL, OVERDUB and HOLD to the right.](img/xpad-strip.png)

**Across** picks the length. **Up and down** bends the pitch, ±12 semitones.
The readout beside the strip shows both the length and the bend.

![Opening X-PAD, then moving across the strip and up and down it: the readout follows both the loop length and the bend.](vid/xpad-strip.mp4)

## HOLD and OVERDUB

**HOLD** keeps the sound playing when you lift your finger. Without it, lifting
your finger stops it.

**OVERDUB** arms a four-beat sequencer. It records *which pad fired and where in
the bar*, not the audio, and replays by re-triggering the sample. There is no
gain stacking and no feedback.

Timing follows the track's beat grid, so a hit recorded at beat 2.7 replays at
every beat 2.7 for as long as OVERDUB is on, without drifting. Turning OVERDUB
off clears it.

## The samples

Put audio in **`mods/loops/`** at the root of your stick. The first eight files,
sorted by name, become pads A to H.

```
mods/loops/01-kick.wav
mods/loops/02-clap.wav
...
```

There is no config file. `.wav` and `.flac` are read; dotfiles and rekordbox's
`.asd` files are skipped. A file the deck cannot decode is skipped and the
pad stays empty.

The [starter pack](https://github.com/nsaintot/cdj3k-mods/releases/download/extras-files/cdj3k-mods-starter.zip) has eight drums already numbered for the pads — a 909 kit
on A–D and an 808 kit on E–H — with a 707 and a 606 kit in `extras/`.

Unlike [groove circuit](groove-circuit.md), no BPM is needed. A one-shot plays
at its own length, so there is nothing to lock to the grid.

Only the **first** bank is used, as with groove circuit. These samples belong
to the deck, not to a track.

## When a pad does nothing

- **No `mods/loops/` on the stick**, or fewer than eight files in it.
- **The file did not decode.** Try a plain `.wav`.
- **The panel is closed**, in which case the pad is an ordinary hot cue.
