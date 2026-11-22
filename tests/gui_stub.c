#include <stddef.h>
#include <stdint.h>

#include "gfx.h"

void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
uint8_t inb(uint16_t port) { (void)port; return 0; }
void terminal_putstring(const char* s) { (void)s; }
void terminal_print_int(uint64_t n) { (void)n; }
void terminal_print_hex(uint64_t n) { (void)n; }
void lapic_eoi(void) { }

struct gfx_fb* gfx_fb_get(void) { return NULL; }
int gfx_ready(void) { return 0; }
void gfx_mark_dirty(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }
uint32_t gfx_blend_premul(uint32_t src_premul, uint32_t dst) {
    uint32_t sa = (src_premul >> 24) & 0xFFu;
    if (sa == 255) return src_premul;
    if (sa == 0) return dst;
    uint32_t inv = 255 - sa;
    uint32_t da = (dst >> 24) & 0xFFu;
    uint32_t dr = (dst >> 16) & 0xFFu;
    uint32_t dg = (dst >> 8) & 0xFFu;
    uint32_t db = dst & 0xFFu;
    uint32_t sr = (src_premul >> 16) & 0xFFu;
    uint32_t sg = (src_premul >> 8) & 0xFFu;
    uint32_t sb = src_premul & 0xFFu;
    uint32_t oa = sa + ((da * inv + 127u) / 255u);
    uint32_t or_ = sr + ((dr * inv + 127u) / 255u);
    uint32_t og = sg + ((dg * inv + 127u) / 255u);
    uint32_t ob = sb + ((db * inv + 127u) / 255u);
    if (oa > 255) oa = 255;
    if (or_ > 255) or_ = 255;
    if (og > 255) og = 255;
    if (ob > 255) ob = 255;
    return (oa << 24) | (or_ << 16) | (og << 8) | ob;
}
void* pmm_alloc_pages(unsigned long long count) { (void)count; return (void*)0; }
void pmm_free_pages(void* addr, unsigned long long count) { (void)addr; (void)count; }
struct gfx_fb* gfx_fb_get(void);
unsigned int gfx_rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { return ((unsigned)a<<24)|((unsigned)r<<16)|((unsigned)g<<8)|b; }
void gfx_present(const struct gfx_rect* rect) { (void)rect; }
void gfx_clear(unsigned int color) { (void)color; }
void gfx_fill_rect(int x,int y,int w,int h,unsigned int color){(void)x;(void)y;(void)w;(void)h;(void)color;}
void gfx_fill_rect_alpha(int x,int y,int w,int h,unsigned int c){(void)x;(void)y;(void)w;(void)h;(void)c;}
struct gfx_font;
const struct gfx_font* gfx_font_get(unsigned char size){(void)size; return (const struct gfx_font*)0;}
int gfx_draw_text(const struct gfx_font* f,int x,int y,const char* t,unsigned int c){(void)f;(void)x;(void)y;(void)t;(void)c; return 0;}
int gfx_draw_char(const struct gfx_font* f,int x,int y,char c,unsigned int color){(void)f;(void)x;(void)y;(void)c;(void)color; return 0;}
