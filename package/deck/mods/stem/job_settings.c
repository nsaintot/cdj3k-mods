// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/job_settings.c - the STEMS rows in MOD SETTINGS, and the server address they carry.
 */
#include "stem/job_internal.h"

/* Names the expected format; the popup is the only feedback the deck gives. */
static const char *const k_addr_error[] = {
    "Value must be an IPv4 address"
};

static const char *const k_stems_notice[] = {
    "The stemd app must be reachable from the LAN",
    "",
    "setup instructions:",
    "cdj3k-mods.com/docs/stems",
};
#include "core/mod_settings.h"
#include "wave/wave.h"
#include "db/db.h"
#include "xpad/ext.h"
#include "kit/menu.h"
#include "kit/mod.h"
#include "kit/popup.h"
#include <pthread.h>

void mods_stem_settings_changed(void)
{
    int on = g_stems_on ? 1 : 0;

    /* Only set a flag: this runs on the message thread, which must not block, and
     * a HELLO round trip includes mDNS discovery that can take seconds on a cold
     * LAN. The worker picks the flag up within its idle tick. */
    g_resettle = 1;

    /* ENABLE STEMS must act on the track already loaded: otherwise a job is only
     * requested by the track watch when the sourceId changes, and the panel would
     * show dead faders until the next load.
     *
     * Switching off tears the set down: a resident set holds ~350 MB of s16, and
     * an in-flight separation would keep uploading. Switching back on takes the
     * normal route, which is a cache hit and a decode, not an upload. */
    if (g_stems_was_on >= 0 && on != g_stems_was_on) {
        MDBG("stem_job: STEMS switched %s mid-track -> %s\n",
             on ? "on" : "off", on ? "requesting stems" : "dropping the set");
        if (on) {
            stem_job_request();
            kit_popup_show(k_stems_notice,
                           (int)(sizeof k_stems_notice / sizeof k_stems_notice[0]));
        } else {
            stem_track_gone();
        }
    }
    g_stems_was_on = on;
}

/* A strict IPv4 dotted quad: four decimal octets, 0-255. The address goes to
 * connect(), so prefixes (10.0.0.5/24), host names and IPv6 are refused.
 * Leading zeros are refused too: "010" is ten to some resolvers and eight to
 * others. */
static int ipv4_ok(const char *s)
{
    int octet;

    if (!s) return 0;
    for (octet = 0; octet < 4; octet++) {
        int digits = 0, value = 0;

        if (octet && *s++ != '.') return 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3) return 0;
            value = value * 10 + (*s++ - '0');
        }
        if (digits == 0 || value > 255) return 0;
        if (digits > 1 && s[-digits] == '0') return 0;
    }
    return *s == '\0';
}

/* An empty address means NOT SET and is valid; a typed address that fails the
 * check is cleared. The overlay calls this before it persists, so the bad value
 * never reaches disk. */
void addr_changed(void)
{
    if (g_stem_addr[0] && !ipv4_ok(g_stem_addr)) {
        MDBG("stem_job: \"%s\" is not a valid IPv4 address -> cleared\n", g_stem_addr);
        g_stem_addr[0] = '\0';
        kit_popup_show(k_addr_error, (int)(sizeof(k_addr_error) / sizeof(k_addr_error[0])));
    }
    /* The sidecar uses the address from the last HELLO; push the new one. */
    mods_stem_settings_changed();
}
