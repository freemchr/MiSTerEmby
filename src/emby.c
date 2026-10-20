#include "emby.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <stddef.h>
#include <ctype.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

/* Absolute path so a planted "curl" earlier in PATH can't be run in our
 * place — this process is root on a MiSTer. /usr/bin/curl is where the
 * stock image ships it (also symlinked at /bin/curl); if it's ever
 * missing, execv fails and the request fails cleanly, same as any other
 * transport error. */
#define CURL_BIN "/usr/bin/curl"

/* Hard ceiling on a single captured response. Legit server JSON for one
 * page of a large library is well under this; the cap only ever trips on a
 * runaway/hostile stream, which would otherwise grow the heap until the
 * kernel OOM-kills something. A capped request just fails and the caller
 * falls back the same way it does for any dropped connection. */
#define EB_MAX_RESPONSE (32 * 1024 * 1024)
/* Client/Device/Version identify us to the server's own Dashboard → Devices
 * list — without them the server had nothing to go on beyond the bare token
 * and showed up as a generic/guessed name instead of "MiSTerEmby". DeviceId is
 * appended separately by eb_auth_header, since it's per-install rather than a
 * compile-time constant (see EbConfig.device_id for why that matters). */
/* Every REST route lives under this prefix on an Emby server. The command
 * socket is the one exception (see session.c). */
#define EB_API_BASE "/emby"

#define EB_CLIENT_HEADERS \
    "Client=\"MiSTerEmby\", Device=\"MiSTer FPGA\", Version=\"" APP_VERSION "\""

/* There is deliberately no lock in this file any more. Each fetch now reads
 * into its own heap buffer instead of a function-local "static char
 * buf[256K]", so two threads calling in at once (the home screen's
 * background cover prefetch does exactly that) no longer share anything
 * mutable. The mutex that used to serialize every request existed solely to
 * protect those static buffers — dropping it lets the prefetch thread
 * actually run in parallel with the main thread instead of taking turns. */

/* Ids and image tags below (item_id, series_id, season_id, parent_id, the
 * ImageTags.Primary hash) come from server JSON and get embedded into
 * popen()/system() shell command lines. Real server ids/tags are GUIDs or
 * hex strings, so a strict allowlist costs nothing against a real server but
 * closes off shell-metacharacter injection from a malicious/compromised one. */
