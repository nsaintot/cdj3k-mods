// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cache.c - the stem cache on the DJ's own media.  [worker]
 *
 * For the case with no server on the network: stick in, track loads, stems play.
 *
 *   <volume>/mods/stemd-cache/<separation-id>/<ab>/<track-id>/
 *       meta   harmonics.flac (or .wav)   vocals.flac
 *
 * `separation-id` is the server's and opaque to us: backend, model, preset and
 * stemd's version in one string, so the server owns what invalidates its output.
 * <ab> is the first byte of the track-id -- exfat directories are a linear scan.
 *
 * The track id is size + first 64 KiB + last 64 KiB, hashed. Not the sourceId:
 * across the eleven tracks on the reference stick it took the values 1, 7, 8 and
 * 0xb, all <= the track count, so it indexes the browse list and moves with sort
 * order. Not the path either, which survives no reorganisation.
 *
 * The frame count is hashed in and is not optional: the stems are aligned to
 * EP122's own decode including its padding (4116 frames on the reference track),
 * so a firmware that pads differently would otherwise produce a structurally
 * perfect, silently misaligned entry -- which presents as a phase problem.
 *
 * Cached files keep whatever extension the sidecar wrote and lookup tries the
 * known ones in turn, so the server can start offering FLAC with no migration.
 */
#include "stem/stem.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* One volume, one cache root. Relative to the mount point, so it travels with
 * the stick rather than with the deck, and UNDER mods/ with everything else this
 * shim puts on a DJ's media -- mods/loops/ is already there. A cache directory in
 * the root of a stick is one more thing in the folder the DJ browses.
 *
 * NO FALLBACK TO THE OLD ROOT PATH. A lookup that tried both would keep writing
 * entries in one place and finding them in two for as long as any stick had the
 * old layout, which is a migration that never ends. A stick with the old
 * directory re-separates, or is moved by hand. */
#define CACHE_ROOT      "mods/stemd-cache"

/* Headroom left on the volume after a write. A stick filled to the last block
 * is a stick that fails the DJ's next export, and exfat itself wants slack. */
#define FREE_MARGIN     (128ull * 1024 * 1024)

/* Hashed from each end of the file. Large enough that two different tracks
 * sharing both windows AND a byte count is not a thing that happens, small
 * enough that the read is invisible next to opening the track at all. */
#define KEY_WINDOW      (64 * 1024)

#define COPY_CHUNK      (256 * 1024)

/* Bumped if the meta format changes. An entry whose meta we cannot parse is
 * treated as absent, so an old shim and a new one can share a stick. */
#define META_VERSION    1

static const char *const k_ext[] = { ".flac", ".wav" };
#define N_EXT ((int)(sizeof(k_ext) / sizeof(k_ext[0])))

/* ---- the key -------------------------------------------------------------- */

#define FNV64_OFFSET 1469598103934665603ull
#define FNV64_PRIME  1099511628211ull

static uint64_t fnv1a(uint64_t h, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    size_t i;

    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV64_PRIME;
    }
    return h;
}

/* Hash the track's identity into `out` as hex. Returns 0 on success.
 *
 * FNV-1a rather than anything stronger on purpose: this is a cache key, not a
 * boundary anyone is attacking, and a 64-bit space against a stick holding a
 * few thousand tracks makes a collision far less likely than the eMMC failing. */
static int key_of(const char *track_path, int64_t frames, char *out, size_t cap)
{
    unsigned char win[KEY_WINDOW];
    struct stat st;
    uint64_t h = FNV64_OFFSET;
    ssize_t n;
    int fd;

    if (cap < 17)
        return -1;
    fd = open(track_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -1;
    }

    {
        uint64_t size = (uint64_t)st.st_size;
        uint64_t nf = (uint64_t)frames;

        h = fnv1a(h, &size, sizeof(size));
        h = fnv1a(h, &nf, sizeof(nf));
    }

    n = pread(fd, win, sizeof(win), 0);
    if (n > 0)
        h = fnv1a(h, win, (size_t)n);

    if (st.st_size > KEY_WINDOW) {
        off_t tail = st.st_size - KEY_WINDOW;

        n = pread(fd, win, sizeof(win), tail);
        if (n > 0)
            h = fnv1a(h, win, (size_t)n);
    }
    close(fd);

    snprintf(out, cap, "%016llx", (unsigned long long)h);
    return 0;
}

