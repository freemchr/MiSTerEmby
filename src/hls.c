/* Live TV direct mode — see hls.h. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hls.h"
#include "emby.h"

#define HLS_URL_MAX 2048

/* ── playlist parsing (pure; covered by tests/test_hls.c) ───────────────── */

int hls_resolve(const char *base, const char *ref, char *out, size_t outsz)
{
    if (!strncasecmp(ref, "http://", 7) || !strncasecmp(ref, "https://", 8))
        return snprintf(out, outsz, "%s", ref) < (int)outsz;

    /* The base without its query string, so "a/b.m3u8?token=x" resolves
     * "c.ts" to "a/c.ts". */
    char b[HLS_URL_MAX];
    if (snprintf(b, sizeof(b), "%s", base) >= (int)sizeof(b)) return 0;
    b[strcspn(b, "?#")] = '\0';

    if (ref[0] == '/') {
        /* Host-relative: keep scheme://host only. */
        char *p = strstr(b, "://");
        if (!p) return 0;
        char *slash = strchr(p + 3, '/');
        if (slash) *slash = '\0';
        return snprintf(out, outsz, "%s%s", b, ref) < (int)outsz;
    }
    char *last = strrchr(b, '/');
    char *host = strstr(b, "://");
    if (!last || !host || last < host + 3) return 0;
    last[1] = '\0';
    return snprintf(out, outsz, "%s%s", b, ref) < (int)outsz;
}

/* Copies the next line of text into line (without its \r\n) and returns a
 * pointer past it, or NULL at the end. */
static const char *next_line(const char *p, char *line, size_t sz)
{
    if (!p || !*p) return NULL;
    size_t n = strcspn(p, "\r\n");
    size_t c = n < sz - 1 ? n : sz - 1;
    memcpy(line, p, c);
    line[c] = '\0';
    p += n;
    while (*p == '\r' || *p == '\n') p++;
    return p;
}

int hls_pick_variant(const char *text, const char *base, int max_w, int max_h,
                     char *out, size_t outsz, int *w, int *h)
{
    if (!text || !strstr(text, "#EXT-X-STREAM-INF")) return 0;
    /* Audio carried separately would need a second stream mplayer can't be
     * handed through one FIFO. */
    const char *m = text;
    while ((m = strstr(m, "#EXT-X-MEDIA:")) != NULL) {
        const char *eol = m + strcspn(m, "\r\n");
        const char *t = strstr(m, "TYPE=AUDIO");
        const char *u = strstr(m, "URI=");
        if (t && t < eol && u && u < eol) return 0;
        m = eol;
    }

    char line[HLS_URL_MAX];
    long best_area = -1, best_bw = -1;
    int  pending = 0, pw = 0, ph = 0;
    long pbw = 0;
    const char *p = text;
    while ((p = next_line(p, line, sizeof(line))) != NULL) {
        if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
            pending = 1; pw = ph = 0; pbw = 0;
            const char *r = strstr(line, "RESOLUTION=");
            if (r) sscanf(r + 11, "%dx%d", &pw, &ph);
            const char *bw = strstr(line, "BANDWIDTH=");
            if (bw) pbw = strtol(bw + 10, NULL, 10);
            continue;
        }
        if (line[0] == '#' || line[0] == '\0') continue;
        if (pending) {
            pending = 0;
            if (pw <= 0 || ph <= 0 || pw > max_w || ph > max_h) continue;
            long area = (long)pw * ph;
            if (area > best_area || (area == best_area && pbw > best_bw)) {
                if (!hls_resolve(base, line, out, outsz)) continue;
                best_area = area; best_bw = pbw;
                if (w) *w = pw;
                if (h) *h = ph;
            }
        }
    }
    return best_area > 0;
}

