// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * discovery.c - finding a stemd server.
 *
 * Two sources, matching the deck's STEM SERVER LOCATION setting:
 *
 *   MANUAL  a host[:port] typed on the deck's keyboard, used verbatim
 *   AUTO    mDNS `_stemd._tcp`
 *
 * AUTO shells out to avahi-browse rather than implementing mDNS: the deck already
 * runs avahi-daemon and ships avahi-browse and avahi-resolve. (stemd dropped its
 * own pure-Rust responder after the service silently went dark within minutes.)
 *
 * Forking is also why discovery lives in the sidecar and not in EP122.
 *
 * `-p` gives stable parseable output; `-r` resolves to address and port; `-t`
 * returns once the cache is exhausted instead of streaming forever:
 *
 *   =;eth0;IPv4;stemd;_stemd._tcp;local;host-1.local;192.168.1.20;8420;"model=..."
 *    0  1     2    3      4         5        6            7          8      9
 */
#include "stemd_client.h"
#include "json.h"

#define AVAHI_CMD "avahi-browse -rtp _stemd._tcp 2>/dev/null"

/* Split a `;`-separated avahi line in place, returning the field count. */
static int split_fields(char *line, char *field[], int max)
{
    int n = 0;

    field[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == ';') {
            *p = '\0';
            field[n++] = p + 1;
        }
    }
    return n;
}

/* Is this resolved address a dotted quad?
 *
 * Checked on the address, not avahi's protocol column, which is the protocol the
 * browse ran over. A dual-stack server can answer an IPv4 browse with its AAAA
 * record and the line still reads `IPv4`:
 *
 *   =;eth0;IPv4;stemd;_stemd._tcp;local;host-1.local;2001:db8:...;8420;"..."
 */
static int addr_is_ipv4(const char *s)
{
    int octet;

    for (octet = 0; octet < 4; octet++) {
        int digits = 0, value = 0;

        if (octet && *s++ != '.')
            return 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3)
                return 0;
            value = value * 10 + (*s++ - '0');
        }
        if (digits == 0 || value > 255)
            return 0;
    }
    return *s == '\0';
}

static int parse_manual(const char *manual, struct stem_server *out)
{
    const char *colon = strrchr(manual, ':');

    /* Default to stemd's port when only a host is given. */
    out->port = 8420;
    if (colon && colon[1]) {
        size_t hostlen = (size_t)(colon - manual);

        if (hostlen == 0 || hostlen >= sizeof(out->host))
            return -1;
        memcpy(out->host, manual, hostlen);
        out->host[hostlen] = '\0';
        out->port = atoi(colon + 1);
    } else {
        snprintf(out->host, sizeof(out->host), "%s", manual);
    }
    return out->host[0] ? 0 : -1;
}

/* A host name safe to hand to a shell. mDNS names come from any responder on the
 * LAN, so anything outside DNS label characters is refused, not quoted. */
static int hostname_ok(const char *s)
{
    int n = 0;

    for (; *s; s++, n++) {
        if (n >= 255)
            return 0;
        if (*s >= 'a' && *s <= 'z') continue;
        if (*s >= 'A' && *s <= 'Z') continue;
        if (*s >= '0' && *s <= '9') continue;
        if (*s == '.' || *s == '-' || *s == '_') continue;
        return 0;
    }
    return n > 0;
}

/* The service's A record, asked for by name.
 *
 * avahi-browse resolves the name itself and returns one address, usually the v6
 * one on a dual-stack host, and the deck's avahi-browse has no `-4`.
 * avahi-resolve-host-name does, so the name from the browse line is re-resolved. */
static int resolve_v4(const char *host, char *out, size_t cap)
{
    char cmd[320], line[256];
    FILE *fp;
    int ok = 0;

    if (!hostname_ok(host))
        return -1;
    snprintf(cmd, sizeof(cmd),
             "avahi-resolve-host-name -4 %s 2>/dev/null", host);
    fp = popen(cmd, "r");
    if (!fp)
        return -1;
    /* `host-1.local\t192.168.1.20` -- the address is the second field. */
    if (fgets(line, sizeof(line), fp)) {
        char *addr = strpbrk(line, " \t");

        if (addr) {
            addr += strspn(addr, " \t");
            addr[strcspn(addr, " \t\r\n")] = '\0';
            if (addr_is_ipv4(addr)) {
                snprintf(out, cap, "%s", addr);
                ok = 1;
            }
        }
    }
    pclose(fp);
    return ok ? 0 : -1;
}

static int browse_mdns(struct stem_server *out)
{
    char line[512];
    FILE *fp = popen(AVAHI_CMD, "r");
    int found = 0;

    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *field[16];
        int n;

        /* Only resolved records ('=') carry an address; the '+' lines that
         * precede them are announcements without one. */
        if (line[0] != '=')
            continue;
        line[strcspn(line, "\r\n")] = '\0';
        n = split_fields(line, field, 16);
        if (n < 9)
            continue;

        /* IPv4 only, decided on the address (see addr_is_ipv4). */
        if (addr_is_ipv4(field[7]))
            snprintf(out->host, sizeof(out->host), "%s", field[7]);
        else if (resolve_v4(field[6], out->host, sizeof(out->host)) != 0)
            continue;                   /* v6-only, or the name will not resolve */

        out->port = atoi(field[8]);
        found = 1;
        break;
    }

    pclose(fp);
    return found ? 0 : -1;
}

