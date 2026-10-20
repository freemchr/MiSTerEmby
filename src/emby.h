#pragma once
#include <stdint.h>
#include <sys/types.h>

/* Emby REST client — curl run directly via fork/execv, no libcurl dependency.
 *
 * Every request goes under the server's /emby base path with
 * X-Emby-Authorization / X-Emby-Token headers, user-scoped calls use the
 * /Users/{id}/... routes, media URLs authenticate with api_key, and sign-in
 * uses Emby's own paths: a password, an Emby Connect account, or an Emby
 * Connect PIN.
 *
 * Ported from MiSTerFin. Comments carried over from it that say a behaviour
 * was "confirmed on a real server" describe MiSTerFin's own testing against
 * its media server unless they name Emby. */

#define EB_MAX_ITEMS   256
/* How many items one browse request asks for. Smaller than EB_MAX_ITEMS on
 * purpose: the whole response has to fit in eb_get's fixed buffer, and at
 * roughly 700 bytes of JSON per row a full 256 would land uncomfortably
 * close to it. Anything past a truncation point is silently lost (the parser
 * just stops at the first malformed object), so the margin matters more than
 * the round-trip count — and a library only pays for the pages actually
 * scrolled to. The client keeps ONE such page loaded at a time and slides it
 * (see eb_list_items' start_index). */
#define EB_PAGE_SIZE   128
/* Upper bound applied to a server-reported TotalRecordCount before it reaches
 * the client's window arithmetic. Far beyond any real library, but finite:
 * the alternative is trusting a remote integer not to overflow int math. */
#define EB_MAX_TOTAL_ITEMS 100000000
#define EB_ID_LEN      40
#define EB_NAME_LEN    256
#define EB_OVERVIEW_LEN 1024
#define EB_PERSON_NAME_LEN 64
#define EB_MAX_CAST    8
#define EB_MAX_SUBS    8
#define EB_MAX_AUDIO   8
/* Long enough for a fully-composed DisplayTitle — "English - Hearing
 * Impaired - Default - SUBRIP - External" is 55 characters. Storing the whole
 * thing means any shortening happens at draw time, where it's clipped to the
 * actual pixel width available, rather than being cut blindly here. */
#define EB_SUB_LABEL_LEN 72

typedef enum {
    EB_TYPE_FOLDER,   /* library view / generic folder */
    EB_TYPE_SERIES,
    EB_TYPE_SEASON,
    EB_TYPE_EPISODE,
    EB_TYPE_MOVIE,
    EB_TYPE_MUSIC_VIDEO, /* MusicVideo — playable video leaf */
    EB_TYPE_ARTIST,   /* MusicArtist — drills into albums, browsed like EB_TYPE_FOLDER */
    EB_TYPE_ALBUM,    /* MusicAlbum — drills into tracks, browsed like EB_TYPE_FOLDER */
    EB_TYPE_TRACK,    /* Audio — playable leaf, like EB_TYPE_MOVIE but audio-only */
    EB_TYPE_OTHER
} EbItemType;

typedef struct {
    char id[EB_ID_LEN];
    char name[EB_PERSON_NAME_LEN];
    char role[EB_PERSON_NAME_LEN];      /* character/role, empty if n/a */
    char image_tag[EB_ID_LEN];          /* PrimaryImageTag, empty if none */
} EbPerson;

typedef struct {
    int  index;                        /* MediaStreams[].Index — pass as subtitleStreamIndex */
    char label[EB_SUB_LABEL_LEN];       /* DisplayTitle if set, else Language. The server
                                         * builds DisplayTitle as the track's own Title
                                         * followed by whichever of Hearing Impaired /
                                         * Default / Forced / codec / External apply —
                                         * which is what separates a dialogue track from a
                                         * signs-and-songs one when both are tagged "eng" */
    char codec[16];                    /* MediaStreams[].Codec, e.g. "subrip", "ass",
                                         * "pgssub", "dvdsub" — see eb_subtitle_is_text() */
    int  is_forced;                    /* MediaStreams[].IsForced */
    int  is_default;                   /* MediaStreams[].IsDefault */
} EbSubtitle;

typedef struct {
    int  index;                        /* MediaStreams[].Index — pass as audioStreamIndex */
    char label[EB_SUB_LABEL_LEN];       /* DisplayTitle if set, else Language — the server
                                         * composes DisplayTitle as e.g. "English - Dolby
                                         * Digital 5.1 - Default", which is what actually
                                         * distinguishes a commentary from the main mix
                                         * when both are tagged the same language */
    char codec[16];                    /* "ac3", "aac", "dts", ... */
    int  channels;                     /* 2, 6, ... — 0 if unreported */
    int  is_default;                   /* MediaStreams[].IsDefault */
} EbAudioTrack;

