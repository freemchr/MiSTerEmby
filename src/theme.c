/* MiSTerEmby's visual theme — see theme.h. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "theme.h"
#include "draw.h"

static double theme_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static inline uint32_t blend_px(uint32_t dst, uint8_t r, uint8_t g, uint8_t b, uint32_t a)
{
    uint32_t ia = 255 - a;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    dr = (r * a + dr * ia) / 255;
    dg = (g * a + dg * ia) / 255;
    db = (b * a + db * ia) / 255;
    return (dr << 16) | (dg << 8) | db;
}

static void hspan(FBDev *fb, int x0, int x1, int y,
                  uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (y < 0 || y >= fb->height) return;
    if (x0 < 0) x0 = 0;
    if (x1 > fb->width) x1 = fb->width;
    if (x0 >= x1 || a == 0) return;
    uint32_t *row = (uint32_t *)(fb->back + (size_t)y * fb->stride);
    if (a == 255) {
        uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        for (int x = x0; x < x1; x++) row[x] = c;
    } else {
        for (int x = x0; x < x1; x++) row[x] = blend_px(row[x], r, g, b, a);
    }
}

/* ── backdrop ──────────────────────────────────────────────────────────── */

/* Composed once per framebuffer size: a vertical charcoal falloff, a little
 * lighter at the top like Emby's app bar region. */
static uint8_t *g_bg_px = NULL;
static int      g_bg_w = 0, g_bg_h = 0;

static void bg_build(FBDev *fb)
{
    if (g_bg_px && g_bg_w == fb->width && g_bg_h == fb->height) return;
    free(g_bg_px);
    g_bg_w = fb->width; g_bg_h = fb->height;
    g_bg_px = malloc((size_t)g_bg_w * g_bg_h * 4);
    if (!g_bg_px) { g_bg_w = g_bg_h = 0; return; }
    for (int y = 0; y < g_bg_h; y++) {
        double t = (double)y / (g_bg_h - 1);
        int v = (int)(0x1C - t * 0x0E);       /* #1c1c1c at the top to #0e0e0e */
        uint8_t *row = g_bg_px + (size_t)y * g_bg_w * 4;
        for (int x = 0; x < g_bg_w; x++) {
            /* Faint vignette toward the sides. */
            double dx = (double)x / (g_bg_w - 1) - 0.5;
            int vv = v - (int)(dx * dx * 16);
            if (vv < 6) vv = 6;
            row[x * 4 + 0] = (uint8_t)vv;
            row[x * 4 + 1] = (uint8_t)vv;
            row[x * 4 + 2] = (uint8_t)vv;
            row[x * 4 + 3] = 255;
        }
    }
}

void theme_fill_bg(FBDev *fb)
{
    bg_build(fb);
    if (g_bg_px) fb_blit_opaque(fb, g_bg_px, g_bg_w, g_bg_h, 0, 0, fb->width, fb->height);
    else         fb_fill_rect_alpha(fb, 0, 0, fb->width, fb->height, THEME_BG, 255);
}

/* Soft radial glow sprite, built once: green with a quadratic alpha
 * falloff, blitted (alpha-blended) at a slowly drifting position. */
#define GLOW_N 96
static uint8_t g_glow_px[GLOW_N * GLOW_N * 4];
static int     g_glow_ready = 0;

static void glow_build(void)
{
    if (g_glow_ready) return;
    for (int y = 0; y < GLOW_N; y++) {
        for (int x = 0; x < GLOW_N; x++) {
            double dx = (x + 0.5) / GLOW_N * 2 - 1, dy = (y + 0.5) / GLOW_N * 2 - 1;
            double d = sqrt(dx * dx + dy * dy);
            double a = d >= 1 ? 0 : (1 - d) * (1 - d);
            uint8_t *p = g_glow_px + (y * GLOW_N + x) * 4;
            p[0] = 0x52; p[1] = 0xB5; p[2] = 0x4B;   /* stored RGBA */
            p[3] = (uint8_t)(a * 70);
        }
    }
    g_glow_ready = 1;
}

