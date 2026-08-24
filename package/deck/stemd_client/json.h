// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * json.h - the flat-object JSON scanning shared by discovery.c and session.c.
 *
 * Not a parser. stemd's replies are flat objects whose shape we control, so a
 * value is found by its "key": prefix and read to the next delimiter. Keys are
 * matched with their quotes and the colon, so a value can never be taken for a
 * key. No escape or nesting handling: a backslash, or a nested object in a
 * value, would be a bug at the server rather than here.
 *
 * These lived privately in both callers until the two copies drifted on what an
 * oversized string should do -- one truncated, one refused. One copy, one
 * answer.
 */
#ifndef STEMD_CLIENT_JSON_H
#define STEMD_CLIENT_JSON_H

#include <stddef.h>

/* String value of "key":"..." into `out`, NUL-terminated. 0 on success; -1 when
 * the key is absent, its value is unterminated, or it does not fit `cap`. A value
 * too long to hold is reported as absent rather than truncated, so a caller with
 * a fallback (a default, a composite) takes it instead of an identifier cut in
 * half -- half a content digest names the wrong thing as surely as none does. */
int json_str(const char *doc, const char *key, char *out, size_t cap);

/* Integer value of "key": as a base-10 long, or -1 when the key is absent. */
long json_int(const char *doc, const char *key);

/* Numeric value of "key": as a double, or `dflt` when the key is absent. */
double json_num(const char *doc, const char *key, double dflt);

#endif /* STEMD_CLIENT_JSON_H */