typedef struct {
    char       id[EB_ID_LEN];
    char       name[EB_NAME_LEN];
    char       overview[EB_OVERVIEW_LEN];
    char       image_tag[EB_ID_LEN];    /* ImageTags.Primary, falling back to
                                          * AlbumPrimaryImageTag for a track that
                                          * doesn't carry its own embedded art (which
                                          * is normal — plenty of files have none, the
                                          * album cover is still the right thing to
                                          * show). Fetch from image_item_id, not id. */
    char       image_item_id[EB_ID_LEN]; /* == id normally; == AlbumId when image_tag
                                           * came from the AlbumPrimaryImageTag fallback */
    char       backdrop_tag[EB_ID_LEN]; /* BackdropImageTags[0], falling back to
                                          * ParentBackdropImageTags[0] for episodes/
                                          * seasons that don't carry their own (which
                                          * is normal — that art is series-level).
                                          * Fetch from backdrop_item_id, not id. */
    char       logo_tag[EB_ID_LEN];     /* ImageTags.Logo, falling back to
                                          * ParentLogoImageTag for the same reason.
                                          * Fetch from logo_item_id, not id. */
    char       backdrop_item_id[EB_ID_LEN]; /* == id normally; == ParentBackdropItemId
                                              * when backdrop_tag came from the fallback */
    char       logo_item_id[EB_ID_LEN];     /* == id normally; == ParentLogoItemId
                                              * when logo_tag came from the fallback */
    char       year[8];
    char       album[EB_NAME_LEN];   /* Album — tracks only, empty otherwise */
    char       artist[EB_NAME_LEN];  /* AlbumArtist — tracks only, empty otherwise */
    /* SeriesName — episodes only, empty otherwise. Redundant inside a season's
     * own episode list (the frame title already says which show it is), but
     * essential for the Continue Watching / Next Up rows, where an episode
     * called "Episode 03" is otherwise unidentifiable. */
    char       series_name[EB_NAME_LEN];
    /* MediaSources[0].Id — empty unless fetched via eb_get_item_details.
     * Emby wants it on stream and subtitle URLs, and it is not always the
     * item id (Emby 4.8+ can report "mediasource_<n>"), so it is read rather
     * than assumed; callers fall back to the item id when it's empty. */
    char       media_source_id[EB_ID_LEN + 16];
    char       collection_type[16];  /* CollectionType — "movies"/"tvshows"/"music"/...,
                                       * library views only (eb_list_views), empty otherwise */
    EbItemType type;
    /* Non-zero only for the client's own synthetic home-screen entries (see
     * EB_SYNTH_*). parse_item_fields memsets, so anything that came from the
     * server always reads 0 — which is the point: identifying these by id
     * alone let a hostile server return an item with that id and shadow a
     * real library into being unreachable. */
    int        synthetic;
    int64_t    runtime_ticks;          /* RunTimeTicks, 0 if unknown */
    int        child_count;            /* ChildCount — track count for a MusicAlbum
                                         * (eb_list_items only requests this field);
                                         * 0/unset for every other item type. */
    /* RecursiveItemCount — every non-folder, non-virtual descendant, i.e.
     * the episode count for a Series (confirmed against the server's own
     * count query, which filters exactly that way, so unaired/missing
     * episodes are excluded — what you'd actually want to read on a row).
     * Requires Fields=RecursiveItemCount AND EnableUserData=true: the
     * server computes it inside its user-data block, batched across the
     * whole result set in one query. That batching is the entire point of
     * using it — the client used to issue one extra recursive count request
     * PER SERIES ROW to get this same number, so a 200-series library meant
     * 200 sequential curl invocations before the list could draw. 0 if not
     * requested or not applicable. */
    int        recursive_item_count;
    int64_t    resume_ticks;           /* UserData.PlaybackPositionTicks */
    int        played;                 /* UserData.Played */
    int        index_number;           /* episode/season number, -1 if n/a */
    int        parent_index_number;    /* ParentIndexNumber — season number for an
                                         * episode, -1 if n/a. With index_number this
                                         * gives the "S1E03" an out-of-context episode
                                         * row needs. */
    double     community_rating;       /* CommunityRating, e.g. 7.4 — 0 if unset/not fetched */
    EbPerson   cast[EB_MAX_CAST];       /* Actors only, empty unless fetched via eb_get_item_details */
    int        cast_count;
    EbSubtitle subs[EB_MAX_SUBS];       /* empty unless fetched via eb_get_item_details */
    int        sub_count;
    /* Selectable audio tracks, same lifecycle as subs[]. Unlike subtitles
     * these can't be switched client-side: the server transcodes exactly one
     * audio stream into the delivered container, so changing tracks means
     * rebuilding the stream URL and restarting playback. */
    EbAudioTrack audio[EB_MAX_AUDIO];
    int          audio_count;
    /* Source (original file) video specs — from MediaStreams, empty unless
     * fetched via eb_get_item_details. What's actually streamed to the
     * device is different (see EbStreamProfile) — this is for display only. */
    char       source_video_codec[16];
    int        source_width, source_height;
    int64_t    source_bitrate;          /* bits/sec, 0 if unknown */
    /* Display aspect ratio (width/height), from the video stream's own
     * "AspectRatio" field (e.g. "16:9") rather than source_width/height —
     * those are raw pixel dims and can be wrong for anamorphic sources
     * (e.g. a PAL DVD's 720x576 is really 16:9 despite a 5:4 pixel ratio,
     * via non-square SAR). 0 if not fetched/parseable — caller should fall
     * back to source_width/source_height in that case. */
    double     source_aspect;
    /* Video stream's IsInterlaced / RealFrameRate — decide whether the
     * source can be streamed with its video untouched (see
     * eb_direct_stream_codec). 0 if not fetched. */
    int        source_interlaced;
    double     source_fps;
    /* Emby's intro/credits markers (Chapters[].MarkerType IntroStart /
     * IntroEnd / CreditsStart, Emby 4.8+ once the server has detected
     * them), in ticks. intro_end_ticks == 0 means no intro marker;
     * credits_start_ticks == 0 means no credits marker. Empty unless
     * fetched via eb_get_item_details. */
    int64_t    intro_start_ticks;
    int64_t    intro_end_ticks;
    int64_t    credits_start_ticks;
} EbItem;

