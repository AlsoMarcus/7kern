// 7OS graphics library: software rendering into 32-bit canvases.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct { int16_t adv64, bx, by, w, h; uint32_t off; } glyph_t;
typedef struct {
    int size, ascent, descent, line_h, count;
    const glyph_t *glyphs;
    const uint8_t *data;
} font_t;
typedef struct { int w, h; const uint32_t *px; } image_t;  // straight-alpha ARGB
typedef struct { int w, h; const uint8_t *px; } mask_t;    // 8-bit coverage

typedef struct { int x, y, w, h; } rect_t;
typedef struct {
    uint32_t *px;
    int w, h, stride;  // stride in pixels
    rect_t clip;
} canvas_t;

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline rect_t rect(int x, int y, int w, int h) { rect_t r = {x, y, w, h}; return r; }
static inline bool rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_contains(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
rect_t rect_intersect(rect_t a, rect_t b);
rect_t rect_union(rect_t a, rect_t b);
static inline rect_t rect_inset(rect_t r, int d) { return rect(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d); }

// colour helpers
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    a += a >> 7;  // 0..255 -> 0..256
    uint32_t ia = 256 - a;
    uint32_t rb = (((src & 0xFF00FF) * a + (dst & 0xFF00FF) * ia) >> 8) & 0xFF00FF;
    uint32_t g = (((src & 0x00FF00) * a + (dst & 0x00FF00) * ia) >> 8) & 0x00FF00;
    return rb | g;
}
static inline uint32_t mix(uint32_t a, uint32_t b, int t) { return blend(a, b, (uint32_t)iclamp(t, 0, 255)); }
uint32_t lighten(uint32_t c, int amt);  // amt -255..255 (negative darkens)

void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void gfx_fill_a(canvas_t *c, int x, int y, int w, int h, uint32_t col, int a);
void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, int a);
void gfx_round_rect_border(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, int a);
void gfx_circle(canvas_t *c, int cx, int cy, int r, uint32_t col, int a);
void gfx_ring(canvas_t *c, int cx, int cy, int r, int thick, uint32_t col, int a);
void gfx_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_shadow(canvas_t *c, rect_t r, int radius, int spread, int alpha, int yoff);
void gfx_image(canvas_t *c, const image_t *img, int x, int y, int alpha);
void gfx_mask(canvas_t *c, const mask_t *m, int x, int y, uint32_t col, int alpha);
void gfx_blit(canvas_t *c, const uint32_t *src, int sstride, int x, int y, int w, int h);
const uint8_t *gfx_corner_table(int r);  // anti-aliased quarter-circle coverage (r*r bytes)
void gfx_line_h(canvas_t *c, int x, int y, int w, uint32_t col, int a);
void gfx_line_v(canvas_t *c, int x, int y, int h, uint32_t col, int a);

int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint32_t col);
int gfx_text_n(canvas_t *c, const font_t *f, int x, int y, const char *s, int n, uint32_t col);
int gfx_text_width(const font_t *f, const char *s);
int gfx_text_width_n(const font_t *f, const char *s, int n);
// draws text, truncating with an ellipsis if wider than maxw
int gfx_text_fit(canvas_t *c, const font_t *f, int x, int y, const char *s, int maxw, uint32_t col);
void gfx_glyph(canvas_t *c, const font_t *f, int x, int y, unsigned char ch, uint32_t col);
static inline int gfx_char_adv(const font_t *f, unsigned char ch) {
    int i = ch - 32;
    if (i < 0 || i >= f->count) i = 0;
    return (f->glyphs[i].adv64 + 32) >> 6;
}
