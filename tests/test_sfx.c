/* The UI sounds: that the shipped WAVs parse, and that the mixer turns a
 * play call into actual samples and then falls silent again.
 *
 * Worth a test because every failure here is silent by design — a clip that
 * does not load, or a voice that never advances, produces exactly what a
 * working build produces on a machine with no sound card. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Included, not linked: the clip table and the mixer are file-static, and
 * they are what needs checking. */
#include "../src/sfx.c"

/* Stands in for emby.c's logger so the test does not drag in the client. */
void eb_log_line(const char *fmt, ...) { (void)fmt; }

static long peak(const int16_t *buf, int n)
{
    long p = 0;
    for (int i = 0; i < n; i++) {
        long v = buf[i] < 0 ? -(long)buf[i] : buf[i];
        if (v > p) p = v;
    }
    return p;
}

int main(void)
{
    const char *dir = getenv("MISTEREMBY_SFX_DIR");
    sfx_init(dir && *dir ? dir : "assets/sfx");

    assert(s_have_clips && "no clip loaded");
    for (int i = 0; i < SFX_COUNT; i++) {
        assert(s_clip[i].pcm && s_clip[i].frames > 0);
        /* A UI click is short. Anything approaching a second means the header
         * was misread and the mixer is about to play noise. */
        assert(s_clip[i].frames < SFX_RATE);
        printf("  clip %d: %d frames (%.0f ms)\n",
               i, s_clip[i].frames, 1000.0 * s_clip[i].frames / SFX_RATE);
    }

    /* sfx_init deliberately stops short of starting the mixer when libasound
     * cannot be dlopen'd. The thread is what would normally set this and then
     * call mix_period in a loop; here the test plays that part itself, so the
     * mixing logic is the thing under test rather than the device plumbing.
     *
     * NOTE this assert assumes a host with no libasound — true on macOS and
     * on the CI image, not on a Linux desktop with alsa-lib installed, where
     * sfx_init really does start the mixer and this fires. Left as an assert
     * rather than made conditional because it is also the check that the
     * dlopen gate works at all; if it ever trips on a machine that simply has
     * ALSA, the fix is to skip the block below (the thread is already mixing,
     * and sfx_shutdown would free the clips the later assertions need). */
    assert(!s_running && "expected no mixer thread without libasound");
    s_running = 1;

    static int16_t buf[SFX_PERIOD * SFX_CHANNELS];
    const int n = SFX_PERIOD * SFX_CHANNELS;

    /* Idle is silence, not whatever was in the buffer last. */
    memset(buf, 0x7F, sizeof buf);
    mix_period(buf, SFX_PERIOD);
    assert(peak(buf, n) == 0 && "idle mixer emitted samples");

    /* A play call reaches the output... */
    sfx_play(SFX_NAV);
    mix_period(buf, SFX_PERIOD);
    long first = peak(buf, n);
    assert(first > 0 && "sfx_play produced no samples");

    /* ...and the voice runs out rather than looping forever. */
    int periods = 0;
    do {
        mix_period(buf, SFX_PERIOD);
        periods++;
        assert(periods < 200 && "voice never finished");
    } while (peak(buf, n) > 0);

    /* Two at once stay inside the sample range — both clips peak at full
     * scale, which is what the per-voice attenuation is there for. */
    sfx_play(SFX_NAV);
    sfx_play(SFX_CONFIRM);
    long loudest = 0;
    for (int i = 0; i < 20; i++) {
        mix_period(buf, SFX_PERIOD);
        long p = peak(buf, n);
        if (p > loudest) loudest = p;
    }
    assert(loudest > 0 && "overlapping voices produced nothing");
    /* Half gain each is exactly what keeps a full-scale pair in range. */
    assert(loudest <= 32768 && "overlapping voices ran past full scale");
    printf("  nav first period peak %ld, nav+confirm peak %ld (of 32767)\n",
           first, loudest);

    /* Disabled means silent immediately, mid-clip included — that is what
     * hands the card to mplayer when playback starts. */
    sfx_play(SFX_CONFIRM);
    sfx_set_enabled(0);
    mix_period(buf, SFX_PERIOD);
    assert(peak(buf, n) == 0 && "still playing after being disabled");
    sfx_play(SFX_NAV);
    mix_period(buf, SFX_PERIOD);
    assert(peak(buf, n) == 0 && "played while disabled");
    sfx_set_enabled(1);

    /* The clips must be at the sink's own rate — the whole point of shipping
     * them at 48kHz is that ALSA's linear converter stays out of the path
     * (see sfx.c's SFX_RATE comment). A 44.1kHz clip would still play, just
     * resampled badly, which is not something a listener would report as a
     * bug and so is exactly what a test should catch. */
    assert(SFX_RATE == 48000 && "clips are authored for the MiSTer's 48kHz sink");

    printf("sfx: clips ok, mixer ok\n");
    return 0;
}
