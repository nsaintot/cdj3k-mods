/* SPDX-License-Identifier: MIT OR Apache-2.0 */
/*
 * mods/stem/job_internal.h - what the STEMS job files share.
 *
 * job.c owns the job state machine and every flag below; job_loader.c hands
 * decoded stems to the audio side; job_settings.c owns the MOD SETTINGS rows.
 * Declarations only.
 */
#ifndef EP122_MOD_STEM_JOB_INTERNAL_H
#define EP122_MOD_STEM_JOB_INTERNAL_H

#include "stem/stem.h"

/* Set when a settings change means the job must be re-placed, and whether
 * STEMS was on last time the rows were read. Defined in job.c. */
extern volatile int  g_resettle;
extern int g_stems_was_on;

/* The settings watcher, and the loader thread job.c starts. */
void addr_changed(void);
void * loader_main(void *arg);

/* Retry delay for a job that could not run for a transient reason. Long enough
 * not to re-probe a deck parked at the cue point constantly, short enough that
 * stems follow soon after PLAY. */
#define JOB_RETRY_SEC 5

/* The job state the loader thread reads. Defined in job.c. */
extern char              g_cur_path[STEM_CACHE_PATH_MAX];
extern int g_job_via_server;

uint64_t job_now_sec(void);

extern volatile uint32_t g_cur_gen;
extern volatile int      g_load_armed;
extern volatile uint32_t g_load_gen;
extern volatile int  g_quit;
extern volatile uint64_t g_retry_at;

struct job_delivery {
    char  track[STEM_CACHE_PATH_MAX];   /* which track these are for */
    char  h[STEM_CACHE_PATH_MAX], v[STEM_CACHE_PATH_MAX];
    float hg, vg;
    int   tmpfs;                        /* ours to unlink once loaded */
    volatile uint32_t gen;
};

struct job_arrived {
    char  path[192];
    float gain;
    int   have;
};

extern struct job_delivery g_delivery;
extern struct job_arrived g_arrived[STEM_N_PARTS];

void job_retry_later(void);
void sep_request(const char *path, int64_t frames);
int track_is_current(const char *path);
void ui_publish(int stage, int percent, int queue_position);

/* How long to block until the sidecar has probed the server and reported back.
 *
 * Generous because AUTO discovery browses mDNS first, which can take seconds on
 * a cold LAN. await_sidecar_ready returns 0 when the server is usable, -1
 * otherwise, including "reachable but incompatible", so nothing is uploaded. */
#define JOB_READY_TIMEOUT_MS  20000

/* How long the worker waits for a sidecar frame before re-checking the cancel
 * flag: prompt on a track change without spinning when idle. */
#define JOB_RECV_TIMEOUT_MS 250

#define SEP_RATE_POLL_MS    250

/* How long to wait for the pool's rate, which the server is asked to deliver
 * stems at.
 *
 * Stems already at the pool's rate are passed through by the deck's own decoder,
 * skipping the 44.1 -> 96 k conversion, which is most of a cached load's cost:
 * a whole track decodes about nine times faster when the rates match.
 *
 * This is only safe because the server converts with the deck's filter. Drums
 * is derived as `mix - harmonics - vocals` and the pool resamples that mix, so a
 * different filter leaves the difference on the drums fader, plainly audible
 * with a generic resampler. stemd reproduces the deck's converter (LTI, 320/147
 * polyphase, 18880 taps) to -141.5 dB, below the
 * 16 bits a stem is stored at.
 *
 * The filter is not negotiated: the shim and stemd ship together. A stemd built
 * without ep122.rs returns stems at the right rate and the wrong phase, audible
 * only on the drums fader.
 *
 * The rate is waited for, not sampled once: on the first separation after a
 * restart nothing has measured it yet (the position measurement needs playback,
 * the stretcher needs the engine running, which it is not 250 ms after a load
 * on a paused deck). Sampling once can return 0 and silently fall back to the
 * slow native-rate path. The wait is negligible next to tens of seconds of
 * model time.
 *
 * Zero when the rate cannot be established: the server uses its default and the
 * deck converts. Slower but correct, since stem files are self-describing and
 * stem_decode_pull converts whatever it opens. */
#define SEP_RATE_WAIT_MS   8000

/* Interval at which to ask the sidecar again whether it can see a server.
 *
 * STATUS arrives only once, at connect, so without this the warning under STEMS
 * is as old as the last cache miss and can show a server that has since gone
 * away as present.
 *
 * Idle branch only: a HELLO puts the sidecar into a blocking probe of about a
 * second, safe only with nothing in flight. No answer means no update; it is
 * retried next interval. */
#define STATUS_REFRESH_SEC   30

struct upload_ctx {
    int64_t sent;
    int64_t total;
    /* Why the sink stopped: a track change and a dead socket abort the decode the
     * same way, and the sidecar must be told which (see run_one_job). */
    int     cancelled;
};

extern int64_t g_job_frames;
extern char    g_job_path[STEM_CACHE_PATH_MAX];
extern volatile int  g_probe_now;
extern volatile int      g_sep_busy;
extern volatile int      g_sep_supersede;
extern volatile uint32_t g_want_gen;

void job_failed(void);
void job_progress(int stage, int pct);
void refresh_status(void);
/* Act on a server that has just become usable. The edge is latched in
 * ui_publish_status, so this is safe to call every idle turn; it consumes the
 * edge once. Idle branch only: it re-serves the loaded track, which must not
 * happen with a job in flight. */
void server_arrived(void);
void run_separation(void);
void ui_publish_status(int reachable, int compatible);

/* The sidecar conversation, in job_separator.c. */
void * separator_main(void *arg);
int upload_chunk(const float *pcm, int64_t frames, void *user);
int await_sidecar_ready(void);
int handle_frame(uint32_t type, const void *buf, uint32_t len);
uint32_t sep_output_rate(const char *path);

#endif /* EP122_MOD_STEM_JOB_INTERNAL_H */
