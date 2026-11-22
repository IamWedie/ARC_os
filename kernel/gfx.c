#include "gfx.h"
#include "kernel.h"
#include "mboot.h"
#include "font_inter.h"

#define DISPI_IOPORT_INDEX 0x1CE
#define DISPI_IOPORT_DATA  0x1CF
#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9
#define VBE_DISPI_DISABLED    0x00
#define VBE_DISPI_ENABLED     0x01
#define VBE_DISPI_LFB_ENABLED 0x40
#define VBE_DISPI_NOCLEARMEM  0x80

#define GFX_MAX_DIRTY 64
#define GFX_DIRTY_FULL ((uint32_t)0xFFFFFFFFu)

static struct gfx_fb g_fb;
static int g_gfx_ready;
static struct gfx_rect g_dirty[GFX_MAX_DIRTY];
static int g_dirty_n;
static int g_dirty_full;
static uint32_t* g_front; /* mapped LFB */
static volatile uint16_t* g_dispi_mmio; /* BAR2+0x500 when using bochs-display MMIO */

static inline uint16_t inw_port(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw_port(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint32_t inl_port(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl_port(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port));
}

static uint16_t dispi_read(uint16_t idx) {
    if (g_dispi_mmio) return g_dispi_mmio[idx];
    outw_port(DISPI_IOPORT_INDEX, idx);
    return inw_port(DISPI_IOPORT_DATA);
}
static void dispi_write(uint16_t idx, uint16_t val) {
    if (g_dispi_mmio) { g_dispi_mmio[idx] = val; return; }
    outw_port(DISPI_IOPORT_INDEX, idx);
    outw_port(DISPI_IOPORT_DATA, val);
}

static uint32_t pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (off & 0xFCu);
    outl_port(0xCF8, addr);
    return inl_port(0xCFC);
}

static int dispi_try_set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    dispi_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    dispi_write(VBE_DISPI_INDEX_XRES, (uint16_t)w);
    dispi_write(VBE_DISPI_INDEX_YRES, (uint16_t)h);
    dispi_write(VBE_DISPI_INDEX_BPP, (uint16_t)bpp);
    dispi_write(VBE_DISPI_INDEX_ENABLE,
                VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    if (dispi_read(VBE_DISPI_INDEX_XRES) != (uint16_t)w) return 0;
    if (dispi_read(VBE_DISPI_INDEX_YRES) != (uint16_t)h) return 0;
    if (dispi_read(VBE_DISPI_INDEX_BPP) != (uint16_t)bpp) return 0;
    return 1;
}

static int find_bochs_display(uint64_t* lfb_out, uint64_t* mmio_out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_cfg_read((uint8_t)bus, dev, 0, 0);
            if ((uint16_t)id == 0xFFFF || (uint16_t)id == 0x0000) continue;
            if ((uint16_t)id != 0x1234) continue;
            uint32_t bar0 = pci_cfg_read((uint8_t)bus, dev, 0, 0x10);
            uint32_t bar2 = pci_cfg_read((uint8_t)bus, dev, 0, 0x18);
            uint64_t lfb = (uint64_t)(bar0 & 0xFFFFFFF0u);
            uint64_t mmio = (uint64_t)(bar2 & 0xFFFFFFF0u);
            /* Accept QEMU VGA (1111) or bochs-display (1111) — both vendor 1234. */
            if (lfb) { *lfb_out = lfb; *mmio_out = mmio; return 1; }
        }
    }
    return 0;
}

int gfx_ready(void) { return g_gfx_ready; }
struct gfx_fb* gfx_fb_get(void) { return g_gfx_ready ? &g_fb : NULL; }

uint32_t gfx_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void gfx_mark_dirty(int x, int y, int w, int h) {
    if (!g_gfx_ready) return;
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;
    x = clampi(x, 0, (int)g_fb.width);
    y = clampi(y, 0, (int)g_fb.height);
    x2 = clampi(x2, 0, (int)g_fb.width);
    y2 = clampi(y2, 0, (int)g_fb.height);
    w = x2 - x; h = y2 - y;
    if (w <= 0 || h <= 0) return;
    if (g_dirty_full) return;
    /* Merge into overlapping/adjacent rects, coalescing as needed. */
    for (int i = 0; i < g_dirty_n; i++) {
        struct gfx_rect* r = &g_dirty[i];
        int rx2 = r->x + r->w, ry2 = r->y + r->h;
        if (x <= rx2 && x2 >= r->x && y <= ry2 && y2 >= r->y) {
            int nx = r->x < x ? r->x : x;
            int ny = r->y < y ? r->y : y;
            int nx2 = rx2 > x2 ? rx2 : x2;
            int ny2 = ry2 > y2 ? ry2 : y2;
            r->x = nx; r->y = ny; r->w = nx2 - nx; r->h = ny2 - ny;
            /* Re-merge this rect against others. */
            x = r->x; y = r->y; w = r->w; h = r->h;
            g_dirty[i] = g_dirty[--g_dirty_n];
            i = -1;
            continue;
        }
    }
    if (g_dirty_n < GFX_MAX_DIRTY) {
        g_dirty[g_dirty_n++] = (struct gfx_rect){x, y, w, h};
    } else {
        g_dirty_full = 1;
    }
}

