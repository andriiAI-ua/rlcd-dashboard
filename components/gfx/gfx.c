// 1-bit graphics on a canvas in RAM. st7305_show() sends the canvas to the panel.
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define ROW_BYTES (GFX_WIDTH / 8)

static uint8_t s_canvas[ROW_BYTES * GFX_HEIGHT];

const uint8_t *gfx_canvas(void)
{
    return s_canvas;
}

void gfx_copy(const uint8_t *image)
{
    memcpy(s_canvas, image, sizeof(s_canvas));
}

void gfx_clear(gfx_color_t color)
{
    memset(s_canvas, color == GFX_BLACK ? 0xFF : 0x00, sizeof(s_canvas));
}

void gfx_pixel(int x, int y, gfx_color_t color)
{
    if (x < 0 || y < 0 || x >= GFX_WIDTH || y >= GFX_HEIGHT) {
        return;
    }
    uint8_t *p = &s_canvas[y * ROW_BYTES + (x >> 3)];
    const uint8_t mask = (uint8_t)(0x80 >> (x & 7));
    if (color == GFX_BLACK) {
        *p |= mask;
    } else {
        *p &= (uint8_t)~mask;
    }
}

void gfx_hline(int x, int y, int w, gfx_color_t color)
{
    if (y < 0 || y >= GFX_HEIGHT || w <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int x1 = x + w > GFX_WIDTH ? GFX_WIDTH : x + w;  // exclusive
    for (int i = x0; i < x1; i++) {
        gfx_pixel(i, y, color);
    }
}

void gfx_vline(int x, int y, int h, gfx_color_t color)
{
    for (int i = 0; i < h; i++) {
        gfx_pixel(x, y + i, color);
    }
}

void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color)
{
    // Bresenham's line algorithm
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color)
{
    for (int row = 0; row < h; row++) {
        gfx_hline(x, y + row, w, color);
    }
}

void gfx_rect(int x, int y, int w, int h, int thickness, gfx_color_t color)
{
    gfx_round_rect(x, y, w, h, 0, thickness, color);
}

// Return how far a row of a rounded rectangle starts from the side, pixels.
static int corner_inset(int r, int h, int row)
{
    int d;
    if (row < r) {
        d = r - row;
    } else if (row >= h - r) {
        d = row - (h - r) + 1;
    } else {
        return 0;
    }
    const float dy = (float)d - 0.5f;
    const float dx = sqrtf((float)(r * r) - dy * dy);
    return r - (int)lroundf(dx);
}

static int clamp_radius(int r, int w, int h)
{
    const int max = (w < h ? w : h) / 2;
    if (r > max) {
        r = max;
    }
    return r < 0 ? 0 : r;
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, gfx_color_t color)
{
    r = clamp_radius(r, w, h);
    for (int row = 0; row < h; row++) {
        const int inset = corner_inset(r, h, row);
        gfx_hline(x + inset, y + row, w - 2 * inset, color);
    }
}

void gfx_round_rect(int x, int y, int w, int h, int r, int thickness, gfx_color_t color)
{
    r = clamp_radius(r, w, h);
    const int t = thickness;
    const int iw = w - 2 * t, ih = h - 2 * t;
    const int ir = r - t > 0 ? r - t : 0;
    for (int row = 0; row < h; row++) {
        const int outer = corner_inset(r, h, row);
        const int irow = row - t;
        if (iw <= 0 || ih <= 0 || irow < 0 || irow >= ih) {
            gfx_hline(x + outer, y + row, w - 2 * outer, color);
            continue;
        }
        const int inner = t + corner_inset(ir, ih, irow);
        gfx_hline(x + outer, y + row, inner - outer, color);
        gfx_hline(x + w - inner, y + row, inner - outer, color);
    }
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t color)
{
    // Fill the pixels whose centers are inside the triangle (edge functions).
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    const long area = (long)(x1 - x0) * (y2 - y0) - (long)(y1 - y0) * (x2 - x0);
    if (area == 0) {
        return;
    }
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            // Pixel center, doubled to stay in integers
            const long px = 2L * x + 1, py = 2L * y + 1;
            long w0 = (2L * x1 - 2L * x0) * (py - 2L * y0) - (2L * y1 - 2L * y0) * (px - 2L * x0);
            long w1 = (2L * x2 - 2L * x1) * (py - 2L * y1) - (2L * y2 - 2L * y1) * (px - 2L * x1);
            long w2 = (2L * x0 - 2L * x2) * (py - 2L * y2) - (2L * y0 - 2L * y2) * (px - 2L * x2);
            if (area < 0) {
                w0 = -w0;
                w1 = -w1;
                w2 = -w2;
            }
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                gfx_pixel(x, y, color);
            }
        }
    }
}