static void eb_sanitize_id(const char *in, char *out, int outlen)
{
    int i = 0;
    for (; in && in[i] && i < outlen - 1; i++) {
        char c = in[i];
        out[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-') ? c : '_';
    }
    out[i] = '\0';
}

/* ── text for an ASCII-only display ──────────────────────────────────────── */

/* Normalises UTF-8 for the on-screen bitmap font. The font now covers ASCII
 * (0x00-0x7F) AND Latin-1 (0x00A0-0x00FF, see font8x8_ext_latin), so accented
 * Latin is kept as-is; only code points beyond that (Latin Extended-A, CJK,
 * Cyrillic, exotic punctuation) are folded to an ASCII approximation or a
 * single '?'.
 *
 * This became necessary the moment escapes were decoded properly. The server
 * escapes every non-ASCII character as \uXXXX, and the old scanner emitted
 * the escape's own letters verbatim — so "Amélie" reached the screen as
 * "Amu00e9lie". Decoding it correctly yields real UTF-8, which without this
 * would render as "Am??lie": more honest, but no more readable. Folding to
 * the base letter gives "Amelie", which is what someone actually wants to
 * see on a CRT.
 *
 * Accents are dropped rather than approximated with digraphs, except where a
 * digraph IS the conventional transliteration (ß→ss, Æ→AE). Anything with no
 * sensible ASCII form collapses to a single '?' per run, so a CJK title
 * reads as "?" rather than a wall of one '?' per byte. */
static void append_ascii(char *out, int outlen, int *pos, const char *s)
{
    while (*s && *pos < outlen - 1) out[(*pos)++] = *s++;
}

static const char *fold_codepoint(unsigned cp)
{
    /* Latin-1 Supplement, in code-point order from U+00C0. */
    static const char *const latin1[] = {
        "A","A","A","A","A","A","AE","C","E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","x","O","U","U","U","U","Y","Th","ss",
        "a","a","a","a","a","a","ae","c","e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","/","o","u","u","u","u","y","th","y"
    };
    /* Latin Extended-A, U+0100..U+017F — base letters only; the pattern is
     * regular enough that a flat table is clearer than per-range rules. */
    static const char *const latin_a[] = {
        "A","a","A","a","A","a","C","c","C","c","C","c","C","c","D","d",
        "D","d","E","e","E","e","E","e","E","e","E","e","G","g","G","g",
        "G","g","G","g","H","h","H","h","I","i","I","i","I","i","I","i",
        "I","i","IJ","ij","J","j","K","k","k","L","l","L","l","L","l","L",
        "l","L","l","N","n","N","n","N","n","n","N","n","O","o","O","o",
        "O","o","OE","oe","R","r","R","r","R","r","S","s","S","s","S","s",
        "S","s","T","t","T","t","T","t","U","u","U","u","U","u","U","u",
        "U","u","U","u","W","w","Y","y","Y","Z","z","Z","z","Z","z","s"
    };

    if (cp >= 0xC0 && cp <= 0xFF) return latin1[cp - 0xC0];
    if (cp >= 0x100 && cp <= 0x17F) return latin_a[cp - 0x100];

    switch (cp) {
    case 0xA0: return " ";           /* non-breaking space */
    case 0xAB: return "\"";          /* « */
    case 0xBB: return "\"";          /* » */
    case 0x2010: case 0x2011:
    case 0x2012: case 0x2013:
    case 0x2014: case 0x2015: return "-";
    case 0x2018: case 0x2019:
    case 0x201A: case 0x201B: return "'";
    case 0x201C: case 0x201D:
    case 0x201E: case 0x201F: return "\"";
    case 0x2026: return "...";
    case 0x2032: return "'";
    case 0x2033: return "\"";
    case 0x20AC: return "EUR";
    case 0x2122: return "TM";
    default: return NULL;
    }
}

/* Decodes one UTF-8 sequence at *s, advancing it. Returns the code point, or
 * 0xFFFD for an invalid sequence (consuming one byte so it always makes
 * progress). */
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p;

    int extra;
    unsigned cp;
    if      (c < 0x80)         { *s += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                       { *s += 1; return 0xFFFD; }

    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += extra + 1;
    return cp;
}

void eb_text_to_display(const char *utf8, char *out, int outlen)
{
    if (!out || outlen <= 0) return;
    out[0] = '\0';
    if (!utf8) return;

    int pos = 0;
    int last_was_unknown = 0;
    const char *s = utf8;

    while (*s && pos < outlen - 1) {
        unsigned cp = utf8_next(&s);

        if (cp < 0x80) {
            /* Newlines inside an overview would break single-line layout;
             * the previous implementation flattened them to spaces too. */
            out[pos++] = (cp == '\n' || cp == '\r' || cp == '\t') ? ' ' : (char)cp;
            last_was_unknown = 0;
            continue;
        }

        if (cp >= 0xA0 && cp <= 0xFF) {
            /* The on-screen font now has Latin-1 glyphs (font8x8_ext_latin),
             * so keep these as real UTF-8 instead of folding to bare ASCII —
             * "Ressurreição" stays "Ressurreição", not "Ressurreicao". */
            if (pos < outlen - 2) {
                out[pos++] = (char)(0xC0 | (cp >> 6));
                out[pos++] = (char)(0x80 | (cp & 0x3F));
            }
            last_was_unknown = 0;
            continue;
        }

        const char *rep = fold_codepoint(cp);
        if (rep) {
            append_ascii(out, outlen, &pos, rep);
            last_was_unknown = 0;
        } else if (!last_was_unknown) {
            out[pos++] = '?';
            last_was_unknown = 1;   /* collapse a run into a single '?' */
        }
    }
    out[pos] = '\0';
}

/* json_copy_str + eb_text_to_display in one step — every string that ends up
 * on screen goes through this, so none of them can skip the fold. */
static int copy_display_str(const JsonDoc *doc, const JsonNode *from,
                             const char *path, char *out, int outlen)
{
    const JsonNode *n = json_find(doc, from, path);
    const char *s = json_as_str(n, NULL);
    if (!s) { if (outlen > 0) out[0] = '\0'; return 0; }
    eb_text_to_display(s, out, outlen);
    return 1;
}

/* Ids and image tags are opaque hex/GUID strings that never get displayed —
 * copy them verbatim, no folding. */
static int copy_raw_str(const JsonDoc *doc, const JsonNode *from,
                         const char *path, char *out, int outlen)
{
    return json_copy_str(doc, from, path, out, outlen);
}

/* ── item parsing ────────────────────────────────────────────────────────── */

/* Fills in the fields shared by a browse-list row and a full single-item
 * fetch. Does NOT touch cast — that's only ever populated by
 * eb_get_item_details() via parse_cast(), since it needs Fields=People
 * (an extra cost we don't want on every row of every browse list).
 *
 * `item` is the item's own object node, so every lookup below is scoped to
 * it. That scoping is the point: these same field names (Name, Type, Id)
 * also appear inside the nested People and MediaStreams arrays, and the
 * previous flat text search found whichever copy happened to come first in
 * the byte stream. It got the right answer only because the server's C# DTO
 * declares Type before People — an ordering no one upstream knows is load
 * bearing. */
static void parse_item_fields(const JsonDoc *doc, const JsonNode *item, EbItem *it)
{
    memset(it, 0, sizeof(*it));
    it->index_number = -1;

    copy_raw_str    (doc, item, "Id",       it->id,       sizeof(it->id));
    copy_display_str(doc, item, "Name",     it->name,     sizeof(it->name));
    copy_display_str(doc, item, "Overview", it->overview, sizeof(it->overview));
    copy_raw_str    (doc, item, "ImageTags.Primary", it->image_tag, sizeof(it->image_tag));
    it->community_rating = json_as_dbl(json_find(doc, item, "CommunityRating"), 0.0);

    strncpy(it->image_item_id,    it->id, sizeof(it->image_item_id) - 1);
    strncpy(it->backdrop_item_id, it->id, sizeof(it->backdrop_item_id) - 1);
    strncpy(it->logo_item_id,     it->id, sizeof(it->logo_item_id) - 1);

    /* A track with no embedded art of its own (ImageTags.Primary empty —
     * common, plenty of files just don't have one) still has its album's
     * cover available directly on its own DTO as AlbumId/
     * AlbumPrimaryImageTag — confirmed on a real server (a Daft Punk album
     * with no per-track embedded art: tracks showed no cover client-side
     * until this fallback, other albums whose files DO have embedded art
     * were unaffected). Same pattern as the Logo/Backdrop fallback below. */
    if (!it->image_tag[0]) {
        if (copy_raw_str(doc, item, "AlbumPrimaryImageTag", it->image_tag, sizeof(it->image_tag)))
            copy_raw_str(doc, item, "AlbumId", it->image_item_id, sizeof(it->image_item_id));
    }

    /* Episodes/seasons normally don't carry their own Logo/BackdropImageTags —
     * that art lives on the series, exposed via ParentLogoImageTag/
     * ParentBackdropImageTags (+ ParentLogoItemId/ParentBackdropItemId to say
     * which item to actually fetch it from). Confirmed on a real server: an
     * episode's own ImageTags has no "Logo" key and BackdropImageTags is
     * empty, which is why the info screen for any TV episode never showed a
     * backdrop/logo before this fallback — not specific to one show. */
    if (!copy_raw_str(doc, item, "ImageTags.Logo", it->logo_tag, sizeof(it->logo_tag))) {
        if (copy_raw_str(doc, item, "ParentLogoImageTag", it->logo_tag, sizeof(it->logo_tag)))
            copy_raw_str(doc, item, "ParentLogoItemId", it->logo_item_id, sizeof(it->logo_item_id));
    }
    if (!copy_raw_str(doc, item, "BackdropImageTags[0]", it->backdrop_tag, sizeof(it->backdrop_tag))) {
        if (copy_raw_str(doc, item, "ParentBackdropImageTags[0]", it->backdrop_tag, sizeof(it->backdrop_tag)))
            copy_raw_str(doc, item, "ParentBackdropItemId", it->backdrop_item_id, sizeof(it->backdrop_item_id));
    }

    const char *type_buf = json_as_str(json_find(doc, item, "Type"), "");
    if      (!strcmp(type_buf, "Series"))       it->type = EB_TYPE_SERIES;
    else if (!strcmp(type_buf, "Season"))        it->type = EB_TYPE_SEASON;
    else if (!strcmp(type_buf, "Episode"))       it->type = EB_TYPE_EPISODE;
    else if (!strcmp(type_buf, "Movie"))         it->type = EB_TYPE_MOVIE;
    else if (!strcmp(type_buf, "MusicVideo"))    it->type = EB_TYPE_MUSIC_VIDEO;
    else if (!strcmp(type_buf, "MusicArtist"))   it->type = EB_TYPE_ARTIST;
    else if (!strcmp(type_buf, "MusicAlbum"))    it->type = EB_TYPE_ALBUM;
    else if (!strcmp(type_buf, "Audio"))         it->type = EB_TYPE_TRACK;
    else if (!strcmp(type_buf, "BoxSet"))        it->type = EB_TYPE_FOLDER;   /* a collection */
    else if (!strcmp(type_buf, "CollectionFolder") ||
             !strcmp(type_buf, "Folder"))        it->type = EB_TYPE_FOLDER;
    else                                          it->type = EB_TYPE_OTHER;

    if (it->type == EB_TYPE_TRACK) {
        copy_display_str(doc, item, "Album",       it->album,  sizeof(it->album));
        copy_display_str(doc, item, "AlbumArtist", it->artist, sizeof(it->artist));
    }
    if (it->type == EB_TYPE_EPISODE)
        copy_display_str(doc, item, "SeriesName", it->series_name, sizeof(it->series_name));

    copy_raw_str(doc, item, "CollectionType", it->collection_type, sizeof(it->collection_type));

    int64_t year = json_as_i64(json_find(doc, item, "ProductionYear"), 0);
    if (year > 0) snprintf(it->year, sizeof(it->year), "%lld", (long long)year);

    it->runtime_ticks        = json_as_i64(json_find(doc, item, "RunTimeTicks"), 0);
    it->child_count          = (int)json_as_i64(json_find(doc, item, "ChildCount"), 0);
    it->recursive_item_count = (int)json_as_i64(json_find(doc, item, "RecursiveItemCount"), 0);
    it->index_number         = (int)json_as_i64(json_find(doc, item, "IndexNumber"), -1);
    it->parent_index_number  = (int)json_as_i64(json_find(doc, item, "ParentIndexNumber"), -1);

    /* Scoped to UserData rather than searched for loose: PlayedPercentage and
     * UnplayedItemCount live in the same object, and a bare "Played" search
     * was one rename away from matching the wrong one. */
    it->resume_ticks = json_as_i64 (json_find(doc, item, "UserData.PlaybackPositionTicks"), 0);
    it->played       = json_as_bool(json_find(doc, item, "UserData.Played"), 0);
}

/* Populates it->cast[] (Actor entries only, first EB_MAX_CAST of them) from
 * the item's People array. */
static void parse_cast(const JsonDoc *doc, const JsonNode *item, EbItem *it)
{
    it->cast_count = 0;
    const JsonNode *people = json_find(doc, item, "People");
    if (!people) return;

    JSON_FOREACH(doc, people, person_node) {
        if (it->cast_count >= EB_MAX_CAST) break;
        if (strcmp(json_as_str(json_find(doc, person_node, "Type"), ""), "Actor") != 0)
            continue;

        EbPerson *person = &it->cast[it->cast_count];
        memset(person, 0, sizeof(*person));
        copy_raw_str    (doc, person_node, "Id",              person->id,        sizeof(person->id));
        copy_display_str(doc, person_node, "Name",            person->name,      sizeof(person->name));
        copy_display_str(doc, person_node, "Role",            person->role,      sizeof(person->role));
        copy_raw_str    (doc, person_node, "PrimaryImageTag", person->image_tag, sizeof(person->image_tag));
        it->cast_count++;
    }
}

/* Populates it->subs[], it->audio[] and it->source_* from the item's
 * MediaStreams array (confirmed shape on a real server: Type is the string
 * "Subtitle"/"Video"/"Audio" — not the numeric enum some other server API
 * responses use). Only the first Video stream's specs are kept (source_* is
 * for display only, next to the track pickers — what's actually streamed is
 * EbStreamProfile). */
static void parse_media_streams(const JsonDoc *doc, const JsonNode *item, EbItem *it)
{
    it->sub_count = 0;
    it->audio_count = 0;
    it->source_video_codec[0] = '\0';
    it->source_width = it->source_height = 0;
    it->source_bitrate = 0;
    it->source_aspect = 0.0;
    int got_video = 0;

    const JsonNode *streams = json_find(doc, item, "MediaStreams");
    if (!streams) return;

    JSON_FOREACH(doc, streams, stream) {
        const char *type_buf = json_as_str(json_find(doc, stream, "Type"), "");

        if (!strcmp(type_buf, "Subtitle") && it->sub_count < EB_MAX_SUBS) {
            EbSubtitle *sub = &it->subs[it->sub_count];
            memset(sub, 0, sizeof(*sub));
            sub->index = (int)json_as_i64(json_find(doc, stream, "Index"), 0);
            /* DisplayTitle first, Language only as a fallback. Language alone
             * is not enough to tell two tracks apart, and that's the common
             * case rather than an edge one: a release with English dialogue
             * subtitles AND an English signs/songs or forced track shows up
             * as two identical "eng" rows, with no way to know which is
             * which except by picking one and watching.
             *
             * The server already composes what's needed — DisplayTitle leads
             * with the track's own Title when it has one ("Signs & Songs"),
             * then appends whichever of Hearing Impaired / Default / Forced /
             * codec / External apply. Worth preferring even though it's
             * longer: if it overflows the label it's the trailing codec and
             * External tags that get cut, which are the least useful parts. */
            if (!copy_display_str(doc, stream, "DisplayTitle", sub->label, sizeof(sub->label)) ||
                !sub->label[0])
                copy_display_str(doc, stream, "Language", sub->label, sizeof(sub->label));
            copy_raw_str(doc, stream, "Codec", sub->codec, sizeof(sub->codec));
            sub->is_forced  = json_as_bool(json_find(doc, stream, "IsForced"), 0);
            sub->is_default = json_as_bool(json_find(doc, stream, "IsDefault"), 0);
            it->sub_count++;

        } else if (!strcmp(type_buf, "Audio") && it->audio_count < EB_MAX_AUDIO) {
            EbAudioTrack *aud = &it->audio[it->audio_count];
            memset(aud, 0, sizeof(*aud));
            aud->index    = (int)json_as_i64(json_find(doc, stream, "Index"), 0);
            aud->channels = (int)json_as_i64(json_find(doc, stream, "Channels"), 0);
            aud->is_default = json_as_bool(json_find(doc, stream, "IsDefault"), 0);
            copy_raw_str(doc, stream, "Codec", aud->codec, sizeof(aud->codec));
            /* DisplayTitle first here, unlike subtitles: the server composes
             * it as e.g. "English - Dolby Digital 5.1 - Default", which is
             * exactly what a track picker wants to show. Language alone
             * can't distinguish two English tracks (a commentary from the
             * main mix), which is the common case worth getting right. */
            if (!copy_display_str(doc, stream, "DisplayTitle", aud->label, sizeof(aud->label)) ||
                !aud->label[0])
                copy_display_str(doc, stream, "Language", aud->label, sizeof(aud->label));
            it->audio_count++;

        } else if (!strcmp(type_buf, "Video") && !got_video) {
            got_video = 1;
            copy_raw_str(doc, stream, "Codec", it->source_video_codec, sizeof(it->source_video_codec));
            it->source_width   = (int)json_as_i64(json_find(doc, stream, "Width"), 0);
            it->source_height  = (int)json_as_i64(json_find(doc, stream, "Height"), 0);
            it->source_bitrate = json_as_i64(json_find(doc, stream, "BitRate"), 0);

            const char *ar_buf = json_as_str(json_find(doc, stream, "AspectRatio"), "");
            int ar_w = 0, ar_h = 0;
            if (ar_buf[0] && sscanf(ar_buf, "%d:%d", &ar_w, &ar_h) == 2 && ar_h > 0)
                it->source_aspect = (double)ar_w / (double)ar_h;
            it->source_interlaced = json_as_bool(json_find(doc, stream, "IsInterlaced"), 0);
            it->source_fps = json_as_dbl(json_find(doc, stream, "RealFrameRate"), 0.0);
            if (it->source_fps <= 0.0)
                it->source_fps = json_as_dbl(json_find(doc, stream, "AverageFrameRate"), 0.0);
        }
    }
}

/* Intro/credits markers from the item's Chapters. A marker is an ordinary
 * chapter entry with a MarkerType; a title with no detected intro simply
 * has none, which leaves every field 0. An IntroEnd before its IntroStart
 * (a server-side detection glitch) is dropped rather than offered. */
static void parse_markers(const JsonDoc *doc, const JsonNode *item, EbItem *it)
{
    it->intro_start_ticks = it->intro_end_ticks = it->credits_start_ticks = 0;
    const JsonNode *chapters = json_find(doc, item, "Chapters");
    if (!chapters) return;
    int64_t intro_start = -1, intro_end = 0, credits = 0;
    JSON_FOREACH(doc, chapters, ch) {
        const char *type = json_as_str(json_find(doc, ch, "MarkerType"), "");
        int64_t t = json_as_i64(json_find(doc, ch, "StartPositionTicks"), 0);
        if      (!strcmp(type, "IntroStart"))   intro_start = t;
        else if (!strcmp(type, "IntroEnd"))     intro_end   = t;
        else if (!strcmp(type, "CreditsStart")) credits     = t;
    }
    if (intro_end > 0) {
        if (intro_start < 0) intro_start = 0;   /* intro from the very start */
        if (intro_end > intro_start) {
            it->intro_start_ticks = intro_start;
            it->intro_end_ticks   = intro_end;
        }
    }
    if (credits > 0) it->credits_start_ticks = credits;
}

void eb_parse_markers(const char *json_text, EbItem *out)
{
    char *copy = strdup(json_text ? json_text : "");
    if (!copy) return;
    JsonDoc doc;
    if (json_parse(&doc, copy)) {
        parse_markers(&doc, json_root(&doc), out);
        json_free(&doc);
    }
    free(copy);
}

/* Parses a {"Items":[ ... ]} BaseItemDtoQueryResult into out[]. */
static int parse_item_list(const JsonDoc *doc, EbItem *out, int max)
{
    const JsonNode *items = json_find(doc, NULL, "Items");
    if (!items) return 0;

    int count = 0;
    JSON_FOREACH(doc, items, item) {
        if (count >= max) break;
        parse_item_fields(doc, item, &out[count]);
        count++;
    }
    return count;
}

/* ── curl transport ──────────────────────────────────────────────────────── */

/* Builds the X-Emby-Authorization header.
 *
 * The Token part is omitted entirely when there isn't one, rather than sent
 * as Token="". That matters for the sign-in calls: AuthenticateByName is
 * unauthenticated by definition (it's how you GET a credential), but it does
 * need the Client/Device/DeviceId fields, since those are what the server
 * records the new session and device under. */
void eb_auth_header(const EbConfig *cfg, char *out, int outlen)
{
    /* X-Emby-Authorization is Emby's own name for this header. The token
     * rides along inside it as well as in X-Emby-Token (eb_token_header),
     * which is the form Emby's own apps send. */
    if (cfg->token[0])
        snprintf(out, outlen,
                 "X-Emby-Authorization: MediaBrowser " EB_CLIENT_HEADERS
                 ", DeviceId=\"%s\", Token=\"%s\"",
                 cfg->device_id, cfg->token);
    else
        snprintf(out, outlen,
                 "X-Emby-Authorization: MediaBrowser " EB_CLIENT_HEADERS ", DeviceId=\"%s\"",
                 cfg->device_id);
}

/* "X-Emby-Token: <token>", or "" when there is no credential yet. */
static void eb_token_header(const EbConfig *cfg, char *out, int outlen)
{
    if (cfg->token[0]) snprintf(out, outlen, "X-Emby-Token: %s", cfg->token);
    else if (outlen > 0) out[0] = '\0';
}

/* A fetched response and the parse tree over it. The tree's strings point
 * into `text`, so the two have to be freed together — eb_response_free does
 * both, and nothing should hold a JsonNode past that. */
typedef struct {
    JsonDoc doc;
    char   *text;
} EbResponse;

static void eb_response_free(EbResponse *r)
{
    if (!r) return;
    json_free(&r->doc);
    free(r->text);
    r->text = NULL;
}

/* Runs curl with an explicit argument vector and NO SHELL, optionally
 * capturing stdout into a NUL-terminated heap buffer (caller frees).
 * Returns the buffer when capture is set and something was read, else NULL;
 * *exit_ok (may be NULL) reports whether curl itself exited 0.
 *
 * The absence of a shell is the entire point. Every one of these arguments —
 * the Authorization header, the URL — carries data that ultimately came from
 * the server, and with popen()/system() that data was being parsed by /bin/sh.
 * A hostile server answering sign-in with
 *     {"AccessToken":"x' ; <command> ; '"}
 * got that command executed as root, on every launch thereafter, because the
 * token is saved to disk. Confirmed by reproduction, not theory. Note the
 * escape: the server encodes an apostrophe as ', so decoding escapes
 * correctly (which this client now does) is what turns a quoted string into
 * a quote-breaking one. execvp takes argv straight to the kernel — there is
 * no parser between us and exec, so no quoting to escape from.
 *
 * The capture path grows to fit rather than reading into a fixed buffer: the
 * old 256KB cap silently truncated large responses mid-JSON. */
static char *eb_curl_run(char *const argv[], int capture, int *exit_ok)
{
    if (exit_ok) *exit_ok = 0;

    int pfd[2];
    if (capture && pipe(pfd) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        if (capture) { close(pfd[0]); close(pfd[1]); }
        return NULL;
    }

    if (pid == 0) {
        if (capture) {
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[0]);
            close(pfd[1]);
        }
        /* curl's own diagnostics would otherwise land in the middle of the
         * framebuffer UI on the console. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            if (!capture) dup2(devnull, STDOUT_FILENO);
            close(devnull);
        }
        execv(CURL_BIN, argv);
        _exit(127);
    }

    char *buf = NULL;
    size_t len = 0;
    if (capture) {
        close(pfd[1]);
        size_t cap = 64 * 1024;
        buf = (char *)malloc(cap);
        if (buf) {
            for (;;) {
                if (len + 1 >= cap) {
                    if (cap >= EB_MAX_RESPONSE) { free(buf); buf = NULL; len = 0; break; }
                    size_t ncap = cap * 2;
                    char *grown = (char *)realloc(buf, ncap);
                    if (!grown) { free(buf); buf = NULL; len = 0; break; }
                    buf = grown;
                    cap = ncap;
                }
                ssize_t got = read(pfd[0], buf + len, cap - len - 1);
                if (got <= 0) break;
                len += (size_t)got;
            }
        }
        /* Drain anything left so curl never blocks on a full pipe while we're
         * waiting for it to exit. */
        if (!buf) { char sink[4096]; while (read(pfd[0], sink, sizeof(sink)) > 0) {} }
        close(pfd[0]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (exit_ok) *exit_ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);

    if (!capture) { free(buf); return NULL; }
    if (!buf || len == 0) { free(buf); return NULL; }
    buf[len] = '\0';
    return buf;
}

/* ── opt-in request log ──────────────────────────────────────────────────
 * See eb_log_init's doc comment in emby.h for the privacy rules this is
 * built to guarantee structurally, not just by convention. */
static FILE *g_jf_log = NULL;

void eb_log_init(const EbConfig *cfg)
{
    if (!cfg->debug_log) return;   /* the only branch that ever touches the SD card */

    const char *paths[] = {
        "/media/fat/misteremby/debug.log",
        "./debug.log",
        NULL
    };
    for (int i = 0; paths[i] && !g_jf_log; i++)
        g_jf_log = fopen(paths[i], "w");   /* "w", not "a" — fresh per launch, see header */
    if (!g_jf_log) return;

    fprintf(g_jf_log, "MiSTerEmby %s — tv_mode=%s profile=%dx%d@%d\n",
            APP_VERSION, cfg->tv_mode, cfg->profile_width, cfg->profile_height,
            cfg->profile_bitrate);
    fflush(g_jf_log);
}

void eb_log_close(void)
{
    if (g_jf_log) { fclose(g_jf_log); g_jf_log = NULL; }
}

/* Freeform line for the non-request diagnostics (fb geometry, input devices,
 * menu core, player lifecycle, ...) — everything that isn't a server round
 * trip and so doesn't fit eb_log_request's fixed columns. Same no-op-unless-
 * enabled and immediate-fflush behavior; callers are responsible for the
 * same privacy rule (no server URL, credentials, or anything a user didn't
 * put in emby.conf/MiSTer.ini themselves). */
void eb_log_line(const char *fmt, ...)
{
    if (!g_jf_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_jf_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_jf_log);
    fflush(g_jf_log);
}

/* method/path_and_query only — deliberately no cfg, no url, no auth header
 * in this function's signature at all, so there is nothing here capable of
 * writing a credential even by mistake. path_and_query is cut at the first
 * '?': every secret this app ever puts in a query string (the sign-in
 * secret, an ApiKey on a media URL) lands after that character, so the cut
 * removes it regardless of which endpoint this is. */
static void eb_log_request(const char *method, const char *path_and_query,
                            int ok, double elapsed_ms, long bytes)
{
    if (!g_jf_log) return;

    /* Emby's user-scoped routes put the user id in the PATH
     * (/Users/<id>/Items), not the query string, so cutting at '?' alone no
     * longer keeps it out. Any id-shaped segment — 16+ hex/dash characters,
     * or all digits — is written as {id} instead. */
    char path_only[160];
    size_t n = strcspn(path_and_query, "?");
    size_t o = 0;
    for (size_t i = 0; i < n && o < sizeof(path_only) - 5; ) {
        size_t seg = i;
        while (seg < n && path_and_query[seg] != '/') seg++;
        size_t len = seg - i, hexish = 0, digits = 0;
        for (size_t k = i; k < seg; k++) {
            char c = path_and_query[k];
            if (c >= '0' && c <= '9') digits++;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F') || c == '-') hexish++;
        }
        /* Emby item ids are plain numbers; user ids are 32-hex GUIDs. */
        if ((len >= 16 && hexish == len) || (len > 0 && digits == len)) {
            memcpy(path_only + o, "{id}", 4); o += 4;
        } else {
            for (size_t k = i; k < seg && o < sizeof(path_only) - 1; k++)
                path_only[o++] = path_and_query[k];
        }
        if (seg < n && o < sizeof(path_only) - 1) path_only[o++] = '/';
        i = seg + 1;
    }
    path_only[o] = '\0';

    time_t now = time(NULL);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&now));

    fprintf(g_jf_log, "[%s] %-4s %-48s %-4s %6.0fms %6ld bytes\n",
            ts, method ? method : "GET", path_only,
            ok ? "ok" : "FAIL", elapsed_ms, bytes);
    fflush(g_jf_log);   /* request rate is low; a crash right after must not lose this line */
}

