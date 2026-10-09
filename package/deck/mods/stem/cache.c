// SPDX-License-Identifier: MIT OR Apache-2.0
/*
 * cache.c - the stem cache on the DJ's own media.  [worker]
 *
 * Lets stems play with no server on the network.
 *
 *   <volume>/mods/stemd-cache/<separation-id>/<ab>/<track-id>/
 *       meta   harmonics.flac (or .wav)   vocals.flac
 *
 * `separation-id` comes from the server and is opaque: backend, model, preset
 * and stemd version in one string, so the server decides what invalidates its
 * output. <ab> is the first byte of the track-id, because exfat directories are
 * a linear scan.
 *
 * The track id is a hash of size + first 64 KiB + last 64 KiB. The sourceId is
 * unusable (it indexes the browse list and changes with sort order), and so is
 * the path, which changes on any reorganisation.
 *
 * The frame count is hashed in: stems are aligned to EP122's own decode
 * including its padding, so a firmware that pads differently would otherwise
 * find a silently misaligned entry, heard as a phase problem.
 *
 * Cached files keep the extension the sidecar wrote and lookup tries each known
 * one, so the server can switch formats (e.g. to FLAC) with no migration.
 */
#include "stem/stem.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* The cache root on each volume, relative to the mount point so it travels with
 * the stick. Under mods/ with everything else the shim puts on a DJ's media
 * (e.g. mods/loops/), keeping the browsed root folder clean.
 *
 * There is deliberately no fallback to the old root path: a stick with the old
 * directory re-separates or is moved by hand. */
#define CACHE_ROOT      "mods/stemd-cache"

/* Headroom left on the volume after a write: a full stick fails the DJ's next
 * export, and exfat needs slack. */
#define FREE_MARGIN     (128ull * 1024 * 1024)

/* Bytes hashed from each end of the file: large enough that two tracks will not
 * share both windows and a byte count, small enough that the read cost is
 * negligible. */
#define KEY_WINDOW      (64 * 1024)

#define COPY_CHUNK      (256 * 1024)

/* Bumped if the meta format changes. An entry with unparseable meta is treated
 * as absent, so old and new shims can share a stick. */
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
 * FNV-1a is enough for a cache key that nobody attacks: with 64 bits and a few
 * thousand tracks per stick, a collision is negligible. */
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
 * The deck can have a USB stick, an SD card, or both, and an entry is written
 * only to the one that holds the track (see store_root). A DJ with both gets a
 * cache on each, so pulling a volume removes its tracks and their stems
 * together. A lookup never has to choose between volumes, since the key is the
 * track's content and only one volume holds that track.
 *
 * Remote media is a source, never a destination
 *
 * A linked player's media appears as /media/player<N>/<slot>, a FuseFilsine
 * mount served over PRO DJ LINK. The remote end stubs every write:
 *
 *     touch /media/player03/usb/.stemtest  ->  No such file or directory
 *
 * so remote volumes are searched on lookup and never used for a store. A stick
 * with cached stems still serves them over LINK, where 61 MB of FLAC takes
 * seconds. The same rule would follow from "stems belong on the track's own
 * volume" even if a linked player became writable.
 *
 * /media/rekordbox is excluded: it is a linked laptop's library, not a mounted
 * volume, and disappears when the laptop closes.
 *
 * The device name does not identify the slot. The slots usually come up as
 *
 *     /dev/sda1 -> /media/usb/sda1        /dev/sdb1 -> /media/sd/sdb1
 *
 * but a USB stick can also land on /media/usb/sdb1. Only the base directory identifies the slot, so
 * this enumerates whatever is mounted under each base. */
static const char *const k_media_base[] = { "/media/usb", "/media/sd" };
#define N_MEDIA_BASE ((int)(sizeof(k_media_base) / sizeof(k_media_base[0])))

/* Linked players mount under /media/<this prefix><N>/<slot>, so the bases are
 * discovered rather than listed. */
