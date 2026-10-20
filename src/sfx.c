#include "sfx.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "emby.h"   /* eb_log_line */

/* 48000, not the 44100 UI sounds are usually authored at, because of what the
 * MiSTer's ALSA default device actually is: /etc/asound.conf wraps a
 * `type file` sink on /dev/MrAudio (the FPGA's audio pipe) in a `type rate`
 * converter pinned to 48000, and no quality converter is installed — so
 * anything arriving at another rate is resampled by ALSA's plain linear
 * interpolator. play_audio() already pre-resamples music to 48kHz for exactly
 * this reason (see its lavcresample comment); the clips are simply shipped at
 * 48kHz so the same plugin drops out of the path here too. Stereo because
 * that is what the sink is configured for. */
#define SFX_RATE     48000
#define SFX_CHANNELS 2
#define SFX_PERIOD   512            /* ~10.7 ms — the click's worst-case delay */
#define SFX_LATENCY_US 40000        /* ALSA-chosen buffer, ~4 periods' worth */

/* Held open across a burst of navigation, released shortly after it stops.
 * Holding it permanently is what a UI mixer wants — every click instant —
 * but on this box the sound card is not ours to keep. /dev/MrAudio is a
 * single-minor character device and there is no dmix in the config, so the
 * card is not shareable at all: while mplayer holds it our open simply fails.
 * The device is therefore borrowed, and the cost is that the first click
 * after a pause is a few tens of milliseconds late while every one after it
 * is not. */
#define SFX_IDLE_PERIODS 40         /* ~0.4 s of silence, then hand the card back */
#define SFX_POLL_US      (8 * 1000) /* how soon a click is noticed while closed */

/* Two of these can overlap — a confirm landing over the click that chose it.
 * Both clips are normalised to full scale, so half each is what makes the sum
 * fit. Menu sounds are better a little under the programme material anyway. */
#define SFX_VOICES    4
#define SFX_VOICE_GAIN 0.5

/* Taken out of the picture without a rebuild: `touch
 * /media/fat/misteremby/no-sfx` over ssh. Sound is the one subsystem here that
 * shares a device with the player, so being able to remove it in one command
 * is worth a line. */
#define SFX_KILL_SWITCH "/media/fat/misteremby/no-sfx"

typedef struct {
    int16_t *pcm;      /* interleaved stereo */
    int      frames;
} Clip;

typedef struct {
    int clip;          /* -1 = free */
    int pos;           /* frames played */
} Voice;

static Clip      s_clip[SFX_COUNT];
static Voice     s_voice[SFX_VOICES];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_thread;
static volatile int s_running;
static volatile int s_enabled = 1;
static int       s_have_clips;

/* ── WAV loading ────────────────────────────────────────────────────────
 * Only what ffmpeg writes for `-c:a pcm_s16le -ac 2 -ar 48000`: a RIFF/WAVE
 * with a fmt chunk and a data chunk, in any order, possibly with others
 * (LIST/INFO) in between. Anything else is refused rather than guessed at —
 * a misread header would play as noise at full volume. */
