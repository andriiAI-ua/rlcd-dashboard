// 1-bit graphics for a 400 x 300 canvas: shapes and UTF-8 text with bitmap fonts.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_WIDTH  400
#define GFX_HEIGHT 300

typedef enum {
    GFX_WHITE = 0,
    GFX_BLACK = 1,
} gfx_color_t;

typedef enum {
    GFX_ALIGN_LEFT,
    GFX_ALIGN_CENTER,
    GFX_ALIGN_RIGHT,
} gfx_align_t;

// One glyph of a bitmap font. tools/fontgen makes these tables.
typedef struct {
    uint32_t code;     // Unicode code point
    uint32_t offset;   // first byte of the bitmap in gfx_font_t.bitmaps
    uint16_t adv16;    // advance width, 1/16 pixel
    uint8_t  w;        // bitmap width, pixels
    uint8_t  h;        // bitmap height, pixels
    int8_t   dx;       // bitmap left edge from the pen position
    int8_t   dy;       // bitmap top edge from the baseline (negative is above)
} gfx_glyph_t;

// A bitmap font. Each glyph bitmap has rows from top to bottom, 1 bit for each pixel,
// the most significant bit first, with no padding at the end of a row.
typedef struct {
    const uint8_t *bitmaps;
    const gfx_glyph_t *glyphs;  // sorted by code
    uint16_t count;
    uint8_t size;               // em size, pixels
    uint8_t ascent;             // pixels above the baseline
    uint8_t descent;            // pixels below the baseline
} gfx_font_t;

// Return the canvas: 50 bytes for each row, the most significant bit first, 1 = black.
const uint8_t *gfx_canvas(void);

// Copy a full image to the canvas. The image has the format of the canvas (see gfx_canvas()).
void gfx_copy(const uint8_t *image);

void gfx_clear(gfx_color_t color);
void gfx_pixel(int x, int y, gfx_color_t color);
void gfx_hline(int x, int y, int w, gfx_color_t color);
void gfx_vline(int x, int y, int h, gfx_color_t color);
void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);
void gfx_rect(int x, int y, int w, int h, int thickness, gfx_color_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, gfx_color_t color);
void gfx_round_rect(int x, int y, int w, int h, int r, int thickness, gfx_color_t color);
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, gfx_color_t color);

// Fill a polygon (even-odd rule). xy has n points: x0, y0, x1, y1, ... (16 points maximum).
void gfx_fill_polygon(const float *xy, int n, gfx_color_t color);

// Return the advance width of a UTF-8 text, pixels.
int gfx_text_width(const gfx_font_t *font, const char *text);

// Draw a UTF-8 text. x is the left edge, the center or the right edge (see align).
// baseline has the same meaning as in Text() of trmnl-server (DashboardRenderer):
// the font ascent below the top of the line. With Roboto the glyphs stand about 0.13 em above it.
// Thus the layout coordinates of the server screens are correct here without changes.
// Return the text width, pixels.
int gfx_text(const gfx_font_t *font, int x, int baseline, gfx_align_t align, gfx_color_t color, const char *text);

#ifdef __cplusplus
}
#endif