/* First CA bundle that actually exists on disk, or NULL. Stock MiSTer curl
 * has no working default CA path — verifying an HTTPS server fails with
 * "unable to get local issuer certificate" even though a perfectly good
 * bundle ships in the image, so a verified request has to point --cacert at
 * one explicitly. (The updater in update.c needs this too and keeps its own
 * copy; kept separate so each translation unit stays self-contained.) */
static const char *eb_ca_bundle(void)
{
    static const char *const cands[] = {
        "/etc/ssl/certs/cacert.pem",
        "/etc/ssl/cert.pem",
        "/usr/lib/python3.9/site-packages/certifi/cacert.pem",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        struct stat st;
        if (stat(cands[i], &st) == 0 && st.st_size > 0) return cands[i];
    }
    return NULL;
}

/* Appends the TLS-verification flags to a curl argv being built, returning
 * the new element count. INSECURE_TLS in the config => "-k" (skip
 * verification, for a self-signed home server); otherwise verify, pointing
 * --cacert at a real bundle so verification can actually succeed on this
 * platform. Plain http:// requests ignore all of this. */
static int eb_add_tls_args(const EbConfig *cfg, const char **argv, int n)
{
    if (cfg->insecure_tls) { argv[n++] = "-k"; return n; }
    const char *ca = eb_ca_bundle();
    if (ca) { argv[n++] = "--cacert"; argv[n++] = ca; }
    return n;
}

/* Unlike eb_curl_run, this never waits for curl to exit: it's meant to run
 * for the whole length of a video/track, in parallel with mplayer reading
 * the other end of the FIFO, so blocking here would defeat the point.
 * curl's own open of fifo_path (for -o) blocks until a reader shows up the
 * same way mplayer's open of it blocks until a writer shows up — two
 * independent processes rendezvousing through the kernel, nothing for this
 * function to coordinate. */
pid_t eb_spawn_stream_curl(const EbConfig *cfg, const char *url, const char *fifo_path)
{
    const char *argv[16];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sf";
    n = eb_add_tls_args(cfg, argv, n);
    argv[n++] = "-o"; argv[n++] = fifo_path;
    argv[n++] = url;
    argv[n]   = NULL;

    pid_t pid = fork();
    if (pid != 0) return pid;   /* parent: pid, or -1 on fork failure */

    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
    execv(CURL_BIN, (char *const *)argv);
    _exit(127);
}

static char *eb_request_alloc(const EbConfig *cfg, const char *method,
                               const char *path_and_query, const char *body_file,
                               int timeout_secs)
{
    char auth[320], token_hdr[224];
    eb_auth_header(cfg, auth, sizeof(auth));
    eb_token_header(cfg, token_hdr, sizeof(token_hdr));

    char timeout[16];
    snprintf(timeout, sizeof(timeout), "%d", timeout_secs);

    char url[1024];
    snprintf(url, sizeof(url), "%s" EB_API_BASE "%s", cfg->server, path_and_query);

    char body_arg[128];
    if (body_file) snprintf(body_arg, sizeof(body_arg), "@%s", body_file);

    /* Built as a plain argv. Each element reaches curl exactly as written,
     * with no quoting, escaping or word splitting anywhere in between. */
    const char *argv[24];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sf";
    n = eb_add_tls_args(cfg, argv, n);
    argv[n++] = "--max-time";
    argv[n++] = timeout;
    if (method) { argv[n++] = "-X"; argv[n++] = method; }
    argv[n++] = "-H";
    argv[n++] = auth;
    if (token_hdr[0]) { argv[n++] = "-H"; argv[n++] = token_hdr; }
    argv[n++] = "-H";
    argv[n++] = "Accept: application/json";
    if (body_file) {
        argv[n++] = "-H";
        argv[n++] = "Content-Type: application/json";
        argv[n++] = "--data-binary";
        argv[n++] = body_arg;
    }
    argv[n++] = url;
    argv[n]   = NULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = 0;
    char *result = eb_curl_run((char *const *)argv, 1, &ok);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    /* ok alone, not "ok && result != NULL": a 204 No Content (the normal
     * response for the Sessions/Playing family) has an empty body, so
     * result is NULL even though curl's own exit status says the request
     * succeeded — logging that as FAIL was blaming the wrong thing. */
    eb_log_request(method, path_and_query, ok, elapsed_ms,
                    result ? (long)strlen(result) : 0);

    return result;
}

/* Fetch + parse in one step. Returns 1 on success, with *r owning both the
 * text and the tree; 0 if the request failed or the response wasn't valid
 * JSON (in which case *r is already cleaned up). */
static int eb_fetch(const EbConfig *cfg, const char *path_and_query, EbResponse *r)
{
    memset(r, 0, sizeof(*r));
    r->text = eb_request_alloc(cfg, NULL, path_and_query, NULL, 8);
    if (!r->text) return 0;
    if (!json_parse(&r->doc, r->text)) { eb_response_free(r); return 0; }
    return 1;
}

/* Fire-and-forget POST with a JSON body (Sessions/Playing family). Body is
 * written to a temp file to avoid quoting issues in the curl command line. */
static void eb_post_json(const EbConfig *cfg, const char *path, const char *json_body)
{
    /* mkstemp, not a getpid()-based name: the pid is identical across
     * threads (so two concurrent POSTs would clash) and predictable (so a
     * pre-planted symlink at the path would be followed by this root
     * process). mkstemp creates the file itself with O_EXCL and a random
     * suffix, closing both. */
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/misteremby_post_XXXXXX");
    int fd = mkstemp(tmp_path);
    if (fd < 0) return;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_path); return; }
    /* A short write here means a truncated/invalid body, so don't send it —
     * the earlier version could POST a partial JSON payload silently. */
    int ok = (fputs(json_body, f) >= 0);
    if (fclose(f) != 0) ok = 0;

    /* Reuses the same no-shell path as every other request — see
     * eb_curl_run. The response is discarded, but the arguments still carry
     * the server-issued token. */
    if (ok) free(eb_request_alloc(cfg, "POST", path, tmp_path, 5));
    unlink(tmp_path);
}

/* ── config ───────────────────────────────────────────────────────────────── */