/* ---- choosing the volume ---------------------------------------------------
 *
 * The deck can have a USB stick, an SD card, or both, and an entry is written to
 * whichever one HOLDS THE TRACK. Nothing else is a candidate -- see store_root
 * for why stems that do not sit beside their own source can never be looked up
 * and can never be identified again either.
 *
 * So a DJ with both gets a cache on each, and that is the point rather than a
 * split: pulling a volume takes its tracks and their stems away together and
 * leaves the other pair intact. A lookup has no winner to pick, because the key
 * is the track's own content and only one volume holds that track.
 *
 * REMOTE MEDIA IS A SOURCE, NOT A DESTINATION
 *
 * A linked player's media appears as /media/player<N>/<slot> -- a FuseFilsine
 * mount served over PRO DJ LINK. Those are read-only, and not by our policy:
 *
 *     touch /media/player03/usb/.stemtest  ->  No such file or directory
 *
 * The remote end stubs every write, so remote volumes are searched on LOOKUP and
 * never considered for a STORE. A stick that already carries cached stems still
 * serves them over LINK, where 61 MB of FLAC is seconds. The read-only mount is
 * only the mechanism -- "a track's stems belong on the track's own volume" reaches
 * the same answer and would hold if a linked player ever became writable.
 *
 * /media/rekordbox is excluded outright: it is a linked laptop's library rather
 * than a mounted volume, and nothing about it survives the laptop closing.
 *
 * THE DEVICE NAME IS NOT A SIGNAL. On hardware the slots come up as
 *
 *     /dev/sda1 -> /media/usb/sda1        /dev/sdb1 -> /media/sd/sdb1
 *
 * while the emulator's loop-mounted stick lands on /media/usb/sdb1 -- the same
 * letter that means SD on a real deck. So the BASE DIRECTORY is the only thing
 * that says which slot a volume is in, which is why this enumerates whatever is
 * mounted under each base instead of deriving anything from sd[a-z]. */
static const char *const k_media_base[] = { "/media/usb", "/media/sd" };
#define N_MEDIA_BASE ((int)(sizeof(k_media_base) / sizeof(k_media_base[0])))

/* Linked players mount under /media/<this prefix><N>/<slot>, so the bases are
 * discovered rather than listed. */
#define REMOTE_PREFIX   "player"
#define MEDIA_DIR       "/media"

/* A directory under a media base is only interesting if something is actually
 * mounted on it: the mount scripts mkdir before they mount and do not always
 * rmdir after, so an empty leftover would otherwise read as a volume with no
 * space. Different st_dev from its parent is the mount test. */
static int is_mounted(const char *base, const char *sub, char *out, size_t cap)
{
    struct stat sb, sd;

    if ((size_t)snprintf(out, cap, "%s/%s", base, sub) >= cap)
        return 0;
    if (stat(base, &sb) != 0 || stat(out, &sd) != 0)
        return 0;
    if (!S_ISDIR(sd.st_mode))
        return 0;
    return sd.st_dev != sb.st_dev;
}

/* Room for `want` bytes plus headroom, on a volume that is not read-only. */
static int volume_fits(const char *root, uint64_t want)
{
    struct statvfs vfs;
    uint64_t avail;

    if (statvfs(root, &vfs) != 0)
        return 0;
    if (vfs.f_flag & ST_RDONLY)
        return 0;
    avail = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    return avail >= want + FREE_MARGIN;
}