void theme_backdrop(FBDev *fb)
{
    theme_fill_bg(fb);
    glow_build();

    double t = theme_now();
    double par = par_correction(fb);
    int gh = fb->height * 9 / 10;
    int gw = (int)(gh * par);
    /* Two glows drifting on slow Lissajous paths, one top-left-ish and one
     * bottom-right-ish, so the screen breathes without anything to read as
     * motion in peripheral vision. */
    int x1 = (int)(fb->width * 0.22 + sin(t * 0.13) * fb->width * 0.10) - gw / 2;
    int y1 = (int)(fb->height * 0.20 + cos(t * 0.11) * fb->height * 0.10) - gh / 2;
    int x2 = (int)(fb->width * 0.80 + cos(t * 0.09) * fb->width * 0.10) - gw / 2;
    int y2 = (int)(fb->height * 0.85 + sin(t * 0.07) * fb->height * 0.08) - gh / 2;
    fb_blit(fb, g_glow_px, GLOW_N, GLOW_N, x1, y1, gw, gh, 255);
    fb_blit(fb, g_glow_px, GLOW_N, GLOW_N, x2, y2, gw, gh, 170);
}

/* ── shapes ────────────────────────────────────────────────────────────── */

void theme_round_rect(FBDev *fb, int x, int y, int w, int h, int radius,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > h) radius = h / 2;
    double par = par_correction(fb);
    double rx = radius * par;
    if (rx * 2 > w) rx = w / 2.0;
    for (int row = 0; row < h; row++) {
        double inset = 0;
        double dy = -1;
        if (row < radius)              dy = radius - row - 0.5;
        else if (row >= h - radius)    dy = row - (h - radius) + 0.5;
        if (dy >= 0 && radius > 0) {
            double k = dy / radius;
            inset = rx * (1 - sqrt(1 - (k > 1 ? 1 : k * k)));
        }
        int in = (int)(inset + 0.5);
        hspan(fb, x + in, x + w - in, y + row, r, g, b, a);
    }
}

void theme_pill(FBDev *fb, int x, int y, int w, int h,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    theme_round_rect(fb, x, y, w, h, h / 2, r, g, b, a);
}

void theme_round_outline(FBDev *fb, int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (w <= 2 || h <= 2) return;
    if (radius * 2 > h) radius = h / 2;
    int rx = (int)(radius * par_correction(fb) + 0.5);
    hspan(fb, x + rx, x + w - rx, y, r, g, b, a);
    hspan(fb, x + rx, x + w - rx, y + h - 1, r, g, b, a);
    fb_fill_rect_alpha(fb, x, y + radius, 1, h - 2 * radius, r, g, b, a);
    fb_fill_rect_alpha(fb, x + w - 1, y + radius, 1, h - 2 * radius, r, g, b, a);
    /* Corners: step the edge pixel along the quarter circle. */
    for (int row = 0; row < radius; row++) {
        double k = (radius - row - 0.5) / radius;
        int in = (int)(rx * (1 - sqrt(1 - k * k)) + 0.5);
        int in_next = row + 1 < radius
            ? (int)(rx * (1 - sqrt(1 - ((radius - row - 1.5) / radius) * ((radius - row - 1.5) / radius))) + 0.5)
            : 0;
        int span = in - in_next; if (span < 1) span = 1;
        hspan(fb, x + in_next, x + in_next + span, y + row, r, g, b, a);
        hspan(fb, x + w - in_next - span, x + w - in_next, y + row, r, g, b, a);
        hspan(fb, x + in_next, x + in_next + span, y + h - 1 - row, r, g, b, a);
        hspan(fb, x + w - in_next - span, x + w - in_next, y + h - 1 - row, r, g, b, a);
    }
}

/* ── chrome ────────────────────────────────────────────────────────────── */

void theme_header(FBDev *fb, int safe_y)
{
    int h = safe_y + 17;   /* just under the scale-2 title row */
    for (int y = 0; y < h; y++) {
        /* Darker at the very top (overscan), easing toward the content. */
        int a = 235 - (y * 60 / h);
        hspan(fb, 0, fb->width, y, 0x14, 0x14, 0x14, (uint8_t)a);
    }
    hspan(fb, 0, fb->width, h, THEME_LINE, 255);
}

