# Grid adjust

Extra BPM controls on the grid adjustment panel, so you can correct a track's
BPM.

Open **GRID ADJUST** as usual. The deck's own five buttons are still there,
shifted left, with the new group beside them on its own plate.

|                |                                                                |
| -------------- | -------------------------------------------------------------- |
| **\|\|\|×2**   | Twice the tempo. Every other beat becomes a bar line           |
| **\|\|\|×1/2** | Half the tempo                                                 |
| **▸\|\|\|◂**   | Reduce the interval by 1 ms — beats closer together, tempo up  |
| **◂\|\|\|▸**   | Enlarge the interval by 1 ms — beats further apart, tempo down |
| **RESET**      | Back to the tempo the track loaded with                        |

The BPM readout above the group shows the current value.

## What each one is for

**×2 and ×1/2** fix a track analysed at the wrong multiple, such as a 140 BPM
track read as 70, or a half-time track read at double. One press lines the
grid up.

**Enlarge and Reduce** fix a grid that is close but drifts across the track:
the first beats line up and the last ones do not. Each press changes the
spacing by a millisecond without moving the downbeat.

## Limits

Multiplication is capped at ×8 and ×1/8. Enlarge and Reduce go 200 steps either
way, or ±200 ms on the beat interval.

On a track with no beat grid, the buttons do nothing.
