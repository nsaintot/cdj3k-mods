# Stems

Drums, harmonics and vocals on three faders on the play screen. The separation
runs on a computer on your network; the deck holds the result and mixes it live.

## What you need

A machine on the same network running
[stemd](https://github.com/nsaintot/stemd). It does the separation; the deck
does the playing. Nothing is separated on the deck itself (the unit does not have enough computing power).

The deck checks the server before sending it anything, and refuses one that is
set up for a different shape of audio: the wrong sample rate, the wrong
channel count, or a model that does not produce the parts the player needs.

## Turning it on

In [MOD SETTINGS](mod-settings.md):

1. **ENABLE STEMS** → ON. Nothing is allocated until you do, so a deck that
   never opts in is completely unaffected.
2. **STEM SERVER LOCATION** → **AUTO** or **MANUAL**.
   - **AUTO** finds the server on the network by itself. Use this unless it
     does not work.
   - **MANUAL** takes an address you type in.
3. **STEM SERVER ADDRESS**, fill in the ipv4 address of the server only if you chose MANUAL.

## What happens when you load a track

When you load a track, the deck uploads it to the server, which processes it and
returns the separated parts. This starts at the load, whether the track is
playing or sits paused on its cue. For reference, on an M1 Pro with a Balanced
setup, an eight-minute track typically took **about 25 seconds** to process.
The track plays normally throughout, and the faders become live when the parts
arrive.

- **The second deck is free.** Two decks loading the same track cost one
  separation, the second attaches to the job already running.
- **A track you have played before is quicker.** The separation is cached, see
  below.

## Where the stems are kept

Three places, in the order the deck looks:

| | Where | Survives |
| --- | --- | --- |
| **Your media** | `mods/stemd-cache/` on the stick the track came from | Travels with the stick |
| **The server** | stemd's own results, while it is running | Until the server restarts |
| **The deck** | Memory, for the track that is loaded | Until you load something else |

The first is the one that matters in a booth: play a track once at home and its
stems are on the stick, so it loads with no server on the network at all.

**Stems live on the same volume as the track.** They are keyed by the track's
own contents, not by its filename or its position in the browser, so renaming a
file, reorganising folders or re-sorting the list all keep the cache valid, and
pulling a stick takes its tracks and their stems away together. A pair made by
a different separation model than the one your deck last used is used too, so
a stick prepared on one deck plays on another.

### The frame count, for anyone writing entries offline

Half of an entry's key is the deck's own decode length in frames **at
44.1 kHz**, and the stems inside it must be aligned to that length. It is not
the length a tag or another decoder reports. Take the container's count, then
round it up:

- **MP3:** Pioneer's parser frame count × 1152. A LAME/Xing tag frame counts as
  a frame of audio (one frame of silence first, the last frame dropped); an
  ffmpeg-written one does not. This is the same number rekordbox stores as the
  analysis's PVBR total.
- **AAC:** packets × 1024, with the priming samples played.
- **Then, every container:** rounded **up** to the next 1/75 s, which is 588
  samples at 44.1 kHz. A 9190-frame MP3 is 10 586 880 samples, and the key is
  18 005 × 588 = 10 586 940.

Worked out on a CDJ-3000 from 16 loads and verified sample-for-sample against 4
uploads; the MP3 rule checks against the PVBR totals on an exported stick. Write
the stems as **96 kHz FLAC**: a pair at the deck's own rate loads in a few
seconds, where a pair the deck has to resample takes about a third of the
track's length.

### Playing from another player's media

When playing a track from a USB stick or SD card inserted in another deck over **PRO DJ LINK**, stems are handled the same way:  
- If the remote stick already has cached stems for that track, they are loaded and used seamlessly.
- If not, the track will be separated and the stems will be cached **temporarily in memory** for as long as the track is loaded. Stems are **never** written back to the remote stick.

This means you get stems with the lowest possible delay if they're already present on the media, or automatically processed on the fly if not, but remote media are never modified.

## The STEMS row

Open it from the **STEMS** button in the quick menu at the top of the play
screen. The button appears there once stems are enabled, and lights up while the
row is open.

![The play screen with the STEMS button in the quick menu, row closed.](img/stems-button.png)

The row sits under the waveform: three controls, one per part (**DRUMS**,
**HARMONICS**, **VOCALS**), with a small square **BYPASS** icon at the far left
of the row.

![The STEMS row open with all three at full.](img/stems-row-unity.png)

While the parts are on their way, the row shows a progress bar in place of the
faders, captioned with the step in hand: **UPLOADING**, **SEPARATING**,
**PREPARING**, **DOWNLOADING**, then **LOADING**. Separating is the long one.
PREPARING holds still while the server writes the parts, since it reports no
count for that step. A track whose stems are already on the stick skips the
server and only shows LOADING.

| | |
| --- | --- |
| **Drag a fader** | Sets that part's level, 0 to 100% |
| **Hold the name above it** | Mutes that part while held; the label blinks **MUTED** |
| **BYPASS** | The whole feature out of circuit, the track exactly as it came |

![DRUMS muted: its caption reads MUTED, its fader greys out, and the waveform above has lost the drum transients.](img/stems-row-muted.png)

### All three at 100% is the untouched track

With every fader at full, the output is the **original mix, bit for bit**. Not "very close". The mixing is skipped entirely and the track's own
audio comes out. **BYPASS** gets you the same thing in one press.

So there is no cost to arriving at a track with stems loaded and not using
them.

## The waveform follows the faders

The detailed waveform shows what you **hear**. Pull the drums down and the low
band drops by the share the drums were actually contributing, measured from
this track's own parts, not by guessing that "low equals drums".

Same track, same point in it, with only the DRUMS fader moved:

![All three faders at full: the drum transients are the tall spikes through the whole track.](img/stems-row-unity.png)
![DRUMS pulled to zero: the spikes are gone and what is left is the harmonics and vocals.](img/stems-row-drums-down.png)

Put the faders back to full and the stock waveform comes back exactly.

> Confirmed on the **3-band** detailed waveform style. The other styles do not
> follow the faders yet.

## The dot on the STEMS button

A small dot in the corner of the STEMS button means **this track is not coming
out as it went in**: a fader off full, or a stem being replaced by
[groove circuit](groove-circuit.md).

![The row closed, with the dot showing in the corner of the STEMS button: something is still off unity underneath.](img/stems-dot.png)

## Turning it off

**ENABLE STEMS** → OFF releases everything the deck is holding and returns the
play screen to normal. You can do it mid-set, with a track loaded.
