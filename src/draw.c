/* Drawing primitives — bitmap-font text rendering and the aspect-corrected
 * blit. Extracted verbatim from main.c; see draw.h for the interface. */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"
#include "font8x8.h"

/* Tried applying the same non-square-pixel correction used for cover art
 * (par_correction()) to font glyph width on NTSC — visually correct per-
 * character, but it doubles every string's on-screen width, which broke
 * layouts never designed for text that wide (confirmed on hardware: home
 * carousel library labels overlapping/running together). Fixing that
 * properly would mean re-deriving card widths/spacing/truncation targets
 * throughout the UI, not just the font — too large a change for what's a
 * cosmetic improvement. Reverted to unconditional passthrough (identical
 * on both PAL and NTSC); the `fb` parameter threaded through text_width()/
 * truncate_to_width()/draw_wrapped() is otherwise unused now but left in
 * place rather than unwinding ~40 call sites for no behavioral gain. */
static int font_scale_x(FBDev *fb, int scale)
{
    (void)fb;
    return scale;
}

/* Text is UTF-8. One glyph cell is drawn per code point (not per byte), so
 * accented Latin (é, ã, ç, ñ, ü, ...) renders properly instead of as one
 * cell per raw byte. ASCII comes from font8x8_basic, U+00A0-U+00FF from
 * font8x8_ext_latin; anything else (CJK/Cyrillic/Greek) shows '?'. Titles/
 * overviews are pre-normalised by eb_text_to_display so only code points we
 * can actually draw ever reach here. */
static unsigned utf8_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned c = *p, cp; int extra;
    if      (c < 0x80)           { *s += 1; return c; }
    else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else                         { *s += 1; return 0xFFFD; }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += extra + 1;
    return cp;
}

static const uint8_t *glyph_for_cp(unsigned cp)
{
    if (cp < 0x80)                return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF) return font8x8_ext_latin[cp - 0xA0];
    return font8x8_basic['?'];
}

/* Number of code points in the first nbytes of s (stops at NUL). */
static int cp_count(const char *s, int nbytes)
{
    int n = 0; const char *end = s + nbytes;
    while (s < end && *s) { utf8_cp(&s); n++; }
    return n;
}

/* Cut s in place to at most keep_cols code points (at a code-point boundary,
 * no ellipsis added). */
static void truncate_cp(char *s, int keep_cols)
{
    const char *p = s; int i = 0;
    while (*p && i < keep_cols) { utf8_cp(&p); i++; }
    s[(int)(p - s)] = '\0';
}

static void draw_char(FBDev *fb, int x, int y, unsigned cp, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int sx = font_scale_x(fb, scale);
    const uint8_t *glyph = glyph_for_cp(cp);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if ((bits >> col) & 1)
                fb_fill_rect_alpha(fb, x + col*sx, y + row*scale,
                                   sx, scale, r, g, b, 255);
        }
    }
}

void draw_text(FBDev *fb, int x, int y, const char *s, int scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    int sx = font_scale_x(fb, scale);
    while (*s) { unsigned cp = utf8_cp(&s); draw_char(fb, x, y, cp, scale, r, g, b); x += 8*sx; }
}

int text_width(FBDev *fb, const char *s, int scale)
{
    int n = 0;
    while (*s) { utf8_cp(&s); n++; }
    return n * 8 * font_scale_x(fb, scale);
}

/* Like draw_text, but skips any character whose whole glyph cell doesn't
 * fit within [clip_x0, clip_x1) — used for the scrolling browse title
 * marquee so it doesn't run into the clock/spinner area to its right (or
 * off the left edge while scrolling). Character-granularity, not
 * pixel-perfect, but fine for a continuously-moving marquee. */
void draw_text_clipped(FBDev *fb, int x, int y, const char *s, int scale,
                               uint8_t r, uint8_t g, uint8_t b, int clip_x0, int clip_x1)
{
    while (*s) {
        unsigned cp = utf8_cp(&s);
        if (x >= clip_x0 && x + 8 * scale <= clip_x1)
            draw_char(fb, x, y, cp, scale, r, g, b);
        x += 8 * scale;
    }
}

/* Truncates s in-place (with a trailing "...") so it fits within max_w
 * pixels at the given scale — draw_text itself doesn't clip, so a long
 * title would otherwise run into whatever's to its right. */
void truncate_to_width(FBDev *fb, char *s, int scale, int max_w)
{
    int max_chars = max_w / (8 * font_scale_x(fb, scale));
    if (max_chars < 1) { s[0] = '\0'; return; }
    int n = 0; { const char *p = s; while (*p) { utf8_cp(&p); n++; } }
    if (n <= max_chars) return;
    if (max_chars > 3) { truncate_cp(s, max_chars - 3); strcat(s, "..."); }
    else               { truncate_cp(s, max_chars); }
}