int theme_logo(FBDev *fb, int x, int y, int h)
{
    if (h < 6) h = 6;
    double par = par_correction(fb);
    int half = h / 2;
    int w = (int)(h * par + 0.5);
    double cx = x + w / 2.0;
    /* Rounded diamond: the half-width at a row shrinks linearly away from
     * the middle row, with the four points clipped flat so it reads as a
     * softened tile rather than a sharp rhombus. */
    double clip = 0.18;
    for (int row = 0; row < h; row++) {
        double v = fabs((row + 0.5) - h / 2.0) / (h / 2.0);   /* 0 middle .. 1 tip */
        if (v > 1 - clip * 0.6) v = 1 - clip * 0.6;
        double hw = (1 - v) * (w / 2.0);
        if (hw > (w / 2.0) * (1 - clip)) hw = (w / 2.0) * (1 - clip);
        hspan(fb, (int)(cx - hw + 0.5), (int)(cx + hw + 0.5), y + row, THEME_GREEN, 255);
    }
    /* White play triangle, pointing right, centered a hair right of middle
     * (optical centre of a triangle). */
    int th = h * 9 / 20;
    if (th < 3) th = 3;
    int ty0 = y + half - th / 2;
    double tw = th * 0.85 * par;
    double tx0 = cx - tw * 0.40;
    for (int row = 0; row < th; row++) {
        double v = fabs((row + 0.5) - th / 2.0) / (th / 2.0);
        double len = tw * (1 - v);
        hspan(fb, (int)(tx0 + 0.5), (int)(tx0 + len + 0.5), ty0 + row, 0xFF, 0xFF, 0xFF, 255);
    }
    return w;
}

void theme_dialog(FBDev *fb, int x, int y, int w, int h, const char *title)
{
    /* Soft shadow: two widening translucent passes. */
    theme_round_rect(fb, x - 3, y - 1, w + 6, h + 5, 7, 0, 0, 0, 90);
    theme_round_rect(fb, x - 1, y + 1, w + 2, h + 2, 6, 0, 0, 0, 110);
    theme_round_rect(fb, x, y, w, h, 5, THEME_SURFACE, 245);
    if (title && title[0]) {
        int lh = 12;
        int lw = theme_logo(fb, x + 12, y + 9, lh);
        draw_text(fb, x + 12 + lw + 8, y + 11, title, 1, THEME_TEXT);
        hspan(fb, x + 10, x + w - 10, y + 27, THEME_LINE, 255);
        hspan(fb, x + 10, x + 10 + 40, y + 27, THEME_GREEN, 255);
    }
}

/* Splits "B:play  A:back" into tokens on runs of 2+ spaces. Calls fn for
 * each with the key (may be empty) and label. */
typedef void (*HintFn)(FBDev *fb, const char *key, const char *label, void *ctx);

static void hints_each(FBDev *fb, const char *hints, HintFn fn, void *ctx)
{
    const char *p = hints;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && !(e[0] == ' ' && e[1] == ' ')) e++;
        char tok[96];
        int n = (int)(e - p);
        if (n >= (int)sizeof(tok)) n = sizeof(tok) - 1;
        memcpy(tok, p, n);
        tok[n] = '\0';
        while (n > 0 && tok[n - 1] == ' ') tok[--n] = '\0';

        char key[32] = "";
        const char *label = tok;
        char *colon = strchr(tok, ':');
        /* Only a short prefix counts as a button name — "LEFT/RIGHT: browse"
         * and "B:select" yes, a sentence with a colon in it no. */
        if (colon && colon - tok > 0 && colon - tok < 12 &&
            memchr(tok, ' ', (size_t)(colon - tok)) == NULL) {
            int kl = (int)(colon - tok);
            memcpy(key, tok, kl);
            key[kl] = '\0';
            label = colon + 1;
            while (*label == ' ') label++;
        }
        fn(fb, key, label, ctx);
        p = e;
    }
}

typedef struct { int x, y, w, measure; } HintCtx;

#define CHIP_PAD 4
#define HINT_GAP 14

