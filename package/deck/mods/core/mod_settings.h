// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mod_settings.h - the persisted MOD SETTINGS store.
 *
 * One fixed binary record on the eMMC settings partition, the only writable
 * mount that survives a reboot. Loaded once from the constructor, rewritten
 * whenever a row is committed.
 *
 * Each value is declared by the feature that owns it: g_theme_id in
 * theme/theme.h, the stem settings in stem/stem.h, the cue behaviours' flags in
 * cue/cue.h. The record layout and the rules for changing it are at struct
 * mod_settings_v1 in common.c.
 */
#ifndef EP122_MOD_SETTINGS_H
#define EP122_MOD_SETTINGS_H

/* Read the saved record into the feature globals. A file that is absent, the
 * wrong size, the wrong version, or fails its CRC leaves every default in
 * place; a record is never partly applied. */
void mods_settings_load(void);

/* Write the current values: temp file, fsync, rename, previous copy kept as
 * .BAK. A power cut can lose the new value but never leave a half-written one. */
void mods_settings_save(void);

/* Record the stem server's separation identity, sanitised to [A-Za-z0-9._-].
 * It arrives over the network and becomes a directory name on removable media.
 * Does not persist by itself. */
void mods_set_sep_id(const char *id);

#endif /* EP122_MOD_SETTINGS_H */
