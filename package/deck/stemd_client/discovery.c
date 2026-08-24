// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * discovery.c - finding a stemd server.
 *
 * Two sources, matching the deck's STEM SERVER LOCATION setting:
 *
 *   MANUAL  a host[:port] typed on the deck's keyboard, used verbatim
 *   AUTO    mDNS `_stemd._tcp`
 *
 * AUTO shells out to avahi-browse rather than implementing mDNS. The deck runs
 * avahi-daemon already and ships both avahi-browse and avahi-resolve, so a
 * responder of our own would be a second cache of the same records with its own
 * bugs -- and stemd's own history is instructive here: it dropped a pure-Rust
 * responder because the service went dark after a couple of minutes while the
 * process stayed up and logged nothing. Using the system daemon on both ends
 * avoids re-learning that.
 *
 * This is also why discovery lives in the sidecar and not the shim: it forks a
 * process, which is not something to do from inside EP122.
 *
 * `-p` gives stable parseable output; `-r` resolves to address and port; `-t`
 * returns once the cache is exhausted instead of streaming forever:
 *
 *   =;eth0;IPv4;stemd;_stemd._tcp;local;LM-226.local;10.10.50.245;8420;"model=..."
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
 * The check has to be on the ADDRESS, because avahi's protocol column is the
 * one the BROWSE ran over and says nothing about what the name resolved to. A
 * server reachable over v4 and v6 answers an IPv4 browse with its AAAA record
 * and the line still reads `IPv4`:
 *
 *   =;eth0;IPv4;stemd;_stemd._tcp;local;LM-226.local;2a02:842b:...;8420;"..."
 *
 * which is then handed to connect() as a host and fails every time. */
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

    /* Default to stemd's port when only a host was given, which is what a user
     * typing an address on a deck keyboard will do. */
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

/* A host name safe to hand to a shell. mDNS names are whatever a responder on
 * the LAN chose to publish, so this is a whitelist and not an escape: anything
 * outside a DNS label is refused rather than quoted. */
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
 * Needed because avahi-browse resolves the name ITSELF and answers with one
 * address -- on a dual-stack host, usually the v6 one -- with no way to ask for
 * the other: the deck's avahi-browse has no `-4`. avahi-resolve-host-name does,
 * so the name from the browse line is re-resolved here. */
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
    /* `LM-226.local\t10.10.0.133` -- the address is the second field. */
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

        /* IPv4 only, decided on the ADDRESS. Checking avahi's protocol column
         * instead is what let a v6 literal through to connect(): that column is
         * the protocol the BROWSE ran over, and an IPv4 browse of a dual-stack
         * host still answers with its AAAA record. */
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
 * Scanned rather than parsed. A JSON parser would be out of proportion for four
 * fields from a server whose shape we control, and the failure mode of scanning
 * is "not compatible" -- which is exactly what an unrecognised document should
 * report anyway. The keys are matched with their quotes and colon so a value
 * can never be mistaken for a key.
 *
 * `derived` is deliberately NOT checked: stemd does not report it. The topology
 * we need is fully described by the rate, the channel count and the two stems
 * actually being offered. */
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

/* What the deck will use as a directory name for cached stems.
 *
 * `model_id` is the answer whenever the server offers it: the pinned digest of
 * the loaded weights, and the identity stemd keys its own cache on. The API
 * documentation is explicit that a client caching stems must key on this and
 * NOT on `model` -- several artefacts share one model name (`htdemucs_mps` and
 * a `--segment` variant both report `htdemucs`), so `model` cannot tell you
 * whether stems on disk came from the weights loaded now.
 *
 * It also subsumes the preset, because the preset is what picks the model:
 * Speed -> hdemucs_mmi, Balanced -> htdemucs, each with its own digest.
 *
 * The composite below is the fallback for a server too old to report a digest.
 * It is deliberately not just `model`, for the reason above.
 *
 * The deck sanitises this again before it touches a filesystem. Doing it here
 * as well is not redundancy for its own sake: it keeps the value legible in
 * logs on both sides. */
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
        /* Name the requirement that actually failed. Compatibility is four
         * conditions and only two of them are numbers, so reporting the rate and
         * channels alone prints two CORRECT values next to "cannot play" when
         * what is missing is a stem. */
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
            /* Edge-triggered: this runs on every refresh, and a typed address
             * stays wrong until someone retypes it. Once per address, not once
             * per 30 s. */
            static char complained[sizeof(out->host)];

            if (strncmp(complained, manual, sizeof(complained) - 1) != 0) {
                snprintf(complained, sizeof(complained), "%s", manual);
                SERR("cannot parse \"%s\" as host[:port]\n", manual);
            }
            return -1;
        }
    } else if (browse_mdns(out) != 0) {
        /* An empty browse is not proof of an absent server: about a quarter of
         * them come back empty on a live deck while the server never moved,
         * because avahi's cache is not always warm at the instant we ask. So
         * this is a debug detail, and what a reader is told at WARN is that
         * readiness CHANGED -- see the HELLO handler in session.c. */
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