typedef struct {
    char server[256];   /* e.g. http://192.168.2.10:8096 (no trailing slash, no /emby) */
    char api_key[64];   /* config line 2 or API_KEY=, may be empty — see eb_config_load */
    char username[64];  /* config line 3 or USERNAME= — resolved to user_id via
                         * eb_resolve_user_id() (API key) or signed in with
                         * eb_authenticate_by_name() (PASSWORD=) */
    char password[128]; /* PASSWORD= — local Emby user password, may be empty */
    char connect_email[128];    /* CONNECT_USER= — Emby Connect name or email */
    char connect_password[128]; /* CONNECT_PASSWORD= */
    char user_id[EB_ID_LEN];   /* empty until resolved; everything else needs this, not username */
    char tv_mode[8];     /* "PAL" (default) or "NTSC" — 4th, optional config line */
    /* The credential actually sent with every request, as both the
     * Authorization header's Token and the ApiKey query parameter on media
     * URLs. Either an API key copied from api_key, or an access token earned
     * through a sign-in — the server treats the two identically, so
     * nothing downstream needs to know which it got. Empty means
     * unauthenticated, which is valid only for the sign-in calls themselves. */
    char token[192];
    /* Identifies this install to the server. Generated once and persisted —
     * NOT a compile-time constant shared by every copy of the app, which it
     * used to be.
     *
     * That matters as soon as a sign-in creates a session: the server
     * logs out every existing session matching
     * (DeviceId, UserId) whenever a new one authenticates, so two MiSTers
     * signed in as the same user and reporting the same DeviceId would
     * silently revoke each other's token on every launch — and since a
     * revoked token sends this client back through sign-in, they'd
     * take turns kicking one another out indefinitely. The API key path
     * creates no session, which is why this was harmless before. */
    char device_id[64];
    /* Transcode resolution/bitrate requested for video, overridable from the
     * config file (see eb_config_load). Configurable because the useful
     * resolution ceiling on this hardware is an open question that can only
     * be answered by measuring on a real MiSTer — and rebuilding and
     * reflashing for each data point makes that experiment tedious enough
     * that it doesn't get done. */
    int  profile_width;
    int  profile_height;
    int  profile_bitrate;
    /* "DEBUGLOG" config line — see eb_log_init. Off unless the user opted
     * in, so nothing is ever written without them asking for it. */
    int  debug_log;
    /* "INSECURE_TLS" config line. Default 0 = verify the server's HTTPS
     * certificate; 1 = skip verification (curl -k), for a home server with
     * a self-signed cert. Only matters for https:// servers — a plain
     * http:// server (the common case) is unaffected either way. */
    int  insecure_tls;
    /* "TRANSCODE_ALWAYS" config line: never stream a source's video
     * untouched, even when eb_direct_stream_codec says it could. For
     * troubleshooting a file that plays badly direct. */
    int  transcode_always;
    /* "CINEMA_MODE" config line: play the trailers Emby's Cinema Mode picks
     * (eb_list_intros) before a film started from the beginning. */
    int  cinema_mode;
} EbConfig;

/* One in-flight Emby Connect PIN request (emby.media/pin). */
typedef struct {
    char pin[16];       /* the short code the user types at emby.media/pin */
} EbConnectPin;

/* An Emby Connect account session — the cloud-side credential, which is
 * only ever used to look up linked servers and exchange for a server one. */