int gfx_dirty_count(void) { return g_dirty_full ? -1 : g_dirty_n; }
const struct gfx_rect* gfx_dirty_rects(int* count, int* full) {
    if (count) *count = g_dirty_n;
    if (full) *full = g_dirty_full;
    return g_dirty;
}
void gfx_dirty_reset(void) { g_dirty_n = 0; g_dirty_full = 0; }

void gfx_test_set_fb(uint32_t* pixels, uint32_t w, uint32_t h) {
    g_fb.width = w;
    g_fb.height = h;
    g_fb.pitch = w * 4u;
    g_fb.bpp = 32;
    g_fb.pixels = pixels;
    g_front = NULL;
    g_dirty_n = 0;
    g_dirty_full = 0;
    g_gfx_ready = pixels && w && h;
}

void gfx_clear(uint32_t color) {
    if (!g_gfx_ready) return;
    uint32_t n = g_fb.width * g_fb.height;
    for (uint32_t i = 0; i < n; i++) g_fb.pixels[i] = color;
    gfx_mark_dirty(0, 0, (int)g_fb.width, (int)g_fb.height);
}

void gfx_putpixel(int x, int y, uint32_t color) {
    if (!g_gfx_ready) return;
    if (x < 0 || y < 0 || (uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;
    g_fb.pixels[(uint32_t)y * g_fb.width + (uint32_t)x] = color;
    gfx_mark_dirty(x, y, 1, 1);
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_gfx_ready || w <= 0 || h <= 0) return;
    int x2 = clampi(x + w, 0, (int)g_fb.width);
    int y2 = clampi(y + h, 0, (int)g_fb.height);
    x = clampi(x, 0, (int)g_fb.width);
    y = clampi(y, 0, (int)g_fb.height);
    if (x2 <= x || y2 <= y) return;
    for (int row = y; row < y2; row++) {
        uint32_t* p = g_fb.pixels + (uint32_t)row * g_fb.width + (uint32_t)x;
        for (int col = x; col < x2; col++) *p++ = color;
    }
    gfx_mark_dirty(x, y, x2 - x, y2 - y);
}

/* Premultiplied-alpha source-over: dst = src + dst*(1-sa) */
uint32_t gfx_blend_premul(uint32_t src, uint32_t dst) {
    uint32_t sa = (src >> 24) & 0xFFu;
    if (sa == 255) return src;
    if (sa == 0) return dst;
    uint32_t inv = 255u - sa;
    uint32_t r = ((src >> 16) & 0xFFu) + ((((dst >> 16) & 0xFFu) * inv + 127u) / 255u);
    uint32_t g = ((src >> 8) & 0xFFu) + ((((dst >> 8) & 0xFFu) * inv + 127u) / 255u);
    uint32_t b = (src & 0xFFu) + (((dst & 0xFFu) * inv + 127u) / 255u);
    uint32_t a = sa + ((((dst >> 24) & 0xFFu) * inv + 127u) / 255u);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void gfx_blend_pixel(int x, int y, uint32_t src_premul) {
    if (!g_gfx_ready) return;
    if (x < 0 || y < 0 || (uint32_t)x >= g_fb.width || (uint32_t)y >= g_fb.height) return;
    uint32_t* p = &g_fb.pixels[(uint32_t)y * g_fb.width + (uint32_t)x];
    *p = gfx_blend_premul(src_premul, *p);
    gfx_mark_dirty(x, y, 1, 1);
}

void gfx_fill_rect_alpha(int x, int y, int w, int h, uint32_t argb_premul) {
    if (!g_gfx_ready || w <= 0 || h <= 0) return;
    uint32_t sa = (argb_premul >> 24) & 0xFFu;
    if (sa == 0) return;
    if (sa == 255) { gfx_fill_rect(x, y, w, h, argb_premul); return; }
    int x2 = clampi(x + w, 0, (int)g_fb.width);
    int y2 = clampi(y + h, 0, (int)g_fb.height);
    x = clampi(x, 0, (int)g_fb.width);
    y = clampi(y, 0, (int)g_fb.height);
    if (x2 <= x || y2 <= y) return;
    for (int row = y; row < y2; row++) {
        uint32_t* p = g_fb.pixels + (uint32_t)row * g_fb.width + (uint32_t)x;
        for (int col = x; col < x2; col++, p++) *p = gfx_blend_premul(argb_premul, *p);
    }
    gfx_mark_dirty(x, y, x2 - x, y2 - y);
}

void gfx_present(const struct gfx_rect* rect) {
    if (!g_gfx_ready || !g_front) return;
    int x = 0, y = 0, w = (int)g_fb.width, h = (int)g_fb.height;
    if (rect) { x = rect->x; y = rect->y; w = rect->w; h = rect->h; }
    int x2 = clampi(x + w, 0, (int)g_fb.width);
    int y2 = clampi(y + h, 0, (int)g_fb.height);
    x = clampi(x, 0, (int)g_fb.width);
    y = clampi(y, 0, (int)g_fb.height);
    for (int row = y; row < y2; row++) {
        uint32_t* src = g_fb.pixels + (uint32_t)row * g_fb.width + (uint32_t)x;
        volatile uint32_t* dst = g_front + (uint32_t)row * g_fb.width + (uint32_t)x;
        for (int col = x; col < x2; col++) *dst++ = *src++;
    }
}

void gfx_present_dirty(void) {
    if (!g_gfx_ready) return;
    if (g_dirty_full) {
        gfx_present(NULL);
        gfx_dirty_reset();
        return;
    }
    for (int i = 0; i < g_dirty_n; i++) gfx_present(&g_dirty[i]);
    gfx_dirty_reset();
}

int gui_is_active(void);
void gui_frame_tick(void);

int gfx_frame_pending(void) { return 0; }
void gfx_frame_ack(void) { }

void gfx_tick_100hz(void) {
    static uint32_t acc;
    static uint32_t frame_div;
    if (!g_gfx_ready) return;
    acc += 60;
    if (acc < 100) return;
    acc -= 100;
    if (gui_is_active()) {
        if (++frame_div >= 6) { /* ~10 fps to avoid starving shell in QEMU */
            frame_div = 0;
            gui_frame_tick();
        }
        return;
    }
    if (g_dirty_full || g_dirty_n > 0)
        gfx_present_dirty();
}

const struct gfx_font* gfx_font_get(uint8_t size) {
    const struct gfx_font* best = gfx_inter_fonts[0];
    uint32_t bestd = (uint32_t)(best->size > size ? best->size - size : size - best->size);
    for (int i = 1; i < GFX_INTER_FONT_COUNT; i++) {
        const struct gfx_font* f = gfx_inter_fonts[i];
        uint32_t d = (uint32_t)(f->size > size ? f->size - size : size - f->size);
        if (d < bestd) { best = f; bestd = d; }
    }
    return best;
}

int gfx_text_width(const struct gfx_font* font, const char* text) {
    int w = 0;
    if (!font || !text) return 0;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c < font->first || c > font->last) c = '?';
        const struct gfx_glyph* g = &font->glyphs[c - font->first];
        w += g->advance;
    }
    return w;
}