static int has_cache_dir(const char *root)
{
    char path[STEM_CACHE_PATH_MAX];
    struct stat st;

    if ((size_t)snprintf(path, sizeof(path), "%s/%s", root, CACHE_ROOT) >=
        sizeof(path))
        return 0;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* LOOKUP AND STORE ASK DIFFERENT QUESTIONS, and conflating them breaks the
 * hybrid case outright.
 *
 * A store asks "which ONE volume does the cache live on" -- there is a single
 * answer and it has to stay stable.
 *
 * A lookup asks "where might THIS ENTRY be", and the answer is plural: the
 * local stick may hold a cache full of other tracks while the stems for the one
 * now playing sit on a linked player's media. Picking a volume and then looking
 * only there would miss it every time.
 *
 * So the two paths are separate below: collect_roots for lookup, store_root for
 * writing. */

/* Append every mounted volume under `base` that holds a cache. Returns the new
 * count. */
static int base_collect(const char *base, char (*roots)[STEM_CACHE_PATH_MAX],
                        int n, int max)
{
    char cand[STEM_CACHE_PATH_MAX];
    struct dirent *de;
    DIR *d = opendir(base);

    if (!d)
        return n;
    while ((de = readdir(d)) != NULL && n < max) {
        if (de->d_name[0] == '.')
            continue;
        if (!is_mounted(base, de->d_name, cand, sizeof(cand)))
            continue;
        if (!has_cache_dir(cand))
            continue;
        snprintf(roots[n], STEM_CACHE_PATH_MAX, "%s", cand);
        n++;
    }
    closedir(d);
    return n;
}

/* The DJ's own first bank, for things that belong to the DECK rather than to a
 * track -- the groove circuit's slot files are the case this exists for.
 *
 * USB only, and the first mount under it: k_media_base[0] IS the first bank by
 * the same argument the base list makes above, that the directory says which
 * slot a volume is in and the device letter does not. A second stick is not
 * searched, because two of them could disagree about what slot 3 is and there
 * is no track to break the tie with -- the cache can answer "the volume holding
 * the track" and this cannot.
 *
 * readdir order is the kernel's, so "first" is only meaningful when one volume
 * is mounted; that is the case this is for. Returns 0 when nothing is. */
int stem_media_first_root(char *out, size_t cap)
{
    char cand[STEM_CACHE_PATH_MAX];
    struct dirent *de;
    DIR *d = opendir(k_media_base[0]);
    int got = 0;

    if (!d)
        return 0;
    while (!got && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (!is_mounted(k_media_base[0], de->d_name, cand, sizeof(cand)))
            continue;
        got = (size_t)snprintf(out, cap, "%s", cand) < cap;
    }
    closedir(d);
    return got;
}

/* Every place an entry could be, in the order worth trying: our own media
 * first, then each linked player's. Local is faster and is the copy we can
 * maintain; remote is read-only but perfectly readable, and a stick carrying
 * cached stems should serve them to a deck reading it over LINK. */
static int collect_roots(char (*roots)[STEM_CACHE_PATH_MAX], int max)
{
    struct dirent *de;
    int n = 0, i;
    DIR *d;

    for (i = 0; i < N_MEDIA_BASE; i++)
        n = base_collect(k_media_base[i], roots, n, max);

    d = opendir(MEDIA_DIR);
    if (!d)
        return n;
    while ((de = readdir(d)) != NULL && n < max) {
        char base[STEM_CACHE_PATH_MAX];

        if (strncmp(de->d_name, REMOTE_PREFIX, sizeof(REMOTE_PREFIX) - 1) != 0)
            continue;
        if ((size_t)snprintf(base, sizeof(base), "%s/%s", MEDIA_DIR,
                             de->d_name) >= sizeof(base))
            continue;
        n = base_collect(base, roots, n, max);
    }
    closedir(d);
    return n;
}

/* Is `path` a file on the volume mounted at `root`? Prefix match on whole path
 * components, so /media/usb/sdb1 does not swallow /media/usb/sdb11. */
static int under_volume(const char *root, const char *path)
{
    size_t n = strlen(root);

    return strncmp(path, root, n) == 0 && path[n] == '/';
}

/* The volume a TRACK lives on, if that volume is one of our own. -1 otherwise.
 *
 * This is the whole of "where does a new entry go", and it is the source that
 * decides -- not a search for somewhere with room.
 *
 * STEMS LIVE BESIDE THE TRACK THEY CAME FROM.
 *
 * A key is computed by opening the track and hashing its bytes (see key_of), so
 * an entry can only ever be FOUND by someone who has the source to hash. Put
 * stems on a volume that does not hold their track and one of two things is
 * true: the track's volume is unreachable, and the entry can never be looked up
 * at all -- or it is reachable, in which case its own cache is reachable too and
 * holds the copy that sits beside the music. Unreachable or redundant; never the
 * entry that gets used.
 *
 * And they cannot be cleaned up either. Identifying an entry means hashing its
 * source, so stems whose track is gone are not merely dead weight, they are dead
 * weight nothing can name. The only way not to accumulate them is not to create
 * them.
 *
 * This is what a deck playing another player's media over LINK would otherwise
 * do: separate a track it is only borrowing, then write 60 MB of stems for it
 * onto the DJ's own stick, where the track itself is not.
 *
 * A DJ with a stick and a card therefore gets a cache on each, which is right
 * rather than a split: each volume carries the stems for its own tracks, so
 * pulling either takes a self-consistent pair away and leaves one behind. */
static int store_root(const char *track_path, char *out, size_t cap,
                      uint64_t want)
{
    char cand[STEM_CACHE_PATH_MAX];
    int i;

    for (i = 0; i < N_MEDIA_BASE; i++) {
        struct dirent *de;
        DIR *d = opendir(k_media_base[i]);

        if (!d)
            continue;
        while ((de = readdir(d)) != NULL) {
            struct statvfs vfs;

            if (de->d_name[0] == '.')
                continue;
            if (!is_mounted(k_media_base[i], de->d_name, cand, sizeof(cand)))
                continue;
            if (!under_volume(cand, track_path))
                continue;

            closedir(d);
            /* From here the volume is decided, so every remaining reason to
             * refuse is reported: this is the DJ's own stick and them not
             * getting a cache on it is worth a line. */
            if (statvfs(cand, &vfs) == 0 && (vfs.f_flag & ST_RDONLY)) {
                MDBG("stem_cache: %s is read-only -> not caching\n", cand);
                return -1;
            }
            if (!volume_fits(cand, want)) {
                MDBG("stem_cache: %s has no room for %llu MB -> not caching\n",
                     cand, (unsigned long long)(want / (1024 * 1024)));
                return -1;
            }
            if ((size_t)snprintf(out, cap, "%s", cand) >= cap)
                return -1;
            if (!has_cache_dir(cand))
                MDBG("stem_cache: starting cache on %s\n", out);
            return 0;
        }
        closedir(d);
    }

    /* Remote media lands here, and so does anything else we are reading but do
     * not hold: the track is someone else's and so are its stems. Whoever owns
     * the volume it came from is the one that caches it, and this deck will find
     * that entry over LINK on a later load -- lookup already searches there. */
    MDBG("stem_cache: %s is not on our media -> its stems are not ours to keep\n",
         track_path);
    return -1;
}

/* Where a key's entry sits under a root, for one separation id. */
static int entry_dir(const char *root, const char *sep_id, const char *key,
                     char *out, size_t cap)
{
    return (size_t)snprintf(out, cap, "%s/%s/%s/%c%c/%s", root, CACHE_ROOT,
                            sep_id, key[0], key[1], key) < cap ? 0 : -1;
}

/* Where a track's entry sits under a given root, for the CURRENT separation
 * id -- the store's question. Creates nothing. */
static int entry_path(const char *root, const char *track_path, int64_t frames,
                      char *out, size_t cap)
{
    char key[20];

    if (!g_stem_sep_id[0])
        return -1;                    /* no server has ever identified itself */
    if (key_of(track_path, frames, key, sizeof(key)) != 0)
        return -1;
    return entry_dir(root, g_stem_sep_id, key, out, cap);
}

/* ---- meta ----------------------------------------------------------------- */

/* The gains the server applied are part of the entry: without them a cached
 * stem plays back at the wrong level, and they are not recoverable from the
 * audio. The frame count rides along as a second, independent check on the
 * alignment the key already guards. */
static int meta_write(const char *dir, int64_t frames, float hg, float vg)
{
    char path[STEM_CACHE_PATH_MAX], buf[128];
    int len, fd;

    if ((size_t)snprintf(path, sizeof(path), "%s/meta", dir) >= sizeof(path))
        return -1;
    len = snprintf(buf, sizeof(buf),
                   "v=%d\nframes=%lld\nharmonics=%.9g\nvocals=%.9g\n",
                   META_VERSION, (long long)frames, (double)hg, (double)vg);
    if (len <= 0)
        return -1;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, buf, (size_t)len) != len) {
        close(fd);
        return -1;
    }
    fsync(fd);
    close(fd);
    return 0;
}

