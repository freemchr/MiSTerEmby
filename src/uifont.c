/* The UI display font — see uifont.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uifont.h"
#include "draw.h"
#include "stb_image.h"   /* declarations only — the implementation is compiled in main.c */
#include "uifont_data.h"

#define N_GLYPHS UIFONT_GLYPHS

static uint8_t *s_strip = NULL;     /* decoded atlas: coverage, one byte per pixel */
static int      s_strip_w = 0, s_strip_h = 0;

/* One scaled copy of the whole strip per size drawn. A screen only ever
 * uses two or three sizes, so a few slots are plenty; the oldest is reused. */
typedef struct {
    int      h;
    double   par;
    int      w;                 /* scaled strip width */
    uint8_t *px;                /* w * h coverage */
    int      x[UIFONT_GLYPHS];
    int      adv[UIFONT_GLYPHS];
} Scaled;
#define SLOTS 4
static Scaled s_slots[SLOTS];
static int    s_next_slot = 0;

static int glyph_index(unsigned cp)
{
    if (cp >= 32 && cp <= 126) return (int)cp - 32;
    if (cp >= UIFONT_FIRST_LATIN1 && cp <= 255) return 95 + (int)cp - UIFONT_FIRST_LATIN1;
    return '?' - 32;
}

/* Next codepoint of UTF-8 text, advancing *p. Malformed bytes read as
 * themselves (Latin-1), so nothing is ever skipped silently. */
static unsigned next_cp(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = s[0];
    if (c >= 0xC0 && c < 0xE0 && (s[1] & 0xC0) == 0x80) {
        *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if (c >= 0xE0 && c < 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *p += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if (c >= 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *p += 4; return 0xFFFD;
    }
    *p += 1;
    return c;
}

static int load_strip(void)
{
    if (s_strip) return 1;
    int comp;
    s_strip = stbi_load_from_memory(uifont_png, (int)sizeof(uifont_png),
                                    &s_strip_w, &s_strip_h, &comp, 1);
    return s_strip != NULL;
}

/* Box filter of the strip's columns [sx0, sx1) into dst columns [dx0, dx1)
 * of an h-row image dw wide. */
static void scale_glyph(uint8_t *dst, int dw, int h, int dx0, int dx1, int sx0, int sx1)
{
    int gw = dx1 - dx0, sw = sx1 - sx0;
    if (gw <= 0 || sw <= 0) return;
    for (int y = 0; y < h; y++) {
        int y0 = y * s_strip_h / h, y1 = (y + 1) * s_strip_h / h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < gw; x++) {
            int x0 = sx0 + x * sw / gw, x1 = sx0 + (x + 1) * sw / gw;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned sum = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < s_strip_h; yy++)
                for (int xx = x0; xx < x1 && xx < s_strip_w; xx++) {
                    sum += s_strip[(size_t)yy * s_strip_w + xx];
                    n++;
                }
            dst[(size_t)y * dw + dx0 + x] = (uint8_t)(n ? sum / n : 0);
        }
    }
}

static Scaled *scaled_for(FBDev *fb, int h)
{
    if (h < 6) h = 6;
    double par = par_correction(fb);
    for (int i = 0; i < SLOTS; i++)
        if (s_slots[i].px && s_slots[i].h == h && s_slots[i].par == par) return &s_slots[i];
    if (!load_strip()) return NULL;

    Scaled *sc = &s_slots[s_next_slot];
    s_next_slot = (s_next_slot + 1) % SLOTS;
    free(sc->px);
    memset(sc, 0, sizeof(*sc));
    double sx = (double)h / s_strip_h * par;
    int total = 0;
    for (int g = 0; g < N_GLYPHS; g++) {
        sc->x[g] = total;
        sc->adv[g] = (int)(uifont_w[g] * sx + 0.5);
        total += sc->adv[g];
    }
    sc->px = calloc((size_t)total * h, 1);
    if (!sc->px) return NULL;
    sc->h = h; sc->par = par; sc->w = total;
    for (int g = 0; g < N_GLYPHS; g++)
        scale_glyph(sc->px, total, h, sc->x[g], sc->x[g] + sc->adv[g],
                    uifont_x[g], uifont_x[g] + uifont_w[g]);
    return sc;
}

int uifont_width(FBDev *fb, const char *text, int h)
{
    Scaled *sc = scaled_for(fb, h);
    if (!sc || !text) return 0;
    int w = 0;
    for (const char *p = text; *p; ) w += sc->adv[glyph_index(next_cp(&p))];
    return w;
}

void uifont_draw_clipped(FBDev *fb, int x, int y, const char *text, int h,
                         uint8_t r, uint8_t g, uint8_t b, int x0, int x1)
{
    Scaled *sc = scaled_for(fb, h);
    if (!sc || !text) return;
    if (x0 < 0) x0 = 0;
    if (x1 > fb->width) x1 = fb->width;
    for (const char *p = text; *p; ) {
        int gi = glyph_index(next_cp(&p));
        int gw = sc->adv[gi];
        for (int row = 0; row < h; row++) {
            int fy = y + row;
            if (fy < 0 || fy >= fb->height) continue;
            uint32_t *dst = (uint32_t *)(fb->back + (size_t)fy * fb->stride);
            const uint8_t *src = sc->px + (size_t)row * sc->w + sc->x[gi];
            for (int col = 0; col < gw; col++) {
                int fx = x + col;
                if (fx < x0 || fx >= x1) continue;
                uint32_t a = src[col];
                if (!a) continue;
                uint32_t d = dst[fx];
                uint32_t or_ = (r * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
                uint32_t og  = (g * a + ((d >>  8) & 0xFF) * (255 - a)) / 255;
                uint32_t ob  = (b * a + ( d        & 0xFF) * (255 - a)) / 255;
                dst[fx] = (or_ << 16) | (og << 8) | ob;
            }
        }
        x += gw;
        if (x >= x1) break;
    }
}

void uifont_draw(FBDev *fb, int x, int y, const char *text, int h,
                 uint8_t r, uint8_t g, uint8_t b)
{
    uifont_draw_clipped(fb, x, y, text, h, r, g, b, 0, fb->width);
}

void uifont_truncate(FBDev *fb, char *text, int h, int max_w)
{
    if (uifont_width(fb, text, h) <= max_w) return;
    size_t orig = strlen(text);
    size_t len = orig;
    while (len > 0) {
        /* Step back over a whole UTF-8 character. */
        do { len--; } while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80);
        char buf[512];
        snprintf(buf, sizeof(buf), "%.*s...", (int)len, text);
        /* Only once "..." fits in the bytes the text already had: the
         * caller's buffer is no bigger than that. */
        if (len + 3 <= orig && (uifont_width(fb, buf, h) <= max_w || len == 0)) {
            memcpy(text, buf, len + 4);
            return;
        }
    }
}
