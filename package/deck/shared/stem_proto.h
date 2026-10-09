// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * stem_proto.h - the wire between the EP122 shim and the stem sidecar.
 *
 * Both sides include this file, so the framing cannot drift between them.
 *
 * Why two processes
 *
 * The sidecar keeps the HTTP client, retries, DNS and blocking sockets out of
 * the process that owns the audio thread. If the network half wedges or
 * crashes, EP122 keeps playing and the deck shows STEMS as unavailable.
 *
 * Decoding stays in the shim because it must use EP122's own decoder. A second
 * decoder can disagree about encoder delay: the deck pads its output (several
 * thousand frames), so stems derived from an independently
 * decoded file come back misaligned, which sounds like a phase problem rather
 * than an obvious offset.
 *
 * Shape
 *
 * Length-prefixed frames, little-endian, no alignment padding beyond what the
 * struct already has. Both directions use the same header; `type` says which
 * union arm follows and `len` covers the payload only.
 *
 *   shim -> sidecar : HELLO, JOB_BEGIN, PCM..., JOB_END, CANCEL
 *   sidecar -> shim : STATUS, PROGRESS, STEM_READY, JOB_FAILED
 *
 * PCM is the only high-volume traffic: the shim writes chunks as its decoder
 * produces them and a blocking socket throttles it, so a whole track is never
 * resident on either side.
 */
#ifndef STEM_PROTO_H
#define STEM_PROTO_H

#include <stdint.h>

/* Where the socket lives. /run is tmpfs on this rootfs and already holds
 * per-service runtime state. */
#define STEM_SOCK_PATH      "/run/stemd-client.sock"

/* Bumped on any incompatible change to the structs below. HELLO carries it and
 * the sidecar refuses a mismatch, so a half-updated deploy fails at connect
 * instead of mid-stream. Version 2 added the manual
 * address to stem_hello; version 3 added the separation id to stem_status and
 * FLAC to the output formats. */
#define STEM_PROTO_VERSION  3

/* Frames the model produces, and the fader names in the order the UI shows
 * them. `drums` is derived on the player and never crosses the network:
 *
 *     out = d*mix + (h-d)*H + (v-d)*V
 *
 * At d == h == v == 1.0 both stem coefficients are exactly zero, so the output
 * is the untouched mix bit for bit. ORIGINAL relies on this, so the faders
 * must snap to exactly 1.0. */
#define STEM_PART_DRUMS     0   /* derived, not shipped */
#define STEM_PART_HARMONICS 1
#define STEM_PART_VOCALS    2
#define STEM_N_PARTS        3

/* Only these two are fetched. Names match stemd's `stems` list verbatim, since
 * they are also the last path element of GET /v1/jobs/{id}/stems/{name}. */
#define STEM_WIRE_HARMONICS "harmonics"
#define STEM_WIRE_VOCALS    "vocals"
#define STEM_N_WIRE         2

/* The topology the deck side expects; GET /v1/health must report it before any
 * PCM is sent. The model accepts only 44.1 kHz stereo; a server built for
 * anything else is rejected. */
#define STEM_WIRE_RATE      44100
#define STEM_WIRE_CHANNELS  2

enum stem_msg_type {
    /* shim -> sidecar */
    STEM_MSG_HELLO      = 1,   /* struct stem_hello   */
    STEM_MSG_JOB_BEGIN  = 2,   /* struct stem_job_begin, then PCM frames */
    STEM_MSG_PCM        = 3,   /* raw f32le interleaved stereo, payload only */
    STEM_MSG_JOB_END    = 4,   /* no payload: the upload is complete */
    STEM_MSG_CANCEL     = 5,   /* no payload: abandon the current job */

    /* sidecar -> shim */
    STEM_MSG_STATUS     = 64,  /* struct stem_status    - connectivity */
    STEM_MSG_PROGRESS   = 65,  /* struct stem_progress  - drives the UI bar */
    STEM_MSG_STEM_READY = 66,  /* struct stem_ready     - a file is on disk */
    STEM_MSG_JOB_FAILED = 67,  /* struct stem_failed                        */
};

struct stem_frame_hdr {
    uint32_t type;   /* enum stem_msg_type */
    uint32_t len;    /* payload bytes following this header */
};

struct stem_hello {
    uint32_t version;            /* STEM_PROTO_VERSION */
    /* The deck's STEM SERVER LOCATION setting, sent at connect because the
     * sidecar cannot read the deck's settings, and discovery must not fall back
     * to mDNS when the user typed an address. Empty means AUTO. */
    uint32_t manual;             /* non-zero when `addr` should be used */
    char     addr[64];           /* "host" or "host:port", NUL-terminated */
};

/* Everything the sidecar needs to build POST /v1/jobs. `frames` is the decoder's
 * count, not the file's; see the encoder-delay note at the top. */