static int meta_read(const char *dir, int64_t *frames, float *hg, float *vg)
{
    char path[STEM_CACHE_PATH_MAX], buf[128];
    ssize_t n;
    char *p;
    int fd, v = 0;

    if ((size_t)snprintf(path, sizeof(path), "%s/meta", dir) >= sizeof(path))
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    if ((p = strstr(buf, "v=")))          v = atoi(p + 2);
    if (v != META_VERSION)
        return -1;
    if ((p = strstr(buf, "frames=")))     *frames = strtoll(p + 7, NULL, 10);
    else return -1;
    if ((p = strstr(buf, "harmonics=")))  *hg = (float)atof(p + 10);
    else return -1;
    if ((p = strstr(buf, "vocals=")))     *vg = (float)atof(p + 7);
    else return -1;
    return 0;
}

/* ---- lookup --------------------------------------------------------------- */

/* How many volumes a lookup will consider. Two local slots plus a handful of
 * linked players is the whole of a realistic booth. */
#define MAX_ROOTS 8

/* Find `stem` in `dir` under any extension we know. Returns 0 and fills `out`. */
static int find_part(const char *dir, const char *stem, char *out, size_t cap)
{
    struct stat st;
    int i;

    for (i = 0; i < N_EXT; i++) {
        if ((size_t)snprintf(out, cap, "%s/%s%s", dir, stem, k_ext[i]) >= cap)
            continue;
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
            return 0;
    }
    out[0] = '\0';
    return -1;
}

