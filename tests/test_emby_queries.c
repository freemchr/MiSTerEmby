/* Unit tests for eb_build_items_path() — build and run with `make test`.
 *
 * Item-list queries differ by collection type because the server can spend
 * enough time computing unused count fields to exceed MiSTerEmby's request
 * timeout. Compare complete paths so changes to traversal, fields, sorting,
 * paging, user data, or image selection cannot pass unnoticed. */

#include <stdio.h>
#include <string.h>
#include "emby.h"

static int failures;
static int checks;

#define CHECK_PATH(label, got, expected) do {                         \
    checks++;                                                         \
    if (strcmp((got), (expected)) != 0) {                             \
        failures++;                                                   \
        printf("  FAIL %s\n    expected: %s\n    got:      %s\n",    \
               (label), (expected), (got));                           \
    }                                                                 \
} while (0)

static EbConfig config(void)
{
    EbConfig cfg = {0};
    strcpy(cfg.user_id, "user-id");
    return cfg;
}

static void test_movie_query(void)
{
    EbConfig cfg = config();
    char path[512];

    eb_build_items_path(&cfg, "movie-view", "movies", 12, 34,
                        path, sizeof(path));

    CHECK_PATH("movie query", path,
        "/Users/user-id/Items?ParentId=movie-view"
        "&Recursive=true&IncludeItemTypes=Movie"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=12&Limit=34");
}

static void test_music_query(void)
{
    EbConfig cfg = config();
    char path[512];

    eb_build_items_path(&cfg, "music-view", "music", 5, 64,
                        path, sizeof(path));

    CHECK_PATH("music query", path,
        "/Users/user-id/Items?ParentId=music-view"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=5&Limit=64");
}

static void test_music_video_query(void)
{
    EbConfig cfg = config();
    char path[512];

    eb_build_items_path(&cfg, "music-video-view", "musicvideos", 7, 50,
                        path, sizeof(path));

    CHECK_PATH("music-video query", path,
        "/Users/user-id/Items?ParentId=music-video-view"
        "&Recursive=true&IncludeItemTypes=MusicVideo"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=7&Limit=50");
}

static void test_standard_query(void)
{
    EbConfig cfg = config();
    char path[512];
    static const char expected[] =
        "/Users/user-id/Items?ParentId=tv-view"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount,RecursiveItemCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=0&Limit=128";

    eb_build_items_path(&cfg, "tv-view", "tvshows", 0, 128,
                        path, sizeof(path));
    CHECK_PATH("TV uses standard query", path, expected);

    eb_build_items_path(&cfg, "tv-view", "books", 0, 128,
                        path, sizeof(path));
    CHECK_PATH("unknown type uses standard query", path, expected);

    eb_build_items_path(&cfg, "tv-view", NULL, 0, 128,
                        path, sizeof(path));
    CHECK_PATH("missing type uses standard query", path, expected);
}

static void test_input_normalization(void)
{
    EbConfig cfg = config();
    char path[512];

    eb_build_items_path(&cfg, "bad/id?", "music", -7, 9,
                        path, sizeof(path));

    CHECK_PATH("parent ID and start index are normalized", path,
        "/Users/user-id/Items?ParentId=bad_id_"
        "&SortBy=SortName&SortOrder=Ascending"
        "&Fields=ProductionYear,RunTimeTicks,ChildCount"
        "&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=0&Limit=9");
}

static void test_collection_item_types(void)
{
    CHECK_PATH("movie collection item type",
               collection_item_type("movies"), "Movie");
    CHECK_PATH("TV collection item type",
               collection_item_type("tvshows"), "Series");
    CHECK_PATH("music collection item type",
               collection_item_type("music"), "MusicAlbum");
    CHECK_PATH("music-video collection item type",
               collection_item_type("musicvideos"), "MusicVideo");
}

/* Which sources stream with their video untouched, and what the stream URL
 * asks Emby for in each case. */
static void test_direct_stream(void)
{
    struct { const char *label, *codec; int w, h, interlaced; double fps; const char *want; } cases[] = {
        { "SD MPEG-4 (Xvid) copies",       "mpeg4",      720, 390, 0, 23.976, "mpeg4" },
        { "DivX 3 copies",                 "msmpeg4v3",  640, 352, 0, 25.0,   "msmpeg4v3" },
        { "progressive MPEG-2 copies",     "mpeg2video", 720, 480, 0, 23.976, "mpeg2video" },
        { "interlaced DVD transcodes",     "mpeg2video", 720, 576, 1, 25.0,   NULL },
        { "HD MPEG-4 transcodes",          "mpeg4",     1280, 720, 0, 23.976, NULL },
        { "SD H.264 transcodes",           "h264",       720, 400, 0, 25.0,   NULL },
        { "60fps SD transcodes",           "mpeg4",      640, 480, 0, 59.94,  NULL },
        { "unknown size transcodes",       "mpeg4",        0,   0, 0, 0.0,    NULL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        EbItem it = {0};
        strcpy(it.source_video_codec, cases[i].codec);
        it.source_width = cases[i].w;
        it.source_height = cases[i].h;
        it.source_interlaced = cases[i].interlaced;
        it.source_fps = cases[i].fps;
        const char *got = eb_direct_stream_codec(&it);
        checks++;
        if ((got == NULL) != (cases[i].want == NULL) ||
            (got && strcmp(got, cases[i].want) != 0)) {
            failures++;
            printf("  FAIL %s: got %s\n", cases[i].label, got ? got : "(transcode)");
        }
    }

    EbConfig cfg = config();
    strcpy(cfg.server, "http://s:8096");
    strcpy(cfg.token, "tok");
    strcpy(cfg.device_id, "dev");
    strcpy(cfg.tv_mode, "NTSC");
    EbStreamProfile prof = { 720, 576, 12000000 };
    char url[1024];

    eb_stream_url(&cfg, "123", "mediasource_123", "mpeg4", &prof, 0, "ps", -1, -1, url, sizeof(url));
    checks++;
    if (!strstr(url, "&VideoCodec=mpeg4&AllowVideoStreamCopy=true&") || strstr(url, "EnableAutoStreamCopy=false")) {
        failures++; printf("  FAIL direct-stream url: %s\n", url);
    }
    eb_stream_url(&cfg, "123", "mediasource_123", NULL, &prof, 0, "ps", -1, -1, url, sizeof(url));
    checks++;
    if (!strstr(url, "&VideoCodec=mpeg4&AllowVideoStreamCopy=false&EnableAutoStreamCopy=false&")) {
        failures++; printf("  FAIL transcode url: %s\n", url);
    }
    checks++;
    if (!strstr(url, "/emby/Videos/123/stream.ts?Static=false&MediaSourceId=mediasource_123&") ||
        !strstr(url, "&AudioCodec=mp3&AudioChannels=2&AudioSampleRate=48000") ||
        !strstr(url, "&MaxFramerate=30&") || !strstr(url, "&DeviceId=dev") || !strstr(url, "&api_key=tok")) {
        failures++; printf("  FAIL stream url shape: %s\n", url);
    }
}

static void test_markers(void)
{
    EbItem it;
    memset(&it, 0, sizeof(it));
    eb_parse_markers("{\"Chapters\":[{\"StartPositionTicks\":0,\"Name\":\"Chapter 1\"},"
                     "{\"StartPositionTicks\":300000000,\"MarkerType\":\"IntroStart\"},"
                     "{\"StartPositionTicks\":900000000,\"MarkerType\":\"IntroEnd\"},"
                     "{\"StartPositionTicks\":24000000000,\"MarkerType\":\"CreditsStart\"}]}", &it);
    checks++;
    if (it.intro_start_ticks != 300000000 || it.intro_end_ticks != 900000000 ||
        it.credits_start_ticks != 24000000000LL) {
        failures++; printf("  FAIL markers: %lld %lld %lld\n", (long long)it.intro_start_ticks,
                           (long long)it.intro_end_ticks, (long long)it.credits_start_ticks);
    }
    memset(&it, 0, sizeof(it));
    eb_parse_markers("{\"Chapters\":[{\"StartPositionTicks\":600000000,\"MarkerType\":\"IntroEnd\"}]}", &it);
    checks++;
    if (it.intro_start_ticks != 0 || it.intro_end_ticks != 600000000) {
        failures++; printf("  FAIL intro without start\n");
    }
    memset(&it, 0, sizeof(it));
    eb_parse_markers("{\"Chapters\":[{\"StartPositionTicks\":900000000,\"MarkerType\":\"IntroStart\"},"
                     "{\"StartPositionTicks\":300000000,\"MarkerType\":\"IntroEnd\"}]}", &it);
    checks++;
    if (it.intro_end_ticks != 0) { failures++; printf("  FAIL backwards intro kept\n"); }
    memset(&it, 0, sizeof(it));
    eb_parse_markers("{\"Name\":\"no chapters\"}", &it);
    checks++;
    if (it.intro_end_ticks || it.credits_start_ticks) { failures++; printf("  FAIL no-chapters\n"); }
}

int main(void)
{
    test_movie_query();
    test_music_query();
    test_music_video_query();
    test_standard_query();
    test_input_normalization();
    test_collection_item_types();
    test_direct_stream();
    test_markers();

    printf("emby queries: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