struct stem_job_begin {
    uint64_t frames;             /* total interleaved stereo frames to follow */
    uint32_t sample_rate;        /* always 44100: the model accepts nothing else */
    uint32_t channels;           /* always 2 */
    uint32_t output_format;      /* enum stem_pcm_format, for the stems we get back */
    /* Delivery rate for the stems, distinct from the separation rate. The
     * model works at 44100 and the deck's page pool runs at 96000; converting
     * on the deck is costly: on an RK3399 a whole track decodes at
     * 907x realtime when the rates match and 102x when they do not, about 3.5 s
     * of every cached load.
     *
     * Zero asks for the server's default and leaves the conversion where it is.
     * Before changing this: the derived part is `mix - H - V`, and the deck resamples the mix, so a stem
     * converted elsewhere only cancels as well as the two resamplers agree. */
    uint32_t output_sample_rate;
};

/* Upload is always f32le, the deck decoder's output format (meow::Float2), so
 * sending it is a memcpy. Download is a preference: s16le halves the transfer
 * and the tmpfs copy at no cost to reconstruction, because the derived part is
 * computed from the same mix the parts are summed against, so the quantisation
 * error cancels exactly. */
enum stem_pcm_format {
    STEM_PCM_S16LE = 0,
    STEM_PCM_F32LE = 1,
    /* FLAC of the same s16 samples, for the stems we get back. Lossless, so
     * reconstruction stays bit-exact at unity, and unlike lossy formats it has
     * no encoder delay. On a typical track it shrinks the pair from 171 MB
     * to 61 MB (vocals, mostly digital silence, to 21%), which makes an
     * on-media cache practical. */
    STEM_PCM_FLAC  = 2,
};

/* How the server identifies the separations it produces. Opaque to the deck;
 * it only scopes the on-media cache, so switching model keeps the old cache
 * beside the new one instead of mixing them.
 *
 * In practice this is stemd's `model_id`: the pinned digest of the loaded
 * weights, 64 hex characters, which the server also keys its own cache on.
 * `model` is shared by several distinct artefacts, and the preset selects the
 * model (Speed -> hdemucs_mmi, Balanced -> htdemucs), so the digest covers it.
 *
 * Sized for the digest plus a NUL, with slack for other identifier formats. */
#define STEM_SEP_ID_LEN 72

/* Connectivity; decides whether the UI shows the warn icon. */
struct stem_status {
    uint32_t reachable;          /* a /v1/health round trip succeeded */
    uint32_t compatible;         /* its topology matches what we can play */
    uint32_t queue_depth;
    uint32_t reserved;
    char     sep_id[STEM_SEP_ID_LEN];  /* empty when not reachable */
};

/* Mirrors stemd's Stage so the bar can weight the stages it knows are slow;
 * `Separating` is where nearly all the time goes. */
enum stem_stage {
    STEM_STAGE_IDLE          = 0,
    STEM_STAGE_UPLOADING     = 1,   /* ours: stemd has no such stage */
    STEM_STAGE_QUEUED        = 2,
    STEM_STAGE_ANALYZING     = 3,
    STEM_STAGE_SEPARATING    = 4,
    STEM_STAGE_RECONSTRUCTING = 5,
    STEM_STAGE_WRITING       = 6,
    STEM_STAGE_FETCHING      = 7,   /* ours: downloading the two stems */
    STEM_STAGE_DONE          = 8,
    STEM_STAGE_FAILED        = 9,
    STEM_STAGE_LOADING       = 10,  /* ours: decoding the pair onto the pool timeline */
};

struct stem_progress {
    uint32_t stage;              /* enum stem_stage */
    /* 0..100 of the current stage only: the server's completed/total for that
     * stage (0 when it reports none), or bytes downloaded over Content-Length
     * while fetching. Not a whole-job fraction or a bar position; the deck maps
     * stages onto the bar. */
    uint32_t percent;
    uint32_t queue_position;     /* jobs ahead of us; meaningful when QUEUED */
    uint32_t reserved;
};

/* One stem has landed. The sidecar writes a WAV so the shim can pass the path
 * to the deck's own FileReadWav through createReaderFor; the page pool then
 * handles paging and the 44.1k->96k resample.
 *
 * The shim opens the file and unlinks it immediately: the inode survives while
 * the reader holds a descriptor, so the data lives as long as the pool needs it
 * and cannot leak if either process dies. */
struct stem_ready {
    uint32_t part;               /* STEM_PART_HARMONICS or STEM_PART_VOCALS */
    uint32_t reserved;
    float    gain;               /* server-applied scale; multiply by 1.0f/gain */
    uint32_t path_len;           /* bytes of path following this struct */
    /* char path[path_len]; not NUL-terminated */
};

struct stem_failed {
    uint32_t http_status;        /* 0 when the failure was not an HTTP one */
    uint32_t reason_len;         /* bytes of UTF-8 text following this struct */
    /* char reason[reason_len]; */
};

#endif /* STEM_PROTO_H */