int gfx_draw_char(const struct gfx_font* font, int x, int y, char c, uint32_t argb) {
    if (!g_gfx_ready || !font) return 0;
    unsigned char uc = (unsigned char)c;
    if (uc < font->first || uc > font->last) uc = '?';
    const struct gfx_glyph* g = &font->glyphs[uc - font->first];
    uint32_t ca = (argb >> 24) & 0xFFu;
    uint32_t cr = (argb >> 16) & 0xFFu;
    uint32_t cg = (argb >> 8) & 0xFFu;
    uint32_t cb = argb & 0xFFu;
    int gx0 = x + g->xoff;
    int gy0 = y + (int)font->ascent + (int)g->yoff;
    for (int row = 0; row < g->h; row++) {
        int py = gy0 + row;
        if (py < 0 || (uint32_t)py >= g_fb.height) continue;
        uint32_t* dst = g_fb.pixels + (uint32_t)py * g_fb.width;
        const uint8_t* src = font->data + g->data_off + (uint32_t)row * g->w;
        for (int col = 0; col < g->w; col++) {
            int px = gx0 + col;
            if (px < 0 || (uint32_t)px >= g_fb.width) continue;
            uint32_t cov = src[col];
            if (!cov) continue;
            uint32_t a = (ca * cov + 127u) / 255u;
            if (!a) continue;
            /* premultiplied source */
            uint32_t pr = (cr * a + 127u) / 255u;
            uint32_t pg = (cg * a + 127u) / 255u;
            uint32_t pb = (cb * a + 127u) / 255u;
            uint32_t srcp = (a << 24) | (pr << 16) | (pg << 8) | pb;
            dst[px] = gfx_blend_premul(srcp, dst[px]);
        }
    }
    if (g->w > 0 && g->h > 0)
        gfx_mark_dirty(gx0, gy0, g->w, g->h);
    return g->advance;
}

