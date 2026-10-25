// 7OS graphics: anti-aliased shapes, soft shadows, images and text
#include "kernel.h"
#include "gfx.h"

rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0) return rect(0, 0, 0, 0);
    return rect(x0, y0, x1 - x0, y1 - y0);
}
rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = imin(a.x, b.x), y0 = imin(a.y, b.y);
    int x1 = imax(a.x + a.w, b.x + b.w), y1 = imax(a.y + a.h, b.y + b.h);
    return rect(x0, y0, x1 - x0, y1 - y0);
}

uint32_t lighten(uint32_t c, int amt) {
    return amt >= 0 ? blend(c, 0xFFFFFF, amt) : blend(c, 0x000000, -amt);
}

static uint64_t isqrt64(uint64_t v) {
    uint64_t r = 0, b = 1ULL << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

static inline bool clipr(canvas_t *c, int *x, int *y, int *w, int *h) {
    rect_t r = rect_intersect(rect(*x, *y, *w, *h), c->clip);
    if (rect_empty(r)) return false;
    *x = r.x; *y = r.y; *w = r.w; *h = r.h;
    return true;
}

void gfx_fill(canvas_t *c, int x, int y, int w, int h, uint32_t col) {
    if (!clipr(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) memset32(c->px + (y + j) * c->stride + x, col, w);
}

void gfx_fill_a(canvas_t *c, int x, int y, int w, int h, uint32_t col, int a) {
    if (a >= 255) { gfx_fill(c, x, y, w, h, col); return; }
    if (a <= 0 || !clipr(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = c->px + (y + j) * c->stride + x;
        for (int i = 0; i < w; i++) p[i] = blend(p[i], col, a);
    }
}

static inline void plot(canvas_t *c, int x, int y, uint32_t col, int a) {
    if (a <= 0 || !rect_contains(c->clip, x, y)) return;
    uint32_t *p = c->px + y * c->stride + x;
    *p = a >= 255 ? col : blend(*p, col, a);
}

void gfx_line_h(canvas_t *c, int x, int y, int w, uint32_t col, int a) { gfx_fill_a(c, x, y, w, 1, col, a); }
void gfx_line_v(canvas_t *c, int x, int y, int h, uint32_t col, int a) { gfx_fill_a(c, x, y, 1, h, col, a); }

// coverage tables for anti-aliased quarter circles, cached by radius
#define MAX_R 48
static uint8_t *corner_cov[MAX_R + 1];

const uint8_t *gfx_corner_table(int r) {
    if (r > MAX_R) r = MAX_R;
    if (corner_cov[r]) return corner_cov[r];
    uint8_t *t = kmalloc(r * r + 1);
    // t[j*r+i]: pixel at offset (i, j) from the top-left of the corner square;
    // circle centre is at (r, r) in corner-square coordinates.
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int cov = 0;
            // 4x4 supersampling for good edges
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int64_t dx = (int64_t)(i * 8 + sx * 2 + 1) - r * 8;
                    int64_t dy = (int64_t)(j * 8 + sy * 2 + 1) - r * 8;
                    if (dx * dx + dy * dy <= (int64_t)r * r * 64) cov++;
                }
            t[j * r + i] = cov * 255 / 16;
        }
    }
    corner_cov[r] = t;
    return t;
}

void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, int a) {
    if (w <= 0 || h <= 0) return;
    r = imin(r, imin(w, h) / 2);
    if (r > MAX_R) r = MAX_R;
    if (r <= 0) { gfx_fill_a(c, x, y, w, h, col, a); return; }
    if (!rect_intersect(rect(x, y, w, h), c->clip).w) return;
    const uint8_t *t = gfx_corner_table(r);
    // middle band
    gfx_fill_a(c, x, y + r, w, h - 2 * r, col, a);
    for (int j = 0; j < r; j++) {
        int ytop = y + j, ybot = y + h - 1 - j;
        // straight part of the top/bottom rows
        gfx_fill_a(c, x + r, ytop, w - 2 * r, 1, col, a);
        gfx_fill_a(c, x + r, ybot, w - 2 * r, 1, col, a);
        for (int i = 0; i < r; i++) {
            int cov = t[j * r + i];
            if (!cov) continue;
            int aa = cov * a / 255;
            plot(c, x + i, ytop, col, aa);
            plot(c, x + w - 1 - i, ytop, col, aa);
            plot(c, x + i, ybot, col, aa);
            plot(c, x + w - 1 - i, ybot, col, aa);
        }
    }
}

