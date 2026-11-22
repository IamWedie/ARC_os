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
uint32_t gfx_blend_premul(uint32_t src, uint32_t dst) { (void)src; return dst; }