typedef struct {
    char user_id[64];
    char token[192];
} EbConnectAuth;

/* Folds a UTF-8 string down to the ASCII subset the on-screen bitmap font can
 * draw (accents stripped, smart quotes and dashes normalised, anything with
 * no ASCII form collapsed to a single '?'). Every server string that reaches
 * the screen already goes through this on the way into a EbItem; it's exposed
 * for callers that get text from somewhere else, e.g. downloaded subtitles. */
void eb_text_to_display(const char *utf8, char *out, int outlen);

/* Loads /media/fat/misteremby/emby.conf (falls back to ./emby.conf for
 * desktop testing). 4 lines: server, api_key, username, tv_mode (optional,
 * defaults to PAL). Returns 1 if there's at least a server URL, 0 otherwise.
 *
 * Only the server URL is genuinely required now. An empty api_key/username is
 * no longer a failure — it means "authenticate with sign-in instead",
 * which is the point: obtaining an API key needs the admin dashboard, so
 * requiring one made every user an admin of their own server or dependent on
 * someone who is. cfg->user_id is NOT populated here; see eb_resolve_user_id
 * or eb_authenticate_by_name/eb_connect_link_server. */
int eb_config_load(EbConfig *cfg);

/* Whether cfg carries a usable credential at all (an API key from the config
 * file, or a token loaded/earned at runtime). */
int eb_has_credential(const EbConfig *cfg);

/* ── opt-in request log ──────────────────────────────────────────────────
 * For diagnosing exactly the kind of bug that otherwise needs someone to SSH
 * in and reproduce it live (see the #8 issue thread) — a user who hits e.g.
 * "Nothing here" or a stuck sign-in screen can turn this on, reproduce
 * it, and attach /media/fat/misteremby/debug.log to the report instead.
 *
 * Deliberately narrow, by construction rather than by care taken at each
 * call site:
 *   - Off unless cfg->debug_log is set ("DEBUGLOG" in emby.conf) — never
 *     writes to the SD card otherwise.
 *   - Truncated fresh on every launch, same as crash.log — one session's
 *     worth, not an unbounded file.
 *   - Every request is logged as METHOD + path only, with the query string
 *     always cut at '?'. That's what keeps a sign-in secret
 *     (the PIN on Emby Connect's /pin poll) and every userId out of the
 *     file without needing per-endpoint judgment calls. The Authorization
 *     header (bearing the API key or access token) is never passed to the
 *     logger at all — eb_log_request's signature has no parameter for it. */
void eb_log_init(const EbConfig *cfg);
void eb_log_close(void);

/* Freeform companion to the request log above — same file, same privacy
 * rule (nothing a user didn't already put in emby.conf/MiSTer.ini
 * themselves), same no-op when DEBUGLOG is off. For startup diagnostics
 * (framebuffer geometry, input devices, menu core/DDR state, player
 * lifecycle, ...) that aren't a server request and so don't fit
 * eb_log_request's columns. */
void eb_log_line(const char *fmt, ...);

/* Fills in cfg->device_id, generating and persisting a fresh random GUID the
 * first time. Call once after eb_config_load, before any request — the id
 * goes in the Authorization header of every one of them, and must not change
 * between authenticating and using the resulting token. Losing the file just
 * means the server sees a new device; nothing breaks. */
void eb_device_id_init(EbConfig *cfg);

/* ── sign-in ────────────────────────────────────────────────────────────────
 * The gamepad-friendly sign-in is Emby Connect's PIN flow: the client asks
 * connect.emby.media for a short PIN, the user enters it at emby.media/pin
 * while signed in to their Emby Connect account, and the client exchanges the
 * confirmed PIN for that account's token. The account's linked servers carry
 * an AccessKey that the server itself trades for an ordinary local user token
 * (/Connect/Exchange) — which is what every other request then uses.
 *
 * Typed credentials are also accepted from emby.conf (PASSWORD= for a local
 * user, CONNECT_USER=/CONNECT_PASSWORD= for an Emby Connect account), for
 * servers whose users aren't linked to Emby Connect. */

/* Local user sign-in: POST /Users/AuthenticateByName {Username, Pw}. Fills
 * cfg->token and cfg->user_id. 1 = signed in, 0 = server rejected the
 * credentials, -1 = request failed (unreachable). */
int eb_authenticate_by_name(EbConfig *cfg);

/* One entry from the server's public user list (/Users/Public) — the
 * accounts Emby shows on its own sign-in screen. */
typedef struct {
    char id[EB_ID_LEN];
    char name[64];
    int  has_password;
} EbPublicUser;

/* Fills out[] with the server's visible users. Returns the count, or -1 if
 * the request failed. */
int eb_list_public_users(const EbConfig *cfg, EbPublicUser *out, int max);

