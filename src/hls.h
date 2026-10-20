#ifndef HLS_H
#define HLS_H

#include <stddef.h>

/* Live TV direct mode: playing an IPTV channel's own HLS stream on the
 * MiSTer instead of having the Emby server convert it.
 *
 * The MiSTer can decode a 640x360 H.264 stream at about 2.2x real time but a
 * 480p one at only 0.7x (measured; the scale down to the screen is what
 * costs), so a channel is played directly only when its playlist offers a
 * variant no bigger than that. The bundled mplayer has no TLS, so the app
 * fetches the segments itself with curl and writes them, in order, into the
 * FIFO mplayer reads: concatenated MPEG-TS segments are one continuous
 * stream to its demuxer. */

/* Largest stream the MiSTer is asked to decode itself. */
#define HLS_MAX_W 640
#define HLS_MAX_H 360

/* Resolves ref (a playlist entry) against base (the playlist's own URL)
 * into out. Handles absolute URLs, host-relative paths and plain relative
 * ones; returns 0 if it doesn't fit. */
int hls_resolve(const char *base, const char *ref, char *out, size_t outsz);

/* From a master playlist's text, the URI of the largest variant that fits
 * max_w x max_h (by RESOLUTION), resolved against base. Returns 1 and its
 * size in *w and *h; 0 when there's none, when the playlist isn't a master
 * playlist, or when audio comes as a separate rendition (one TS stream
 * couldn't carry it). */
int hls_pick_variant(const char *text, const char *base, int max_w, int max_h,
                     char *out, size_t outsz, int *w, int *h);

/* Whether a media playlist is one this player can take straight into
 * mplayer: unencrypted MPEG-TS segments (no #EXT-X-KEY other than NONE, no
 * fragmented-MP4 #EXT-X-MAP). */
int hls_media_playable(const char *text);

/* One pass over a media playlist: calls fn for each segment with its
 * sequence number and resolved URL, and returns the target duration in
 * seconds (0 if unstated). *ended is set when the playlist is complete
 * (#EXT-X-ENDLIST). */
typedef void (*hls_segment_fn)(long long seq, const char *url, void *ctx);
int hls_media_segments(const char *text, const char *base, hls_segment_fn fn, void *ctx,
                       int *ended);

/* Works out whether a channel can play directly: fetches its master
 * playlist and the chosen variant's media playlist, and checks both.
 * Returns 1 with the media playlist's URL in out (and its size in *w and *h). */
int hls_probe(const char *master_url, char *out, size_t outsz, int *w, int *h);

/* Starts feeding media_url's segments into fifo_path on a background
 * thread, from a few segments behind the live edge. Returns 0 on failure. */
int  hls_start(const char *media_url, const char *fifo_path);
/* Stops the feeder (and its curl) and waits for it. Safe to call when
 * nothing is running. */
void hls_stop(void);

#endif