static int wav_load(const char *path, Clip *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fclose(f); return -1;
    }

    int have_fmt = 0;
    for (;;) {
        unsigned char ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t len = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                       ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if (!memcmp(ch, "fmt ", 4) && len >= 16) {
            unsigned char fmt[16];
            if (fread(fmt, 1, 16, f) != 16) break;
            int format   = fmt[0] | (fmt[1] << 8);
            int channels = fmt[2] | (fmt[3] << 8);
            int rate     = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            int bits     = fmt[14] | (fmt[15] << 8);
            if (format != 1 || channels != SFX_CHANNELS ||
                rate != SFX_RATE || bits != 16) {
                eb_log_line("sfx: %s is %d-bit %d ch %d Hz, need 16-bit %d ch %d Hz",
                            path, bits, channels, rate, SFX_CHANNELS, SFX_RATE);
                fclose(f); return -1;
            }
            have_fmt = 1;
            if (len > 16) fseek(f, (long)(len - 16), SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) break;
            int frames = (int)(len / (SFX_CHANNELS * 2));
            if (frames <= 0) break;
            int16_t *pcm = (int16_t *)malloc((size_t)frames * SFX_CHANNELS * 2);
            if (!pcm) break;
            if (fread(pcm, (size_t)SFX_CHANNELS * 2, (size_t)frames, f) != (size_t)frames) {
                free(pcm); break;
            }
            out->pcm = pcm; out->frames = frames;
            fclose(f);
            return 0;
        } else {
            if (fseek(f, (long)len + (len & 1), SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    return -1;
}

/* ── mixing ─────────────────────────────────────────────────────────────── */

static int voices_active(void)
{
    int active = 0;
    pthread_mutex_lock(&s_lock);
    for (int v = 0; v < SFX_VOICES; v++)
        if (s_voice[v].clip >= 0) { active = 1; break; }
    pthread_mutex_unlock(&s_lock);
    return active;
}

static void mix_period(int16_t *dst, int frames)
{
    memset(dst, 0, (size_t)frames * SFX_CHANNELS * 2);

    pthread_mutex_lock(&s_lock);
    for (int v = 0; v < SFX_VOICES; v++) {
        if (s_voice[v].clip < 0) continue;
        const Clip *c = &s_clip[s_voice[v].clip];
        int n = c->frames - s_voice[v].pos;
        if (n > frames) n = frames;
        if (n <= 0) { s_voice[v].clip = -1; continue; }

        const int16_t *src = c->pcm + (size_t)s_voice[v].pos * SFX_CHANNELS;
        for (int i = 0; i < n * SFX_CHANNELS; i++) {
            int sample = dst[i] + (int)(src[i] * SFX_VOICE_GAIN);
            if (sample >  32767) sample =  32767;
            if (sample < -32768) sample = -32768;
            dst[i] = (int16_t)sample;
        }
        s_voice[v].pos += n;
        if (s_voice[v].pos >= c->frames) s_voice[v].clip = -1;
    }
    pthread_mutex_unlock(&s_lock);
}

/* ── ALSA, resolved at runtime ───────────────────────────────────────────
 * dlopen rather than -lasound because the ARM libasound lives on the MiSTer,
 * not in this project's toolchain: linking it would make `make arm` on a
 * clean checkout either fail or — worse — quietly produce a permanently
 * silent binary that still looks like a working build. Resolved here, the
 * build has no new dependency at all and a device without the library just
 * runs silent, which is already how every other failure in this file
 * behaves.
 *
 * Only the eight symbols below are needed, because snd_pcm_set_params does
 * in one call what the hw_params dance does in nine — and it avoids
 * snd_pcm_hw_params_alloca(), a macro that would drag in the library's own
 * struct sizing. The three constants are part of ALSA's public ABI and have
 * held these values for the whole life of libasound 1.x. */
typedef struct _snd_pcm snd_pcm_t;

#define SND_PCM_STREAM_PLAYBACK        0
#define SND_PCM_FORMAT_S16_LE          2
#define SND_PCM_ACCESS_RW_INTERLEAVED  3

static void *s_dl;
static int  (*p_open)(snd_pcm_t **, const char *, int, int);
static int  (*p_set_params)(snd_pcm_t *, int, int, unsigned, unsigned, int, unsigned);
static int  (*p_prepare)(snd_pcm_t *);
static int  (*p_drop)(snd_pcm_t *);
static int  (*p_close)(snd_pcm_t *);
static long (*p_writei)(snd_pcm_t *, const void *, unsigned long);
static int  (*p_recover)(snd_pcm_t *, int, int);
static const char *(*p_strerror)(int);

static int alsa_load(void)
{
    if (s_dl) return 1;
    /* soname, not the bare .so: the development symlink is not present on a
     * stock MiSTer image, the versioned one always is. */
    s_dl = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!s_dl) {
        eb_log_line("sfx: no libasound.so.2 (%s) — running silent", dlerror());
        return 0;
    }
    #define SYM(var, name) do {                                        \
        *(void **)&(var) = dlsym(s_dl, name);                          \
        if (!(var)) {                                                  \
            eb_log_line("sfx: libasound has no %s — running silent", name); \
            dlclose(s_dl); s_dl = NULL; return 0;                      \
        }                                                              \
    } while (0)
    SYM(p_open,       "snd_pcm_open");
    SYM(p_set_params, "snd_pcm_set_params");
    SYM(p_prepare,    "snd_pcm_prepare");
    SYM(p_drop,       "snd_pcm_drop");
    SYM(p_close,      "snd_pcm_close");
    SYM(p_writei,     "snd_pcm_writei");
    SYM(p_recover,    "snd_pcm_recover");
    SYM(p_strerror,   "snd_strerror");
    #undef SYM
    return 1;
}

static snd_pcm_t *s_pcm;

static int pcm_open(void)
{
    snd_pcm_t *pcm = NULL;
    int err = p_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) return err;

    /* soft_resample 0: the clips are already at the sink's rate, and letting
     * ALSA insert its linear converter is the one thing shipping them at
     * 48kHz was meant to avoid. A short buffer on purpose — this stream is
     * silent most of the time, and anything already queued is delay between
     * the keypress and its click. */
    err = p_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                       SFX_CHANNELS, SFX_RATE, 0 /* soft_resample */,
                       SFX_LATENCY_US);
    if (err < 0) { p_close(pcm); return err; }
    if ((err = p_prepare(pcm)) < 0) { p_close(pcm); return err; }

    s_pcm = pcm;
    /* Once: the device opens per burst of sounds, and a line per open would
     * turn the log into a click counter. */
    static int announced;
    if (!announced) {
        announced = 1;
        eb_log_line("sfx: playing at %d Hz, %d-frame period", SFX_RATE, SFX_PERIOD);
    }
    return 0;
}