/* Emby Connect account sign-in with a name/email and password. 1 on success. */
int eb_connect_login(const EbConfig *cfg, EbConnectAuth *ca);

/* Starts a PIN request. Returns 1 and fills pin->pin on success. */
int eb_pin_create(const EbConfig *cfg, EbConnectPin *pin);

/* Polls a pending PIN: 1 = confirmed, 0 = still waiting, -1 = expired or the
 * request failed. Poll at a human pace (a few seconds). */
int eb_pin_poll(const EbConfig *cfg, const EbConnectPin *pin);

/* Exchanges a confirmed PIN for the Emby Connect account's credential. */
int eb_pin_exchange(const EbConfig *cfg, const EbConnectPin *pin, EbConnectAuth *ca);

/* Finds cfg->server among the account's linked servers (by its local or
 * remote address, or the only linked server if there's just one) and
 * exchanges its AccessKey for a local user token, filling cfg->token,
 * cfg->user_id and cfg->username. 1 = signed in, 0 = this server isn't
 * linked to the account, -1 = a request failed. */
int eb_connect_link_server(EbConfig *cfg, const EbConnectAuth *ca);

/* Persist / restore the earned token so sign-in is a one-time step
 * rather than something to repeat at every launch. Stored next to
 * emby.conf as plain text — no worse than the API key that already lives
 * there, and anyone who can read the SD card can read both. */
int eb_token_save(const EbConfig *cfg);
int eb_token_load(EbConfig *cfg);
void eb_token_clear(void);

/* Cheap authenticated request, to tell a still-valid saved token from one the
 * server has since revoked. Returns 1 if the credential works, 0 if the
 * server rejected it, -1 if the server couldn't be reached at all (keep the
 * credential — it may be perfectly good). */
int eb_credential_works(const EbConfig *cfg);

/* 1 if the server answers its unauthenticated /System/Info/Public call. */
int eb_server_reachable(const EbConfig *cfg);

/* Looks up cfg->username in GET /Users and fills in cfg->user_id. The raw
 * Emby user id (a GUID) is not something a user can easily find in the
 * Emby web UI (it only shows up in a dashboard URL) — a plain username
 * is what everyone actually knows, so the client resolves it instead of
 * asking for the id directly.
 * Returns 1 on success, 0 if the server answered but no user matched
 * cfg->username (typo, or the user genuinely doesn't exist), or -1 if the
 * request itself failed (server unreachable — wrong URL, server down,
 * network issue) — distinct from 0 so the setup screen can tell "can't
 * reach your server" apart from "server's fine, but check your config"
 * instead of always blaming the config either way. */
int eb_resolve_user_id(EbConfig *cfg);

/* Top-level library views (Movies, TV Shows, ...). */
int eb_list_views(const EbConfig *cfg, EbItem *out, int max);

/* Recursive item count under parent_id, filtered to item_type if non-NULL/
 * non-empty (a BaseItemKind string — "Movie", "Series", "MusicAlbum", ...),
 * else an unfiltered recursive count. Limit=0 so the server only computes
 * TotalRecordCount and serializes no items — confirmed against a real
 * server that this is cheap even recursively. Used for the library
 * carousel's "N movies/series/albums" line — the view's own ChildCount
 * field (from eb_list_views) is NOT this: confirmed on a real server it
 * counts something else entirely (e.g. read 6 for a music library whose
 * root folder has exactly 1 direct child). Returns -1 on failure. */
int64_t eb_count_items(const EbConfig *cfg, const char *parent_id, const char *item_type);

/* Items under parent_id (a view, a folder, ...), starting at start_index —
 * one window of a potentially much longer list. Movie and music-video
 * libraries are searched recursively and filtered to their respective item
 * types. Music and other collection types list direct children, with
 * type-specific fields for the counts their rows use.
 * Writes the server's untruncated TotalRecordCount to *total_out (may be
 * NULL), which lets the caller distinguish "that's the whole library" from
 * "that's all that fit".
 *
 * Returns the number of items, or -1 if the request itself failed. That's a
 * distinct value from 0 on purpose: a caller sliding a window has to roll
 * back to the page it already had rather than commit an empty one, and an
 * empty page is a legitimate answer it must not confuse with a dead network. */
int eb_list_items(const EbConfig *cfg, const char *parent_id,
                   const char *collection_type, int start_index,
                   EbItem *out, int max, int64_t *total_out);

/* Builds the request path used by eb_list_items. Exposed so the type-specific
 * query can be unit-tested without contacting a server. */
void eb_build_items_path(const EbConfig *cfg, const char *parent_id,
                         const char *collection_type, int start_index, int max,
                         char *path, size_t path_size);