int draw_wrapped(FBDev *fb, int x, int y, const char *text,
                         int scale, int max_w, int max_lines,
                         uint8_t r, uint8_t g, uint8_t b)
{
    int cols    = max_w / (8 * font_scale_x(fb, scale));
    int line_h  = 8 * scale + 2;
    char line[256] = {0};
    int  li         = 0;   /* bytes in `line` */
    int  lcols      = 0;   /* display columns in `line` */
    int  drawn      = 0;
    const char *p   = text;

    while (*p && drawn < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *we = p;
        while (*we && *we != ' ') we++;
        int wlen  = (int)(we - p);
        int wcols = cp_count(p, wlen);
        if (lcols > 0 && lcols + 1 + wcols > cols) {
            int is_last = (drawn == max_lines - 1);
            if (is_last && *we) {
                if (lcols > cols - 3) truncate_cp(line, cols - 3 > 0 ? cols - 3 : 0);
                strcat(line, "...");
            }
            draw_text(fb, x, y + drawn * line_h, line, scale, r, g, b);
            drawn++; li = 0; lcols = 0; memset(line, 0, sizeof(line));
        }
        if (lcols > 0 && li < (int)sizeof(line) - 2) { line[li++] = ' '; lcols++; }
        if (li + wlen < (int)sizeof(line) - 1) { memcpy(line+li, p, wlen); li += wlen; lcols += wcols; }
        p = we;
    }
    if (li > 0 && drawn < max_lines) {
        draw_text(fb, x, y + drawn * line_h, line, scale, r, g, b);
        drawn++;
    }
    return drawn * line_h;
}

/* Pixel-aspect-ratio correction: our framebuffer's pixels aren't square on
 * a 4:3 CRT/component display, so a source image's width needs stretching
 * relative to naive (sw/sh)*dh math to look right. Derived from the live
 * framebuffer size rather than hardcoded, so it's automatically correct at
 * any active-line count (PAL 288, NTSC 240, ...) instead of only the one
 * resolution it happened to be tuned against. At fb->height=288 this comes
 * out to exactly 5/3 — this platform's proven PAL correction factor
 * (confirmed unchanged: was hardcoded 5.0/3.0 before this generalization).
 *
 * The 4:3-display assumption only holds for the CRT-class modes, which all
 * have fewer than 360 logical lines (288/240 layouts, including the
 * line-doubled 576/480 rasters those become). Anything taller is one of
 * MiSTer's standard square-pixel HDMI canvases — 640x360 (720p at
 * fb_size=2), 800x600, the 640x480 window clamped out of a 720p/1080p
 * canvas — where no correction is needed. For the 4:3 ones among those
 * (600-line, the 480 window) the old formula already computed 1.0, so this
 * gate only actually changes the 16:9 canvases it was wrong on. */
double par_correction(FBDev *fb)
{
    if (fb->height < 360)
        return (3.0 * fb->width) / (4.0 * fb->height);
    return 1.0;
}

/* Blits src into a max_w x max_h box, preserving its own real-world aspect
 * ratio and centering it (letterboxing/pillarboxing as needed — handles a
 * landscape screen-grab used as a poster fine, same as a normal portrait
 * one). Used for the logo and any poster/cover, which — unlike the
 * full-bleed backdrop crop-fill — must never look distorted.
 *
 * par_correction() is this platform's proven correction for its own
 * non-square pixels, generalized to the live framebuffer size: our buffer
 * is 640 wide feeding a 4:3 CRT through a narrower final PAL/NTSC DDR
 * resolution, so plain w/h aspect math alone renders posters visibly too
 * narrow ("elongated") on the real screen — was hardcoded to 5/3 (correct
 * only at 288 active lines), now derived so it's also correct at NTSC's
 * 240. */
void blit_fit_centered(FBDev *fb, const uint8_t *src, int sw, int sh,
                               int cx, int cy, int max_w, int max_h, uint8_t alpha)
{
    if (!src || sw <= 0 || sh <= 0) return;
    int dh = max_h;
    int dw = (int)((double)sw / sh * dh * par_correction(fb) + 0.5);
    if (dw > max_w) { dh = dh * max_w / dw; dw = max_w; }
    fb_blit(fb, src, sw, sh, cx - dw / 2, cy - dh / 2, dw, dh, alpha);
}

int spinner_width(FBDev *fb)
{
    return (int)(SPINNER_SIZE * par_correction(fb) + 0.5);
}