#define REMOTE_PREFIX   "player"
#define MEDIA_DIR       "/media"

/* Whether something is mounted on `base`/`sub`. The mount scripts mkdir before
 * mounting and do not always rmdir after, so an empty leftover directory must
 * not count as a volume. Mounted means a different st_dev from the parent. */
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

/* Lookup and store choose volumes differently. A store needs the single volume
 * holding the track (store_root). A lookup must search every candidate
 * (collect_roots): the local stick may hold a cache of other tracks while the
 * stems for the current one are on a linked player's media. */

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

/* The DJ's first bank, for files that belong to the deck rather than to a
 * track (the groove circuit's slot files).
 *
 * USB only, first mount under it: k_media_base[0] is the first bank because the
 * base directory, not the device letter, identifies the slot. A second stick is
 * not searched: two could disagree about slot 3 and there is no track to decide
 * between them.
 *
 * readdir order is the kernel's, so "first" is only meaningful with one volume
 * mounted, which is the intended case. Returns 0 when nothing is mounted. */
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

/* Every root an entry could be under: our own media first (faster, and
 * writable), then each linked player's (read-only, but a stick with cached
 * stems should serve them over LINK). */
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

/* The volume a track lives on, if it is one of our own; -1 otherwise. This
 * alone decides where a new entry goes, not which volume has room.
 *
 * Stems live beside the track they came from. A key is computed by hashing the
 * track's bytes (see key_of), so an entry can only be found by someone who has
 * the source. On any other volume the entry would be either unreachable (the
 * track's volume is gone) or redundant (the track's own cache is reachable),
 * and it could never be cleaned up, since identifying it needs the source.
 *
 * In particular, a deck playing another player's media over LINK must not
 * write 60 MB of stems for a borrowed track onto the DJ's own stick. */
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
            /* The volume is decided; log every remaining reason to refuse,
             * since this is the DJ's own media. */
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

    /* Remote media, or anything else not on our volumes. The deck owning the
     * volume caches it, and lookup finds that entry over LINK. */
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

/* Where a track's entry sits under a given root for the current separation id,
 * as the store needs. Creates nothing. */
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

/* The gains the server applied are stored with the entry, since they cannot be
 * recovered from the audio and a stem without them plays at the wrong level.
 * The frame count is a second check on the alignment the key already guards. */
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

/* How many volumes a lookup considers: two local slots plus a few linked
 * players. */
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
    /* The key already covers the frame count, so a mismatch means the entry was
     * written with a different key scheme. Skip it; another may be good. */
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

    /* Search every root, and under each every separation id, the current one
     * first. The id only says which model made the pair; the key and meta
     * guarantee it fits this track. This lets a deck read entries made under
     * another model and lets linked decks with different ids share a cache,
     * which matters most offline. */
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
    /* A stick can be pulled at any moment; an entry visible but not on the
     * medium would be read short and play as a truncated stem. */
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

/* Copy `src` into `dir` as `stem` plus the source's extension, so the same
 * reader can open it. */
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

    /* Size the request from the actual files: a WAV pair is nearly three times
     * a FLAC pair. */
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

    /* The entry must appear whole or not at all: a directory holding only
     * `harmonics` looks complete to code that stats one path at a time. It is
     * built in a staging directory beside the destination and renamed into
     * place in one operation, so a stick pulled earlier leaves nothing that
     * will be found. */
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
        /* Read-only media or no room. Not fatal: the stems are already in RAM
         * and will play; only the tmpfs copy exists, and it goes with the
         * track. */
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

    /* If another deck already wrote this entry, the rename fails and its copy,
     * equally valid, is kept. */
    if (rename(staging, final) != 0) {
        MDBG("stem_cache: rename -> %s failed (errno=%d)\n", final, errno);
        rm_rf_shallow(staging);
        return -1;
    }
    MDBG("stem_cache: STORED %s\n", final);
    return 0;
}