static void hint_draw_one(FBDev *fb, const char *key, const char *label, void *vctx)
{
    HintCtx *c = vctx;
    if (c->w > 0) c->x += HINT_GAP, c->w += HINT_GAP;
    if (key[0]) {
        int kw = text_width(fb, key, 1) + CHIP_PAD * 2;
        if (kw < 12) kw = 12;
        if (!c->measure) {
            /* B is the confirm button throughout the app, so its chip takes
             * the accent colour; every other key gets a neutral chip. */
            if (!strcmp(key, "B"))
                theme_pill(fb, c->x, c->y - 2, kw, 12, THEME_GREEN, 255);
            else
                theme_pill(fb, c->x, c->y - 2, kw, 12, 0x48, 0x48, 0x48, 255);
            draw_text(fb, c->x + (kw - text_width(fb, key, 1)) / 2, c->y, key, 1, 0xFF, 0xFF, 0xFF);
        }
        c->x += kw + 4; c->w += kw + 4;
    }
    int lw = text_width(fb, label, 1);
    if (!c->measure) draw_text(fb, c->x, c->y, label, 1, THEME_TEXT2);
    c->x += lw; c->w += lw;
}

int theme_hints_at(FBDev *fb, int x, int y, const char *hints, int measure_only)
{
    HintCtx c = { x, y, 0, measure_only };
    hints_each(fb, hints, hint_draw_one, &c);
    return c.w;
}

int theme_safe_y_bot = 14;   /* main.c's SAFE_Y, mirrored at startup */

void theme_hint_bar(FBDev *fb, const char *hints)
{
    int y = fb->height - 8 - theme_safe_y_bot;
    int w = theme_hints_at(fb, 0, y, hints, 1);
    /* A faint plate behind the row so chips stay legible over artwork. */
    for (int yy = y - 7; yy < fb->height; yy++)
        hspan(fb, 0, fb->width, yy, 0x0C, 0x0C, 0x0C, (uint8_t)(yy < y - 3 ? 120 : 190));
    theme_hints_at(fb, (fb->width - w) / 2, y, hints, 0);
}

void theme_progress(FBDev *fb, int x, int y, int w, int h, double frac)
{
    if (w <= 0) return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fw = (int)(w * frac + 0.5);
    fb_fill_rect_alpha(fb, x, y, w, h, 0x3A, 0x3A, 0x3A, 230);
    if (fw > 0) fb_fill_rect_alpha(fb, x, y, fw, h, THEME_GREEN, 255);
}

void theme_check_badge(FBDev *fb, int x, int y, int size)
{
    if (size < 7) size = 7;
    theme_round_rect(fb, x, y, (int)(size * par_correction(fb) + 0.5), size, size / 2,
                     THEME_GREEN, 255);
    /* Tick: a short down-stroke then a long up-stroke, 2px thick vertically. */
    double par = par_correction(fb);
    int w = (int)(size * par + 0.5);
    int x0 = x + w * 25 / 100, xm = x + w * 43 / 100, x1 = x + w * 76 / 100;
    int ym = y + size * 72 / 100, y0 = y + size * 50 / 100, y1 = y + size * 28 / 100;
    for (int xx = x0; xx <= x1; xx++) {
        int yy;
        if (xx <= xm) yy = y0 + (ym - y0) * (xx - x0) / (xm - x0 ? xm - x0 : 1);
        else          yy = ym - (ym - y1) * (xx - xm) / (x1 - xm ? x1 - xm : 1);
        hspan(fb, xx, xx + 1, yy, 0xFF, 0xFF, 0xFF, 255);
        hspan(fb, xx, xx + 1, yy - 1, 0xFF, 0xFF, 0xFF, 255);
    }
}

void theme_dots(FBDev *fb, int cx, int y)
{
    double t = theme_now();
    int d = 6;
    int gap = (int)(10 * par_correction(fb) / 1.6);
    if (gap < 8) gap = 8;
    for (int i = 0; i < 3; i++) {
        double ph = t * 3.0 - i * 0.7;
        double k = 0.5 + 0.5 * sin(ph);
        uint8_t a = (uint8_t)(70 + 185 * k);
        int x = cx + (i - 1) * (gap + d) - d / 2;
        theme_round_rect(fb, x, y, d, d, d / 2, THEME_GREEN, a);
    }
}
