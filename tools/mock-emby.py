#!/usr/bin/env python3
"""
A tiny fake Emby server for developing MiSTerEmby off-hardware.

Serves just enough of the API for the client to browse, page, and
authenticate against, backed by a synthetic library that is deliberately
bigger than any fixed client-side buffer — 500 movies, so pagination has
something real to page through, which a small personal library wouldn't
exercise.

Stdlib only (http.server + zlib for the placeholder cover art). No
dependencies, and nothing here talks to a real server or real credentials.

Usage:
    python3 tools/mock-emby.py [port]        # default 8096

Then point ./emby.conf at it with any one of the sign-in methods:
    SERVER=http://127.0.0.1:8096
    USERNAME=mockuser            (local user: password "mockpass")
    PASSWORD=mockpass
or  API_KEY=mock-api-key + USERNAME=mockuser
or  nothing else, and run the client with
    MISTEREMBY_CONNECT_BASE=http://127.0.0.1:8096/connect/service
    to walk the Emby Connect PIN flow (auto-confirms after a few polls).
"""

import json
import re
import struct
import sys
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

USER_ID = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
USER_NAME = "mockuser"

# Deliberately > 256 (the client's EB_MAX_ITEMS) so paging past the end of a
# single window is reachable, and not a round number so off-by-ones show up.
N_MOVIES = 503
N_SERIES = 30
SEASONS_PER_SERIES = 2
EPISODES_PER_SEASON = 10
N_ARTISTS = 20
ALBUMS_PER_ARTIST = 3
TRACKS_PER_ALBUM = 10

TICKS_PER_MIN = 60 * 10_000_000


def dotnet_escape(text):
    """Makes json.dumps output match what the server actually puts on the wire.

    the server configures no custom Encoder (see JsonDefaults.cs), so
    System.Text.Json uses JavaScriptEncoder.Default — which escapes every
    non-ASCII character AND the HTML-sensitive set below as \\uXXXX. Python's
    ensure_ascii=True covers the non-ASCII half; these are the rest.

    This matters for client testing far more than it looks: an apostrophe is
    escaped, so a perfectly ordinary title like "Don't Look Up" arrives as
    "Don\\u0027t Look Up". A mock that didn't do this would make a client's
    escape handling look correct when it isn't.
    """
    for ch in ("'", "<", ">", "&", "+"):
        text = text.replace(ch, "\\u%04x" % ord(ch))
    return text


def _png(r, g, b, w=8, h=8):
    """Smallest useful solid-colour PNG — same from-scratch encoder approach
    as tools/raw_to_png.py, for the same reason (no image library assumed)."""
    raw = b"".join(b"\x00" + bytes((r, g, b)) * w for _ in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


# A few distinct colours so the cover grid and browse panel are visibly
# populated rather than uniformly grey.
COVERS = [_png(200, 60, 60), _png(60, 160, 200), _png(200, 170, 60),
          _png(120, 90, 200), _png(70, 190, 120)]

PALETTES = [(200, 60, 60), (60, 160, 200), (200, 170, 60), (120, 90, 200),
            (70, 190, 120), (220, 120, 50), (90, 110, 140)]


def _png_rgba(w, h, pixel):
    """PNG from a pixel(x, y) -> (r, g, b, a) function."""
    raw = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) +
            chunk(b"IEND", b""))


_ART = {}