/* Same shape as eb_list_items but recursive, and filtered to item_type if
 * non-NULL/non-empty (a BaseItemKind string, e.g. "MusicAlbum"). Used by
 * the carousel's background cover grid to reach real leaf-level art (movie/
 * series/album covers) even for libraries organized in intermediate folders
 * — a by-artist music library's direct children are MusicArtist folders,
 * which often carry no cover art of their own; a plain non-recursive
 * listing there found nothing to show (confirmed against a real server). */
int eb_list_items_recursive(const EbConfig *cfg, const char *parent_id,
                             const char *item_type, EbItem *out, int max);

/* Random batch of Audio items anywhere under parent_id (e.g. a whole Music
 * library) — SELECT on the artist list starts an infinite shuffle with
 * this, refetching a fresh batch each time the current one runs out. */
int eb_list_random_tracks(const EbConfig *cfg, const char *parent_id, EbItem *out, int max);

/* Sentinel ids for the two synthetic home-screen entries. These aren't real
 * library views — there is nothing on the server they correspond to — so they
 * get ids that cannot collide with a server GUID while still surviving
 * eb_sanitize_id unchanged (which is what makes them safe as grid-cache
 * filenames). */
#define EB_VIEW_RESUME  "misteremby-resume"
#define EB_VIEW_NEXTUP  "misteremby-nextup"
#define EB_VIEW_COLLECTIONS "misteremby-collections"

/* Values for EbItem.synthetic. The ids above remain as grid-cache keys; these
 * are what the code actually branches on. */
#define EB_SYNTH_RESUME 1
#define EB_SYNTH_NEXTUP 2
#define EB_SYNTH_COLLECTIONS 3

/* The two home rows are presented as extra cards on the library carousel, but
 * they aren't libraries — no ParentId to list, no collection type, and their
 * contents change every time something is watched. Everything that would
 * normally reach for the server via a view id has to route around them. */
int view_is_resume(const EbItem *v);
int view_is_nextup(const EbItem *v);
int view_is_collections(const EbItem *v);
int view_is_synthetic(const EbItem *v);

/* The BaseItemKind that actually represents "one unit" of a library, keyed
 * off CollectionType — used both for the carousel's "N movies/series/
 * albums" count (eb_count_items) and its background cover grid
 * (eb_list_items_recursive): a plain direct-children listing of a by-artist
 * music library only finds MusicArtist folders, which often have no cover
 * art of their own, so both need to look past the top level at the real
 * leaf type. NULL (unrecognized collection type) means "don't filter". */
const char *collection_item_type(const char *collection_type);

/* Partly-watched items across the whole library, most recently played first
 * (the server's "Continue Watching"). Returns movies and episodes together.
 * Writes the untruncated count to *total_out (may be NULL) so the home screen
 * can show one without fetching the whole row. */
int eb_list_resume(const EbConfig *cfg, EbItem *out, int max, int64_t *total_out);

/* The next unwatched episode of each series in progress (the server's
 * "Next Up"). Episodes only, by definition. */
int eb_list_nextup(const EbConfig *cfg, EbItem *out, int max, int64_t *total_out);

/* The user's Emby collections (BoxSet items), by name. A collection opens
 * like a folder: its contents list under its own id as ParentId. Emby keeps
 * box sets whether or not a Collections library is shown, so this is how
 * they get a home tile on servers without one. */
int eb_list_collections(const EbConfig *cfg, EbItem *out, int max, int64_t *total_out);

/* TV hierarchy. */
int eb_list_seasons(const EbConfig *cfg, const char *series_id, EbItem *out, int max);
/* Paginated like eb_list_items — a season is normally short, but "all
 * episodes of a long-running series" in one folder is not, and it costs
 * nothing to treat both the same way. */
int eb_list_episodes(const EbConfig *cfg, const char *series_id, const char *season_id,
                      int start_index, EbItem *out, int max, int64_t *total_out);

/* The intros Emby's Cinema Mode has chosen for item_id (trailers, plus any
 * custom intros configured on the server), in play order. Empty when the
 * server has none to offer. Returns the count, or -1 on failure. */
int eb_list_intros(const EbConfig *cfg, const char *item_id, EbItem *out, int max);

/* Fetches full details for a single item — Overview, People (cast), and the
 * Backdrop/Logo image tags — that eb_list_items() deliberately omits to keep
 * browse-list fetches cheap (no point pulling 20 actors per movie for every
 * row in a list). Call this once when entering the info screen. */
int eb_get_item_details(const EbConfig *cfg, const char *item_id, EbItem *out);

/* Parses an item's Chapters array (a JSON object with a "Chapters" member,
 * e.g. a whole item-details response) for the intro/credits markers above.
 * Exposed for unit testing. */
void eb_parse_markers(const char *json_text, EbItem *out);

/* Builds the primary image URL for an item, capped at 200px wide server-side
 * (empty image_tag => returns 0). */
int eb_image_url(const EbConfig *cfg, const EbItem *item, char *out, int outlen);

