#ifndef ARC_COMPOSITOR_H
#define ARC_COMPOSITOR_H

#include <stdint.h>
#include "gfx.h"

#define COMP_MAX_WINDOWS 16

struct comp_window {
    uint32_t id;
    int x, y;
    int w, h;
    uint32_t* surface; /* ARGB8888 premultiplied, row stride = w */
    uint32_t flags;
    uint8_t used;
};

#define COMP_WIN_VISIBLE 1u

struct compositor {
    struct comp_window windows[COMP_MAX_WINDOWS];
    int zorder[COMP_MAX_WINDOWS]; /* window indices, bottom-to-top */
    int zcount;
    uint32_t next_id;
};

void comp_init(struct compositor* c);
struct comp_window* comp_create_window(struct compositor* c, int x, int y, int w, int h, uint32_t* surface);
int comp_destroy_window(struct compositor* c, uint32_t id);
struct comp_window* comp_find(struct compositor* c, uint32_t id);
int comp_bring_to_front(struct compositor* c, uint32_t id);
int comp_window_index(const struct compositor* c, uint32_t id);
int comp_hit_test(const struct compositor* c, int px, int py);
/* Returns window id or 0 if none. */
uint32_t comp_hit_test_id(const struct compositor* c, int px, int py);

void comp_damage_window(struct compositor* c, uint32_t id, struct gfx_rect* damage);
void comp_composite(const struct compositor* c, struct gfx_fb* fb, const struct gfx_rect* damage);

#endif
