# cdj3k-mods

QoL features for the CDJ-3000 without replacing its firmware.

A set of modifications for the CDJ-3000, installed as a firmware update. They
run alongside the deck's own application rather than replacing it, and come off
from the front panel.

<p align="center">
  <img src="https://cdj3k-mods.com/img/stems-row-unity.png" width="49%" alt="The STEMS row: drums, harmonics and vocals on three faders over the play screen">
  <img src="https://cdj3k-mods.com/img/theme-comb.png" width="49%" alt="The play screen in six of the themes, one vertical strip each">
</p>

Features:

- **Cues** — gate cues (press a pad while paused: hold to play, release to
  return), smart cues (last hot cue is cue), and preview hot cue to set a hot
  cue with the preview.
- **Stems** — drums, harmonics and vocals on three toggles and three faders.
  Separation runs on a computer on your network via
  [stemd](https://github.com/nsaintot/stemd), not on the deck. Groove circuit
  sits on the hot cue pads.
- **X-PAD** — a sampler on a touch strip: loop length across, pitch bend up and down, eight samples on the hot cue pads.
- **Themes** — six new themes, including a true white one.
- **Browsing** — reorder tracks inside a playlist from the deck.
- **Grid adjust** — the BPM half of the grid panel, which the 3000 does not have:
  double, halve, or nudge the beat interval a millisecond at a time.

**[Documentation at cdj3k-mods.com](https://cdj3k-mods.com)** · [Download the
latest release](https://github.com/nsaintot/cdj3k-mods/releases/latest)

## Supported decks

Both hardware variants — **Renesas** and **Rockchip (rk3399)** — on firmware
**3.13 to 3.22**. The installer refuses anything older than 3.13. On a firmware
the mods do not fully recognise, nothing installs and the deck runs stock.

## Repository

```
package/ the mods themselves, the STEMS sidecar, and the .UPD that installs
         them on a deck. package/deck/docs/mods.md is the developer entry
         point.
docs/    the DOCUMENTATION, as markdown. The site renders these; they are the
         single source for how to use any of it.
web/     the site. Vue 3 + Vite, no SSR.
```

## Before you install

This is an independent community project. It is **not affiliated with, endorsed
by, or connected to AlphaTheta Corporation or Pioneer DJ.** Trademarks belong to
their owners — see [TRADEMARKS](TRADEMARKS.md).

**Installing voids your warranty.** The software comes with no warranty of any
kind, and the authors accept no liability for damage, a deck that will not
start, lost data, or anything that happens during a performance. The risk is
yours. See [legal](docs/legal.md) in full.

## Licence

Licensed under either of

- Apache Licence, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT licence ([LICENSE-MIT](LICENSE-MIT))

at your option.

Unless you state otherwise, any contribution you intentionally submit for
inclusion in this work, as defined in the Apache-2.0 licence, is dual licensed
as above, with no additional terms or conditions.