void gfx_fill_polygon(const float *xy, int n, gfx_color_t color)
{
    if (n < 3 || n > 16) {
        return;
    }
    float miny = xy[1], maxy = xy[1];
    for (int i = 1; i < n; i++) {
        miny = fminf(miny, xy[2 * i + 1]);
        maxy = fmaxf(maxy, xy[2 * i + 1]);
    }
    // For each row, find where the edges cross the line through the pixel centers.
    for (int y = (int)floorf(miny); y <= (int)ceilf(maxy); y++) {
        const float sy = (float)y + 0.5f;
        float xs[16];
        int k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            const float x0 = xy[2 * i], y0 = xy[2 * i + 1];
            const float x1 = xy[2 * j], y1 = xy[2 * j + 1];
            if ((y0 <= sy) != (y1 <= sy)) {
                xs[k++] = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
            }
        }
        // Sort the crossings (there are few of them).
        for (int a = 1; a < k; a++) {
            const float v = xs[a];
            int b = a - 1;
            while (b >= 0 && xs[b] > v) {
                xs[b + 1] = xs[b];
                b--;
            }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < k; a += 2) {
            const int from = (int)ceilf(xs[a] - 0.5f);
            const int to = (int)floorf(xs[a + 1] - 0.5f);
            gfx_hline(from, y, to - from + 1, color);
        }
    }
}

/* Text */

// Decode one UTF-8 character. Return the code point and move *s to the next character.
static uint32_t next_code_point(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        *s += 1;
        return '?';
    }
    p++;
    for (int i = 0; i < extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s = (const char *)(p + i);
            return '?';
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s = (const char *)(p + extra);
    return cp;
}

static const gfx_glyph_t *find_glyph(const gfx_font_t *font, uint32_t code)
{
    int lo = 0, hi = font->count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t c = font->glyphs[mid].code;
        if (c == code) {
            return &font->glyphs[mid];
        }
        if (c < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static const gfx_glyph_t *glyph_or_fallback(const gfx_font_t *font, uint32_t code)
{
    const gfx_glyph_t *g = find_glyph(font, code);
    if (!g && code == 0x2212) {
        g = find_glyph(font, '-');  // minus sign
    }
    if (!g && (code == 0x2019 || code == 0x02BC)) {
        g = find_glyph(font, '\'');  // apostrophe
    }
    if (!g && code == 0x00A0) {
        g = find_glyph(font, ' ');  // no-break space
    }
    return g ? g : find_glyph(font, '?');
}

int gfx_text_width(const gfx_font_t *font, const char *text)
{
    int adv16 = 0;
    while (*text) {
        const gfx_glyph_t *g = glyph_or_fallback(font, next_code_point(&text));
        if (g) {
            adv16 += g->adv16;
        }
    }
    return (adv16 + 8) / 16;
}

static void draw_glyph(const gfx_font_t *font, const gfx_glyph_t *g, int x, int y, gfx_color_t color)
{
    const uint8_t *bits = font->bitmaps + g->offset;
    int bit = 0;
    for (int row = 0; row < g->h; row++) {
        for (int col = 0; col < g->w; col++, bit++) {
            if (bits[bit >> 3] & (0x80 >> (bit & 7))) {
                gfx_pixel(x + col, y + row, color);
            }
        }
    }
}

int gfx_text(const gfx_font_t *font, int x, int baseline, gfx_align_t align, gfx_color_t color, const char *text)
{
    const int width = gfx_text_width(font, text);
    if (align == GFX_ALIGN_RIGHT) {
        x -= width;
    } else if (align == GFX_ALIGN_CENTER) {
        x -= width / 2;
    }
    int pen16 = x * 16;
    while (*text) {
        const gfx_glyph_t *g = glyph_or_fallback(font, next_code_point(&text));
        if (!g) {
            continue;
        }
        draw_glyph(font, g, (pen16 + 8) / 16 + g->dx, baseline + g->dy, color);
        pen16 += g->adv16;
    }
    return width;
}