int eb_config_load(EbConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->profile_width   = EB_PROFILE_DEFAULT_W;
    cfg->profile_height  = EB_PROFILE_DEFAULT_H;
    cfg->profile_bitrate = EB_PROFILE_DEFAULT_RATE;

    const char *paths[] = {
        "/media/fat/misteremby/emby.conf",
        "./emby.conf",
        NULL
    };
    FILE *f = NULL;
    for (int i = 0; paths[i] && !f; i++) f = fopen(paths[i], "r");
    if (!f) return 0;

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        /* An optional transcode profile, "WxH" or "WxH@BITRATE" (e.g.
         * "640x480@12000000"). Matched by shape rather than by position for
         * the same reason as PAL/NTSC below — and the shape is unambiguous:
         * a server URL contains "://", and no API key or username is two
         * integers joined by an 'x'. sscanf's %n confirms the whole line was
         * consumed, so "480x270junk" is not silently accepted. */
        {
            int w = 0, h = 0, rate = 0, used = 0;
            if ((sscanf(line, "%dx%d@%d%n", &w, &h, &rate, &used) == 3 && !line[used]) ||
                (sscanf(line, "%dx%d%n", &w, &h, &used) == 2 && !line[used])) {
                if (w >= EB_PROFILE_MIN_W && w <= EB_PROFILE_MAX_W &&
                    h >= EB_PROFILE_MIN_H && h <= EB_PROFILE_MAX_H) {
                    cfg->profile_width  = w;
                    cfg->profile_height = h;
                    if (rate >= EB_PROFILE_MIN_RATE && rate <= EB_PROFILE_MAX_RATE)
                        cfg->profile_bitrate = rate;
                }
                /* Consumed either way — a malformed profile line must not
                 * fall through and be read as an API key. */
                continue;
            }
        }

        /* PAL/NTSC is recognised wherever it appears rather than strictly as
         * the 4th line. The file is positional and blank lines are skipped,
         * so with sign-in making the API key and username optional
         * there was no way to write "no key, but NTSC" — the mode would slide
         * up into the api_key slot and be read as a credential. Matching it
         * by value instead keeps every combination expressible, and costs
         * nothing: no server URL, key or username is ever the literal string
         * "PAL" or "NTSC". */
        if (!strcasecmp(line, "PAL") || !strcasecmp(line, "NTSC")) {
            strncpy(cfg->tv_mode, line, sizeof(cfg->tv_mode) - 1);
            continue;
        }

        /* Same "recognised wherever it appears" treatment as PAL/NTSC above,
         * and for the same reason — no server URL, key, username or TV mode
         * is ever literally the string "DEBUGLOG". See eb_log_init. */
        if (!strcasecmp(line, "DEBUGLOG")) {
            cfg->debug_log = 1;
            continue;
        }

        /* Same treatment again — a real config value is never this literal. */
        if (!strcasecmp(line, "INSECURE_TLS")) {
            cfg->insecure_tls = 1;
            continue;
        }

        if (!strcasecmp(line, "TRANSCODE_ALWAYS")) {
            cfg->transcode_always = 1;
            continue;
        }

        /* Named settings, KEY=value, recognised anywhere. A server URL never
         * starts with one of these keys, and the positional API key and
         * username lines below can't contain "=" in practice (Emby API keys
         * are hex). Values are kept verbatim — a password may contain
         * anything except a newline. */
        {
            static const struct { const char *key; size_t off, len; } named[] = {
                { "SERVER=",           offsetof(EbConfig, server),           sizeof(((EbConfig *)0)->server) },
                { "API_KEY=",          offsetof(EbConfig, api_key),          sizeof(((EbConfig *)0)->api_key) },
                { "USERNAME=",         offsetof(EbConfig, username),         sizeof(((EbConfig *)0)->username) },
                { "PASSWORD=",         offsetof(EbConfig, password),         sizeof(((EbConfig *)0)->password) },
                { "CONNECT_USER=",     offsetof(EbConfig, connect_email),    sizeof(((EbConfig *)0)->connect_email) },
                { "CONNECT_PASSWORD=", offsetof(EbConfig, connect_password), sizeof(((EbConfig *)0)->connect_password) },
                { "TV_MODE=",          offsetof(EbConfig, tv_mode),          sizeof(((EbConfig *)0)->tv_mode) },
            };
            int matched = 0;
            for (size_t k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
                size_t kl = strlen(named[k].key);
                if (strncasecmp(line, named[k].key, kl) != 0) continue;
                char *dst = (char *)cfg + named[k].off;
                memset(dst, 0, named[k].len);
                strncpy(dst, line + kl, named[k].len - 1);
                matched = 1;
                break;
            }
            if (matched) continue;
        }

        char *dst;
        int dstlen;
        switch (line_no) {
            case 0: dst = cfg->server;   dstlen = (int)sizeof(cfg->server);   break;
            case 1: dst = cfg->api_key;  dstlen = (int)sizeof(cfg->api_key);  break;
            case 2: dst = cfg->username; dstlen = (int)sizeof(cfg->username); break;
            default: dst = cfg->tv_mode; dstlen = (int)sizeof(cfg->tv_mode); break;
        }
        strncpy(dst, line, dstlen - 1);
        line_no++;
        if (line_no >= 4) break;
    }
    fclose(f);

    /* Strip trailing slashes, and a trailing "/emby" — people paste the
     * address out of the browser bar, and every request adds EB_API_BASE
     * itself. */
    size_t slen = strlen(cfg->server);
    while (slen > 0 && cfg->server[slen - 1] == '/') cfg->server[--slen] = '\0';
    if (slen >= 5 && !strcasecmp(cfg->server + slen - 5, "/emby")) {
        slen -= 5;
        cfg->server[slen] = '\0';
        while (slen > 0 && cfg->server[slen - 1] == '/') cfg->server[--slen] = '\0';
    }

    if (strcasecmp(cfg->tv_mode, "NTSC") != 0)
        strncpy(cfg->tv_mode, "PAL", sizeof(cfg->tv_mode) - 1);

    /* An API key in the config file is used as-is; without one the caller
     * falls back to a saved token, then to sign-in. */
    strncpy(cfg->token, cfg->api_key, sizeof(cfg->token) - 1);

    /* Only the server URL is required. api_key/username missing used to be a
     * hard failure, which forced every user through the admin dashboard to
     * mint a key — sign-in exists precisely so they don't have to. */
    return cfg->server[0] != '\0';
}

int eb_has_credential(const EbConfig *cfg)
{
    return cfg && cfg->token[0] != '\0';
}

/* Rejects control characters (and DEL) anywhere in a credential. Empty is
 * fine — callers check that separately where it matters. */
static int eb_credential_is_sane(const char *s)
{
    if (!s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F) return 0;
    return 1;
}

/* ── device identity ──────────────────────────────────────────────────────── */

static const char *eb_device_paths[] = {
    "/media/fat/misteremby/device.conf",
    "./device.conf",
    NULL
};

/* A random RFC-4122-shaped v4 GUID, which is what every other client
 * reports. Seeded from /dev/urandom; rand() is the fallback for the
 * essentially-impossible case of that being unavailable, and collisions there
 * would only mean two installs sharing a device entry, not anything unsafe. */
static void eb_generate_device_id(char *out, int outlen)
{
    unsigned char b[16];
    int got = 0;

    FILE *ur = fopen("/dev/urandom", "rb");
    if (ur) {
        got = (fread(b, 1, sizeof(b), ur) == sizeof(b));
        fclose(ur);
    }
    if (!got) {
        srand((unsigned)(time(NULL) ^ (getpid() << 16)));
        for (size_t i = 0; i < sizeof(b); i++) b[i] = (unsigned char)(rand() & 0xFF);
    }

    b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);   /* version 4 */
    b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);   /* variant 1 */

    snprintf(out, outlen,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void eb_device_id_init(EbConfig *cfg)
{
    FILE *f = NULL;
    for (int i = 0; eb_device_paths[i] && !f; i++) f = fopen(eb_device_paths[i], "r");

    if (f) {
        char line[80] = {0};
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            /* Sanitised on the way in: it's read from disk, and it ends up
             * inside a single-quoted shell argument. Generated ids only ever
             * contain hex and dashes, so this is a no-op for them and only
             * bites a hand-corrupted file. */
            eb_sanitize_id(line, cfg->device_id, sizeof(cfg->device_id));
        }
        fclose(f);
        if (cfg->device_id[0]) return;
    }

    eb_generate_device_id(cfg->device_id, sizeof(cfg->device_id));

    /* Prefer the real install location; fall back to the working directory
     * when it isn't there, same as the token file. */
    const char *path = eb_device_paths[1];
    FILE *probe = fopen("/media/fat/misteremby/emby.conf", "r");
    if (probe) { fclose(probe); path = eb_device_paths[0]; }

    FILE *w = fopen(path, "w");
    if (w) { fprintf(w, "%s\n", cfg->device_id); fclose(w); }
    /* A write failure is survivable — the id just won't persist, so the
     * server sees a new device next launch. Not worth failing startup over. */
}

/* ── saved token ──────────────────────────────────────────────────────────── */

/* Written next to emby.conf, with the desktop fallback the config loader
 * uses so off-hardware runs don't try to write to /media/fat. Two lines:
 * access token, then user id — the id is stored rather than re-resolved
 * because /Users needs elevated access that a plain user token doesn't have. */
static const char *eb_token_paths[] = {
    "/media/fat/misteremby/token.conf",
    "./token.conf",
    NULL
};

static const char *eb_token_writable_path(void)
{
    /* Prefer the real install location, but fall back to the working
     * directory when it isn't there (desktop runs). */
    FILE *probe = fopen("/media/fat/misteremby/emby.conf", "r");
    if (probe) { fclose(probe); return eb_token_paths[0]; }
    return eb_token_paths[1];
}

int eb_token_save(const EbConfig *cfg)
{
    if (!cfg->token[0]) return 0;
    const char *path = eb_token_writable_path();
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%s\n%s\n%s\n", cfg->token, cfg->user_id, cfg->username);
    fclose(f);
    return 1;
}

int eb_token_load(EbConfig *cfg)
{
    FILE *f = NULL;
    for (int i = 0; eb_token_paths[i] && !f; i++) f = fopen(eb_token_paths[i], "r");
    if (!f) return 0;

    char token[192] = {0}, user_id[EB_ID_LEN] = {0}, username[64] = {0};
    int ok = 0;
    if (fgets(token, sizeof(token), f)) {
        token[strcspn(token, "\r\n")] = '\0';
        if (fgets(user_id, sizeof(user_id), f)) {
            user_id[strcspn(user_id, "\r\n")] = '\0';
            if (fgets(username, sizeof(username), f))
                username[strcspn(username, "\r\n")] = '\0';
            ok = token[0] && user_id[0];
        }
    }
    fclose(f);
    if (!ok) return 0;

    strncpy(cfg->token,   token,   sizeof(cfg->token) - 1);
    strncpy(cfg->user_id, user_id, sizeof(cfg->user_id) - 1);
    if (username[0]) strncpy(cfg->username, username, sizeof(cfg->username) - 1);
    return 1;
}

void eb_token_clear(void)
{
    for (int i = 0; eb_token_paths[i]; i++) unlink(eb_token_paths[i]);
}

/* ── sign-in ──────────────────────────────────────────────────────────────── */

/* POST + parse, for server calls that return a body (sign-in).
 * json_body may be NULL. */
static int eb_post_fetch(const EbConfig *cfg, const char *path,
                          const char *json_body, EbResponse *r)
{
    memset(r, 0, sizeof(*r));

    char tmp_path[64] = "";
    if (json_body) {
        /* mkstemp for the same reason as eb_post_json — unique per call,
         * not a followable predictable path. */
        snprintf(tmp_path, sizeof(tmp_path), "/tmp/misteremby_auth_XXXXXX");
        int fd = mkstemp(tmp_path);
        if (fd < 0) return 0;
        FILE *f = fdopen(fd, "w");
        if (!f) { close(fd); unlink(tmp_path); return 0; }
        int ok = (fputs(json_body, f) >= 0);
        if (fclose(f) != 0) ok = 0;
        if (!ok) { unlink(tmp_path); return 0; }
    }

    r->text = eb_request_alloc(cfg, "POST", path, json_body ? tmp_path : NULL, 10);
    if (json_body) unlink(tmp_path);

    if (!r->text) return 0;
    if (!json_parse(&r->doc, r->text)) { eb_response_free(r); return 0; }
    return 1;
}

/* Copies a credential pair out of a response only when neither part carries
 * control characters. token.conf is line-oriented, and a newline inside a
 * token (which a server can send as an escape this client decodes
 * faithfully) would write a file that reloads as a different, truncated
 * credential: a permanent self-inflicted lockout. */