/* Downloads the primary image to dest_path (curl -o). Returns 1 on success. */
int eb_download_image(const EbConfig *cfg, const EbItem *item, const char *dest_path);

/* General form for any image type ("Primary", "Logo", "Backdrop/0", a cast
 * member's own Primary, ...) at any server-side-resized max_width (<=0 for
 * original size — avoid that; see eb_item_image_url comment in emby.c). */
int eb_item_image_url(const EbConfig *cfg, const char *item_id, const char *image_type,
                       const char *tag, int max_width, char *out, int outlen);
int eb_download_item_image(const EbConfig *cfg, const char *item_id, const char *image_type,
                            const char *tag, int max_width, const char *dest_path);

typedef struct {
    int max_width;
    int max_height;
    int video_bitrate;   /* bits/sec */
} EbStreamProfile;

/* What the client asks the server to transcode down to, before mplayer scales
 * it back up (or down) to fill the framebuffer.
 *
 * Raised from the original 480x270@8M — full PAL source resolution, i.e. the
 * most detail this pipeline can meaningfully carry — after a measured
 * hardware pass: a 5-minute A/B soak (480x270@8M vs 640x288@12M: both zero
 * dropped frames, zero A/V drift, ~2-3 percentage points apart in total CPU)
 * followed by live sweeps up to 720x576@12M including the worst case (4:3
 * content filling the whole box, ~720x540 decode) — which still left ~60%+
 * of the CPU idle. An old note here claimed 720x576 "pegged the CPU and
 * drifted"; re-measured, that turned out to be an artifact of the UI redraw
 * loop running behind the old measurement, not the decode itself (the
 * dominant vo/scale cost also doesn't grow with source size — decode is the
 * only part that does, 8%->~30% usr from 480x270 to full-box 720x576).
 *
 * Two things this default leans on, both in this file / play():
 * allowVideoStreamCopy=false in eb_stream_url (a 720x576 box would otherwise
 * let a PAL DVD rip stream-copy raw interlaced MPEG-2 — see that comment),
 * and the DAR-aware -vf chain for non-480x270 profiles (heights above the
 * framebuffer must downscale, not overflow). Users on a weak network
 * (12Mbps is ~1.5MB/s, trivial for the wired connection MiSTers normally
 * use) can set 480x270@8000000 in emby.conf to get the old behavior
 * back — those exact dimensions also re-select the original PAL scaling
 * chain (see play()). */
#define EB_PROFILE_DEFAULT_W    720
#define EB_PROFILE_DEFAULT_H    576
#define EB_PROFILE_DEFAULT_RATE 12000000

/* Bounds for a profile read from the config file. Wide enough not to get in
 * the way of experimenting, narrow enough that a typo can't ask the server
 * for something absurd. */
#define EB_PROFILE_MIN_W     160
#define EB_PROFILE_MAX_W    1920
#define EB_PROFILE_MIN_H     120
#define EB_PROFILE_MAX_H    1080
#define EB_PROFILE_MIN_RATE   100000
#define EB_PROFILE_MAX_RATE 50000000

/* Builds a server-transcoded progressive stream URL for direct HTTP playback
 * (mplayer opens this URL as-is — ApiKey is passed as a query param here
 * since mplayer cannot set custom HTTP headers on a plain URL).
 * play_session_id MUST be included and unique per playback attempt — without
 * it, the server can serve back a stale cached transcode from an earlier
 * request instead of honoring the current profile/resolution (confirmed
 * against a real server: omitting it silently served an old 720x300 file
 * regardless of the maxWidth/maxHeight passed here).
 * burn_in_sub_index: -1 for none (the normal case — subtitles are rendered
 * client-side by mplayer, see eb_download_subtitle), otherwise a
 * EbSubtitle.index for an image-based track (pgssub/dvdsub/...) that has no
 * text to hand back as .srt and so can only be shown by having the server
 * burn it into the video itself. Confirmed on a real server that burning
 * subtitles into a mid-file restart forces the server to decode+discard from
 * the true start of the file up to the seek target (a seek to 40:00 dropped
 * exactly 40:00 worth of frames, on both mpeg2video and h264) — a
 * multi-minute stall for a deep seek, and nothing a client request
 * parameter can avoid. Only worth paying for image-based tracks, which have
 * no other way to display at all.
 * audio_stream_index: a EbAudioTrack.index to select a specific audio
 * track, or -1 to let the server pick the source's default. Audio is
 * transcoded server-side into the delivered container, so unlike a text
 * subtitle this cannot be switched client-side — changing it means building
 * a new URL and restarting playback at the current position. */
