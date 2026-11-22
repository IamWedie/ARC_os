#include "compositor.h"

void comp_init(struct compositor* c) {
    if (!c) return;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        c->windows[i].used = 0;
        c->windows[i].id = 0;
        c->windows[i].surface = 0;
        c->zorder[i] = -1;
    }
    c->zcount = 0;
    c->next_id = 1;
}

struct comp_window* comp_find(struct compositor* c, uint32_t id) {
    if (!c || !id) return 0;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (c->windows[i].used && c->windows[i].id == id)
            return &c->windows[i];
    }
    return 0;
}

int comp_window_index(const struct compositor* c, uint32_t id) {
    if (!c || !id) return -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (c->windows[i].used && c->windows[i].id == id)
            return i;
    }
    return -1;
}

static void zorder_remove_index(struct compositor* c, int idx) {
    int pos = -1;
    for (int i = 0; i < c->zcount; i++) {
        if (c->zorder[i] == idx) { pos = i; break; }
    }
    if (pos < 0) return;
    for (int i = pos; i + 1 < c->zcount; i++)
        c->zorder[i] = c->zorder[i + 1];
    c->zcount--;
    c->zorder[c->zcount] = -1;
}

struct comp_window* comp_create_window(struct compositor* c, int x, int y, int w, int h, uint32_t* surface) {
    if (!c || w <= 0 || h <= 0) return 0;
    int slot = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!c->windows[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;
    struct comp_window* win = &c->windows[slot];
    win->used = 1;
    win->id = c->next_id++;
    if (c->next_id == 0) c->next_id = 1;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->surface = surface;
    win->flags = COMP_WIN_VISIBLE;
    win->alpha = 255;
    c->zorder[c->zcount++] = slot;
    return win;
}

int comp_destroy_window(struct compositor* c, uint32_t id) {
    int idx = comp_window_index(c, id);
    if (idx < 0) return -1;
    zorder_remove_index(c, idx);
    c->windows[idx].used = 0;
    c->windows[idx].id = 0;
    c->windows[idx].surface = 0;
    c->windows[idx].flags = 0;
    return 0;
}

int comp_bring_to_front(struct compositor* c, uint32_t id) {
    int idx = comp_window_index(c, id);
    if (idx < 0) return -1;
    zorder_remove_index(c, idx);
    c->zorder[c->zcount++] = idx;
    return 0;
}

uint32_t comp_hit_test_id(const struct compositor* c, int px, int py) {
    if (!c) return 0;
    for (int i = c->zcount - 1; i >= 0; i--) {
        int idx = c->zorder[i];
        const struct comp_window* win = &c->windows[idx];
        if (!win->used || !(win->flags & COMP_WIN_VISIBLE)) continue;
        if (px >= win->x && px < win->x + win->w &&
            py >= win->y && py < win->y + win->h) {
            return win->id;
        }
    }
    return 0;
}

int comp_hit_test(const struct compositor* c, int px, int py) {
    uint32_t id = comp_hit_test_id(c, px, py);
    if (!id) return -1;
    return comp_window_index(c, id);
}

static int rect_intersect(const struct gfx_rect* a, const struct gfx_rect* b, struct gfx_rect* out) {
    int ax2 = a->x + a->w, ay2 = a->y + a->h;
    int bx2 = b->x + b->w, by2 = b->y + b->h;
    int x = a->x > b->x ? a->x : b->x;
    int y = a->y > b->y ? a->y : b->y;
    int x2 = ax2 < bx2 ? ax2 : bx2;
    int y2 = ay2 < by2 ? ay2 : by2;
    if (x2 <= x || y2 <= y) return 0;
    out->x = x; out->y = y; out->w = x2 - x; out->h = y2 - y;
    return 1;
}

void comp_damage_window(struct compositor* c, uint32_t id, struct gfx_rect* damage) {
    (void)c;
    struct comp_window* win = comp_find(c, id);
    if (!win || !damage) return;
    damage->x = win->x;
    damage->y = win->y;
    damage->w = win->w;
    damage->h = win->h;
}

void comp_composite(const struct compositor* c, struct gfx_fb* fb, const struct gfx_rect* damage) {
    if (!c || !fb || !fb->pixels || !damage) return;
    struct gfx_rect fb_rect = {0, 0, (int)fb->width, (int)fb->height};
    struct gfx_rect clip;
    if (!rect_intersect(&fb_rect, damage, &clip)) return;

    for (int zi = 0; zi < c->zcount; zi++) {
        int idx = c->zorder[zi];
        const struct comp_window* win = &c->windows[idx];
        if (!win->used || !(win->flags & COMP_WIN_VISIBLE) || !win->surface) continue;
        struct gfx_rect wrect = {win->x, win->y, win->w, win->h};
        struct gfx_rect isect;
        if (!rect_intersect(&clip, &wrect, &isect)) continue;
        uint32_t wa = win->alpha;
        for (int y = isect.y; y < isect.y + isect.h; y++) {
            int sy = y - win->y;
            int sx0 = isect.x - win->x;
            const uint32_t* src_row = win->surface + sy * win->w + sx0;
            uint32_t* dst_row = fb->pixels + y * (int)fb->width + isect.x;
            for (int x = 0; x < isect.w; x++) {
                uint32_t s = src_row[x];
                uint32_t sa = (s >> 24) & 0xFFu;
                if (wa != 255) {
                    if (sa == 0) continue;
                    sa = (sa * wa + 127u) / 255u;
                    uint32_t sr = (((s >> 16) & 0xFFu) * wa + 127u) / 255u;
                    uint32_t sg = (((s >> 8) & 0xFFu) * wa + 127u) / 255u;
                    uint32_t sb = ((s & 0xFFu) * wa + 127u) / 255u;
                    s = (sa << 24) | (sr << 16) | (sg << 8) | sb;
                }
                if (sa == 255) dst_row[x] = s;
                else if (sa != 0) dst_row[x] = gfx_blend_premul(s, dst_row[x]);
            }
        }
    }
}