/* GET /v1/health, which is the liveness check and the compatibility gate in one.
 * A live document looks like:
 *
 *   {"version":"0.1.0","backend":"demucs","model":"htdemucs","device":"mps",
 *    "sample_rate":44100,"channels":2,"stems":["harmonics","vocals"], ...}
 *
 * Scanned with json.h, not parsed; an unrecognised document reads as not
 * compatible.
 *
 * `derived` is not checked because stemd does not report it. The rate, channel
 * count and the two offered stems are enough. */
#define HEALTH_MAX 2048

struct health_buf {
    char   data[HEALTH_MAX];
    size_t len;
};

static int health_sink(const void *buf, size_t len, void *user)
{
    struct health_buf *h = user;
    size_t room = sizeof(h->data) - 1 - h->len;

    if (len > room)
        len = room;
    memcpy(h->data + h->len, buf, len);
    h->len += len;
    h->data[h->len] = '\0';
    return 0;
}

/* The directory name the deck uses for cached stems.
 *
 * `model_id` when offered: the pinned digest of the loaded weights, which stemd
 * keys its own cache on. The API docs say clients must key on it, not on `model`:
 * several artefacts share one model name (`htdemucs_mps` and a `--segment`
 * variant both report `htdemucs`). It also covers the preset, which picks the
 * model (Speed -> hdemucs_mmi, Balanced -> htdemucs).
 *
 * The backend-model-preset composite is the fallback for servers too old to
 * report a digest.
 *
 * The deck sanitises this again before using it as a path; sanitising here keeps
 * the value consistent in logs on both sides. */
static void derive_sep_id(const char *doc, char *out, size_t cap)
{
    char backend[24], model[24], preset[24];
    size_t i;

    if (json_str(doc, "model_id", out, cap) == 0)
        goto sanitise;
    if (json_str(doc, "separation_id", out, cap) == 0)
        goto sanitise;

    if (json_str(doc, "backend", backend, sizeof(backend)) != 0)
        snprintf(backend, sizeof(backend), "unknown");
    if (json_str(doc, "model", model, sizeof(model)) != 0)
        snprintf(model, sizeof(model), "unknown");
    if (json_str(doc, "preset", preset, sizeof(preset)) != 0)
        snprintf(preset, sizeof(preset), "default");
    snprintf(out, cap, "%s-%s-%s", backend, model, preset);

sanitise:
    for (i = 0; out[i]; i++) {
        char c = out[i];

        if (c >= 'A' && c <= 'Z')
            out[i] = (char)(c - 'A' + 'a');
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '.' || c == '_'))
            out[i] = '_';
    }
}

static int health_probe(struct stem_server *out)
{
    struct health_buf h;
    struct http_request req;
    int status;

    memset(&h, 0, sizeof(h));
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/v1/health";
    req.push = health_sink;
    req.push_user = &h;

    status = http_perform(out, &req);
    if (status != 200) {
        SDBG("%s:%d health failed (status %d)\n",
             out->host, out->port, status);
        return -1;
    }
    out->reachable = 1;

    out->compatible =
        json_int(h.data, "sample_rate") == STEM_WIRE_RATE &&
        json_int(h.data, "channels") == STEM_WIRE_CHANNELS &&
        strstr(h.data, "\"" STEM_WIRE_HARMONICS "\"") != NULL &&
        strstr(h.data, "\"" STEM_WIRE_VOCALS "\"") != NULL;

    derive_sep_id(h.data, out->sep_id, sizeof(out->sep_id));

    SDBG("%s:%d reachable, %s (rate=%ld ch=%ld) sep=%s\n",
         out->host, out->port,
         out->compatible ? "compatible" : "INCOMPATIBLE",
         json_int(h.data, "sample_rate"), json_int(h.data, "channels"),
         out->sep_id);
    if (!out->compatible) {
        /* Name each of the four requirements that failed. */
        SWARN("%s:%d is up but this deck cannot play it:%s%s%s%s\n",
              out->host, out->port,
              json_int(h.data, "sample_rate") == STEM_WIRE_RATE ? "" : " rate",
              json_int(h.data, "channels") == STEM_WIRE_CHANNELS ? "" : " channels",
              strstr(h.data, "\"" STEM_WIRE_HARMONICS "\"") ? "" : " no " STEM_WIRE_HARMONICS,
              strstr(h.data, "\"" STEM_WIRE_VOCALS "\"") ? "" : " no " STEM_WIRE_VOCALS);
    }
    return out->compatible ? 0 : -1;
}

int discovery_find(const char *manual, struct stem_server *out)
{
    memset(out, 0, sizeof(*out));

    if (manual && manual[0]) {
        if (parse_manual(manual, out) != 0) {
            /* Logged once per address, not on every 30 s refresh. */
            static char complained[sizeof(out->host)];

            if (strncmp(complained, manual, sizeof(complained) - 1) != 0) {
                snprintf(complained, sizeof(complained), "%s", manual);
                SERR("cannot parse \"%s\" as host[:port]\n", manual);
            }
            return -1;
        }
    } else if (browse_mdns(out) != 0) {
        /* An empty browse does not prove the server is gone: about a quarter
         * come back empty on a live deck because avahi's cache is cold. So this
         * is DEBUG; readiness changes are logged at WARN by the HELLO handler
         * in session.c. */
        SDBG("no _stemd._tcp on the network (avahi returned nothing)\n");
        return -1;
    }

    return health_probe(out);
}

int discovery_recheck(struct stem_server *out)
{
    if (!out->host[0])
        return -1;                  /* never found one; nothing to re-probe */
    return health_probe(out);
}
