/* HLS playlist parsing for Live TV direct mode (src/hls.c). */
#include <stdio.h>
#include <string.h>
#include "hls.h"

static int checks = 0, failures = 0;
#define CHECK(label, cond) do { checks++; if (!(cond)) { failures++; printf("  FAIL %s\n", label); } } while (0)
#define CHECK_STR(label, got, want) do { checks++; if (strcmp((got), (want)) != 0) { \
    failures++; printf("  FAIL %s\n    got:  %s\n    want: %s\n", label, got, want); } } while (0)

static void test_resolve(void)
{
    char out[512];
    hls_resolve("https://a.example/live/index.m3u8?token=x", "mid/stream.m3u8", out, sizeof(out));
    CHECK_STR("relative, query dropped", out, "https://a.example/live/mid/stream.m3u8");
    hls_resolve("https://a.example/live/index.m3u8", "/seg/1.ts", out, sizeof(out));
    CHECK_STR("host-relative", out, "https://a.example/seg/1.ts");
    hls_resolve("https://a.example/live/index.m3u8", "http://b.example/x.ts", out, sizeof(out));
    CHECK_STR("absolute", out, "http://b.example/x.ts");
    CHECK("too long fails", !hls_resolve("https://a.example/x", "y.ts", out, 10));
}

static const char *MASTER =
    "#EXTM3U\r\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=854x480\r\n"
    "program_mid/stream.m3u8\r\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=640x360\r\n"
    "program_low/stream.m3u8\r\n"
    "#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=426x240\r\n"
    "program_lowest/stream.m3u8\r\n";

static void test_pick(void)
{
    char out[512];
    int w = 0, h = 0;
    CHECK("picks a variant",
          hls_pick_variant(MASTER, "https://a.example/WF/index.m3u8", 640, 360, out, sizeof(out), &w, &h));
    CHECK_STR("largest that fits", out, "https://a.example/WF/program_low/stream.m3u8");
    CHECK("its size", w == 640 && h == 360);

    CHECK("nothing small enough",
          !hls_pick_variant(MASTER, "https://a.example/x.m3u8", 400, 200, out, sizeof(out), &w, &h));
    CHECK("not a master playlist",
          !hls_pick_variant("#EXTM3U\n#EXTINF:6,\na.ts\n", "https://a/x.m3u8", 640, 360,
                            out, sizeof(out), &w, &h));
    CHECK("no RESOLUTION means unknown, skipped",
          !hls_pick_variant("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nv.m3u8\n", "https://a/x.m3u8",
                            640, 360, out, sizeof(out), &w, &h));
    CHECK("separate audio rendition refused",
          !hls_pick_variant("#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",URI=\"audio.m3u8\"\n"
                            "#EXT-X-STREAM-INF:BANDWIDTH=1,RESOLUTION=640x360,AUDIO=\"a\"\nv.m3u8\n",
                            "https://a/x.m3u8", 640, 360, out, sizeof(out), &w, &h));
}

static void test_playable(void)
{
    CHECK("plain TS", hls_media_playable("#EXTM3U\n#EXTINF:6,\na.ts\n"));
    CHECK("METHOD=NONE ok", hls_media_playable("#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:6,\na.ts\n"));
    CHECK("AES refused", !hls_media_playable("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:6,\na.ts\n"));
    CHECK("fMP4 refused", !hls_media_playable("#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:6,\na.m4s\n"));
    CHECK("master isn't media", !hls_media_playable(MASTER));
}

typedef struct { int n; long long seq[8]; char url[8][256]; } Got;
static void got_seg(long long seq, const char *url, void *ctx)
{
    Got *g = ctx;
    if (g->n < 8) { g->seq[g->n] = seq; snprintf(g->url[g->n], 256, "%s", url); g->n++; }
}

static void test_segments(void)
{
    Got g = { 0 };
    int ended = -1;
    int target = hls_media_segments(
        "#EXTM3U\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:1200\n"
        "#EXTINF:6.0,\nseg1200.ts\n#EXTINF:6.0,\nseg1201.ts\n#EXTINF:6.0,\n/abs/seg1202.ts\n",
        "https://a.example/live/low.m3u8", got_seg, &g, &ended);
    CHECK("target duration", target == 6);
    CHECK("three segments", g.n == 3);
    CHECK("sequence numbers", g.seq[0] == 1200 && g.seq[2] == 1202);
    CHECK_STR("relative segment", g.url[1], "https://a.example/live/seg1201.ts");
    CHECK_STR("host-relative segment", g.url[2], "https://a.example/abs/seg1202.ts");
    CHECK("live, not ended", ended == 0);
    Got g2 = { 0 };
    hls_media_segments("#EXTM3U\n#EXTINF:6,\na.ts\n#EXT-X-ENDLIST\n", "https://a/x.m3u8", got_seg, &g2, &ended);
    CHECK("ENDLIST noticed", ended == 1);
}

int main(void)
{
    test_resolve();
    test_pick();
    test_playable();
    test_segments();
    printf("hls: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