// 1px anti-aliased outline of a rounded rectangle
void gfx_round_rect_border(canvas_t *c, int x, int y, int w, int h, int r, uint32_t col, int a) {
    if (w <= 2 || h <= 2) return;
    r = imin(r, imin(w, h) / 2);
    if (r > MAX_R) r = MAX_R;
    gfx_fill_a(c, x + r, y, w - 2 * r, 1, col, a);
    gfx_fill_a(c, x + r, y + h - 1, w - 2 * r, 1, col, a);
    gfx_fill_a(c, x, y + r, 1, h - 2 * r, col, a);
    gfx_fill_a(c, x + w - 1, y + r, 1, h - 2 * r, col, a);
    if (r <= 0) return;
    const uint8_t *to = gfx_corner_table(r);
    const uint8_t *ti = r > 1 ? gfx_corner_table(r - 1) : NULL;
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int outer = to[j * r + i];
            int inner = 0;
            if (ti && i >= 1 && j >= 1) inner = ti[(j - 1) * (r - 1) + (i - 1)];
            int cov = outer - inner;
            if (cov <= 0) continue;
            int aa = cov * a / 255;
            plot(c, x + i, y + j, col, aa);
            plot(c, x + w - 1 - i, y + j, col, aa);
            plot(c, x + i, y + h - 1 - j, col, aa);
            plot(c, x + w - 1 - i, y + h - 1 - j, col, aa);
        }
    }
}

void gfx_circle(canvas_t *c, int cx, int cy, int r, uint32_t col, int a) {
    gfx_round_rect(c, cx - r, cy - r, 2 * r, 2 * r, r, col, a);
}

void gfx_ring(canvas_t *c, int cx, int cy, int r, int th, uint32_t col, int a) {
    int x0 = cx - r, y0 = cy - r;
    for (int y = 0; y < 2 * r; y++) {
        for (int x = 0; x < 2 * r; x++) {
            if (!rect_contains(c->clip, x0 + x, y0 + y)) continue;
            int64_t dx = 2 * x + 1 - 2 * r, dy = 2 * y + 1 - 2 * r;
            int64_t d = isqrt64((uint64_t)((dx * dx + dy * dy) << 14));  // distance in 1/256 px
            int64_t ro = (int64_t)r * 256, ri = (int64_t)(r - th) * 256;
            int64_t co = iclamp((int)(ro - d + 128), 0, 256);
            int64_t ci = iclamp((int)(ri - d + 128), 0, 256);
            int cov = (int)(co - ci);
            if (cov > 0) plot(c, x0 + x, y0 + y, col, cov * a / 256);
        }
    }
}

void gfx_vgradient(canvas_t *c, int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    int ox = x, oy = y, ow = w, oh = h;
    (void)ox; (void)ow;
    if (!clipr(c, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        int t = oh > 1 ? (y + j - oy) * 255 / (oh - 1) : 0;
        memset32(c->px + (y + j) * c->stride + x, mix(top, bot, t), w);
    }
}

// Soft drop shadow around a rounded rectangle. The area under the rectangle itself is skipped
// (windows are opaque), so only the visible halo is computed.
static uint8_t shadow_falloff[64];
static int shadow_spread_cached = -1;

void gfx_shadow(canvas_t *c, rect_t r, int radius, int spread, int alpha, int yoff) {
    if (spread > 63) spread = 63;
    if (shadow_spread_cached != spread) {
        for (int d = 0; d < 64; d++) {
            int t = d >= spread ? 0 : (spread - d) * 256 / spread;  // 256 .. 0
            shadow_falloff[d] = (uint8_t)((t * t) >> 8 > 255 ? 255 : (t * t) >> 8);
        }
        shadow_spread_cached = spread;
    }
    rect_t s = rect(r.x, r.y + yoff, r.w, r.h);  // shadow-casting shape
    rect_t area = rect_intersect(rect(s.x - spread, s.y - spread, s.w + 2 * spread, s.h + 2 * spread), c->clip);
    if (rect_empty(area)) return;
    int rad = imin(radius, imin(s.w, s.h) / 2);
    for (int y = area.y; y < area.y + area.h; y++) {
        uint32_t *row = c->px + y * c->stride;
        // horizontal distance outside depends on x; vertical on y
        int dy = 0;
        if (y < s.y) dy = s.y - y;
        else if (y >= s.y + s.h) dy = y - (s.y + s.h - 1);
        // span hidden under the (opaque) window; corner squares stay visible because they are rounded
        int skip0 = 1, skip1 = 0;
        if (y >= r.y && y < r.y + r.h) {
            bool corner_row = y < r.y + rad || y >= r.y + r.h - rad;
            skip0 = corner_row ? r.x + rad : r.x;
            skip1 = corner_row ? r.x + r.w - rad : r.x + r.w;
        }
        for (int x = area.x; x < area.x + area.w; x++) {
            if (x >= skip0 && x < skip1) { x = skip1 - 1; continue; }
            int dx = 0;
            if (x < s.x) dx = s.x - x;
            else if (x >= s.x + s.w) dx = x - (s.x + s.w - 1);
            int d;
            // inside the corner zones use the distance to the rounded corner
            int cxl = s.x + rad, cxr = s.x + s.w - 1 - rad, cyt = s.y + rad, cyb = s.y + s.h - 1 - rad;
            int ex = x < cxl ? cxl - x : (x > cxr ? x - cxr : 0);
            int ey = y < cyt ? cyt - y : (y > cyb ? y - cyb : 0);
            if (ex && ey) {
                d = (int)isqrt64((uint64_t)(ex * ex + ey * ey)) - rad;
                if (d < 0) d = 0;
            } else {
                d = dx > dy ? dx : dy;
            }
            if (d >= spread) continue;
            int a = shadow_falloff[d] * alpha >> 8;
            if (a > 0) row[x] = blend(row[x], 0, a);
        }
    }
}

void gfx_image(canvas_t *c, const image_t *img, int x, int y, int alpha) {
    rect_t r = rect_intersect(rect(x, y, img->w, img->h), c->clip);
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint32_t *src = img->px + (j - y) * img->w + (r.x - x);
        uint32_t *dst = c->px + j * c->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            uint32_t s = src[i];
            int a = (s >> 24) * alpha / 255;
            if (a) dst[i] = a >= 255 ? (s & 0xFFFFFF) : blend(dst[i], s & 0xFFFFFF, a);
        }
    }
}

