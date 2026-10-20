#ifndef THEME_H
#define THEME_H

#include <stdint.h>
#include "fb.h"

/* MiSTerEmby's look — Emby's dark charcoal surfaces with its green as the
 * one accent colour (focus, progress, played badges, primary buttons),
 * white primary text and grey secondary text. Everything that gives a
 * screen its chrome (backdrop, header, dialogs, button-hint bar) lives here
 * so the screens in main.c only lay out their own content. */

/* Colour triples, usable straight in a draw_text/fb_fill_rect_alpha call. */
#define THEME_GREEN       0x52, 0xB5, 0x4B   /* Emby green */
#define THEME_GREEN_DARK  0x3A, 0x87, 0x34
#define THEME_GREEN_LIGHT 0x8E, 0xD8, 0x86
#define THEME_BG          0x10, 0x10, 0x10
#define THEME_SURFACE     0x1E, 0x1E, 0x1E
#define THEME_SURFACE_HI  0x2C, 0x2C, 0x2C
#define THEME_LINE        0x3A, 0x3A, 0x3A
#define THEME_TEXT        0xF2, 0xF2, 0xF2
#define THEME_TEXT2       0xA6, 0xA6, 0xA6
#define THEME_TEXT3       0x70, 0x70, 0x70

/* Full-screen backdrop: charcoal with a soft vertical falloff and two slow
 * drifting green glows. Replaces fb_clear() + starfield on the static
 * screens (About, setup, sign-in, empty states). Cheap — the base is
 * composed once per screen size and copied opaque. */
void theme_backdrop(FBDev *fb);

/* Plain charcoal fill (no glows) — the base under list/info screens. */
void theme_fill_bg(FBDev *fb);

/* Translucent header strip across the top with a hairline under it. The
 * caller draws the logo/title/clock on top (see main.c's draw_top_bar). */
void theme_header(FBDev *fb, int safe_y);

/* MiSTerEmby's mark: a green rounded diamond with a white play triangle,
 * h pixels tall, top-left at (x, y). Returns its drawn width. */
int theme_logo(FBDev *fb, int x, int y, int h);

/* Rounded rectangle fill. radius is in screen rows; the horizontal radius
 * is widened by the panel's pixel aspect so corners read round, not oval. */
void theme_round_rect(FBDev *fb, int x, int y, int w, int h, int radius,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* A fully rounded "button" plate (radius = h/2). */
void theme_pill(FBDev *fb, int x, int y, int w, int h,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* 1px rounded outline. */
void theme_round_outline(FBDev *fb, int x, int y, int w, int h, int radius,
                         uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* Emby-style dialog card: soft shadow, surface body, header row with title
 * and a green accent rule. */
void theme_dialog(FBDev *fb, int x, int y, int w, int h, const char *title);

/* main.c's bottom safe margin, set once at startup. */
extern int theme_safe_y_bot;

/* Bottom button-hint bar. hints is the existing "B:play  A:back" form —
 * tokens separated by two or more spaces, each "KEY:label" — drawn as key
 * chips followed by their labels, centered. Tokens without a ':' draw as
 * plain text. */
void theme_hint_bar(FBDev *fb, const char *hints);

/* Same chips, left- or right-aligned at an explicit baseline; returns width. */
int theme_hints_at(FBDev *fb, int x, int y, const char *hints, int measure_only);

/* Thin green progress bar over a dark track (resume points, timelines). */
void theme_progress(FBDev *fb, int x, int y, int w, int h, double frac);

/* Green circle with a white tick — Emby's "played" indicator. size ~ 9. */
void theme_check_badge(FBDev *fb, int x, int y, int size);

/* Three pulsing green dots centered on cx — "working on it". */
void theme_dots(FBDev *fb, int cx, int y);

#endif