void draw_spinner_frame(FBDev *fb, int frame_idx)
{
    /* Emby's look: a ring of green dots chasing round a white play
     * triangle, on a dark plate so it reads over artwork. Round on screen —
     * widths go through par_correction like every other shape here. */
    double par = par_correction(fb);
    int w  = spinner_width(fb);
    int dx = fb->width - w - SPINNER_MARGIN;
    int dy = SPINNER_MARGIN;
    fb_fill_rect_alpha(fb, dx - 2, dy - 1, w + 4, SPINNER_SIZE + 2, 0x10, 0x10, 0x10, 220);

    double cx = dx + w / 2.0, cy = dy + SPINNER_SIZE / 2.0;
    double r  = SPINNER_SIZE / 2.0 - 2.0;
    int dot_h = SPINNER_SIZE >= 16 ? 3 : 2;
    int dot_w = (int)(dot_h * par + 0.5);
    for (int k = 0; k < 8; k++) {
        double a = k * (M_PI / 4.0) - M_PI / 2.0;          /* 12 o'clock first, clockwise */
        int age = ((frame_idx % 8) - k + 8) % 8;           /* 0 = the leading dot */
        uint8_t r8, g8, b8;
        if      (age == 0) { r8 = 0x6C; g8 = 0xD8; b8 = 0x64; }   /* bright Emby green */
        else if (age == 1) { r8 = 0x52; g8 = 0xB5; b8 = 0x4B; }
        else if (age == 2) { r8 = 0x3A; g8 = 0x7E; b8 = 0x35; }
        else               { r8 = 0x2A; g8 = 0x4A; b8 = 0x28; }
        int px = (int)(cx + cos(a) * r * par - dot_w / 2.0 + 0.5);
        int py = (int)(cy + sin(a) * r - dot_h / 2.0 + 0.5);
        fb_fill_rect_alpha(fb, px, py, dot_w, dot_h, r8, g8, b8, 255);
    }

    /* Play triangle, pointing right, a hair right of centre. */
    int th = SPINNER_SIZE / 2 - 1;      /* 7 rows: odd, so it has a single-row tip */
    double tw = th * 0.75 * par;
    double tx0 = cx - tw * 0.42;
    int ty0 = (int)(cy - th / 2.0 + 0.5);
    for (int row = 0; row < th; row++) {
        double v = fabs((row + 0.5) - th / 2.0) / (th / 2.0);
        int len = (int)(tw * (1 - v) + 0.5);
        if (len > 0) fb_fill_rect_alpha(fb, (int)(tx0 + 0.5), ty0 + row, len, 1, 0xFF, 0xFF, 0xFF, 255);
    }
}

/* Bakes "draw this image at `alpha` over black, then darken it by a top-down
 * wash that reaches full black at the bottom row" into a standalone,
 * destination-sized RGBA buffer. Returns malloc'd memory the caller owns, or
 * NULL if the inputs are degenerate or the allocation fails.
 *
 * The point is doing it ONCE per image instead of on every redraw. It is
 * correct only because the caller composes onto a frame just cleared to pure
 * black: blending a pixel at alpha a over black is exactly pixel*a/255, and
 * the wash is one more such per-row factor. Both are properties of the
 * pixels, not of the frame, so replaying the result collapses to a single
 * opaque copy — see fb_blit_opaque's own comment, which cites this class of
 * trick as the app's biggest single CPU saving.
 *
 * Nearest-neighbour, matching fb_blit, so pre-scaling here changes nothing
 * about how the picture reads versus drawing it straight to the screen. */
uint8_t *compose_backdrop_wash(const uint8_t *src, int sw, int sh,
                               int dw, int dh, uint8_t alpha)
{
    if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NULL;
    uint8_t *out = (uint8_t *)malloc((size_t)dw * dh * 4);
    if (!out) return NULL;

    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        /* Top row keeps the full `alpha`, bottom row reaches black, so the
         * artwork blends into the cleared frame below it with no visible
         * seam. dh == 1 is the degenerate case: the single row IS the bottom
         * one, so it washes to black rather than keeping alpha. */
        int wash = (dh > 1) ? (255 - y * 255 / (dh - 1)) : 0;
        int k    = (int)alpha * wash;                /* 0 .. 255*255 */
        const uint8_t *srow = src + (size_t)sy * sw * 4;
        uint8_t *drow = out + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            const uint8_t *sp = srow + (size_t)(x * sw / dw) * 4;
            uint8_t *dp = drow + (size_t)x * 4;
            for (int c = 0; c < 3; c++)
                dp[c] = (uint8_t)((sp[c] * k) / (255 * 255));
            dp[3] = 255;                             /* replay is an opaque copy */
        }
    }
    return out;
}
