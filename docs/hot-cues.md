# Hot cues

Changes to what the eight hot cue pads do. They are independent, and with all
of them off the pads behave exactly as they always did.

## GATE CUE

Ships **OFF**. Makes the pads momentary while the deck is paused.

| | |
| --- | --- |
| Press a pad **while paused** | Jumps to the cue and plays for as long as you hold it |
| **Release the pad** | Returns to the cue and pauses |
| Press **PLAY** while still holding | Latches, and the track keeps playing when you let go |
| Press a pad **while playing** | A normal hot cue: jumps to the cue and keeps playing |

So a short stab is a stab, and a press you decide to keep is a press you keep,
without having to decide before you press. Whether a press gates is decided
when you press it, from whether the deck was paused at that moment; a second
pad pressed during a hold joins the first.

![GATE CUE on: holding a pad plays from the cue, and releasing it returns there and pauses.](vid/gate-cue.mp4)

### Setting a cue is not affected

Pressing an **empty** pad still sets a hot cue at the play head, and releasing
it does nothing. There is nowhere to return from, and jumping somewhere you did
not ask to go would be worse than doing nothing.

### Very short taps

A press of a few milliseconds may not reach the deck at all. The panel scans
the buttons on a cycle, and a pulse can fall between two scans. If a very short
synthetic tap seems to be ignored, that is where it went; anything you can do
with a finger is comfortably long enough.

### The shortcut on the play screen

GATE CUE also has a button on the play screen, in the gap between the time
display and the tempo, so you can arm it mid-set without going into a settings
screen.

It reads **white on the accent colour** while gate cue is on, and as a grey
plate with dim lettering while it is off. The grey is the one the deck's own
STEMS, BEAT LOOP and KEY SHIFT buttons wear, which is how the deck marks a thing
you can press; A.HOT CUE and MT beside it are outlines because they are
indicators, not buttons.

![Gate cue off: the GATE CUE button is a grey plate.](img/gate-shortcut-off.png)
![Gate cue on: the same button filled with the accent colour.](img/gate-shortcut-on.png)

The button and the [MOD SETTINGS](mod-settings.md) row are the same switch, and
they always agree: change one and the other follows.

## SMART CUE

Ships **OFF**. Makes the memory cue follow you.

With it on, the cue point moves to whichever hot cue you last pressed, so
pressing **CUE** takes you back to where you last jumped rather than to where
the cue happened to be set.

It rides on the press itself, so it is exact. There is no delay and no
guessing.

## PREVIEW HOTCUE

Ships **OFF**. Lets you place a cue where you are pointing instead of where the
track is playing.

1. **Hold** the preview zone on the waveform, as you would to preview a part of
   the track.
2. With it still held, **press an unassigned pad.**

The cue lands **under your finger**, snapped to the beat grid by the deck
itself, not at the play head.

![Holding the preview zone and pressing an unassigned pad: the cue is set where the finger is, and the deck does not jump.](vid/preview-cue.mp4)

What separates it from a normal press with a correction on top:

- **The deck does not jump.** The press means "put a cue here" and nothing
  else. The track carries on wherever it was.
- **Nothing else reacts to it.** Gate cue does not back-cue on the release, and
  smart cue does not follow. Assigning a cue and recalling one are different
  instructions, and a press meaning the first carries none of the second.

The cue you get is a completely normal hot cue, in the same colour as any other
cue the deck sets itself.

Pressing a pad that **already has a cue** is unaffected, preview zone held or
not. It recalls that cue as usual.

## The pad lamps

When a quicksetting panel is open, the mod updates the color of the pad lamps
to reflect the actions that are actually available in that mode. This way,
the pads light up based on what they will really do according to the current quicksetting,
not just what the deck would usually indicate. For details on how this
is handled in performance scenarios, see [groove circuit](groove-circuit.md).
