#ifndef ARC_GFX_H
#define ARC_GFX_H

#include <stdint.h>
#include <stddef.h>

struct gfx_fb {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;   /* bytes per row */
    uint32_t bpp;     /* 32 */
    uint32_t* pixels; /* back buffer (RAM), ARGB8888 premultiplied-friendly */
};

struct gfx_rect {
    int x, y, w, h;
};

int gfx_init(void);
struct gfx_fb* gfx_fb_get(void);
int gfx_ready(void);

uint32_t gfx_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t gfx_blend_premul(uint32_t src_premul, uint32_t dst);

void gfx_clear(uint32_t color);
void gfx_putpixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_fill_rect_alpha(int x, int y, int w, int h, uint32_t argb_premul);
void gfx_blend_pixel(int x, int y, uint32_t src_premul);
void gfx_present(const struct gfx_rect* rect); /* NULL = full screen */
void gfx_present_dirty(void);
void gfx_mark_dirty(int x, int y, int w, int h);
int  gfx_dirty_count(void);
const struct gfx_rect* gfx_dirty_rects(int* count, int* full);
void gfx_dirty_reset(void);
void gfx_tick_100hz(void);

/* Font rendering (M1, 8-bit coverage alpha blend) */
struct gfx_glyph {
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t advance;
    uint16_t data_off;
};

struct gfx_font {
    uint8_t size;
    uint8_t first;
    uint8_t last;
    uint8_t ascent;
    uint8_t line_height;
    const struct gfx_glyph* glyphs;
    const uint8_t* data;
};

const struct gfx_font* gfx_font_get(uint8_t size);
int gfx_draw_char(const struct gfx_font* font, int x, int y, char c, uint32_t argb);
int gfx_draw_text(const struct gfx_font* font, int x, int y, const char* text, uint32_t argb);
int gfx_text_width(const struct gfx_font* font, const char* text);

/* Host-test helpers */
void gfx_test_set_fb(uint32_t* pixels, uint32_t w, uint32_t h);

#endif
