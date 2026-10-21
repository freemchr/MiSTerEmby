#ifndef UIFONT_H
#define UIFONT_H

#include <stdint.h>
#include "fb.h"

/* The UI's display font: Fredoka SemiBold, smooth-edged, for big text —
 * the home tiles, screen titles, the focused title under the posters. The
 * 8x8 bitmap font (draw.h) stays for small text, where it reads better on a
 * CRT. Built in (src/uifont_data.h, from tools/gen_uifont.py), and scaled
 * once per size drawn, so drawing is a blend of ready-made glyphs.
 *
 * h is the line height in framebuffer rows; capitals come out about two
 * thirds of it. Widths account for the screen's non-square pixels. Text is
 * UTF-8; anything beyond Latin-1 draws as '?', like the bitmap font. */

int  uifont_width(FBDev *fb, const char *text, int h);
void uifont_draw(FBDev *fb, int x, int y, const char *text, int h,
                 uint8_t r, uint8_t g, uint8_t b);
/* Draws only the part of the text between columns x0 and x1 (for a
 * scrolling title). */
void uifont_draw_clipped(FBDev *fb, int x, int y, const char *text, int h,
                         uint8_t r, uint8_t g, uint8_t b, int x0, int x1);
/* Shortens text in place with "..." until it fits max_w. */
void uifont_truncate(FBDev *fb, char *text, int h, int max_w);

#endif