static void pcm_close(void)
{
    if (!s_pcm) return;
    p_drop(s_pcm);
    p_close(s_pcm);
    s_pcm = NULL;
}

/* Feeds the card continuously — silence when nothing is playing. Writing only
 * during a sound would mean opening or re-preparing the stream on the
 * keypress itself, which is exactly where the delay must not be. */
static void *sfx_thread(void *arg)
{
    (void)arg;
    static int16_t buf[SFX_PERIOD * SFX_CHANNELS];
    int complained = 0;
    int idle = 0;
    int failures = 0;

    while (s_running) {
        if (!s_enabled) {
            pcm_close();
            idle = 0;
            usleep(50 * 1000);
            continue;
        }
        if (!s_pcm && !voices_active()) {
            usleep(SFX_POLL_US);      /* nothing to play and nothing to hold */
            continue;
        }
        if (!s_pcm) {
            int err = pcm_open();
            if (err < 0) {
                if (!complained) {
                    eb_log_line("sfx: no sound (%s) — running silent", p_strerror(err));
                    complained = 1;
                }
                pthread_mutex_lock(&s_lock);
                for (int v = 0; v < SFX_VOICES; v++) s_voice[v].clip = -1;
                pthread_mutex_unlock(&s_lock);
                usleep(1000 * 1000);   /* the card may come back (mplayer exits) */
                continue;
            }
            complained = 0;
            idle = failures = 0;
        }

        mix_period(buf, SFX_PERIOD);
        idle = voices_active() ? 0 : idle + 1;
        if (idle >= SFX_IDLE_PERIODS) { pcm_close(); idle = 0; continue; }

        long n = p_writei(s_pcm, buf, SFX_PERIOD);
        if (n < 0) {
            n = p_recover(s_pcm, (int)n, 1 /* silent */);
            /* snd_pcm_writei blocks, so the loop is self-pacing while it
             * works. Once it stops working there is nothing left to pace it,
             * and a mixer thread spinning on a dead device would take the CPU
             * the video decoder needs — so give up on the device instead. */
            if (n < 0 || ++failures > 8) { pcm_close(); failures = 0; usleep(200 * 1000); }
        } else {
            failures = 0;
        }
    }
    pcm_close();
    return NULL;
}

/* ── public ─────────────────────────────────────────────────────────────── */

void sfx_init(const char *dir)
{
    static const char *name[SFX_COUNT] = { "nav.wav", "confirm.wav" };

    for (int i = 0; i < SFX_VOICES; i++) s_voice[i].clip = -1;

    if (access(SFX_KILL_SWITCH, F_OK) == 0) {
        eb_log_line("sfx: disabled by " SFX_KILL_SWITCH);
        return;
    }

    for (int i = 0; i < SFX_COUNT; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, name[i]);
        if (wav_load(path, &s_clip[i]) == 0) s_have_clips = 1;
        else eb_log_line("sfx: cannot load %s", path);
    }
    if (!s_have_clips) return;

    /* Checked before the thread starts so the "no sound" line lands once at
     * startup, next to the rest of the boot log, rather than the first time
     * the user presses a direction. */
    if (!alsa_load()) return;

    s_running = 1;
    if (pthread_create(&s_thread, NULL, sfx_thread, NULL) != 0) {
        s_running = 0;
        eb_log_line("sfx: cannot start mixer thread — running silent");
    }
}

void sfx_play(int id)
{
    if (!s_running || id < 0 || id >= SFX_COUNT || !s_clip[id].pcm) return;
    if (!s_enabled) return;

    pthread_mutex_lock(&s_lock);
    /* Retrigger rather than layer: holding a direction walks the list faster
     * than the click is long, and overlapping copies of one short sample turn
     * into a rasp instead of a rhythm. */
    int slot = -1;
    for (int v = 0; v < SFX_VOICES; v++) {
        if (s_voice[v].clip == id) { slot = v; break; }
        if (s_voice[v].clip < 0 && slot < 0) slot = v;
    }
    if (slot < 0) slot = 0;             /* all busy with other clips — steal */
    s_voice[slot].clip = id;
    s_voice[slot].pos  = 0;
    pthread_mutex_unlock(&s_lock);
}

void sfx_set_enabled(int on)
{
    if (on == s_enabled) return;
    s_enabled = on;
    if (!on) {
        pthread_mutex_lock(&s_lock);
        for (int v = 0; v < SFX_VOICES; v++) s_voice[v].clip = -1;
        pthread_mutex_unlock(&s_lock);
    }
}

void sfx_shutdown(void)
{
    if (!s_running) return;
    s_running = 0;
    pthread_join(s_thread, NULL);
    for (int i = 0; i < SFX_COUNT; i++) { free(s_clip[i].pcm); s_clip[i].pcm = NULL; }
}