/* The source video codec to ask Emby to COPY rather than transcode, or
 * NULL when the video has to be transcoded. Copying means the server only
 * remuxes the video (and converts the audio) — almost no CPU on a server
 * without hardware transcoding — but the MiSTer then decodes the original
 * stream, so only light codecs at SD sizes qualify, and never interlaced
 * material (this pipeline has no deinterlacer; see the AllowVideoStreamCopy
 * comment in eb_stream_url). */
const char *eb_direct_stream_codec(const EbItem *it);

/* copy_codec: NULL to transcode as usual, or the value returned by
 * eb_direct_stream_codec to stream the source video untouched. */
int eb_stream_url(const EbConfig *cfg, const char *item_id,
                   const char *media_source_id, const char *copy_codec,
                   const EbStreamProfile *profile, int64_t start_ticks,
                   const char *play_session_id, int burn_in_sub_index,
                   int audio_stream_index,
                   char *out, int outlen);

/* Builds a direct-play audio stream URL (static=true — no server transcode:
 * confirmed against a real server that a plain FLAC/MP3 track streams back
 * byte-identical with static=true, and this mplayer build already decodes
 * FLAC/MP3 fine, so there's no reason to pay for a transcode like video
 * needs). play_session_id isn't strictly required for direct play the way
 * it is for eb_stream_url()'s transcode (no cache to go stale), but it's
 * included anyway so progress reporting has a consistent session id. */
int eb_audio_stream_url(const EbConfig *cfg, const char *item_id,
                         const char *play_session_id, char *out, int outlen);

/* Forks curl to fetch an https:// stream url and write its body into
 * fifo_path (a FIFO the caller has already created with mkfifo), for
 * mplayer builds whose own bundled FFmpeg has no TLS backend. Does not
 * wait — curl keeps running for as long as mplayer is reading the other
 * end, so the caller tracks/kills the returned pid itself (the same way it
 * already does for the player pid). Returns -1 if fork() failed. */
pid_t eb_spawn_stream_curl(const EbConfig *cfg, const char *url,
                            const char *fifo_path);

/* Downloads subtitle track sub_index (EbSubtitle.index, i.e. the
 * MediaStreams[].Index) as .srt to dest_path — served directly (no
 * transcode) since the server can just hand back the original text file for
 * text-based subtitle codecs. media_source_id is the same as item_id for
 * every case this app handles (single-version items). mplayer loads/renders
 * this itself at runtime (slave "sub_load" + "sub_select") — no server
 * restart needed to change subtitles, unlike the old burn-in approach. */
int eb_download_subtitle(const EbConfig *cfg, const char *item_id,
                          const char *media_source_id, int sub_index,
                          const char *dest_path);

/* True for subtitle codecs the server can hand back as plain text (.srt) —
 * false for image-based codecs (pgssub/dvdsub/dvbsub/...) that only exist as
 * bitmaps and so need burn_in_sub_index in eb_stream_url instead. */
int eb_subtitle_is_text(const char *codec);

/* Playback progress reporting (Sessions/Playing family). play_session_id is
 * a client-generated opaque string reused across start/progress/stopped for
 * a single playback so the server can correlate/cancel the transcode job.
 * play_method is what the dashboard's session card displays: "Transcode"
 * for video (always a real transcode here), "DirectStream" for music
 * (original file, untouched) — remembered for the rest of that playback's
 * progress/stopped reports. */
void eb_report_start   (const EbConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks,
                         const char *play_method);
void eb_report_progress(const EbConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks, int paused);
/* Dashboard-only live position/pause update (no resume persistence) — for
 * the music player, whose resume position shouldn't be saved per track. */
void eb_report_session_progress(const EbConfig *cfg, const char *item_id,
                                 const char *play_session_id,
                                 int64_t position_ticks, int paused);
void eb_report_stopped (const EbConfig *cfg, const char *item_id,
                         const char *play_session_id, int64_t position_ticks,
                         int played);

/* eb_report_stopped, but never handing the Stopped report to a background
 * thread — for app shutdown, where a detached thread would die with the
 * process before sending it, leaving the server's transcode running. */
void eb_report_stopped_sync(const EbConfig *cfg, const char *item_id,
                             const char *play_session_id, int64_t position_ticks,
                             int played);

/* Registers session capabilities (message display, remote media control)
 * for this DeviceId — see the implementation's comment for when. */
void eb_report_capabilities(const EbConfig *cfg);

/* The full "Authorization: MediaBrowser Client=..., DeviceId=..., Token=..."
 * header line every request carries. Public for session.c's WebSocket
 * handshake — the server resolves which session a socket belongs to from
 * exactly this header (confirmed live: with only api_key/deviceId in the
 * query string the socket got filed under the bare API key's identity, a
 * different session than the one the curl requests build up). */
void eb_auth_header(const EbConfig *cfg, char *out, int outlen);

/* Generates a client-side play session id, e.g. "misteremby-<pid>-<time>". */
void eb_make_play_session_id(char *out, int outlen);