int hls_media_playable(const char *text)
{
    if (!text || !strstr(text, "#EXTINF")) return 0;
    if (strstr(text, "#EXT-X-MAP")) return 0;          /* fragmented MP4 */
    const char *k = text;
    while ((k = strstr(k, "#EXT-X-KEY:")) != NULL) {
        const char *eol = k + strcspn(k, "\r\n");
        const char *none = strstr(k, "METHOD=NONE");
        if (!none || none > eol) return 0;              /* encrypted */
        k = eol;
    }
    return 1;
}

int hls_media_segments(const char *text, const char *base, hls_segment_fn fn, void *ctx,
                       int *ended)
{
    if (ended) *ended = 0;
    char line[HLS_URL_MAX], url[HLS_URL_MAX];
    long long seq = 0;
    int target = 0, in_seg = 0;
    const char *p = text;
    while ((p = next_line(p, line, sizeof(line))) != NULL) {
        if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) seq = strtoll(line + 22, NULL, 10);
        else if (!strncmp(line, "#EXT-X-TARGETDURATION:", 22)) target = atoi(line + 22);
        else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) { if (ended) *ended = 1; }
        else if (!strncmp(line, "#EXTINF", 7)) in_seg = 1;
        else if (line[0] != '#' && line[0] != '\0' && in_seg) {
            in_seg = 0;
            if (hls_resolve(base, line, url, sizeof(url))) fn(seq, url, ctx);
            seq++;
        }
    }
    return target;
}

int hls_probe(const char *master_url, char *out, size_t outsz, int *w, int *h)
{
    char eff[HLS_URL_MAX];
    char *master = eb_fetch_url(master_url, 10, eff, sizeof(eff));
    if (!master) return 0;
    int ok = hls_pick_variant(master, eff, HLS_MAX_W, HLS_MAX_H, out, outsz, w, h);
    free(master);
    if (!ok) return 0;

    char *media = eb_fetch_url(out, 10, eff, sizeof(eff));
    if (!media) return 0;
    ok = hls_media_playable(media);
    free(media);
    /* The variant's own URL after redirects, so the feeder resolves its
     * segments against where it really lives. */
    if (ok && eff[0]) snprintf(out, outsz, "%s", eff);
    return ok;
}

/* ── the feeder ─────────────────────────────────────────────────────────── */

/* Segments this far back from the live edge are where playback starts: a
 * little in hand for the network, without starting minutes in the past. */
#define HLS_START_BEHIND 3
#define HLS_SEGS 32

/* Each feed has its own state, so stopping one never waits on it: the
 * thread may be inside a playlist fetch (up to 10s), and stop has to come
 * back at once. The thread notices the flag and frees its own state. */
typedef struct {
    volatile int    stop;
    pthread_mutex_t lock;
    pid_t           curl;                    /* the segment download in flight */
    char            media_url[HLS_URL_MAX];
    char            fifo[256];
    long long       seq[HLS_SEGS];
    char            url[HLS_SEGS][HLS_URL_MAX];
    int             n;
} Feed;

/* The current feed. g_feed_lock guards it and every feed's lifetime: a feed
 * frees itself only while holding it, so hls_stop can never touch one that
 * has just gone, and a feed that ends on its own clears the pointer. */
static Feed           *g_feed = NULL;
static pthread_mutex_t g_feed_lock = PTHREAD_MUTEX_INITIALIZER;

static void collect(long long seq, const char *url, void *ctx)
{
    Feed *f = ctx;
    if (f->n >= HLS_SEGS) {                     /* keep the newest */
        memmove(&f->seq[0], &f->seq[1], sizeof(f->seq[0]) * (HLS_SEGS - 1));
        memmove(&f->url[0], &f->url[1], sizeof(f->url[0]) * (HLS_SEGS - 1));
        f->n = HLS_SEGS - 1;
    }
    f->seq[f->n] = seq;
    snprintf(f->url[f->n], HLS_URL_MAX, "%s", url);
    f->n++;
}