def mock_art(item_id, kind):
    """Artwork shaped like the real thing, so layouts can be judged: a 2:3
    poster with a title band, a 16:9 backdrop or episode still, or a logo of
    white "letters" on transparency. Coloured per item, cached per shape."""
    item = ITEMS.get(item_id, {})
    wide = (kind.startswith("Backdrop") or item.get("Type") == "Episode"
            or item_id.startswith("channel-"))
    shape = "logo" if kind == "Logo" else ("wide" if wide else "poster")
    key = (zlib.crc32(item_id.encode()) % len(PALETTES), shape)
    if key in _ART:
        return _ART[key]
    r, g, b = PALETTES[key[0]]
    if shape == "logo":
        w, h = 160, 40
        def px(x, y):
            on = 8 <= y < 32 and (x // 12) % 3 != 2 and x < 150
            return (255, 255, 255, 255) if on else (0, 0, 0, 0)
    else:
        w, h = (160, 90) if shape == "wide" else (80, 120)
        def px(x, y):
            t = y / h
            k = 1.0 - 0.7 * t                     # darker towards the bottom
            if shape == "poster" and 0.72 < t < 0.84 and 0.12 < x / w < 0.88:
                return (235, 235, 235, 255)       # the title band
            if (x + y) % 40 < 20:                  # a diagonal stripe, so it isn't flat
                k *= 0.8
            return (int(r * k), int(g * k), int(b * k), 255)
    _ART[key] = _png_rgba(w, h, px)
    return _ART[key]

VIEWS = [
    {"Id": "view-movies", "Name": "Movies",   "CollectionType": "movies"},
    {"Id": "view-tv",     "Name": "TV Shows", "CollectionType": "tvshows"},
    {"Id": "view-music",  "Name": "Music",    "CollectionType": "music"},
    # Emby lists Live TV as a view too; the client hides it in favour of its
    # own Live TV and Recordings tiles.
    {"Id": "view-livetv", "Name": "Live TV",  "CollectionType": "livetv"},
]


def base_item(item_id, name, item_type, **extra):
    item = {
        "Id": item_id,
        "Name": name,
        "Type": item_type,
        "ServerId": "mockserver",
        "ImageTags": {"Primary": "tag-" + item_id},
        "BackdropImageTags": ["backdrop-" + item_id],
        "UserData": {"PlaybackPositionTicks": 0, "Played": False, "PlayCount": 0},
    }
    item.update(extra)
    return item


def build_library():
    """One flat dict of every item, plus parent->children ordering."""
    items, children = {}, {}

    # Titles the real server will escape as \uXXXX: System.Text.Json's default
    # encoder escapes apostrophes and every non-ASCII character, so these are
    # what an ordinary library actually looks like over the wire.
    tricky = [
        "Don't Look Up", "Amélie", "A Fistful of Dollars — Extended",
        "Spinal Tap: This Is It", "Zoë & Co.", "Æon Flux",
    ]

    movies = []
    for idx, title in enumerate(tricky):
        mid = f"movie-tricky-{idx}"
        items[mid] = base_item(mid, title, "Movie",
                               ProductionYear=2000 + idx,
                               RunTimeTicks=100 * TICKS_PER_MIN)
        movies.append(mid)

    for i in range(N_MOVIES):
        mid = f"movie-{i:04d}"
        # A handful get watched/resume state so those badges are exercised.
        user_data = {"PlaybackPositionTicks": 0, "Played": False, "PlayCount": 0}
        if i % 17 == 0:
            user_data = {"PlaybackPositionTicks": 0, "Played": True, "PlayCount": 1}
        elif i % 11 == 0:
            user_data = {"PlaybackPositionTicks": 23 * TICKS_PER_MIN,
                         "Played": False, "PlayCount": 0}
        items[mid] = base_item(
            mid, f"Movie {i:04d} The Motion Picture", "Movie",
            ProductionYear=1980 + (i % 45),
            RunTimeTicks=(90 + i % 50) * TICKS_PER_MIN,
            Overview=f"Synthetic movie #{i} used for client testing. " * 4,
            CommunityRating=round(4.0 + (i % 60) / 10.0, 1),
            UserData=user_data)
        movies.append(mid)
    stress_id = "movie-stress"
    items[stress_id] = base_item(stress_id, "Stress Test Many Tracks", "Movie",
                                 ProductionYear=2024, RunTimeTicks=120 * TICKS_PER_MIN)
    movies.append(stress_id)

    children["view-movies"] = movies

    series_ids = []
    for s in range(N_SERIES):
        sid = f"series-{s:03d}"
        n_eps = SEASONS_PER_SERIES * EPISODES_PER_SEASON
        items[sid] = base_item(
            sid, f"Series {s:03d}", "Series",
            ProductionYear=1995 + (s % 30),
            ChildCount=SEASONS_PER_SERIES,      # season count
            RecursiveItemCount=n_eps,           # episode count, batched server-side
            Overview=f"Synthetic series #{s}. " * 6)
        series_ids.append(sid)

        season_ids = []
        for se in range(1, SEASONS_PER_SERIES + 1):
            season_id = f"{sid}-s{se}"
            items[season_id] = base_item(season_id, f"Season {se}", "Season",
                                         IndexNumber=se, ChildCount=EPISODES_PER_SEASON)
            season_ids.append(season_id)

            ep_ids = []
            for e in range(1, EPISODES_PER_SEASON + 1):
                eid = f"{season_id}e{e:02d}"
                items[eid] = base_item(
                    eid, f"Episode {e:02d}", "Episode",
                    IndexNumber=e, ParentIndexNumber=se,
                    RunTimeTicks=42 * TICKS_PER_MIN,
                    SeriesId=sid, SeriesName=f"Series {s:03d}",
                    ParentBackdropItemId=sid,
                    ParentBackdropImageTags=["backdrop-" + sid],
                    ParentLogoItemId=sid, ParentLogoImageTag="logo-" + sid,
                    Overview=f"Synthetic episode S{se}E{e}. " * 4)
                ep_ids.append(eid)
            children[season_id] = ep_ids
        children[sid] = season_ids
    children["view-tv"] = series_ids

    artist_ids = []
    for a in range(N_ARTISTS):
        aid = f"artist-{a:03d}"
        items[aid] = base_item(aid, f"Artist {a:03d}", "MusicArtist",
                               ChildCount=ALBUMS_PER_ARTIST)
        artist_ids.append(aid)

        album_ids = []
        for al in range(ALBUMS_PER_ARTIST):
            alid = f"{aid}-album{al}"
            items[alid] = base_item(alid, f"Album {al} by Artist {a:03d}", "MusicAlbum",
                                    ProductionYear=1990 + ((a + al) % 35),
                                    ChildCount=TRACKS_PER_ALBUM,
                                    RecursiveItemCount=TRACKS_PER_ALBUM)
            album_ids.append(alid)

            track_ids = []
            for t in range(1, TRACKS_PER_ALBUM + 1):
                tid = f"{alid}-t{t:02d}"
                items[tid] = base_item(
                    tid, f"Track {t:02d}", "Audio",
                    IndexNumber=t, RunTimeTicks=(3 + t % 4) * TICKS_PER_MIN,
                    Album=f"Album {al} by Artist {a:03d}",
                    AlbumArtist=f"Artist {a:03d}",
                    AlbumId=alid, AlbumPrimaryImageTag="tag-" + alid)
                track_ids.append(tid)
            children[alid] = track_ids
        children[aid] = album_ids
    children["view-music"] = artist_ids

    return items, children


ITEMS, CHILDREN = build_library()

# Live TV: a few channels, each with what's on now, and two recordings.
CHANNELS = [
    base_item("channel-%d" % n, name, "TvChannel", ChannelNumber=str(num),
              CurrentProgram={"Name": prog, "Overview": "Mock programme on " + name})
    for n, (num, name, prog) in enumerate([
        (2, "Mock One", "Evening News"), (7, "Mock Seven", "Quiz Night"),
        (9, "Mock Nine", "Late Movie"), (10, "Mock Ten", "Weather"),
    ])
]
RECORDINGS = [
    base_item("rec-0", "Recorded Film", "Movie", RunTimeTicks=90 * TICKS_PER_MIN),
    base_item("rec-1", "Pilot", "Episode", SeriesName="Recorded Show", IndexNumber=1,
              ParentIndexNumber=1, RunTimeTicks=30 * TICKS_PER_MIN),
]

# Two collections (Emby BoxSets) over a few of the movies. They sit under a
# parent that isn't one of VIEWS, like Emby box sets on a server with no
# Collections library shown.
for _n, (_name, _members) in enumerate([("Mock Trilogy", 3), ("Mock Saga", 5)]):
    _bid = "boxset-%d" % _n
    ITEMS[_bid] = base_item(_bid, _name, "BoxSet", ChildCount=_members)
    CHILDREN[_bid] = CHILDREN["view-movies"][_n * 10:_n * 10 + _members]
CHILDREN["boxsets"] = ["boxset-0", "boxset-1"]


def descendants(parent_id, kinds=None):
    """Every item under parent_id, optionally filtered to BaseItemKinds."""
    out, stack = [], list(CHILDREN.get(parent_id, []))
    while stack:
        item_id = stack.pop(0)
        item = ITEMS.get(item_id)
        if not item:
            continue
        if kinds is None or item["Type"] in kinds:
            out.append(item_id)
        stack = list(CHILDREN.get(item_id, [])) + stack
    return out


def media_streams(item_id=""):
    """Multiple audio tracks and a mix of text/image subtitles, so the track
    pickers have something non-trivial to show.

    An item id containing "stress" instead returns the maximum the client
    stores (EB_MAX_SUBS / EB_MAX_AUDIO = 8 each), which is what the track
    picker has to stay on screen for — the menu box is sized from its content,
    so the full list on a 240-line NTSC frame is the case that overflows."""
    if "stress" in item_id:
        streams = [{"Index": 0, "Type": "Video", "Codec": "h264", "Width": 1920,
                    "Height": 1080, "BitRate": 8_000_000, "AspectRatio": "16:9"}]
        langs = ["eng", "jpn", "fra", "deu", "spa", "ita", "por", "rus"]
        for n, lang in enumerate(langs):
            streams.append({"Index": 1 + n, "Type": "Audio", "Codec": "ac3",
                            "Language": lang, "Channels": 6, "IsDefault": n == 0,
                            "DisplayTitle": f"{lang.upper()} - Dolby Digital 5.1 - Default"})
        for n, lang in enumerate(langs):
            streams.append({"Index": 9 + n, "Type": "Subtitle", "Codec": "subrip",
                            "Language": lang,
                            "DisplayTitle": f"{lang.upper()} - SUBRIP - External"})
        return streams

    return [
        {"Index": 0, "Type": "Video", "Codec": "h264", "Width": 1920,
         "Height": 1080, "BitRate": 8_000_000, "AspectRatio": "16:9",
         "DisplayTitle": "1080p H264"},
        {"Index": 1, "Type": "Audio", "Codec": "ac3", "Language": "eng",
         "Channels": 6, "IsDefault": True,
         "DisplayTitle": "English - Dolby Digital 5.1 - Default"},
        {"Index": 2, "Type": "Audio", "Codec": "aac", "Language": "jpn",
         "Channels": 2, "IsDefault": False, "DisplayTitle": "Japanese - AAC - Stereo"},
        {"Index": 3, "Type": "Audio", "Codec": "aac", "Language": "eng",
         "Channels": 2, "IsDefault": False,
         "DisplayTitle": "English - AAC - Stereo - Commentary"},
        # Two English subtitle tracks — dialogue and signs/songs — which is
        # the case a bare language code can't distinguish. DisplayTitle is
        # composed by the server exactly as below: the track's own Title
        # first, then Forced/Default/codec/External as they apply.
        {"Index": 4, "Type": "Subtitle", "Codec": "subrip", "Language": "eng",
         "IsDefault": True, "IsForced": False,
         "DisplayTitle": "English - Default - SUBRIP"},
        {"Index": 5, "Type": "Subtitle", "Codec": "ass", "Language": "eng",
         "Title": "Signs & Songs", "IsDefault": False, "IsForced": True,
         "DisplayTitle": "Signs & Songs - English - Forced - ASS"},
        {"Index": 6, "Type": "Subtitle", "Codec": "subrip", "Language": "eng",
         "IsHearingImpaired": True, "IsDefault": False, "IsForced": False,
         "DisplayTitle": "English - Hearing Impaired - SUBRIP"},
        {"Index": 7, "Type": "Subtitle", "Codec": "pgssub", "Language": "jpn",
         "DisplayTitle": "Japanese - PGSSUB"},
    ]


def people():
    return [{"Id": f"person-{i}", "Name": f"Actor {i}", "Role": f"Character {i}",
             "Type": "Actor", "PrimaryImageTag": f"tag-person-{i}"} for i in range(6)]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # ── plumbing ────────────────────────────────────────────────────────────
    def log_message(self, fmt, *args):
        if "-v" in sys.argv:
            sys.stderr.write("%s %s\n" % (self.command, self.path))

    def _token(self):
        """The credential, wherever an Emby client put it: X-Emby-Token,
        Token="..." inside X-Emby-Authorization, or api_key on the URL."""
        tok = self.headers.get("X-Emby-Token", "")
        if tok:
            return tok
        header = self.headers.get("X-Emby-Authorization", "")
        m = re.search(r'Token="([^"]*)"', header)
        if m:
            return m.group(1)
        q = parse_qs(urlparse(self.path).query)
        return q.get("api_key", [""])[0]

    def _authorized(self):
        """A real server rejects an unknown or revoked token with 401, and
        the client depends on that: it's how a saved token the server has
        since forgotten gets detected and replaced by a fresh sign-in.
        A mock that accepted anything would make that path untestable."""
        return self._token() in VALID_TOKENS

    def _send_401(self):
        return self._send({"Error": "invalid token"}, status=401)

    def _send(self, payload, content_type="application/json", status=200):
        if isinstance(payload, (dict, list)):
            # Compact separators on purpose: a real server sets
            # WriteIndented = false, and a client that scans the raw bytes
            # would behave differently against pretty-printed output.
            text = json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
            payload = dotnet_escape(text).encode()
        elif isinstance(payload, str):
            payload = payload.encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _query_result(self, ids, query):
        """Applies StartIndex/Limit the way the real server does, and always
        reports the untruncated TotalRecordCount — that total is the whole
        point for a paging client."""
        total = len(ids)
        start = int(query.get("StartIndex", query.get("startIndex", [0]))[0])
        limit = query.get("Limit", query.get("limit", [None]))[0]
        window = ids[start:]
        if limit is not None:
            window = window[:int(limit)]
        return {"Items": [ITEMS[i] for i in window],
                "TotalRecordCount": total,
                "StartIndex": start}

    # ── routes ──────────────────────────────────────────────────────────────
    def _route(self):
        """Strips Emby's /emby REST prefix. Anything without it 404s, as an
        Emby server behind a strict reverse proxy would, so a client that
        forgot the prefix fails loudly here instead of only in the field.
        Emby Connect routes (/connect/...) are served too, for clients run
        with MISTEREMBY_CONNECT_BASE pointed at this mock."""
        url = urlparse(self.path)
        path = url.path
        if path.startswith("/connect/"):
            return "connect", path[len("/connect"):], parse_qs(url.query)
        if not path.startswith("/emby/"):
            return None, path, parse_qs(url.query)
        return "emby", path[len("/emby"):], parse_qs(url.query)

    def _form(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b""
        if self.headers.get("Content-Type", "").startswith("application/json"):
            return json.loads(raw or b"{}")
        return {k: v[0] for k, v in parse_qs(raw.decode()).items()}

    def do_GET(self):
        kind, path, query = self._route()
        if kind is None:
            return self._send({"Error": "not found: " + path}, status=404)
        if kind == "connect":
            return self._connect_get(path, query)

        # Everything needs a credential except the public info call and
        # image requests — Emby serves artwork without auth, and the client
        # relies on that: covers are fetched with a bare curl.
        exempt = path in ("/System/Info/Public", "/Users/Public") or "/Images/" in path
        if path == "/Connect/Exchange":
            if self._token() != CONNECT_ACCESS_KEY:
                return self._send_401()
            VALID_TOKENS.add("mock-access-token")
            return self._send({"LocalUserId": USER_ID, "AccessToken": "mock-access-token"})
        if not exempt and not self._authorized():
            return self._send_401()

        if path == "/Users/Public":
            # Emby's sign-in screen list: one password user, one without.
            return self._send([{"Id": USER_ID, "Name": USER_NAME, "HasPassword": True},
                               {"Id": USER_ID, "Name": "Kids", "HasPassword": False}])

        if path == "/System/Info/Public":
            return self._send({"ServerName": "Mock Emby", "Version": "4.9.1.0", "Id": "mockserver"})

        if path == "/Users":
            return self._send([{"Id": USER_ID, "Name": USER_NAME}])
        if path == "/Users/" + USER_ID:
            return self._send({"Id": USER_ID, "Name": USER_NAME})

        if path in ("/LiveTv/Channels", "/LiveTv/Recordings"):
            rows = CHANNELS if path.endswith("Channels") else RECORDINGS
            start = int(query.get("StartIndex", ["0"])[0])
            limit = int(query.get("Limit", ["100"])[0])
            return self._send({"Items": rows[start:start + limit],
                               "TotalRecordCount": len(rows)})

        if path == "/Users/%s/Views" % USER_ID:
            views = [base_item(v["Id"], v["Name"], "CollectionFolder",
                               CollectionType=v["CollectionType"]) for v in VIEWS]
            return self._send({"Items": views, "TotalRecordCount": len(views)})

        m = re.match(r"^/Items/([^/]+)/Images/([^/?]+)", path)
        if m:
            return self._send(mock_art(m.group(1), m.group(2)), "image/png")

        m = re.match(r"^/Shows/([^/]+)/Seasons$", path)
        if m:
            return self._send(self._query_result(CHILDREN.get(m.group(1), []), query))

        m = re.match(r"^/Shows/([^/]+)/Episodes$", path)
        if m:
            season = query.get("SeasonId", [None])[0]
            ids = CHILDREN.get(season, []) if season else descendants(m.group(1), {"Episode"})
            return self._send(self._query_result(ids, query))

        if path == "/Shows/NextUp":
            # First episode of each series, mimicking the real thing closely
            # enough for the client's purposes.
            ids = [descendants(s, {"Episode"})[0] for s in CHILDREN["view-tv"][:12]]
            return self._send(self._query_result(ids, query))

        if path == "/Users/%s/Items/Resume" % USER_ID:
            ids = [i for i in CHILDREN["view-movies"]
                   if ITEMS[i]["UserData"]["PlaybackPositionTicks"] > 0][:12]
            return self._send(self._query_result(ids, query))

        m = re.match(r"^/Users/%s/Items/([^/?]+)/Intros$" % USER_ID, path)
        if m:
            # Cinema Mode: one trailer ahead of every movie.
            trailer = base_item("trailer-0", "Mock Trailer", "Trailer", RunTimeTicks=120 * 10_000_000)
            return self._send({"Items": [trailer], "TotalRecordCount": 1})

        m = re.match(r"^/Users/%s/Items/([^/?]+)$" % USER_ID, path)
        if m and m.group(1) in ITEMS:
            item = dict(ITEMS[m.group(1)])
            streams = media_streams(m.group(1))
            # Emby nests the streams inside each media source.
            item["MediaSources"] = [{"Id": "mediasource_" + m.group(1), "MediaStreams": streams}]
            item["People"] = people()
            if item.get("Type") == "Episode":
                # Emby's skip-intro markers, as detected by the server.
                item["Chapters"] = [
                    {"StartPositionTicks": 0, "Name": "Chapter 1"},
                    {"StartPositionTicks": 30 * 10_000_000, "MarkerType": "IntroStart"},
                    {"StartPositionTicks": 90 * 10_000_000, "MarkerType": "IntroEnd"},
                    {"StartPositionTicks": 40 * 60 * 10_000_000, "MarkerType": "CreditsStart"},
                ]
            item["ImageTags"] = dict(item.get("ImageTags", {}))
            item["ImageTags"]["Logo"] = "logo-" + m.group(1)
            return self._send(item)

        if path == "/Users/%s/Items" % USER_ID:
            parent = query.get("ParentId", [None])[0]
            recursive = query.get("Recursive", ["false"])[0].lower() == "true"
            kinds = query.get("IncludeItemTypes", [None])[0]
            kinds = set(kinds.split(",")) if kinds else None

            if parent is None and kinds == {"BoxSet"}:
                ids = CHILDREN["boxsets"]
            elif parent is None:
                ids = []
            elif recursive:
                ids = descendants(parent, kinds)
            else:
                ids = [i for i in CHILDREN.get(parent, [])
                       if kinds is None or ITEMS[i]["Type"] in kinds]

            if query.get("SortBy", [""])[0] == "Random":
                import random
                ids = list(ids)
                random.shuffle(ids)
            return self._send(self._query_result(ids, query))

        return self._send({"Error": "not found: " + path}, status=404)

    # ── Emby Connect (cloud) ───────────────────────────────────────────────
    def _connect_get(self, path, query):
        if not self.headers.get("X-Application"):
            return self._send({"Error": "X-Application required"}, status=400)
        if path == "/service/pin":
            pin = PINS.get(query.get("pin", [""])[0])
            if pin is None:
                return self._send({"Error": "unknown pin"}, status=404)
            # Auto-confirms after a few polls so an unattended run completes;
            # the real service waits for the user at emby.media/pin.
            pin["Polls"] += 1
            if pin["Polls"] >= 3:
                pin["IsConfirmed"] = True
            return self._send({k: v for k, v in pin.items() if k != "Polls"})
        if path == "/service/servers":
            if self.headers.get("X-Connect-UserToken") != "mock-connect-token":
                return self._send({"Error": "bad token"}, status=401)
            host = "http://127.0.0.1:%d" % self.server.server_address[1]
            return self._send([{"Name": "Mock Emby", "Url": host, "LocalAddress": host,
                                "SystemId": "mockserver", "AccessKey": CONNECT_ACCESS_KEY}])
        return self._send({"Error": "not found"}, status=404)

    def _connect_post(self, path):
        form = self._form()
        if path == "/service/pin":
            code = "%04d" % (4821 + len(PINS))
            PINS[code] = {"Pin": code, "DeviceId": form.get("deviceId", ""),
                          "IsConfirmed": False, "IsExpired": False, "Polls": 0}
            return self._send({k: v for k, v in PINS[code].items() if k != "Polls"})
        if path == "/service/pin/authenticate":
            pin = PINS.get(form.get("pin", ""))
            if not pin or not pin["IsConfirmed"]:
                return self._send({"Error": "not confirmed"}, status=401)
            return self._send({"UserId": "connect-user-1", "AccessToken": "mock-connect-token"})
        if path == "/service/user/authenticate":
            if form.get("nameOrEmail") != "mock@example.com" or form.get("rawpw") != "mockpass":
                return self._send({"Error": "bad credentials"}, status=401)
            return self._send({"AccessToken": "mock-connect-token", "User": {"Id": "connect-user-1"}})
        return self._send({"Error": "not found"}, status=404)

    def do_POST(self):
        kind, path, _ = self._route()
        if kind is None:
            return self._send({"Error": "not found: " + path}, status=404)
        if kind == "connect":
            return self._connect_post(path)

        if path == "/Users/AuthenticateByName":
            body = self._form()
            ok = ((body.get("Username") == USER_NAME and body.get("Pw") == "mockpass") or
                  (body.get("Username") == "Kids" and body.get("Pw", "") == ""))
            if not ok:
                return self._send({"Error": "Invalid username or password"}, status=401)
            VALID_TOKENS.add("mock-access-token")
            return self._send({"AccessToken": "mock-access-token",
                               "ServerId": "mockserver",
                               "User": {"Id": USER_ID, "Name": USER_NAME}})

        if not self._authorized():
            return self._send_401()
        m = re.match(r"^/Items/(channel-\d+)/PlaybackInfo$", path)
        if m:
            return self._send({"MediaSources": [{"Id": "ms-" + m.group(1),
                                                 "LiveStreamId": "live-" + m.group(1)}],
                               "PlaySessionId": "mock"})
        # Playback reporting / user data writes — accepted and ignored.
        length = int(self.headers.get("Content-Length", 0))
        if length:
            self.rfile.read(length)
        return self._send({}, status=204)


PINS = {}
CONNECT_ACCESS_KEY = "mock-connect-access-key"

# Credentials the mock accepts: the API key an emby.conf might carry, plus
# any access token handed out by a sign-in.
VALID_TOKENS = {"mock-api-key"}


if __name__ == "__main__":
    port = 8096
    for arg in sys.argv[1:]:
        if arg.isdigit():
            port = int(arg)
    print(f"mock emby on http://127.0.0.1:{port}  "
          f"({N_MOVIES} movies, {N_SERIES} series, {N_ARTISTS} artists)")
    # Threaded: the client holds its command socket open for the whole run,
    # which would otherwise park the only worker.
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