void gfx_mask(canvas_t *c, const mask_t *m, int x, int y, uint32_t col, int alpha) {
    rect_t r = rect_intersect(rect(x, y, m->w, m->h), c->clip);
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint8_t *src = m->px + (j - y) * m->w + (r.x - x);
        uint32_t *dst = c->px + j * c->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            int a = src[i] * alpha / 255;
            if (a) dst[i] = blend(dst[i], col, a);
        }
    }
}

void gfx_blit(canvas_t *c, const uint32_t *src, int sstride, int x, int y, int w, int h) {
    rect_t r = rect_intersect(rect(x, y, w, h), c->clip);
    for (int j = r.y; j < r.y + r.h; j++)
        memcpy(c->px + j * c->stride + r.x, src + (j - y) * sstride + (r.x - x), r.w * 4);
}

// ---------------------------------------------------------------- text
static inline const glyph_t *glyph(const font_t *f, unsigned char ch) {
    int i = ch - 32;
    if (i < 0 || i >= f->count) i = '?' - 32;
    return &f->glyphs[i];
}

void gfx_glyph(canvas_t *c, const font_t *f, int x, int y, unsigned char ch, uint32_t col) {
    const glyph_t *g = glyph(f, ch);
    if (!g->w) return;
    int gx = x + g->bx, gy = y + g->by;
    rect_t r = rect_intersect(rect(gx, gy, g->w, g->h), c->clip);
    if (rect_empty(r)) return;
    const uint8_t *data = f->data + g->off;
    for (int j = r.y; j < r.y + r.h; j++) {
        const uint8_t *src = data + (j - gy) * g->w + (r.x - gx);
        uint32_t *dst = c->px + j * c->stride + r.x;
        for (int i = 0; i < r.w; i++) {
            int a = src[i];
            if (a) dst[i] = a == 255 ? col : blend(dst[i], col, a);
        }
    }
}

int gfx_text_n(canvas_t *c, const font_t *f, int x, int y, const char *s, int n, uint32_t col) {
    int pen = x * 64;
    // skip whole string quickly if vertically out of the clip
    bool vis = !(y >= c->clip.y + c->clip.h || y + f->line_h <= c->clip.y);
    for (int i = 0; i < n && s[i]; i++) {
        unsigned char ch = s[i];
        const glyph_t *g = glyph(f, ch);
        int px = (pen + 32) >> 6;
        if (vis && px < c->clip.x + c->clip.w) gfx_glyph(c, f, px, y, ch, col);
        pen += g->adv64;
    }
    return (pen + 32) >> 6;
}
int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint32_t col) {
    return gfx_text_n(c, f, x, y, s, 0x7FFFFFFF, col);
}
int gfx_text_width_n(const font_t *f, const char *s, int n) {
    int pen = 0;
    for (int i = 0; i < n && s[i]; i++) pen += glyph(f, (unsigned char)s[i])->adv64;
    return (pen + 32) >> 6;
}
int gfx_text_width(const font_t *f, const char *s) { return gfx_text_width_n(f, s, 0x7FFFFFFF); }

int gfx_text_fit(canvas_t *c, const font_t *f, int x, int y, const char *s, int maxw, uint32_t col) {
    if (gfx_text_width(f, s) <= maxw) return gfx_text(c, f, x, y, s, col);
    int ell = gfx_text_width(f, "\x82");
    int n = strlen(s);
    while (n > 0 && gfx_text_width_n(f, s, n) + ell > maxw) n--;
    int e = gfx_text_n(c, f, x, y, s, n, col);
    return gfx_text(c, f, e, y, "\x82", col);
}