static int eb_take_credential(EbConfig *cfg, const char *token, const char *user_id,
                              const char *username)
{
    if (!token[0] || !user_id[0]) return 0;
    if (!eb_credential_is_sane(token) || !eb_credential_is_sane(user_id) ||
        !eb_credential_is_sane(username))
        return 0;
    memset(cfg->token, 0, sizeof(cfg->token));
    memset(cfg->user_id, 0, sizeof(cfg->user_id));
    strncpy(cfg->token,   token,   sizeof(cfg->token) - 1);
    strncpy(cfg->user_id, user_id, sizeof(cfg->user_id) - 1);
    if (username && username[0]) {
        memset(cfg->username, 0, sizeof(cfg->username));
        strncpy(cfg->username, username, sizeof(cfg->username) - 1);
    }
    return 1;
}

/* Minimal JSON string escaping for values this client puts in a body itself
 * (a username/password typed into emby.conf). */
static void eb_json_escape(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 7 < outsz; p++) {
        if (*p == '"' || *p == '\\') { out[j++] = '\\'; out[j++] = (char)*p; }
        else if (*p < 0x20) j += (size_t)snprintf(out + j, outsz - j, "\\u%04x", *p);
        else out[j++] = (char)*p;
    }
    out[j] = '\0';
}

int eb_authenticate_by_name(EbConfig *cfg)
{
    char user[160], pw[288], body[512];
    eb_json_escape(cfg->username, user, sizeof(user));
    eb_json_escape(cfg->password, pw,   sizeof(pw));
    /* "Pw" is Emby's plaintext password field. */
    snprintf(body, sizeof(body), "{\"Username\":\"%s\",\"Pw\":\"%s\"}", user, pw);

    /* Unauthenticated by definition — make sure no stale token rides along. */
    char saved[sizeof(cfg->token)];
    memcpy(saved, cfg->token, sizeof(saved));
    cfg->token[0] = '\0';

    EbResponse r;
    int ok = eb_post_fetch(cfg, "/Users/AuthenticateByName", body, &r);
    memcpy(cfg->token, saved, sizeof(saved));
    if (!ok) {
        /* curl -f turns the 401 for a wrong password into the same failure
         * as a dead network. Tell them apart with a cheap public call. */
        EbResponse probe;
        if (!eb_fetch(cfg, "/System/Info/Public", &probe)) return -1;
        eb_response_free(&probe);
        return 0;
    }

    char token[sizeof(cfg->token)] = {0}, user_id[EB_ID_LEN] = {0}, username[64] = {0};
    json_copy_str(&r.doc, NULL, "AccessToken", token,    sizeof(token));
    json_copy_str(&r.doc, NULL, "User.Id",     user_id,  sizeof(user_id));
    json_copy_str(&r.doc, NULL, "User.Name",   username, sizeof(username));
    eb_response_free(&r);
    return eb_take_credential(cfg, token, user_id, username) ? 1 : 0;
}

int eb_list_public_users(const EbConfig *cfg, EbPublicUser *out, int max)
{
    EbResponse r;
    if (!eb_fetch(cfg, "/Users/Public", &r)) return -1;
    int n = 0;
    JSON_FOREACH(&r.doc, json_root(&r.doc), u) {
        if (n >= max) break;
        EbPublicUser *p = &out[n];
        memset(p, 0, sizeof(*p));
        copy_raw_str(&r.doc, u, "Id", p->id, sizeof(p->id));
        copy_display_str(&r.doc, u, "Name", p->name, sizeof(p->name));
        /* HasPassword is the field Emby's own sign-in screen keys off;
         * older servers only send HasConfiguredPassword. */
        const JsonNode *hp = json_find(&r.doc, u, "HasPassword");
        if (!hp) hp = json_find(&r.doc, u, "HasConfiguredPassword");
        p->has_password = json_as_bool(hp, 1);
        if (p->id[0] && p->name[0] && eb_credential_is_sane(p->name)) n++;
    }
    eb_response_free(&r);
    return n;
}

/* ── Emby Connect (connect.emby.media) ───────────────────────────────────── */

#define EB_CONNECT_BASE "https://connect.emby.media/service"
#define EB_CONNECT_APP  "X-Application: MiSTerEmby/" APP_VERSION

/* One request to the Emby Connect service. Always TLS-verified — it's a
 * public service with a real certificate, so INSECURE_TLS (meant for a
 * self-signed home server) never applies here. form is a NULL-terminated
 * list of "key=value" pairs sent url-encoded (the service's own apps post
 * forms, not JSON); user_token adds X-Connect-UserToken. */
static int eb_connect_request(const char *path_and_query, const char *const *form,
                              const char *user_token, EbResponse *r)
{
    memset(r, 0, sizeof(*r));
    char url[512], tok_hdr[256] = "";
    /* MISTEREMBY_CONNECT_BASE points this at tools/mock-emby.py for
     * off-hardware testing; unset (always, on a MiSTer) it's the real
     * service. */
    const char *base = getenv("MISTEREMBY_CONNECT_BASE");
    snprintf(url, sizeof(url), "%s%s", base && base[0] ? base : EB_CONNECT_BASE, path_and_query);
    if (user_token && user_token[0])
        snprintf(tok_hdr, sizeof(tok_hdr), "X-Connect-UserToken: %s", user_token);

    const char *argv[32];
    int n = 0;
    argv[n++] = "curl";
    argv[n++] = "-sf";
    const char *ca = eb_ca_bundle();
    if (ca) { argv[n++] = "--cacert"; argv[n++] = ca; }
    argv[n++] = "--max-time"; argv[n++] = "15";
    argv[n++] = "-H"; argv[n++] = EB_CONNECT_APP;
    argv[n++] = "-H"; argv[n++] = "Accept: application/json";
    if (tok_hdr[0]) { argv[n++] = "-H"; argv[n++] = tok_hdr; }
    for (int i = 0; form && form[i] && n < 26; i++) {
        argv[n++] = "--data-urlencode";
        argv[n++] = form[i];
    }
    argv[n++] = url;
    argv[n]   = NULL;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ok = 0;
    r->text = eb_curl_run((char *const *)argv, 1, &ok);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    char logpath[96];
    snprintf(logpath, sizeof(logpath), "[connect]%s", path_and_query);
    eb_log_request(form ? "POST" : "GET", logpath, ok, ms, r->text ? (long)strlen(r->text) : 0);

    if (!ok || !r->text) { eb_response_free(r); return 0; }
    if (!json_parse(&r->doc, r->text)) { eb_response_free(r); return 0; }
    return 1;
}

/* Accepts either spelling the service uses for the account credential. */
static int eb_connect_take(const EbResponse *r, EbConnectAuth *ca)
{
    memset(ca, 0, sizeof(*ca));
    if (!json_copy_str(&r->doc, NULL, "AccessToken", ca->token, sizeof(ca->token)) || !ca->token[0])
        json_copy_str(&r->doc, NULL, "ConnectAccessToken", ca->token, sizeof(ca->token));
    if (!json_copy_str(&r->doc, NULL, "User.Id", ca->user_id, sizeof(ca->user_id)) || !ca->user_id[0])
        if (!json_copy_str(&r->doc, NULL, "UserId", ca->user_id, sizeof(ca->user_id)) || !ca->user_id[0])
            json_copy_str(&r->doc, NULL, "ConnectUserId", ca->user_id, sizeof(ca->user_id));
    return ca->token[0] && ca->user_id[0] &&
           eb_credential_is_sane(ca->token) && eb_credential_is_sane(ca->user_id);
}

int eb_connect_login(const EbConfig *cfg, EbConnectAuth *ca)
{
    char f_user[160], f_pw[176];
    snprintf(f_user, sizeof(f_user), "nameOrEmail=%s", cfg->connect_email);
    snprintf(f_pw,   sizeof(f_pw),   "rawpw=%s",       cfg->connect_password);
    const char *form[] = { f_user, f_pw, NULL };

    EbResponse r;
    if (!eb_connect_request("/user/authenticate", form, NULL, &r)) return 0;
    int ok = eb_connect_take(&r, ca);
    eb_response_free(&r);
    return ok;
}

int eb_pin_create(const EbConfig *cfg, EbConnectPin *pin)
{
    memset(pin, 0, sizeof(*pin));
    char f_dev[96];
    snprintf(f_dev, sizeof(f_dev), "deviceId=%s", cfg->device_id);
    const char *form[] = { f_dev, NULL };

    EbResponse r;
    if (!eb_connect_request("/pin", form, NULL, &r)) return 0;
    json_copy_str(&r.doc, NULL, "Pin", pin->pin, sizeof(pin->pin));
    eb_response_free(&r);

    char safe[sizeof(pin->pin)];
    eb_sanitize_id(pin->pin, safe, sizeof(safe));   /* goes into a URL below */
    memcpy(pin->pin, safe, sizeof(safe));
    return pin->pin[0] != '\0';
}

int eb_pin_poll(const EbConfig *cfg, const EbConnectPin *pin)
{
    char path[160];
    snprintf(path, sizeof(path), "/pin?deviceId=%s&pin=%s", cfg->device_id, pin->pin);

    EbResponse r;
    if (!eb_connect_request(path, NULL, NULL, &r)) return -1;
    int confirmed = json_as_bool(json_find(&r.doc, NULL, "IsConfirmed"), 0);
    int expired   = json_as_bool(json_find(&r.doc, NULL, "IsExpired"), 0);
    eb_response_free(&r);
    if (confirmed) return 1;
    return expired ? -1 : 0;
}

int eb_pin_exchange(const EbConfig *cfg, const EbConnectPin *pin, EbConnectAuth *ca)
{
    char f_dev[96], f_pin[40];
    snprintf(f_dev, sizeof(f_dev), "deviceId=%s", cfg->device_id);
    snprintf(f_pin, sizeof(f_pin), "pin=%s", pin->pin);
    const char *form[] = { f_dev, f_pin, NULL };

    EbResponse r;
    if (!eb_connect_request("/pin/authenticate", form, NULL, &r)) return 0;
    int ok = eb_connect_take(&r, ca);
    eb_response_free(&r);
    return ok;
}

/* Lower-cased address with any trailing slashes and "/emby" removed, for
 * comparing the configured server against what Emby Connect reports. */
static void eb_norm_addr(const char *in, char *out, size_t outsz)
{
    size_t j = 0;
    for (; in[j] && j < outsz - 1; j++) out[j] = (char)tolower((unsigned char)in[j]);
    out[j] = '\0';
    while (j > 0 && out[j - 1] == '/') out[--j] = '\0';
    if (j >= 5 && !strcmp(out + j - 5, "/emby")) { j -= 5; out[j] = '\0'; }
    while (j > 0 && out[j - 1] == '/') out[--j] = '\0';
}

int eb_connect_link_server(EbConfig *cfg, const EbConnectAuth *ca)
{
    char path[128];
    snprintf(path, sizeof(path), "/servers?userId=%s", ca->user_id);

    EbResponse r;
    if (!eb_connect_request(path, NULL, ca->token, &r)) return -1;

    char want[256];
    eb_norm_addr(cfg->server, want, sizeof(want));

    char access_key[192] = "", only_key[192] = "";
    int count = 0;
    JSON_FOREACH(&r.doc, json_root(&r.doc), srv) {
        char key[192] = "", url[256] = "", local[256] = "";
        json_copy_str(&r.doc, srv, "AccessKey",    key,   sizeof(key));
        json_copy_str(&r.doc, srv, "Url",          url,   sizeof(url));
        json_copy_str(&r.doc, srv, "LocalAddress", local, sizeof(local));
        if (!key[0]) continue;
        count++;
        if (count == 1) snprintf(only_key, sizeof(only_key), "%s", key);
        char nu[256], nl[256];
        eb_norm_addr(url, nu, sizeof(nu));
        eb_norm_addr(local, nl, sizeof(nl));
        if ((nu[0] && !strcmp(nu, want)) || (nl[0] && !strcmp(nl, want))) {
            snprintf(access_key, sizeof(access_key), "%s", key);
            break;
        }
    }
    eb_response_free(&r);

    /* A single linked server is almost certainly this one, reached by an
     * address (a hostname, a different port mapping) Connect doesn't know. */
    if (!access_key[0] && count == 1) snprintf(access_key, sizeof(access_key), "%s", only_key);
    if (!access_key[0] || !eb_credential_is_sane(access_key)) return 0;

    /* The server trades the AccessKey for an ordinary local user token. It
     * authenticates this one call with the key itself. */
    char saved[sizeof(cfg->token)];
    memcpy(saved, cfg->token, sizeof(saved));
    memset(cfg->token, 0, sizeof(cfg->token));
    strncpy(cfg->token, access_key, sizeof(cfg->token) - 1);

    char ex_path[160];
    snprintf(ex_path, sizeof(ex_path), "/Connect/Exchange?format=json&ConnectUserId=%s", ca->user_id);
    EbResponse ex;
    int got = eb_fetch(cfg, ex_path, &ex);
    memcpy(cfg->token, saved, sizeof(saved));
    if (!got) return -1;

    char token[sizeof(cfg->token)] = {0}, user_id[EB_ID_LEN] = {0};
    json_copy_str(&ex.doc, NULL, "AccessToken", token,   sizeof(token));
    json_copy_str(&ex.doc, NULL, "LocalUserId", user_id, sizeof(user_id));
    eb_response_free(&ex);
    if (!eb_take_credential(cfg, token, user_id, "")) return -1;

    /* The exchange only returns an id; fetch the display name for the log
     * and the dashboard. Best effort. */
    char upath[96];
    snprintf(upath, sizeof(upath), "/Users/%s", cfg->user_id);
    EbResponse ur;
    if (eb_fetch(cfg, upath, &ur)) {
        char name[64] = "";
        json_copy_str(&ur.doc, NULL, "Name", name, sizeof(name));
        if (name[0] && eb_credential_is_sane(name)) {
            memset(cfg->username, 0, sizeof(cfg->username));
            strncpy(cfg->username, name, sizeof(cfg->username) - 1);
        }
        eb_response_free(&ur);
    }
    return 1;
}

