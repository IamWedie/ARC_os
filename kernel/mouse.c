#include "mouse.h"
#include "kernel.h"
#include "gfx.h"

static struct mouse_state g_mouse;
static uint8_t g_pkt[3];
static int g_pkt_idx;
static int g_mouse_ready;

static void mouse_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(0x64) & 2) == 0) return;
    }
}

static void mouse_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 1) return;
    }
}

static void mouse_write_cmd(uint8_t cmd) {
    mouse_wait_write();
    outb(0x64, cmd);
}

static void mouse_write_data(uint8_t data) {
    mouse_wait_write();
    outb(0x60, data);
}

static uint8_t mouse_read_data(void) {
    mouse_wait_read();
    return inb(0x60);
}

static void mouse_send(uint8_t cmd) {
    mouse_write_cmd(0xD4);
    mouse_write_data(cmd);
}

int mouse_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int mouse_accel(int delta) {
    int sign = delta < 0 ? -1 : 1;
    int d = delta < 0 ? -delta : delta;
    if (d > 32) d = d * 3;
    else if (d > 16) d = d * 2;
    return d * sign;
}

int mouse_decode_packet(const uint8_t raw[3], struct mouse_packet* out) {
    uint8_t b0 = raw[0];
    if (!(b0 & 0x08)) return 0; /* always-1 bit */
    if (b0 & 0xC0) return 0;    /* overflow */
    int dx = (int)raw[1];
    int dy = (int)raw[2];
    if (b0 & 0x10) dx -= 256;
    if (b0 & 0x20) dy -= 256;
    out->dx = dx;
    out->dy = -dy; /* PS/2 Y up -> screen Y down */
    out->buttons = b0 & 0x07;
    return 1;
}

struct mouse_state mouse_get_state(void) {
    return g_mouse;
}

void mouse_irq_handler(void) {
    uint8_t b = inb(0x60);
    if (g_pkt_idx == 0 && !(b & 0x08)) {
        outb(0x20, 0x20);
        outb(0xA0, 0x20);
        lapic_eoi();
        return;
    }
    g_pkt[g_pkt_idx++] = b;
    if (g_pkt_idx == 3) {
        g_pkt_idx = 0;
        struct mouse_packet p;
        if (mouse_decode_packet(g_pkt, &p)) {
            int w = 1920, h = 1080;
            struct gfx_fb* fb = gfx_fb_get();
            if (fb) { w = (int)fb->width; h = (int)fb->height; }
            g_mouse.x = mouse_clamp(g_mouse.x + mouse_accel(p.dx), 0, w - 1);
            g_mouse.y = mouse_clamp(g_mouse.y + mouse_accel(p.dy), 0, h - 1);
            g_mouse.buttons = p.buttons;
        }
    }
    outb(0x20, 0x20);
    outb(0xA0, 0x20);
    lapic_eoi();
}

void mouse_init(void) {
    g_pkt_idx = 0;
    g_mouse.x = 960;
    g_mouse.y = 540;
    g_mouse.buttons = 0;

    mouse_write_cmd(0xA8); /* enable aux port */

    mouse_write_cmd(0x20);
    uint8_t cfg = mouse_read_data();
    cfg |= 0x02;  /* enable IRQ12 */
    cfg &= ~0x20; /* enable mouse clock */
    mouse_write_cmd(0x60);
    mouse_write_data(cfg);

    mouse_send(0xF6); /* set defaults */
    mouse_read_data(); /* ACK */

    mouse_send(0xF4); /* enable streaming */
    mouse_read_data(); /* ACK */

    g_mouse_ready = 1;
    terminal_putstring("PS/2 mouse initialized\n");
}
