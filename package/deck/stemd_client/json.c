// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * json.c - see json.h.
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int json_str(const char *doc, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p, *end;
    size_t n;

    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    p = strstr(doc, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    end = strchr(p, '"');
    if (!end)
        return -1;
    n = (size_t)(end - p);
    if (n >= cap)
        return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

long json_int(const char *doc, const char *key)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(doc, pat);
    if (!p)
        return -1;
    return strtol(p + strlen(pat), NULL, 10);
}

double json_num(const char *doc, const char *key, double dflt)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(doc, pat);
    if (!p)
        return dflt;
    return strtod(p + strlen(pat), NULL);
}