int eb_credential_works(const EbConfig *cfg)
{
    if (!eb_has_credential(cfg) || !cfg->user_id[0]) return 0;

    /* UserViews is the smallest authenticated, user-scoped call available —
     * it fails for a revoked token but doesn't need elevated access the way
     * /Users does. */
    char path[256];
    snprintf(path, sizeof(path), "/Users/%s/Views", cfg->user_id);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) {
        /* A failed request is either the server refusing the token or the
         * server not answering at all — and only the first means the saved
         * sign-in is dead. Throwing it away for the second (right after a
         * reboot, before the network was up) cost a PIN sign-in on real
         * hardware. The unauthenticated public-info call tells them apart. */
        return eb_server_reachable(cfg) ? 0 : -1;
    }
    int ok = json_find(&r.doc, NULL, "Items") != NULL;
    eb_response_free(&r);
    return ok;
}

int eb_server_reachable(const EbConfig *cfg)
{
    EbResponse r;
    if (!eb_fetch(cfg, "/System/Info/Public", &r)) return 0;
    eb_response_free(&r);
    return 1;
}

int eb_resolve_user_id(EbConfig *cfg)
{
    EbResponse r;
    if (!eb_fetch(cfg, "/Users", &r)) return -1;

    /* /Users returns a bare array, so the root node is what to iterate. */
    int result = 0;
    JSON_FOREACH(&r.doc, json_root(&r.doc), user) {
        const char *name = json_as_str(json_find(&r.doc, user, "Name"), "");
        if (strcasecmp(name, cfg->username) != 0) continue;
        json_copy_str(&r.doc, user, "Id", cfg->user_id, sizeof(cfg->user_id));
        result = cfg->user_id[0] != '\0';
        break;
    }

    eb_response_free(&r);
    return result;
}

/* ── browsing ─────────────────────────────────────────────────────────────── */

int eb_list_views(const EbConfig *cfg, EbItem *out, int max)
{
    char path[256];
    snprintf(path, sizeof(path), "/Users/%s/Views", cfg->user_id);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return 0;
    int n = parse_item_list(&r.doc, out, max);
    for (int i = 0; i < n; i++) out[i].type = EB_TYPE_FOLDER;
    eb_response_free(&r);
    return n;
}

int64_t eb_count_items(const EbConfig *cfg, const char *parent_id, const char *item_type)
{
    char safe_parent[EB_ID_LEN];
    eb_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    char path[384];
    if (item_type && item_type[0])
        snprintf(path, sizeof(path),
            "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=%s&Limit=0",
            cfg->user_id, safe_parent, item_type);
    else
        snprintf(path, sizeof(path),
            "/Users/%s/Items?ParentId=%s&Recursive=true&Limit=0",
            cfg->user_id, safe_parent);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;
    int64_t result = json_as_i64(json_find(&r.doc, NULL, "TotalRecordCount"), -1);
    eb_response_free(&r);
    return result;
}

/* Reads TotalRecordCount into *total_out (may be NULL), leaving it untouched
 * if the server didn't send one — the caller then falls back to however many
 * items actually arrived. */
static void parse_total_count(const JsonDoc *doc, int64_t *total_out)
{
    if (!total_out) return;
    const JsonNode *n = json_find(doc, NULL, "TotalRecordCount");
    if (n && n->type == JSON_NUMBER) *total_out = n->i64;
}

/* Builds the paginated item-list path for a library or folder, selecting the
 * fields and traversal rules appropriate to its collection type.
 *
 * EnableImageTypes carries Backdrop as well as Primary because the browse
 * list draws the selected row's backdrop behind itself (see main.c's hero
 * comment) and needs the tag to build that URL. One image tag per row, unlike
 * the count fields this function is otherwise careful about — those cost the
 * server a query each, a tag costs it a column. Overview is
 * deliberately omitted because browse rows never display it. The info screen
 * fetches it separately through eb_get_item_details(). */
void eb_build_items_path(const EbConfig *cfg, const char *parent_id,
                         const char *collection_type, int start_index, int max,
                         char *path, size_t path_size)
{
    char safe_parent[EB_ID_LEN];
    eb_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));
    if (start_index < 0) start_index = 0;

    /* Movie libraries can contain intermediate folders, but the browse view
     * presents a flat movie list. Search recursively and filter to Movie so
     * folders and other descendants do not appear. Movie rows display year
     * and runtime, but neither count field, and asking the server for those
     * counts can make a large library exceed the request timeout. */
    if (collection_type && !strcmp(collection_type, "movies"))
        snprintf(path, path_size,
            "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=Movie"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks"
            "&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=%d&Limit=%d",
            cfg->user_id, safe_parent, start_index, max);
    /* Music-video libraries use the same flat browsing model as movies.
     * Search recursively and filter to MusicVideo so intermediate folders do
     * not appear. Rows display year and runtime, but neither count field. */
    else if (collection_type && !strcmp(collection_type, "musicvideos"))
        snprintf(path, path_size,
            "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=MusicVideo"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks"
            "&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=%d&Limit=%d",
            cfg->user_id, safe_parent, start_index, max);
    /* Music keeps MiSTerEmby's artist -> album -> track hierarchy, so list
     * direct children instead of flattening the library recursively. Keep
     * ChildCount for the album/track totals shown on artist and album rows.
     * RecursiveItemCount is not displayed for music, and computing it across
     * a large library can make the server exceed the request timeout. */
    else if (collection_type && !strcmp(collection_type, "music"))
        snprintf(path, path_size,
            "/Users/%s/Items?ParentId=%s&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks,ChildCount"
            "&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=%d&Limit=%d",
            cfg->user_id, safe_parent, start_index, max);
    /* TV and other collection types keep the standard direct-child query.
     * Series rows use ChildCount for seasons and RecursiveItemCount for
     * episodes, both computed in this batched listing instead of separate
     * requests per series. Unknown collection types retain the established
     * fields and behavior rather than assuming movie or music semantics. */
    else
        snprintf(path, path_size,
            "/Users/%s/Items?ParentId=%s&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks,ChildCount,RecursiveItemCount"
            "&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=%d&Limit=%d",
            cfg->user_id, safe_parent, start_index, max);
}

int eb_list_items(const EbConfig *cfg, const char *parent_id,
                   const char *collection_type, int start_index,
                   EbItem *out, int max, int64_t *total_out)
{
    char path[512];
    eb_build_items_path(cfg, parent_id, collection_type, start_index, max,
                        path, sizeof(path));

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;   /* transport/parse failure, not an empty page */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    eb_response_free(&r);
    return n;
}

int eb_list_items_recursive(const EbConfig *cfg, const char *parent_id,
                             const char *item_type, EbItem *out, int max)
{
    char safe_parent[EB_ID_LEN];
    eb_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    /* No Overview here either — see eb_list_items. This one only ever feeds
     * the carousel's background cover grid, which needs nothing but ids and
     * image tags. */
    char path[560];
    if (item_type && item_type[0])
        snprintf(path, sizeof(path),
            "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=%s"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
            cfg->user_id, safe_parent, item_type, max);
    else
        snprintf(path, sizeof(path),
            "/Users/%s/Items?ParentId=%s&Recursive=true"
            "&SortBy=SortName&SortOrder=Ascending"
            "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true"
            "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
            cfg->user_id, safe_parent, max);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return 0;
    int n = parse_item_list(&r.doc, out, max);
    eb_response_free(&r);
    return n;
}

int eb_list_random_tracks(const EbConfig *cfg, const char *parent_id, EbItem *out, int max)
{
    char safe_parent[EB_ID_LEN];
    eb_sanitize_id(parent_id, safe_parent, sizeof(safe_parent));

    char path[512];
    snprintf(path, sizeof(path),
        "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=Audio&SortBy=Random"
        "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary&Limit=%d",
        cfg->user_id, safe_parent, max);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return 0;
    int n = parse_item_list(&r.doc, out, max);
    eb_response_free(&r);
    return n;
}

/* Shared field list for the two home rows. Both want enough to draw a browse
 * row (year, runtime, watched/resume state, a cover) and nothing more. */
#define EB_HOME_ROW_FIELDS \
    "&Fields=ProductionYear,RunTimeTicks&EnableUserData=true" \
    "&ImageTypeLimit=1&EnableImageTypes=Primary&EnableTotalRecordCount=true"

int eb_list_resume(const EbConfig *cfg, EbItem *out, int max, int64_t *total_out)
{
    /* MediaTypes=Video keeps half-listened music out of a row the user reads
     * as "films and episodes I'm partway through". The server orders these by
     * DatePlayed descending server-side, which is the order to preserve. */
    char path[384];
    snprintf(path, sizeof(path),
        "/Users/%s/Items/Resume?MediaTypes=Video&Limit=%d" EB_HOME_ROW_FIELDS,
        cfg->user_id, max);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;   /* transport/parse failure, not an empty page */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    eb_response_free(&r);
    return n;
}

int eb_list_nextup(const EbConfig *cfg, EbItem *out, int max, int64_t *total_out)
{
    /* Emby's NextUp has no enableResumable switch, so a part-watched episode
     * can come back here as well as under Continue Watching — eb_list_nextup's
     * callers drop anything with a resume position, keeping Next Up strictly
     * "what to start next". */
    char path[384];
    snprintf(path, sizeof(path),
        "/Shows/NextUp?UserId=%s&Limit=%d" EB_HOME_ROW_FIELDS,
        cfg->user_id, max);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;   /* see eb_list_items */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    /* NextUp returns episodes by definition, but says so only via each item's
     * own Type — set it explicitly for the same reason eb_list_episodes does. */
    /* Emby has no enableResumable=false, so drop part-watched episodes here
     * — they already sit in Continue Watching. */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (out[i].resume_ticks > 0) continue;
        if (kept != i) out[kept] = out[i];
        out[kept].type = EB_TYPE_EPISODE;
        kept++;
    }
    if (total_out && *total_out > 0) *total_out -= (n - kept);
    eb_response_free(&r);
    return kept;
}

int eb_list_collections(const EbConfig *cfg, EbItem *out, int max, int64_t *total_out)
{
    char path[384];
    snprintf(path, sizeof(path),
        "/Users/%s/Items?Recursive=true&IncludeItemTypes=BoxSet"
        "&SortBy=SortName&SortOrder=Ascending&Fields=ProductionYear,ChildCount"
        "&EnableUserData=true&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop"
        "&EnableTotalRecordCount=true&Limit=%d",
        cfg->user_id, max);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    for (int i = 0; i < n; i++) out[i].type = EB_TYPE_FOLDER;
    eb_response_free(&r);
    return n;
}

