# Stems

Drums, harmonics and vocals on three faders on the play screen. The separation
runs on a computer on your network; the deck stores the result and mixes it
live.

## What you need

A computer on the same network running
[stemd](https://github.com/nsaintot/stemd). It does the separation; the deck
does the playback. The deck itself does not have enough computing power to
separate tracks.

The deck checks the server before sending it anything, and refuses one that is
set up for the wrong audio format: the wrong sample rate, the wrong channel
count, or a model that does not produce the parts the player needs.

## Turning it on

In [MOD SETTINGS](mod-settings.md):

1. **ENABLE STEMS** → ON. Nothing is allocated until you do, so a deck with
   stems off is unaffected.
2. **STEM SERVER LOCATION** → **AUTO** or **MANUAL**.
   - **AUTO** finds the server on the network by itself. Use this unless it
     does not work.
   - **MANUAL** uses an address you type in.
3. **STEM SERVER ADDRESS**: if you chose MANUAL, enter the server's IPv4 address.

## What happens when you load a track

When you load a track, the deck uploads it to the server, which separates it
and sends back the parts. For reference, an eight-minute track took **about 25
seconds** on an M1 Pro with a Balanced setup.

The track plays normally meanwhile, and the faders become active when the
parts arrive.

- **The second deck is free.** Two decks loading the same track share one
  separation; the second joins the job already running.
- **A track you have played before is quicker.** The separation is cached, see
  below.

## Where the stems are kept

Three places, in the order the deck checks them:

| | Where | Survives |
| --- | --- | --- |
| **Your media** | `mods/stemd-cache/` on the stick the track came from | Travels with the stick |
| **The server** | stemd's own results, while it is running | Until the server restarts |
| **The deck** | Memory, for the track that is loaded | Until you load something else |

The first matters most in a booth: play a track once at home and its stems are
on the stick, so it loads with no server on the network at all.

**Stems are stored on the same volume as the track.** They are keyed by the
track's audio content, not by its filename or its position in the browser, so
renaming files, reorganising folders or re-sorting lists keeps the cache valid,
and a stick carries its tracks and their stems together. Stems made with a
different separation model than the one your deck last used are still used, so
a stick prepared on one deck plays on another.

### The frame count, for anyone writing entries offline

Half of an entry's key is the deck's own decode length in frames **at
44.1 kHz**, and the stems inside it must be aligned to that length. It is not
the length a tag or another decoder reports. Take the container's count, then
round it up:

- **MP3:** the deck's parser frame count × 1152. A LAME/Xing tag frame counts as
  a frame of audio (one frame of silence first, the last frame dropped); an
  ffmpeg-written one does not. This is the same number rekordbox stores as the
  analysis's PVBR total.
- **AAC:** packets × 1024, with the priming samples played.
- **Then, every container:** rounded **up** to the next 1/75 s, which is 588
  samples at 44.1 kHz. A 9190-frame MP3 is 10 586 880 samples, and the key is
  18 005 × 588 = 10 586 940.

Write the stems as **96 kHz FLAC**: a pair at the deck's own rate loads in a few seconds,
while a pair the deck has to resample takes about a third of the track's
length.

### Playing from another player's media

When you play a track from a USB stick or SD card in another deck over **PRO DJ LINK**, stems work the same way:
- If the remote stick already has cached stems for that track, they are loaded and used.
- If not, the track is separated and the stems are kept **in memory only** while the track is loaded. Stems are **never** written to the remote stick.

## The STEMS row

Open it with the **STEMS** button in the quick menu at the top of the play
screen. The button appears once stems are enabled, and lights up while the row
is open.

![The play screen with the STEMS button in the quick menu, row closed.](img/stems-button.png)

The row sits under the waveform: three controls, one per part (**DRUMS**,
**HARMONICS**, **VOCALS**), with a small square **BYPASS** icon at the far left.

![The STEMS row open with all three at full.](img/stems-row-unity.png)

While stems are loading, a progress bar replaces the faders and shows the
current step: **UPLOADING**, **SEPARATING**, **PREPARING**, **DOWNLOADING** or
**LOADING**. If the stems are already on the stick, only **LOADING** appears.

| | |
| --- | --- |
| **Drag a fader** | Sets that part's level, 0 to 100% |
| **Hold the name above it** | Mutes that part while held; the label blinks **MUTED** |
| **BYPASS** | Turns the feature off: the track plays exactly as it came |

![DRUMS muted: its caption reads MUTED, its fader greys out, and the waveform above has lost the drum transients.](img/stems-row-muted.png)

### All three at 100% is the untouched track

With every fader at full, the output is the **original mix, bit for bit**: the
mixing is skipped and the track's own audio plays. **BYPASS** does the same in
one press.

So having stems loaded costs nothing if you do not use them.

## The waveform follows the faders

The detailed waveform shows what you **hear**. Pull the drums down and the low
band drops by the share the drums actually contribute, measured from this
track's own parts.

Same track, same point in it, with only the DRUMS fader moved:

![All three faders at full: the drum transients are the tall spikes through the whole track.](img/stems-row-unity.png)
![DRUMS pulled to zero: the spikes are gone and what is left is the harmonics and vocals.](img/stems-row-drums-down.png)

Put the faders back to full and the stock waveform returns exactly.

> Only the **3-band** detailed waveform style follows the faders. The other
> styles do not yet.

## The dot on the STEMS button

A small dot in the corner of the STEMS button means **the track is not playing
as it came**: a fader is below full, or [groove circuit](groove-circuit.md) is
replacing a stem.

![The row closed, with the dot showing in the corner of the STEMS button: something is still off unity underneath.](img/stems-dot.png)

## Turning it off

**ENABLE STEMS** → OFF releases everything the deck is holding and returns the
play screen to normal. You can do this mid-set, with a track loaded.
