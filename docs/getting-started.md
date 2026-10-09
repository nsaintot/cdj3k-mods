# Getting started

What the mods need, how to install them, what happens when the deck starts,
and how to remove them.

> [!WARNING]
> **This is unstable software.** Expect bugs, freezes and crashes. It is not
> finished and it is not tested the way a firmware release is.
>
> **Do not put it on a deck you are relying on.** Not on the night of a gig,
> not on a club's unit, and not on the only player you own. Try it where a
> restart costs you nothing.

## What you need

- A **CDJ-3000** running firmware **3.13 to 3.22**, either hardware variant.
  The installer refuses older firmware.
- A USB stick or SD card with your library on it, as usual.
  [X-PAD](xpad.md) and [groove circuit](groove-circuit.md) load any extra files they need from the same drive.
- For [Stems](stems.md) only: a computer on the same network running
  [stemd](https://github.com/nsaintot/stemd). Separation does not happen on the
  deck.

## Installing

The mods ship as a **`.UPD` file**, the deck's own update format.

1. Download the `.UPD` from the
   [latest release](https://github.com/nsaintot/cdj3k-mods/releases/latest).
2. Copy it to a USB stick formatted **FAT32** or **exFAT**. It must be the only
   `.UPD` on the stick; with two, the deck stops and asks you to fix it.
3. Put the stick in the **top USB port**. Updates are not read from the SD slot.
4. Run the deck's firmware update procedure, as for any other update. The unit
   restarts twice after the installation.

**If the deck keeps asking you to connect a USB device**, it is not finding the
update on your stick. The usual cause is a stick formatted on a Mac with a
*GUID Partition Map*: the deck finds the small hidden EFI partition first, sees
no update on it, and stops there. Reformat with a **Master Boot Record**
partition map and a single FAT32 partition.

**This is a patch, not a full replacement.** The `.UPD` updates only specific
parts of the system. The original application is not replaced, and the system
is not reflashed.

[Removal](#removing) is done entirely from the front panel.

As with any update:

- **Do not cut the power while it is updating**, and do not pull the stick.
- **It is third-party code, and the risk is yours.** It is open source, so you
  can read it and build it yourself first. It still voids your warranty, nobody
  is liable if it goes wrong, and there is no vendor to call.
  [Legal](legal.md) has the full terms.

## Preparing your stick

Nothing extra is needed to play. Two features read their own files from the
stick you are playing from, both under a `mods/` folder at its root:

```
mods/loops/            up to eight samples for X-PAD, taken in name order
mods/gc/gc-config.txt  which pads groove circuit owns, and what they play
```

[X-PAD](xpad.md) needs no configuration; it takes the first eight files it
finds. [Groove circuit](groove-circuit.md) needs a config file, because each
loop has to declare its tempo to stay locked to the grid.

**To get started quickly**, download the [starter pack](https://github.com/nsaintot/cdj3k-mods/releases/download/extras-files/cdj3k-mods-starter.zip): eight drum samples
ready for X-PAD, plus extra kits to swap in. Unzip it and copy its `mods`
folder to the root of your stick.

Installing or removing the mods does not touch these files.

## Removing

1. **Power the unit off.**
2. Hold **CUE/LOOP CALL ◀** and **TEMPO ±6/±10/±16/WIDE** together, and **power it on**
   while holding them. The deck starts in service mode and removes the mods as
   it starts.
3. **Reboot the unit** normally.

The deck is now stock. Nothing needs restoring, because nothing was replaced.

A **removal `.UPD`** is also published with each release. Flash it the same way
as the mods; it must be the only `.UPD` on the stick. The front-panel method
needs nothing but the deck, so try that first.

## What happens when the deck starts

The mods check the firmware they are running on and install either completely
or not at all.

**Installation is all-or-nothing.** If any required component is missing from your firmware, no mods are installed and the deck stays fully stock. There is no partial install.

Possible outcomes:

| | |
| --- | --- |
| Firmware recognised | All features install. Firmware version shows `-m`. |
| Firmware not recognised | No features install. Deck remains unchanged. |

**Check the firmware version.** If the mods are active, an `-m` is added to the version number on the UTILITY screen. This suffix is also your entry point to [MOD SETTINGS](mod-settings.md).

A firmware that is not recognised is one the mods do not support yet.

## Nothing is permanent

Installing writes to the deck, but it does not replace anything on it. The mods
are a **patch** that runs alongside the deck's own application, which still
plays your music.

Your media is not touched. The only thing that writes to your USB stick is the
playlist reorder in [browsing](browsing.md), and only while EDIT is on.

Neither way back needs the stock firmware:

- **Switch it off.** Every feature can be turned off in
  [MOD SETTINGS](mod-settings.md). A feature that is off leaves the deck
  running its own code, so with everything off the deck behaves as stock.
- **Remove it.** [Removing](#removing) is done from the front panel.

## What the mods never do

- Replace a stock feature.
- Upload anything except to your own stem server, or collect telemetry.
- Keep a second copy of your library. It is read through the deck's own
  database.