int eb_get_item_details(const EbConfig *cfg, const char *item_id, EbItem *out)
{
    char safe_id[EB_ID_LEN];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));

    char path[384];
    snprintf(path, sizeof(path),
        "/Users/%s/Items/%s?Fields=Overview,ProductionYear,RunTimeTicks,People,MediaStreams,MediaSources,CommunityRating,Chapters"
        "&EnableUserData=true&EnableImageTypes=Primary,Logo,Backdrop",
        cfg->user_id, safe_id);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return 0;

    /* A single-item fetch returns the item object itself as the root, so all
     * three parsers are scoped to it rather than to a list element. */
    const JsonNode *item = json_root(&r.doc);
    parse_item_fields(&r.doc, item, out);
    parse_cast(&r.doc, item, out);
    parse_media_streams(&r.doc, item, out);
    parse_markers(&r.doc, item, out);
    copy_raw_str(&r.doc, item, "MediaSources[0].Id", out->media_source_id,
                 sizeof(out->media_source_id));
    /* Emby nests the streams inside each media source; the top-level
     * MediaStreams copy is not always present. */
    if (out->sub_count == 0 && out->audio_count == 0 && !out->source_video_codec[0]) {
        const JsonNode *src = json_find(&r.doc, item, "MediaSources[0]");
        if (src) parse_media_streams(&r.doc, src, out);
    }
    eb_response_free(&r);
    return 1;
}

int eb_list_seasons(const EbConfig *cfg, const char *series_id, EbItem *out, int max)
{
    char safe_series[EB_ID_LEN];
    eb_sanitize_id(series_id, safe_series, sizeof(safe_series));

    char path[256];
    snprintf(path, sizeof(path), "/Shows/%s/Seasons?UserId=%s", safe_series, cfg->user_id);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;   /* see eb_list_items */
    int n = parse_item_list(&r.doc, out, max);
    for (int i = 0; i < n; i++) out[i].type = EB_TYPE_SEASON;
    eb_response_free(&r);
    return n;
}

int eb_list_episodes(const EbConfig *cfg, const char *series_id, const char *season_id,
                      int start_index, EbItem *out, int max, int64_t *total_out)
{
    char safe_series[EB_ID_LEN], safe_season[EB_ID_LEN];
    eb_sanitize_id(series_id, safe_series, sizeof(safe_series));
    eb_sanitize_id(season_id, safe_season, sizeof(safe_season));
    if (start_index < 0) start_index = 0;

    char path[416];
    snprintf(path, sizeof(path),
        "/Shows/%s/Episodes?SeasonId=%s&UserId=%s"
        "&Fields=RunTimeTicks&EnableUserData=true"
        "&ImageTypeLimit=1&EnableImageTypes=Primary,Backdrop&StartIndex=%d&Limit=%d",
        safe_series, safe_season, cfg->user_id, start_index, max);

    EbResponse r;
    if (!eb_fetch(cfg, path, &r)) return -1;   /* see eb_list_items */
    int n = parse_item_list(&r.doc, out, max);
    parse_total_count(&r.doc, total_out);
    for (int i = 0; i < n; i++) out[i].type = EB_TYPE_EPISODE;
    eb_response_free(&r);
    return n;
}

/* ── images ───────────────────────────────────────────────────────────────── */

/* image_type is a caller-supplied constant (e.g. "Primary", "Logo",
 * "Backdrop/0"), never server/JSON data, so it's used as-is (it may
 * legitimately contain '/'); item_id and tag DO come from server JSON and
 * are sanitized. max_width <= 0 requests the original, full-size image —
 * always pass a real cap: the server resizes server-side (confirmed against a
 * real server: a 1.5MB backdrop dropped to ~15KB at maxWidth=320), which
 * matters a lot given this device's decode is scalar C with no SIMD path. */
int eb_item_image_url(const EbConfig *cfg, const char *item_id, const char *image_type,
                       const char *tag, int max_width, char *out, int outlen)
{
    if (!tag[0]) return 0;
    char safe_id[EB_ID_LEN], safe_tag[EB_ID_LEN];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));
    eb_sanitize_id(tag, safe_tag, sizeof(safe_tag));
    if (max_width > 0)
        snprintf(out, outlen, "%s" EB_API_BASE "/Items/%s/Images/%s?tag=%s&maxWidth=%d&quality=80",
                  cfg->server, safe_id, image_type, safe_tag, max_width);
    else
        snprintf(out, outlen, "%s" EB_API_BASE "/Items/%s/Images/%s?tag=%s",
                  cfg->server, safe_id, image_type, safe_tag);
    return 1;
}

int eb_download_item_image(const EbConfig *cfg, const char *item_id, const char *image_type,
                            const char *tag, int max_width, const char *dest_path)
{
    char url[512];
    if (!eb_item_image_url(cfg, item_id, image_type, tag, max_width, url, sizeof(url))) return 0;

    /* No shell — see eb_curl_run. The URL embeds an image tag that came
     * from server JSON. -L follows redirects; TLS verification per config
     * (see eb_add_tls_args). */
    const char *argv[12];
    int n = 0;
    argv[n++] = "curl"; argv[n++] = "-sfL";
    n = eb_add_tls_args(cfg, argv, n);
    argv[n++] = "--max-time"; argv[n++] = "15";
    argv[n++] = url; argv[n++] = "-o"; argv[n++] = dest_path;
    argv[n] = NULL;
    int ok = 0;
    eb_curl_run((char *const *)argv, 0, &ok);
    return ok;
}

int eb_image_url(const EbConfig *cfg, const EbItem *item, char *out, int outlen)
{
    return eb_item_image_url(cfg, item->id, "Primary", item->image_tag, 200, out, outlen);
}

int eb_download_image(const EbConfig *cfg, const EbItem *item, const char *dest_path)
{
    return eb_download_item_image(cfg, item->id, "Primary", item->image_tag, 200, dest_path);
}

/* ── playback ─────────────────────────────────────────────────────────────── */

const char *eb_direct_stream_codec(const EbItem *it)
{
    /* Codecs the MiSTer's Cortex-A9 decodes comfortably in software at SD
     * sizes: MPEG-4 Part 2 is what Emby itself transcodes to (measured at
     * ~3.8x real time for 720x300 on hardware), and the MS-MPEG4 / H.263 /
     * MPEG-1/2 family is lighter still. H.264 is left out — measured at only
     * ~2.6x for 720x300, which leaves too little margin for a full 720x576
     * frame. Names are Emby's MediaStreams[].Codec values. */
    static const char *const light[] = {
        "mpeg4", "msmpeg4v3", "msmpeg4v2", "msmpeg4", "h263",
        "mpeg1video", "mpeg2video", NULL
    };
    if (!it || !it->source_video_codec[0]) return NULL;
    if (it->source_width <= 0 || it->source_height <= 0) return NULL;
    if (it->source_width > 720 || it->source_height > 576) return NULL;
    if (it->source_interlaced) return NULL;
    if (it->source_fps > 30.5) return NULL;   /* 50/60fps would double the decode work */
    for (int i = 0; light[i]; i++)
        if (!strcasecmp(it->source_video_codec, light[i])) return light[i];
    return NULL;
}

int eb_stream_url(const EbConfig *cfg, const char *item_id,
                   const char *media_source_id, const char *copy_codec,
                   const EbStreamProfile *profile, int64_t start_ticks,
                   const char *play_session_id, int burn_in_sub_index,
                   int audio_stream_index,
                   char *out, int outlen)
{
    char safe_id[EB_ID_LEN];
    char safe_session[64];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));
    eb_sanitize_id(play_session_id, safe_session, sizeof(safe_session));
    /* audioCodec=mp3 (not aac) is deliberate: most sources here are already
     * AAC, and requesting audioCodec=aac lets the server just stream-copy the
     * original audio untouched — confirmed on a real server, even with
     * audioChannels=2 and allowAudioStreamCopy=false, neither budged it off
     * "copy". A source's original AAC track is very often 5.1, which means
     * mplayer has to decode 6 channels and downmix to stereo itself — extra
     * CPU work on top of an already-marginal software decode. Asking for a
     * codec that can't just be a source-format match (mp3) forces a genuine
     * transcode to stereo (confirmed: ffmpeg cmd showed
     * "-codec:a:0 libmp3lame -ac 2"), so mplayer only ever decodes simple
     * stereo MP3.
     *
     * VideoCodec=mpeg4 (MPEG-4 Part 2, not h264): Emby's bundled ffmpeg has
     * no MPEG-2 encoder — VideoCodec=mpeg2video returns HTTP 500 "No video
     * encoder found for 'mpeg2video'" (Emby 4.10.1), which is what MiSTerFin
     * asks its server for. MPEG-4 Part 2 is the closest light codec Emby
     * will encode: no CABAC, no in-loop deblocking, simple motion
     * compensation. Measured on a MiSTer against Emby 4.10.1 with a 720x300
     * transcode: MPEG-4 decoded at ~3.8x real time, H.264 at ~2.6x — the
     * headroom matters most for 4:3 titles that fill the whole box. The
     * stream.ts extension keeps MPEG-TS muxing, the container our
     * -demuxer lavf path is built around.
     *
     * maxFramerate, matched to the TV mode (PAL 25 / NTSC 30): the display
     * refresh is fixed by the TV mode, so a source on the OTHER standard's
     * rate (29.97fps NTSC material on a 50Hz PAL output) can never sit on a
     * clean cadence — frames alternate 1/2 vsyncs and the motion reads as
     * lagging/stuttering even though nothing is actually behind (confirmed
     * with a 29.97fps DVD-sourced .mpg on PAL: decode CPU, network, and the
     * delivered frames all measured healthy, yet playback looked wrong).
     * The server only acts on the cap when the source EXCEEDS it (verified
     * live on both cases: 29.97 source gained "fps=25" + "-r 25", a 23.976
     * source's command was byte-identical with and without the param), so
     * at-or-below-rate sources — the already-proven paths — are untouched.
     *
     * burn_in_sub_index — see eb_stream_url's header comment for why this
     * is only ever set for image-based subtitle tracks. */
    char sub_params[64] = "";
    if (burn_in_sub_index >= 0)
        snprintf(sub_params, sizeof(sub_params),
                 "&SubtitleStreamIndex=%d&SubtitleMethod=Encode", burn_in_sub_index);

    /* Omitted entirely rather than guessed when no explicit track is wanted,
     * so the server applies the source's own default selection. */
    char audio_params[32] = "";
    if (audio_stream_index >= 0)
        snprintf(audio_params, sizeof(audio_params),
                 "&AudioStreamIndex=%d", audio_stream_index);

    /* allowVideoStreamCopy=false: without it, a source that already matches
     * the request (an MPEG-2 DVD rip whose 720x576 fits inside the profile
     * box) gets stream-COPIED instead of transcoded — confirmed on hardware
     * with two PAL DVD rips at a 720x576 profile: severe slowdown plus
     * codec-looking glitches without this parameter, clean playback with it,
     * same titles/profile/session (note /Sessions' TranscodingInfo can't
     * tell the two apart — it comes back empty for ApiKey progressive
     * streams either way). The whole playback pipeline here is built on the
     * server delivering a clean, progressive, freshly-encoded stream (the
     * mplayer chain has no deinterlacer, and raw DVD MPEG-2 is interlaced
     * VBR with DVD-style timestamps) — so forbid the copy shortcut
     * outright; a genuine re-encode of an already-small source is cheap for
     * the server anyway. Audio needs no equivalent: audioCodec=mp3 already
     * forces a real audio transcode, see the comment above. */
    int fps_cap = (strcasecmp(cfg->tv_mode, "NTSC") == 0) ? 30 : 25;

    /* audioSampleRate=48000: without it the mp3 output keeps the SOURCE's
     * sample rate (confirmed live: a 22050Hz-AAC movie produced a 22050Hz
     * mp3 stream), and the MiSTer's ALSA default device force-resamples
     * everything to 48kHz through ALSA's low-quality linear-interpolation
     * rate plugin on its way to the FPGA (/etc/asound.conf on the device).
     * Pinning the stream to 48kHz moves that resample to the server's
     * proper swresample and turns the device-side one into a no-op.
     * (Music playback can't get this server-side fix — it streams the
     * original file untranscoded — so the audio player resamples in
     * mplayer instead; see play_audio.)
     *
     * deviceId: mplayer fetches this URL itself, so unlike every curl
     * request it can't carry the Authorization header that normally
     * identifies this install — without the query param the server files
     * the transcode job under an anonymous device and the dashboard's
     * session panel can never attach its TranscodingInfo (what's actually
     * being converted, and to what) to this client's session entry. */
    /* Emby picks the output container from the URL's extension (stream.ts)
     * and wants an explicit MediaSourceId on a transcode; DeviceId is
     * mandatory for Emby to create the transcode job at all. */
    char safe_msid[EB_ID_LEN + 16];
    eb_sanitize_id(media_source_id && media_source_id[0] ? media_source_id : item_id,
                   safe_msid, sizeof(safe_msid));
    /* Direct stream: name the source's own codec and allow the copy, so
     * Emby remuxes the video into TS untouched and only converts the audio
     * (still mp3 stereo, for the same reasons as above). copy_codec is one
     * of eb_direct_stream_codec's constants, never server data. */
    char video_params[96];
    if (copy_codec && copy_codec[0])
        snprintf(video_params, sizeof(video_params),
                 "&VideoCodec=%s&AllowVideoStreamCopy=true", copy_codec);
    else
        snprintf(video_params, sizeof(video_params),
                 "&VideoCodec=mpeg4&AllowVideoStreamCopy=false&EnableAutoStreamCopy=false");
    snprintf(out, outlen,
        "%s" EB_API_BASE "/Videos/%s/stream.ts?Static=false&MediaSourceId=%s"
        "%s&AudioCodec=mp3&AudioChannels=2"
        "&AudioSampleRate=48000"
        "&MaxWidth=%d&MaxHeight=%d&VideoBitRate=%d&MaxFramerate=%d"
        "&StartTimeTicks=%lld&PlaySessionId=%s&DeviceId=%s%s%s&api_key=%s",
        cfg->server, safe_id, safe_msid, video_params,
        profile->max_width, profile->max_height, profile->video_bitrate, fps_cap,
        (long long)(start_ticks > 0 ? start_ticks : 0),
        safe_session, cfg->device_id, sub_params, audio_params, cfg->token);
    return 1;
}

