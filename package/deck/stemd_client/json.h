// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * json.h - the flat-object JSON scanning shared by discovery.c and session.c.
 *
 * Not a parser. stemd's replies are flat objects whose shape we control, so a
 * value is found by its "key": prefix and read to the next delimiter. Keys are
 * matched with their quotes and the colon, so a value cannot be taken for a key.
 * No escape or nesting handling; the server must not send either.
 */
#ifndef STEMD_CLIENT_JSON_H
#define STEMD_CLIENT_JSON_H

#include <stddef.h>

/* String value of "key":"..." into `out`, NUL-terminated. 0 on success; -1 when
 * the key is absent, its value is unterminated, or it does not fit `cap`. An
 * oversized value is reported as absent, not truncated, so callers use their
 * fallback rather than a truncated identifier. */
int json_str(const char *doc, const char *key, char *out, size_t cap);

/* Integer value of "key": as a base-10 long, or -1 when the key is absent. */
long json_int(const char *doc, const char *key);

/* Numeric value of "key": as a double, or `dflt` when the key is absent. */
double json_num(const char *doc, const char *key, double dflt);

#endif /* STEMD_CLIENT_JSON_H */