/* One entry directory: 0 and `out` filled when it holds a usable pair. */
static int try_entry(const char *dir, int64_t frames, struct stem_cache_entry *out)
{
    int64_t meta_frames = 0;

    if (meta_read(dir, &meta_frames, &out->harmonics_gain,
                  &out->vocals_gain) != 0)
        return -1;
    /* The key already covers the frame count, so a disagreement here means an
     * entry written by something that did not agree with us about what the key
     * means. Skip it rather than reason about it -- another may still be good. */
    if (meta_frames != frames) {
        MDBG("stem_cache: %s frames %lld != %lld, ignoring entry\n",
             dir, (long long)meta_frames, (long long)frames);
        return -1;
    }
    if (find_part(dir, "harmonics", out->harmonics_path,
                  sizeof(out->harmonics_path)) != 0 ||
        find_part(dir, "vocals", out->vocals_path,
                  sizeof(out->vocals_path)) != 0)
        return -1;
    MDBG("stem_cache: HIT %s\n", dir);
    return 0;
}

int stem_cache_lookup(const char *track_path, int64_t frames,
                      struct stem_cache_entry *out)
{
    char roots[MAX_ROOTS][STEM_CACHE_PATH_MAX];
    char dir[STEM_CACHE_PATH_MAX], base[STEM_CACHE_PATH_MAX];
    char key[20];
    int n, i;

