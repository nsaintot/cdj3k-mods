// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * mods/stem/job_settings.c - the STEMS rows in MOD SETTINGS, and the server address they carry.
 */
#include "stem/job_internal.h"

/* Says what was expected rather than just that it was wrong -- the deck has no
 * other way to find out. */
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

    /* A flag, not a HELLO. This is called from the settings screen, on the message
     * thread, and the round trip includes a discovery that legitimately takes
     * seconds on a cold LAN -- the one thread that must never block is not the one
     * to wait for mDNS. The worker picks this up within its idle tick. */
    g_resettle = 1;

    /* ENABLE STEMS is the master gate, and flipping it has to act on the track
     * that is ALREADY loaded. Nothing else will: a job is otherwise only ever
     * requested from the track watch, which fires on the sourceId moving, so
     * without this switching the feature on mid-track does nothing until the DJ
     * loads something else -- the panel opens with its faders drawn and dead.
     *
     * Off is the mirror image and is a teardown, not a pause: the mix is already
     * gated per block, but a set left resident holds ~350 MB of s16 for a feature
     * that is switched off, and a separation left in flight keeps uploading a
     * track nobody asked about. Turning it back on runs the normal route again,
     * which is a cache hit and a decode rather than an upload. */
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

void addr_changed(void)
{
    if (g_stem_addr[0] && !ipv4_ok(g_stem_addr)) {
        MDBG("stem_job: \"%s\" is not a valid IPv4 address -> cleared\n", g_stem_addr);
        g_stem_addr[0] = '\0';
        kit_popup_show(k_addr_error, (int)(sizeof(k_addr_error) / sizeof(k_addr_error[0])));
    }
    /* The address the sidecar is using came from the last HELLO, so a typed one
     * changes nothing until it is pushed. */
    mods_stem_settings_changed();
}