int gfx_draw_text(const struct gfx_font* font, int x, int y, const char* text, uint32_t argb) {
    int cx = x;
    if (!font || !text) return 0;
    while (*text) {
        cx += gfx_draw_char(font, cx, y, *text, argb);
        text++;
    }
    return cx - x;
}

int gfx_init(void) {
    if (g_gfx_ready) return 0;
    uint64_t lfb_pa = 0, mmio_pa = 0;
    find_bochs_display(&lfb_pa, &mmio_pa);

    uint16_t id = dispi_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0C6) {
        /* Legacy I/O ports not responding (e.g. -device bochs-display).
         * Try MMIO BAR2: dispi regs at offset 0x500, 16-bit indexed. */
        if (mmio_pa) {
            for (uint64_t off = 0; off < 0x1000; off += 4096)
                vmm_map_page(mmio_pa + off, mmio_pa + off, 0);
            g_dispi_mmio = (volatile uint16_t*)(uintptr_t)(mmio_pa + 0x500u);
            id = dispi_read(VBE_DISPI_INDEX_ID);
        }
    }
    if (id < 0xB0C0 || id > 0xB0C6) {
        terminal_putstring("GFX: Bochs DISPI not found\n");
        return -1;
    }

    uint16_t max_x = dispi_read(7);
    uint16_t max_y = dispi_read(8);
    terminal_putstring("GFX: DISPI max ");
    terminal_print_int(max_x);
    terminal_putstring("x");
    terminal_print_int(max_y);
    terminal_putstring("\n");

    static const uint32_t modes[][2] = {{1920,1080},{1280,720},{1024,768}};
    uint32_t mw = 0, mh = 0;
    for (int i = 0; i < 3; i++) {
        if (dispi_try_set_mode(modes[i][0], modes[i][1], 32)) {
            mw = modes[i][0]; mh = modes[i][1];
            break;
        }
    }
    if (!mw) {
        terminal_putstring("GFX: no supported mode\n");
        return -1;
    }

    if (!lfb_pa) {
        terminal_putstring("GFX: no Bochs PCI LFB\n");
        return -1;
    }

    uint64_t fb_bytes = (uint64_t)mw * mh * 4u;
    uint64_t fb_pages = (fb_bytes + 4095u) / 4096u;
    /* Reserve LFB range so PMM never hands it out. */
    pmm_reserve_range(lfb_pa, fb_pages * 4096u);
    for (uint64_t off = 0; off < fb_pages * 4096u; off += 4096u)
        vmm_map_page(lfb_pa + off, lfb_pa + off, 0);

    /* Back buffer in system RAM. */
    uint32_t* back = (uint32_t*)pmm_alloc_pages((size_t)fb_pages);
    if (!back) {
        terminal_putstring("GFX: back buffer alloc failed\n");
        return -1;
    }
    /* pmm_alloc_pages returns physical identity-mapped address. */
    for (uint64_t i = 0; i < fb_pages * 1024u; i++) back[i] = 0;

    g_fb.width = mw;
    g_fb.height = mh;
    g_fb.pitch = mw * 4u;
    g_fb.bpp = 32;
    g_fb.pixels = back;
    g_front = (uint32_t*)(uintptr_t)lfb_pa;
    g_dirty_n = 0;
    g_dirty_full = 0;
    g_gfx_ready = 1;

    terminal_putstring("GFX: ");
    terminal_print_int(mw);
    terminal_putstring("x");
    terminal_print_int(mh);
    terminal_putstring("x32 LFB=");
    terminal_print_hex(lfb_pa);
    terminal_putstring("\n");
    return 0;
}