    if (!track_path || !track_path[0] || frames <= 0 || !out)
        return -1;
    if (key_of(track_path, frames, key, sizeof(key)) != 0)
        return -1;

    /* Every candidate, not the first volume that happens to hold a cache: the
     * local stick can be full of other tracks while the stems for the one now
     * playing sit on a linked player's media.
     *
     * And under each, EVERY separation id, the current one first. The id only
     * says which model made the pair; the key is the track and the meta is the
     * alignment, so a pair another model made is a pair this track can play.
     * Without this a deck that last spoke to the server under one model could
     * not read what its own stick holds under another, and two linked decks
     * with different ids could not share a cache at all -- offline, that was
     * the whole feature gone. */
    n = collect_roots(roots, MAX_ROOTS);
    for (i = 0; i < n; i++) {
        struct dirent *de;
        DIR *d;

        if (g_stem_sep_id[0] &&
            entry_dir(roots[i], g_stem_sep_id, key, dir, sizeof(dir)) == 0 &&
            try_entry(dir, frames, out) == 0)
            return 0;

        if ((size_t)snprintf(base, sizeof(base), "%s/%s", roots[i], CACHE_ROOT)
                >= sizeof(base) || !(d = opendir(base)))
            continue;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' || !strcmp(de->d_name, g_stem_sep_id))
                continue;
            if (entry_dir(roots[i], de->d_name, key, dir, sizeof(dir)) == 0 &&
                try_entry(dir, frames, out) == 0) {
                closedir(d);
                return 0;
            }
        }
        closedir(d);
    }
    return -1;
}

/* ---- store ---------------------------------------------------------------- */

static int mkdir_p(const char *path)
{
    char buf[STEM_CACHE_PATH_MAX];
    size_t i, len = strlen(path);

    if (len + 1 > sizeof(buf))
        return -1;
    memcpy(buf, path, len + 1);

    /* Start past the leading slash so the loop never tries to mkdir "". */
    for (i = 1; i < len; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST)
            return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* tmpfs -> stick, which is always a cross-device copy: no rename shortcut. */
static int copy_file(const char *src, const char *dst)
{
    char *buf;
    int in, outfd, rc = -1;

    in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    outfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (outfd < 0) {
        close(in);
        return -1;
    }
    buf = malloc(COPY_CHUNK);
    if (!buf)
        goto out;

    for (;;) {
        ssize_t n = read(in, buf, COPY_CHUNK), done = 0;

        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        while (done < n) {
            ssize_t w = write(outfd, buf + done, (size_t)(n - done));

            if (w > 0) {
                done += w;
                continue;
            }
            if (w < 0 && errno == EINTR)
                continue;
            goto out;
        }
    }
    /* A stick can be pulled at any moment, and an entry that is visible but not
     * on the medium is worse than no entry: it would be found, read short, and
     * play as a truncated stem. */
    if (fsync(outfd) != 0)
        goto out;
    rc = 0;

out:
    free(buf);
    close(in);
    close(outfd);
    if (rc != 0)
        unlink(dst);
    return rc;
}

/* Copy `src` into `dir` as `stem` + whatever extension the source carried, so
 * the cached file stays openable by the same reader that would have opened the
 * original. */
static int copy_part(const char *dir, const char *stem, const char *src)
{
    char dst[STEM_CACHE_PATH_MAX];
    const char *dot = strrchr(src, '.');
    const char *ext = dot ? dot : "";

    if ((size_t)snprintf(dst, sizeof(dst), "%s/%s%s", dir, stem, ext) >=
        sizeof(dst))
        return -1;
    return copy_file(src, dst);
}

static void rm_rf_shallow(const char *dir)
{
    char path[STEM_CACHE_PATH_MAX];
    static const char *const names[] = { "meta", "harmonics", "vocals" };
    size_t i;
    int j;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (i == 0) {
            snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
            unlink(path);
            continue;
        }
        for (j = 0; j < N_EXT; j++) {
            snprintf(path, sizeof(path), "%s/%s%s", dir, names[i], k_ext[j]);
            unlink(path);
        }
    }
    rmdir(dir);
}