/* Codec strings per ffmpeg/the server's known text subtitle codecs — anything
 * not in this list is treated as image-based (safer default: an unknown
 * text codec would just fail eb_download_subtitle's fetch harmlessly via
 * the existing "previous subtitle stays loaded" fallback in main.c, whereas
 * treating an actual image codec as text would silently show nothing with
 * no fallback at all). */
int eb_subtitle_is_text(const char *codec)
{
    static const char *const text_codecs[] = {
        "subrip", "srt", "ass", "ssa", "vtt", "webvtt", "mov_text",
        "microdvd", "sami", "smi", "ttml", "stl", NULL
    };
    for (int i = 0; text_codecs[i]; i++)
        if (!strcasecmp(codec, text_codecs[i])) return 1;
    return 0;
}

int eb_audio_stream_url(const EbConfig *cfg, const char *item_id,
                         const char *play_session_id, char *out, int outlen)
{
    char safe_id[EB_ID_LEN];
    char safe_session[64];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));
    eb_sanitize_id(play_session_id, safe_session, sizeof(safe_session));
    snprintf(out, outlen,
        "%s" EB_API_BASE "/Audio/%s/stream?Static=true&PlaySessionId=%s&DeviceId=%s&api_key=%s",
        cfg->server, safe_id, safe_session, cfg->device_id, cfg->token);
    return 1;
}

int eb_download_subtitle(const EbConfig *cfg, const char *item_id,
                          const char *media_source_id, int sub_index,
                          const char *dest_path)
{
    char safe_id[EB_ID_LEN], safe_msid[EB_ID_LEN + 16];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));
    eb_sanitize_id(media_source_id && media_source_id[0] ? media_source_id : item_id,
                   safe_msid, sizeof(safe_msid));

    /* Sized for a full server URL plus a 191-byte token: 384 bytes could
     * truncate the ApiKey, which showed up as subtitles silently failing to
     * load rather than as any visible error. */
    char url[768];
    snprintf(url, sizeof(url), "%s" EB_API_BASE "/Videos/%s/%s/Subtitles/%d/Stream.srt?api_key=%s",
              cfg->server, safe_id, safe_msid, sub_index, cfg->token);

    const char *argv[12];
    int n = 0;
    argv[n++] = "curl"; argv[n++] = "-sfL";
    n = eb_add_tls_args(cfg, argv, n);
    argv[n++] = "--max-time"; argv[n++] = "15";
    argv[n++] = url; argv[n++] = "-o"; argv[n++] = dest_path;
    argv[n] = NULL;
    int ok = 0;
    eb_curl_run((char *const *)argv, 0, &ok);
    return ok;
}

void eb_make_play_session_id(char *out, int outlen)
{
    snprintf(out, outlen, "misteremby-%d-%ld", (int)getpid(), (long)time(NULL));
}

/* Video is always a transcode (eb_stream_url forbids stream copy); music
 * streams the original file untouched (static=true) — the dashboard's
 * session panel shows whichever one the reports claim, so claiming
 * "Transcode" for everything (as this used to) told admins their music was
 * being converted when it wasn't. Set by eb_report_start, reused by the
 * progress/stopped bodies for the rest of that playback. */
static char g_play_method[16] = "Transcode";

static void build_playstate_json(const char *item_id, const char *play_session_id,
                                  int64_t position_ticks, int is_paused,
                                  char *out, int outlen)
{
    char safe_id[EB_ID_LEN], safe_session[64];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));
    eb_sanitize_id(play_session_id, safe_session, sizeof(safe_session));
    snprintf(out, outlen,
        "{\"ItemId\":\"%s\",\"PlaySessionId\":\"%s\","
        "\"PositionTicks\":%lld,\"IsPaused\":%s,\"PlayMethod\":\"%s\"}",
        safe_id, safe_session, (long long)position_ticks,
        is_paused ? "true" : "false", g_play_method);
}

void eb_report_start(const EbConfig *cfg, const char *item_id,
                      const char *play_session_id, int64_t position_ticks,
                      const char *play_method)
{
    snprintf(g_play_method, sizeof(g_play_method), "%s", play_method);
    char body[512];
    build_playstate_json(item_id, play_session_id, position_ticks, 0, body, sizeof(body));
    eb_post_json(cfg, "/Sessions/Playing", body);
    /* The server applies PlayMethod from PROGRESS reports only — confirmed
     * empirically against a real server: a start report claiming Transcode
     * left the session's PlayState at DirectPlay (so the dashboard's "i"
     * panel showed "source entirely compatible... without modifications"),
     * and the very first progress report flipped it to Transcode. One
     * immediate progress makes the dashboard truthful from the first
     * second instead of whenever the periodic report happens to fire. */
    eb_post_json(cfg, "/Sessions/Playing/Progress", body);
}

/* Sessions/Playing/Progress and /Stopped (used above for eb_report_start)
 * need a real logged-in session to correlate against — confirmed on a real
 * server: POSTing to them with just an API key returns 200/204 but silently
 * does NOT persist PlaybackPositionTicks (resume always came back to the
 * same old position no matter where playback actually stopped). The direct
 * per-user item data endpoint below has no such requirement and is
 * confirmed working. */
static void report_user_data(const EbConfig *cfg, const char *item_id,
                              int64_t position_ticks, int played)
{
    char safe_id[EB_ID_LEN];
    eb_sanitize_id(item_id, safe_id, sizeof(safe_id));
    char path[256];
    snprintf(path, sizeof(path), "/Users/%s/Items/%s/UserData", cfg->user_id, safe_id);

    char body[128];
    snprintf(body, sizeof(body), "{\"PlaybackPositionTicks\":%lld,\"Played\":%s}",
              (long long)position_ticks, played ? "true" : "false");
    eb_post_json(cfg, path, body);

    /* Emby's dedicated played-state route — what its own apps call when a
     * title finishes. Sent in addition to the UserData update above so a
     * finished title reliably leaves Continue Watching. */
    if (played) {
        snprintf(path, sizeof(path), "/Users/%s/PlayedItems/%s", cfg->user_id, safe_id);
        eb_post_json(cfg, path, "{}");
    }
}

/* Live session-state update for the dashboard only (position, paused flag,
 * play method) — per the comment above, the /Sessions endpoints don't
 * persist the resume position, so this never REPLACES report_user_data,
 * it's the other half: user_data persists, this one keeps the admin
 * dashboard's Active Sessions card (and its pause/position display)
 * current. */
void eb_report_session_progress(const EbConfig *cfg, const char *item_id,
                                 const char *play_session_id,
                                 int64_t position_ticks, int paused)
{
    char body[512];
    build_playstate_json(item_id, play_session_id, position_ticks, paused, body, sizeof(body));
    eb_post_json(cfg, "/Sessions/Playing/Progress", body);
}

void eb_report_progress(const EbConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks, int paused)
{
    report_user_data(cfg, item_id, position_ticks, 0);
    eb_report_session_progress(cfg, item_id, play_session_id, position_ticks, paused);
}

/* Fire-and-forget POST on its own detached thread. Exists for exactly one
 * caller so far: the video-stop session report below, where the server
 * synchronously kills the live ffmpeg transcode before answering —
 * measured at 2+ seconds against a real server, which on the main thread
 * was a visible multi-second hang on every exit from playback. */
typedef struct {
    const EbConfig *cfg;   /* main's g_cfg — outlives every thread */
    char  path[64];
    char *body;
} EbAsyncPost;

static void *eb_post_json_thread(void *arg)
{
    EbAsyncPost *p = arg;
    eb_post_json(p->cfg, p->path, p->body);
    free(p->body);
    free(p);
    return NULL;
}

static void eb_post_json_async(const EbConfig *cfg, const char *path, const char *body)
{
    EbAsyncPost *p = malloc(sizeof(*p));
    if (!p) return;
    p->cfg = cfg;
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->body = strdup(body);
    if (!p->body) { free(p); return; }
    pthread_t t;
    if (pthread_create(&t, NULL, eb_post_json_thread, p) == 0)
        pthread_detach(t);
    else {
        free(p->body);
        free(p);
    }
}

static void report_stopped(const EbConfig *cfg, const char *item_id,
                           const char *play_session_id, int64_t position_ticks,
                           int played, int allow_async);

void eb_report_stopped(const EbConfig *cfg, const char *item_id,
                        const char *play_session_id, int64_t position_ticks,
                        int played)
{
    report_stopped(cfg, item_id, play_session_id, position_ticks, played, 1);
}

void eb_report_stopped_sync(const EbConfig *cfg, const char *item_id,
                             const char *play_session_id, int64_t position_ticks,
                             int played)
{
    report_stopped(cfg, item_id, play_session_id, position_ticks, played, 0);
}

static void report_stopped(const EbConfig *cfg, const char *item_id,
                           const char *play_session_id, int64_t position_ticks,
                           int played, int allow_async)
{
    /* Tell the session tracker playback ended so the dashboard's card
     * clears right away instead of lingering until the session times out.
     * Async for video (see eb_post_json_async — the server kills the live
     * ffmpeg job synchronously and takes seconds to answer); kept inline
     * for music, where there's no transcode job (the answer is instant)
     * and a next/previous-track skip posts the next track's start right
     * behind this, so keeping order costs nothing and avoids the stop
     * landing after the new start on the server. */
    {
        char body[512];
        build_playstate_json(item_id, play_session_id, position_ticks, 0, body, sizeof(body));
        if (allow_async && strcmp(g_play_method, "Transcode") == 0)
            eb_post_json_async(cfg, "/Sessions/Playing/Stopped", body);
        else
            eb_post_json(cfg, "/Sessions/Playing/Stopped", body);
    }
    /* When the title was watched to the end, the server's convention is
     * Played=true with the resume position cleared to 0. Reporting the
     * near-end position with Played=false unconditionally (what this used to
     * do) left every finished title still offering a resume that jumped
     * straight to the last few seconds — and kept it sitting in the Continue
     * Watching row instead of moving the series on to Next Up. */
    report_user_data(cfg, item_id, played ? 0 : position_ticks, played);
}

/* Registers what this client can do with the session the server keeps for
 * our DeviceId — this is what makes the dashboard's Active Sessions card
 * grow its message and pause/stop controls. Re-sent by the session module
 * after every (re)connect of the command socket rather than once at
 * startup, since the server drops session state when the device goes away. */
void eb_report_capabilities(const EbConfig *cfg)
{
    eb_post_json(cfg, "/Sessions/Capabilities/Full",
        "{\"PlayableMediaTypes\":[\"Video\",\"Audio\"],"
        "\"SupportedCommands\":[\"DisplayMessage\"],"
        "\"SupportsMediaControl\":true,"
        "\"SupportsPersistentIdentifier\":false}");
}

int view_is_resume(const EbItem *v)    { return v->synthetic == EB_SYNTH_RESUME; }
int view_is_nextup(const EbItem *v)    { return v->synthetic == EB_SYNTH_NEXTUP; }
int view_is_collections(const EbItem *v) { return v->synthetic == EB_SYNTH_COLLECTIONS; }
int view_is_synthetic(const EbItem *v) { return v->synthetic != 0; }

const char *collection_item_type(const char *collection_type)
{
    if (!strcmp(collection_type, "movies"))      return "Movie";
    if (!strcmp(collection_type, "tvshows"))     return "Series";
    if (!strcmp(collection_type, "music"))       return "MusicAlbum";
    if (!strcmp(collection_type, "musicvideos")) return "MusicVideo";
    return NULL;
}
