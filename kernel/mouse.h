#ifndef ARC_MOUSE_H
#define ARC_MOUSE_H

#include <stdint.h>

#define MOUSE_BTN_LEFT  1u
#define MOUSE_BTN_RIGHT 2u
#define MOUSE_BTN_MIDDLE 4u

struct mouse_state {
    int x;
    int y;
    uint8_t buttons;
};

struct mouse_packet {
    int dx;
    int dy;
    uint8_t buttons;
};

void mouse_init(void);
void mouse_irq_handler(void);
struct mouse_state mouse_get_state(void);
int mouse_decode_packet(const uint8_t raw[3], struct mouse_packet* out);
int mouse_clamp(int v, int lo, int hi);
int mouse_accel(int delta);

#endif