int stem_cache_store(const char *track_path, int64_t frames,
                     const char *harmonics_src, float harmonics_gain,
                     const char *vocals_src, float vocals_gain)
{
    char final[STEM_CACHE_PATH_MAX], staging[STEM_CACHE_PATH_MAX];
    char parent[STEM_CACHE_PATH_MAX];
    uint64_t want;
    char *slash;

    if (!track_path || !harmonics_src || !vocals_src || frames <= 0)
        return -1;

    /* Ask for exactly what is about to be copied rather than a guess: the two
     * files are right there to stat, and the difference between a WAV pair and
     * a FLAC pair is nearly threefold. */
    {
        struct stat hs, vs;

        if (stat(harmonics_src, &hs) != 0 || stat(vocals_src, &vs) != 0)
            return -1;
        want = (uint64_t)hs.st_size + (uint64_t)vs.st_size;
    }

    {
        char root[STEM_CACHE_PATH_MAX];

        if (store_root(track_path, root, sizeof(root), want) != 0)
            return -1;               /* store_root logged why */
        if (entry_path(root, track_path, frames, final, sizeof(final)) != 0)
            return -1;
    }

    /* The entry has to appear whole or not at all: two files, and a directory
     * holding only `harmonics` looks complete to anything that stats one path
     * at a time. So everything is built beside the destination and the
     * DIRECTORY is renamed into place -- one operation, and a stick pulled at
     * any point before it leaves nothing that will ever be found. */
    if ((size_t)snprintf(staging, sizeof(staging), "%s.incoming-%d", final,
                         (int)getpid()) >= sizeof(staging))
        return -1;

    if ((size_t)snprintf(parent, sizeof(parent), "%s", final) >= sizeof(parent))
        return -1;
    slash = strrchr(parent, '/');
    if (!slash)
        return -1;
    *slash = '\0';

    if (mkdir_p(parent) != 0) {
        /* Read-only media, or a stick with no room. Not an error worth failing
         * the job over: the stems are already in RAM and will play. The tmpfs
         * copy simply becomes the whole of the cache, and dies with the track. */
        MDBG("stem_cache: cannot create %s (errno=%d) -> not caching\n",
             parent, errno);
        return -1;
    }
    rm_rf_shallow(staging);
    if (mkdir(staging, 0755) != 0) {
        MDBG("stem_cache: cannot create %s (errno=%d)\n", staging, errno);
        return -1;
    }

    if (copy_part(staging, "harmonics", harmonics_src) != 0 ||
        copy_part(staging, "vocals", vocals_src) != 0 ||
        meta_write(staging, frames, harmonics_gain, vocals_gain) != 0) {
        MDBG("stem_cache: write failed (errno=%d) -> discarding %s\n",
             errno, staging);
        rm_rf_shallow(staging);
        return -1;
    }

    /* Losing the race against another deck writing the same entry is a success,
     * not a failure: whatever is there is as valid as what we built. */
    if (rename(staging, final) != 0) {
        MDBG("stem_cache: rename -> %s failed (errno=%d)\n", final, errno);
        rm_rf_shallow(staging);
        return -1;
    }
    MDBG("stem_cache: STORED %s\n", final);
    return 0;
}
