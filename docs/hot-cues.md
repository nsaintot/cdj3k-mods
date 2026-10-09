# Hot cues

Changes to what the eight hot cue pads do. Each one is independent, and with
all of them off the pads behave as stock.

## GATE CUE

Ships **OFF**. Makes the pads momentary while the deck is paused.

| | |
| --- | --- |
| Press a pad **while paused** | Jumps to the cue and plays for as long as you hold it |
| **Release the pad** | Returns to the cue and pauses |
| Press **PLAY** while still holding | Latches, and the track keeps playing when you let go |
| Press a pad **while playing** | A normal hot cue: jumps to the cue and keeps playing |

Whether a press gates depends on whether the deck was paused when you pressed.
A second pad pressed during a hold joins the first.

![GATE CUE on: holding a pad plays from the cue, and releasing it returns there and pauses.](vid/gate-cue.mp4)

### Setting a cue is not affected

Pressing an **empty** pad still sets a hot cue at the play head, and releasing
it does nothing.

### Very short taps

A press of a few milliseconds may not register. The panel scans the buttons on
a cycle, and a very short pulse can fall between two scans. Any press made with
a finger is long enough.

### The shortcut on the play screen

GATE CUE also has a button on the play screen, between the time display and
the tempo, so you can switch it mid-set without opening the settings.

It shows **white on the accent colour** while gate cue is on, and as a grey
plate with dim lettering while it is off. The grey matches the deck's own
STEMS, BEAT LOOP and KEY SHIFT buttons. A.HOT CUE and MT beside it are outlined
because they are indicators, not buttons.

![Gate cue off: the GATE CUE button is a grey plate.](img/gate-shortcut-off.png)
![Gate cue on: the same button filled with the accent colour.](img/gate-shortcut-on.png)

The button and the [MOD SETTINGS](mod-settings.md) row are the same switch:
change one and the other follows.

## SMART CUE

Ships **OFF**. Makes the memory cue follow you.

With it on, the cue point moves to whichever hot cue you last pressed, so
pressing **CUE** takes you back to where you last jumped.

The cue point moves on the pad press itself, with no delay.

## PREVIEW HOTCUE

Ships **OFF**. Lets you set a cue where you point on the waveform instead of at
the play head.

1. **Hold** the preview zone on the waveform, as you would to preview a part of
   the track.
2. While still holding, **press an unassigned pad.**

The cue is set **under your finger**, snapped to the beat grid by the deck, not
at the play head.

![Holding the preview zone and pressing an unassigned pad: the cue is set where the finger is, and the deck does not jump.](vid/preview-cue.mp4)

How it differs from a normal pad press:

- **The deck does not jump.** The track keeps playing from where it was.
- **Nothing else reacts to it.** Gate cue does not return to the cue on
  release, and smart cue does not move the cue point.

The result is a normal hot cue, in the same colour as any cue the deck sets
itself.

Pressing a pad that **already has a cue** works as usual, with or without the
preview zone held: it recalls that cue.

## The pad lamps

When a quick-setting panel is open, the mods change the pad lamp colours to
show what each pad will do in that mode, not what the deck would normally
show. See [groove circuit](groove-circuit.md) for how this works with the
STEMS row.