/* Sleeps up to ms, waking early when told to stop. */
static void nap(Feed *f, int ms)
{
    for (int t = 0; t < ms && !f->stop; t += 50) usleep(50000);
}

static int download_into(Feed *f, const char *url, int fd)
{
    pthread_mutex_lock(&f->lock);
    pid_t pid = f->stop ? -1 : eb_spawn_url_to_fd(url, fd, 30);
    f->curl = pid;
    pthread_mutex_unlock(&f->lock);
    if (pid <= 0) return 0;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    pthread_mutex_lock(&f->lock);
    f->curl = -1;
    pthread_mutex_unlock(&f->lock);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void *feeder(void *arg)
{
    Feed *f = arg;
    /* The FIFO's write end, opened without blocking until mplayer has the
     * read end open — a plain open() would hang here for good if mplayer
     * never starts. */
    int fd = -1;
    for (int tries = 0; fd < 0 && !f->stop && tries < 600; tries++) {
        fd = open(f->fifo, O_WRONLY | O_NONBLOCK);
        if (fd < 0) usleep(50000);
    }
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);

    long long last = -1;
    int failures = 0;
    while (fd >= 0 && !f->stop) {
        char eff[HLS_URL_MAX];
        char *text = eb_fetch_url(f->media_url, 10, eff, sizeof(eff));
        if (!text) {
            if (++failures > 5) break;
            nap(f, 1000);
            continue;
        }
        f->n = 0;
        int ended = 0;
        int target = hls_media_segments(text, eff[0] ? eff : f->media_url, collect, f, &ended);
        free(text);
        if (target <= 0) target = 6;

        int first = 0;
        if (last < 0) first = f->n > HLS_START_BEHIND ? f->n - HLS_START_BEHIND : 0;
        int fed = 0;
        for (int i = first; i < f->n && !f->stop; i++) {
            if (f->seq[i] <= last) continue;
            if (!download_into(f, f->url[i], fd)) {
                /* A failed segment is skipped, not retried: on a live
                 * stream it's already history. Several in a row (or mplayer
                 * gone, which fails every write) ends the feed. */
                if (++failures > 5) { f->stop = 1; break; }
            } else {
                failures = 0;
            }
            last = f->seq[i];
            fed++;
        }
        if (ended && !fed) break;
        /* New segments appear once per target duration; look again after
         * half of one, sooner when nothing was new. */
        nap(f, fed ? target * 500 : target * 250 + 250);
    }
    if (fd >= 0) close(fd);
    pthread_mutex_lock(&g_feed_lock);
    if (g_feed == f) g_feed = NULL;
    pthread_mutex_destroy(&f->lock);
    free(f);
    pthread_mutex_unlock(&g_feed_lock);
    return NULL;
}

int hls_start(const char *media_url, const char *fifo_path)
{
    hls_stop();
    Feed *f = calloc(1, sizeof(*f));
    if (!f) return 0;
    pthread_mutex_init(&f->lock, NULL);
    f->curl = -1;
    snprintf(f->media_url, sizeof(f->media_url), "%s", media_url);
    snprintf(f->fifo, sizeof(f->fifo), "%s", fifo_path);
    pthread_t t;
    if (pthread_create(&t, NULL, feeder, f) != 0) {
        pthread_mutex_destroy(&f->lock);
        free(f);
        return 0;
    }
    pthread_detach(t);
    pthread_mutex_lock(&g_feed_lock);
    g_feed = f;
    pthread_mutex_unlock(&g_feed_lock);
    return 1;
}

void hls_stop(void)
{
    pthread_mutex_lock(&g_feed_lock);
    Feed *f = g_feed;
    g_feed = NULL;
    if (f) {
        pthread_mutex_lock(&f->lock);
        f->stop = 1;
        if (f->curl > 0) kill(f->curl, SIGKILL);
        pthread_mutex_unlock(&f->lock);
    }
    pthread_mutex_unlock(&g_feed_lock);
}
