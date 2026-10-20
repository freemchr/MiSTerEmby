/* compose_backdrop_wash() — the browse screen's hero backdrop.
 *
 * Worth a test because every way this can be wrong is quiet. An inverted wash
 * still draws a picture, just one that fades into the top of the screen
 * instead of the bottom, leaving the list sitting on the bright end. A wrong
 * scale factor is simply a slightly-too-dark background. Neither looks like a
 * bug in a screenshot, and neither is reachable off-hardware any other way:
 * the hero only appears inside a library, which no --preview mode reaches. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "draw.h"

/* draw.c's text and image helpers call into fb.c, which needs Linux headers
 * and so cannot be linked here. compose_backdrop_wash touches neither — it
 * only reads and writes caller-owned memory — so standing them up as stubs
 * links the file without pulling the framebuffer in. If one of these ever
 * actually fires, that is itself the bug. */
void fb_blit(FBDev *fb, const uint8_t *pixels, int sw, int sh,
             int dx, int dy, int dw, int dh, uint8_t layer_alpha)
{
    (void)fb; (void)pixels; (void)sw; (void)sh;
    (void)dx; (void)dy; (void)dw; (void)dh; (void)layer_alpha;
    assert(!"compose_backdrop_wash must not draw to a framebuffer");
}
void fb_fill_rect_alpha(FBDev *fb, int x, int y, int w, int h,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t alpha)
{
    (void)fb; (void)x; (void)y; (void)w; (void)h;
    (void)r; (void)g; (void)b; (void)alpha;
    assert(!"compose_backdrop_wash must not draw to a framebuffer");
}

#define ALPHA 77

static int checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { \
    printf("  FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); exit(1); } } while (0)

/* A solid white source: every output pixel is then purely the alpha and the
 * wash, with no source variation to reason around. */
static uint8_t *white(int w, int h)
{
    uint8_t *p = malloc((size_t)w * h * 4);
    memset(p, 0xFF, (size_t)w * h * 4);
    return p;
}

static const uint8_t *px(const uint8_t *buf, int w, int x, int y)
{
    return buf + ((size_t)y * w + x) * 4;
}

int main(void)
{
    const int dw = 64, dh = 48;

    /* ── degenerate inputs are refused, not guessed at ── */
    CHECK(compose_backdrop_wash(NULL, 4, 4, dw, dh, ALPHA) == NULL, "NULL src refused");
    uint8_t *tiny = white(4, 4);
    CHECK(compose_backdrop_wash(tiny, 0, 4, dw, dh, ALPHA) == NULL, "sw=0 refused");
    CHECK(compose_backdrop_wash(tiny, 4, 4, 0, dh, ALPHA) == NULL, "dw=0 refused");
    CHECK(compose_backdrop_wash(tiny, 4, 4, dw, 0, ALPHA) == NULL, "dh=0 refused");

    /* ── the wash ── */
    uint8_t *src = white(8, 6);
    uint8_t *out = compose_backdrop_wash(src, 8, 6, dw, dh, ALPHA);
    CHECK(out != NULL, "composed");

    /* Top row keeps the full alpha: white * 77/255 == 77. */
    CHECK(px(out, dw, 0, 0)[0] == ALPHA, "top row is the un-washed alpha");
    CHECK(px(out, dw, dw - 1, 0)[0] == ALPHA, "top row uniform across the width");

    /* Bottom row is fully washed to black — this is the end the list's text
     * sits on, and the reason the rect blends into the cleared frame below. */
    CHECK(px(out, dw, 0, dh - 1)[0] == 0, "bottom row is black");

    /* Monotonically darker downward. The direction is the whole point: get it
     * backwards and the artwork is brightest exactly where the text is. */
    for (int y = 1; y < dh; y++) {
        int prev = px(out, dw, 0, y - 1)[0];
        int cur  = px(out, dw, 0, y)[0];
        CHECK(cur <= prev, "wash never brightens going down");
    }
    CHECK(px(out, dw, 0, dh / 2)[0] < ALPHA, "midpoint is darker than the top");
    CHECK(px(out, dw, 0, dh / 2)[0] > 0,     "midpoint is not yet black");

    /* Opaque throughout: replay is fb_blit_opaque, which ignores alpha, so a
     * non-255 value here would be a silent lie about the buffer's contract. */
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            CHECK(px(out, dw, x, y)[3] == 255, "every pixel opaque");

    /* alpha scales the whole thing linearly: at 255 the top row is the source
     * itself, at 0 the buffer is black. */
    uint8_t *full = compose_backdrop_wash(src, 8, 6, dw, dh, 255);
    CHECK(px(full, dw, 0, 0)[0] == 255, "alpha=255 keeps the source at the top");
    uint8_t *zero = compose_backdrop_wash(src, 8, 6, dw, dh, 0);
    CHECK(px(zero, dw, 0, 0)[0] == 0, "alpha=0 is black");

    /* dh == 1: one row, and it is the bottom one. Guards the (dh-1) divide. */
    uint8_t *one = compose_backdrop_wash(src, 8, 6, dw, 1, ALPHA);
    CHECK(one != NULL, "dh=1 composes");
    CHECK(px(one, dw, 0, 0)[3] == 255, "dh=1 pixel opaque");

    /* Upscaling and downscaling both stay in bounds — sampled, not read past.
     * A source smaller than the destination is the normal case for a narrow
     * canvas, larger is the normal case for a wide one. */
    uint8_t *big = white(200, 150);
    uint8_t *down = compose_backdrop_wash(big, 200, 150, dw, dh, ALPHA);
    CHECK(down && px(down, dw, dw - 1, dh - 1)[3] == 255, "downscale in bounds");

    free(tiny); free(src); free(out); free(full); free(zero); free(one);
    free(big); free(down);
    printf("hero: %d checks, 0 failures\n", checks);
    return 0;
}
