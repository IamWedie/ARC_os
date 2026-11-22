#include "gui.h"
#include "kernel.h"

int gui_ease_out(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    /* quadratic ease-out: 1 - (1-t)^2, fixed point 0..256 */
    int inv = 256 - t;
    return 256 - (inv * inv + 127) / 256;
}

int gui_spring(int t) {
    if (t <= 0) return 0;
    if (t >= 256) return 256;
    /* simple damped overshoot approximation */
    int e = gui_ease_out(t);
    int over = (t < 200) ? ((256 - t) * (256 - e)) / 512 : 0;
    int v = e + over / 4;
    if (v > 280) v = 280;
    return v;
}

#define GUI_CORNER_R 10
static uint8_t g_corner_mask[GUI_CORNER_R + 1][GUI_CORNER_R + 1];
static int g_corner_mask_ready = 0;

static void gui_corner_mask_init(void) {
    if (g_corner_mask_ready) return;
    for (int dy = 0; dy <= GUI_CORNER_R; dy++) {
        for (int dx = 0; dx <= GUI_CORNER_R; dx++) {
            /* distance from corner circle center at (R,R) for pixel inside corner */
            int ddx = GUI_CORNER_R - dx;
            int ddy = GUI_CORNER_R - dy;
            int d2 = ddx * ddx + ddy * ddy;
            int r2 = GUI_CORNER_R * GUI_CORNER_R;
            if (d2 <= (GUI_CORNER_R - 1) * (GUI_CORNER_R - 1)) g_corner_mask[dy][dx] = 255;
            else if (d2 > (GUI_CORNER_R + 1) * (GUI_CORNER_R + 1)) g_corner_mask[dy][dx] = 0;
            else {
                /* simple edge AA */
                int d = 0;
                /* integer sqrt approx */
                for (int s = 0; s <= GUI_CORNER_R + 1; s++) { if (s*s <= d2) d = s; else break; }
                int cov = GUI_CORNER_R - d;
                if (cov < 0) cov = 0;
                if (cov > 1) cov = 1;
                g_corner_mask[dy][dx] = cov ? 128 : 0;
                (void)r2;
            }
        }
    }
    g_corner_mask_ready = 1;
}

static uint8_t gui_corner_alpha(int x, int y, int w, int h) {
    int r = GUI_CORNER_R;
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    int dx = x < r ? x : (x >= w - r ? (w - 1 - x) : r);
    int dy = y < r ? y : (y >= h - r ? (h - 1 - y) : r);
    if (dx >= r || dy >= r) return 255;
    if (!g_corner_mask_ready) gui_corner_mask_init();
    return g_corner_mask[dy][dx];
}

#define GUI_SHADOW_S 18
static uint8_t g_shadow_corner[GUI_SHADOW_S + 1][GUI_SHADOW_S + 1];
static uint8_t g_shadow_edge[GUI_SHADOW_S + 1];
static int g_shadow_ready = 0;

static void gui_shadow_init(void) {
    if (g_shadow_ready) return;
    for (int i = 0; i <= GUI_SHADOW_S; i++) {
        /* edge falloff: 0 at outer edge, stronger near window */
        int a = (i * 40) / GUI_SHADOW_S;
        if (a > 40) a = 40;
        g_shadow_edge[i] = (uint8_t)a;
    }
    for (int dy = 0; dy <= GUI_SHADOW_S; dy++) {
        for (int dx = 0; dx <= GUI_SHADOW_S; dx++) {
            /* distance outside rounded corner; inside corner area uses edge */
            int ddx = GUI_SHADOW_S - dx;
            int ddy = GUI_SHADOW_S - dy;
            int d2 = ddx * ddx + ddy * ddy;
            int r = GUI_SHADOW_S;
            int a = 0;
            if (d2 <= r * r) {
                /* approx distance from corner arc */
                int d = 0;
                while ((d + 1) * (d + 1) <= d2) d++;
                int dist = r - d; /* 0..r, larger means farther inside */
                a = (dist * 40) / r;
                if (a < 0) a = 0;
                if (a > 40) a = 40;
            } else {
                a = 0;
            }
            g_shadow_corner[dy][dx] = (uint8_t)a;
        }
    }
    g_shadow_ready = 1;
}

static void draw_window_shadow(struct gfx_fb* fb, int wx, int wy, int ww, int wh) {
    if (!fb || !fb->pixels) return;
    gui_shadow_init();
    int S = GUI_SHADOW_S;
    int ox = 0, oy = 6; /* shadow offset */
    int sx = wx - S + ox;
    int sy = wy - S + oy;
    int sw = ww + S * 2;
    int sh = wh + S * 2;
    int fbw = (int)fb->width, fbh = (int)fb->height;
    for (int y = 0; y < sh; y++) {
        int py = sy + y;
        if (py < 0 || py >= fbh) continue;
        uint32_t* row = fb->pixels + py * fbw;
        for (int x = 0; x < sw; x++) {
            int px = sx + x;
            if (px < 0 || px >= fbw) continue;
            /* determine 9-slice region */
            int in_left = x < S;
            int in_right = x >= sw - S;
            int in_top = y < S;
            int in_bot = y >= sh - S;
            uint8_t a = 0;
            if (in_top && in_left) a = g_shadow_corner[y][x];
            else if (in_top && in_right) a = g_shadow_corner[y][sw - 1 - x];
            else if (in_bot && in_left) a = g_shadow_corner[sh - 1 - y][x];
            else if (in_bot && in_right) a = g_shadow_corner[sh - 1 - y][sw - 1 - x];
            else if (in_top) a = g_shadow_edge[S - 1 - y];
            else if (in_bot) a = g_shadow_edge[S - 1 - (sh - 1 - y)];
            else if (in_left) a = g_shadow_edge[S - 1 - x];
            else if (in_right) a = g_shadow_edge[S - 1 - (sw - 1 - x)];
            else a = 40; /* center */
            if (!a) continue;
            /* skip area under window (opaque window will cover) - keep simple */
            uint32_t bg = row[px];
            uint32_t sp = ((uint32_t)a << 24); /* premul black shadow */
            row[px] = gfx_blend_premul(sp, bg);
        }
    }
}

int gui_hit_test_titlebar(const struct comp_window* win, int px, int py) {
    if (!win) return 0;
    return px >= win->x && px < win->x + win->w &&
           py >= win->y && py < win->y + GUI_TITLE_H;
}

int gui_hit_test_close(const struct comp_window* win, int px, int py) {
    if (!win) return 0;
    int bx = win->x + 12;
    int by = win->y + 8;
    return px >= bx && px < bx + 12 && py >= by && py < by + 12;
}

static void draw_wallpaper(struct gfx_fb* fb, uint32_t* cache) {
    if (!fb || !fb->pixels) return;
    if (cache) {
        for (uint32_t i = 0; i < fb->width * fb->height; i++)
            fb->pixels[i] = cache[i];
        return;
    }
    for (uint32_t y = 0; y < fb->height; y++) {
        uint8_t r = (uint8_t)(20 + (20 * y) / fb->height);
        uint8_t g = (uint8_t)(40 + (40 * y) / fb->height);
        uint8_t b = (uint8_t)(90 + (50 * y) / fb->height);
        uint32_t c = gfx_rgba(r, g, b, 255);
        uint32_t* row = fb->pixels + y * fb->width;
        for (uint32_t x = 0; x < fb->width; x++) row[x] = c;
    }
}

static void fill_rect_buf(uint32_t* buf, int bw, int bh, int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > bw) x1 = bw;
    int y1 = y + h; if (y1 > bh) y1 = bh;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t* row = buf + yy * bw + x0;
        for (int xx = x0; xx < x1; xx++) *row++ = color;
    }
}

static void draw_rounded_rect(uint32_t* buf, int bw, int bh, int x, int y, int w, int h, int r, uint32_t color) {
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= bh) continue;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if (px < 0 || px >= bw) continue;
            int in = 1;
            int cx = -1, cy = -1;
            if (xx < r && yy < r) { cx = r - 1 - xx; cy = r - 1 - yy; }
            else if (xx >= w - r && yy < r) { cx = xx - (w - r); cy = r - 1 - yy; }
            else if (xx < r && yy >= h - r) { cx = r - 1 - xx; cy = yy - (h - r); }
            else if (xx >= w - r && yy >= h - r) { cx = xx - (w - r); cy = yy - (h - r); }
            if (cx >= 0 && (cx * cx + cy * cy) > r * r) in = 0;
            if (in) buf[py * bw + px] = color;
        }
    }
}

static void draw_text_buf(uint32_t* buf, int bw, int bh, int x, int y, const char* text, uint32_t argb) {
    const struct gfx_font* font = gfx_font_get(14);
    if (!font || !text) return;
    int cx = x;
    while (*text) {
        unsigned char uc = (unsigned char)*text++;
        if (uc < font->first || uc > font->last) uc = '?';
        const struct gfx_glyph* g = &font->glyphs[uc - font->first];
        uint32_t ca=(argb>>24)&0xFFu, cr=(argb>>16)&0xFFu, cg=(argb>>8)&0xFFu, cb=argb&0xFFu;
        int gx0 = cx + g->xoff;
        int gy0 = y + (int)font->ascent + (int)g->yoff;
        for (int row = 0; row < g->h; row++) {
            int py = gy0 + row;
            if (py < 0 || py >= bh) continue;
            const uint8_t* src = font->data + g->data_off + (uint32_t)row * g->w;
            for (int col = 0; col < g->w; col++) {
                int px = gx0 + col;
                if (px < 0 || px >= bw) continue;
                uint32_t cov = src[col];
                if (!cov) continue;
                uint32_t a = (ca * cov + 127u) / 255u;
                uint32_t pr=(cr*a+127u)/255u, pg=(cg*a+127u)/255u, pb=(cb*a+127u)/255u;
                uint32_t sp = (a<<24)|(pr<<16)|(pg<<8)|pb;
                uint32_t* dst = &buf[py*bw+px];
                *dst = gfx_blend_premul(sp, *dst);
            }
        }
        cx += g->advance;
    }
}

static void draw_window_chrome(struct comp_window* win, const char* title, int focused) {
    if (!win || !win->surface) return;
    int w = win->w, h = win->h;
    uint32_t* s = win->surface;
    uint32_t bg = gfx_rgba(245, 245, 247, 255);
    uint32_t tbar = focused ? gfx_rgba(235, 235, 238, 255) : gfx_rgba(228, 228, 232, 255);
    uint32_t titlec = focused ? gfx_rgba(40, 40, 45, 255) : gfx_rgba(120, 120, 128, 255);
    uint32_t border = focused ? gfx_rgba(170, 170, 178, 255) : gfx_rgba(190, 190, 196, 255);
    uint32_t sep = gfx_rgba(210, 210, 215, 255);
    gui_corner_mask_init();
    for (int i = 0; i < w * h; i++) s[i] = 0; /* transparent */
    draw_rounded_rect(s, w, h, 0, 0, w, h, GUI_CORNER_R, bg);
    /* title bar */
    draw_rounded_rect(s, w, h, 0, 0, w, GUI_TITLE_H + GUI_CORNER_R, GUI_CORNER_R, tbar);
    fill_rect_buf(s, w, h, 0, GUI_CORNER_R, w, GUI_TITLE_H - GUI_CORNER_R, tbar);
    /* separator */
    fill_rect_buf(s, w, h, 1, GUI_TITLE_H - 1, w - 2, 1, sep);
    /* traffic lights */
    uint32_t red = focused ? gfx_rgba(255, 95, 87, 255) : gfx_rgba(200, 200, 205, 255);
    uint32_t yellow = focused ? gfx_rgba(255, 189, 46, 255) : gfx_rgba(200, 200, 205, 255);
    uint32_t green = focused ? gfx_rgba(39, 201, 63, 255) : gfx_rgba(200, 200, 205, 255);
    int cy = 14;
    int r = 6;
    for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
        if (dx*dx + dy*dy > r*r) continue;
        int yy = cy + dy;
        if (yy < 0 || yy >= h) continue;
        int x0 = 18 + dx; if (x0 >= 0 && x0 < w) s[yy*w + x0] = red;
        int x1 = 38 + dx; if (x1 >= 0 && x1 < w) s[yy*w + x1] = yellow;
        int x2 = 58 + dx; if (x2 >= 0 && x2 < w) s[yy*w + x2] = green;
    }
    /* rounded 1px border */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t ca = gui_corner_alpha(x, y, w, h);
            if (ca == 0) { s[y*w+x] = 0; continue; }
            int border_px = (x == 0 || y == 0 || x == w-1 || y == h-1);
            if (!border_px) {
                /* inner edge of rounded corner */
                uint8_t ca_r = (x+1 < w) ? gui_corner_alpha(x+1, y, w, h) : 0;
                uint8_t ca_l = (x>0) ? gui_corner_alpha(x-1, y, w, h) : 0;
                uint8_t ca_d = (y+1 < h) ? gui_corner_alpha(x, y+1, w, h) : 0;
                uint8_t ca_u = (y>0) ? gui_corner_alpha(x, y-1, w, h) : 0;
                if (ca_r == 0 || ca_l == 0 || ca_d == 0 || ca_u == 0) border_px = 1;
            }
            if (border_px) s[y*w+x] = border;
        }
    }
    /* title text */
    draw_text_buf(s, w, h, 80, 6, title, titlec);
}
static void draw_menu_bar(struct gfx_fb* fb, struct gui_state* g) {
    if (!fb || !fb->pixels) return;
    int w = (int)fb->width;
    for (int y = 0; y < GUI_MENU_H && y < (int)fb->height; y++) {
        uint32_t* row = fb->pixels + y * fb->width;
        for (int x = 0; x < w; x++) {
            uint32_t bg = row[x];
            /* translucent white overlay ~0.75 alpha premul */
            uint32_t overlay = 0xBFFFFFFFu; /* premul white a=191 */
            row[x] = gfx_blend_premul(overlay, bg);
        }
    }
    gfx_draw_text(gfx_font_get(14), 16, 6, "ARC OS", gfx_rgba(20, 20, 25, 255));
    const char* appname = "Desktop";
    if (g && g->focused_id) {
        struct gui_window* gw = 0;
        for (int i = 0; i < g->win_count; i++) if (g->wins[i].comp_id == g->focused_id) gw = &g->wins[i];
        if (gw) appname = gw->title;
    }
    gfx_draw_text(gfx_font_get(14), 90, 6, appname, gfx_rgba(30, 30, 35, 255));
    gfx_draw_text(gfx_font_get(14), w - 100, 6, "12:00", gfx_rgba(30, 30, 35, 255));
}

static int dock_icon_x(int index, int count, int screen_w) {
    int icon_w = 56;
    int gap = 8;
    int total = count * icon_w + (count - 1) * gap;
    int start = (screen_w - total) / 2;
    return start + index * (icon_w + gap);
}

static void draw_dock(struct gfx_fb* fb, struct gui_state* g) {
    if (!fb || !fb->pixels) return;
    int sw = (int)fb->width, sh = (int)fb->height;
    int count = 4;
    int icon_w = 56, gap = 8;
    int total = count * icon_w + (count - 1) * gap;
    int pad = 16;
    int dw = total + pad * 2;
    int dh = 64;
    int dx = (sw - dw) / 2;
    int dy = sh - dh - 8;
    /* pill background */
    for (int y = dy; y < dy + dh; y++) {
        if (y < 0 || y >= sh) continue;
        for (int x = dx; x < dx + dw; x++) {
            if (x < 0 || x >= sw) continue;
            int lx = x < dx+16 ? dx+16-x : (x >= dx+dw-16 ? x-(dx+dw-17) : 0);
            int ly = y < dy+16 ? dy+16-y : (y >= dy+dh-16 ? y-(dy+dh-17) : 0);
            if (lx*lx + ly*ly > 16*16) continue;
            uint32_t bg = fb->pixels[y*sw+x];
            fb->pixels[y*sw+x] = gfx_blend_premul(0x99FFFFFFu, bg);
        }
    }
    const char* labels[] = {"Abt","Not","Fil","Sys"};
    uint32_t colors[] = {0xFF5A9BD5u,0xFFF4B942u,0xFF7BC67Bu,0xFFB07CC6u};
    for (int i = 0; i < count; i++) {
        int ix = dock_icon_x(i, count, sw);
        int iy = dy + 4;
        int hover = (g && g->mouse_x >= ix && g->mouse_x < ix+icon_w && g->mouse_y >= iy && g->mouse_y < iy+icon_w);
        int sz = hover ? 60 : 52;
        int ox = ix + (icon_w - sz)/2;
        int oy = iy + (icon_w - sz)/2 - (hover ? 4 : 0);
        for (int y = oy; y < oy+sz; y++) for (int x = ox; x < ox+sz; x++) {
            if (x<0||x>=sw||y<0||y>=sh) continue;
            int lx = x < ox+10 ? ox+10-x : (x >= ox+sz-10 ? x-(ox+sz-11) : 0);
            int ly = y < oy+10 ? oy+10-y : (y >= oy+sz-10 ? y-(oy+sz-11) : 0);
            if (lx*lx+ly*ly > 10*10) continue;
            fb->pixels[y*sw+x] = colors[i];
        }
        gfx_draw_text(gfx_font_get(12), ix+14, iy+18, labels[i], gfx_rgba(255,255,255,255));
        /* running indicator */
        int running = 0;
        if (g) for (int w = 0; w < g->win_count; w++) if (g->wins[w].app == (enum gui_app)(i+1)) running = 1;
        if (running) {
            int dotx = ix + icon_w/2;
            int doty = dy + dh - 6;
            for (int yy=-2;yy<=2;yy++) for (int xx=-2;xx<=2;xx++) if(xx*xx+yy*yy<=4) {
                int px=dotx+xx, py=doty+yy; if(px>=0&&px<sw&&py>=0&&py<sh) fb->pixels[py*sw+px]=gfx_rgba(60,60,70,255);
            }
        }
    }
}

static void draw_cursor_simple(struct gfx_fb* fb, int mx, int my) {
    /* simple arrow cursor */
    static const unsigned char cur[12][8] = {
        {1,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0},
        {1,2,2,1,0,0,0,0},
        {1,2,2,2,1,0,0,0},
        {1,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,1,0},
        {1,2,2,2,2,2,2,1},
        {1,2,2,2,2,1,1,1},
        {1,2,2,1,2,1,0,0},
        {1,2,1,0,1,2,1,0},
        {1,1,0,0,1,2,1,0},
    };
    int sw=(int)fb->width, sh=(int)fb->height;
    for(int y=0;y<12;y++) for(int x=0;x<8;x++){
        unsigned char v=cur[y][x]; if(!v) continue;
        int px=mx+x, py=my+y; if(px<0||px>=sw||py<0||py>=sh) continue;
        fb->pixels[py*sw+px] = v==1 ? 0xFF000000u : 0xFFFFFFFFu;
    }
}

static void gui_remove_window(struct gui_state* g, uint32_t comp_id) {
    int idx=-1;
    for(int i=0;i<g->win_count;i++) if(g->wins[i].comp_id==comp_id) { idx=i; break; }
    if(idx<0) return;
    struct comp_window* cw = comp_find(&g->comp, comp_id);
    uint32_t* surf = 0;
    size_t pages = 0;
    if (cw && cw->surface && cw->w > 0 && cw->h > 0) {
        surf = cw->surface;
        pages = ((size_t)cw->w * (size_t)cw->h * 4u + 4095u) / 4096u;
    }
    comp_destroy_window(&g->comp, comp_id);
    if (surf && pages) pmm_free_pages(surf, pages);
    for(int i=idx;i+1<g->win_count;i++) g->wins[i]=g->wins[i+1];
    g->win_count--;
    if(g->focused_id==comp_id) g->focused_id = g->win_count ? g->wins[g->win_count-1].comp_id : 0;
}

static void gui_draw_window_content(struct gui_window* gw, struct comp_window* cw, struct gui_state* g) {
    if(!gw||!cw||!cw->surface) return;
    draw_window_chrome(cw, gw->title, gw->comp_id == g->focused_id);
    int cx=16, cy=GUI_TITLE_H+16;
    uint32_t textc = gfx_rgba(30,30,35,255);
    if (gw->app == GUI_APP_ABOUT) {
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy,"ARC OS",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+24,"Milestone M2 desktop",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+48,"Kernel-mode compositor",textc);
    } else if (gw->app == GUI_APP_NOTES) {
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy,"Notes",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+24,"Type here (focused window)",gfx_rgba(80,80,90,255));
        if (gw->text_len > 0) draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+52,gw->text,textc);
    } else if (gw->app == GUI_APP_FILES) {
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy,"Files",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+24,"/ readme.txt",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+44,"/ hello.bin",textc);
    } else if (gw->app == GUI_APP_SYSMON) {
        char buf[64];
        /* simple numbers */
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy,"System Monitor",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+24,"RAM: see serial log",textc);
        draw_text_buf(cw->surface,cw->w,cw->h,cx,cy+44,"FPS:",textc);
        /* fps number */
        char nbuf[16]; int fps=g?g->fps:0; int ni=0;
        if(fps==0) nbuf[ni++]='0';
        else { char tmp[16]; int ti=0; while(fps>0&&ti<15){tmp[ti++]='0'+(fps%10);fps/=10;} while(ti--) nbuf[ni++]=tmp[ti]; }
        nbuf[ni]=0;
        draw_text_buf(cw->surface,cw->w,cw->h,cx+40,cy+44,nbuf,textc);
        (void)buf;
    }
}

static struct gui_window* gui_open_app(struct gui_state* g, enum gui_app app) {
    if(!g || g->win_count>=GUI_MAX_WINDOWS) return 0;
    /* focus existing */
    for(int i=0;i<g->win_count;i++) if(g->wins[i].app==app) {
        g->focused_id=g->wins[i].comp_id;
        comp_bring_to_front(&g->comp, g->wins[i].comp_id);
        return &g->wins[i];
    }
    int w=420,h=280,x=120+g->win_count*30,y=80+g->win_count*24;
    size_t bytes = (size_t)w*h*4;
    size_t pages = (bytes + 4095)/4096;
    uint32_t* surf = (uint32_t*)pmm_alloc_pages(pages);
    if(!surf) return 0;
    for(size_t i=0;i<pages*1024;i++) surf[i]=0;
    struct comp_window* cw = comp_create_window(&g->comp,x,y,w,h,surf);
    if(!cw){ pmm_free_pages(surf,pages); return 0; }
    struct gui_window* gw = &g->wins[g->win_count++];
    gw->comp_id=cw->id; gw->app=app; gw->closing=0; gw->focused=1;
    gw->text_len=0; gw->text[0]=0;
    const char* t="Window";
    if(app==GUI_APP_ABOUT) t="About";
    else if(app==GUI_APP_NOTES) t="Notes";
    else if(app==GUI_APP_FILES) t="Files";
    else if(app==GUI_APP_SYSMON) t="System Monitor";
    for(int i=0;i<31 && t[i];i++){gw->title[i]=t[i];gw->title[i+1]=0;}
    g->focused_id=cw->id;
    comp_bring_to_front(&g->comp,cw->id);
    gui_draw_window_content(gw,cw,g);
    return gw;
}

void gui_init(struct gui_state* g, struct gfx_fb* fb) {
    if(!g) return;
    comp_init(&g->comp);
    g->win_count=0; g->focused_id=0; g->mouse_x=fb?fb->width/2:960; g->mouse_y=fb?fb->height/2:540;
    g->mouse_buttons=0; g->prev_buttons=0; g->drag_win=-1; g->cursor_visible=1;
    g->frame=0; g->fps=0; g->fps_frames=0; g->fps_last_sec=0; g->wallpaper=0;
    if(fb && fb->pixels) {
        size_t bytes = (size_t)fb->width*fb->height*4;
        size_t pages=(bytes+4095)/4096;
        g->wallpaper=(uint32_t*)pmm_alloc_pages(pages);
        if(g->wallpaper) {
            for(uint32_t y=0;y<fb->height;y++) {
                uint8_t r=(uint8_t)(20+(20*y)/fb->height);
                uint8_t gv=(uint8_t)(40+(40*y)/fb->height);
                uint8_t b=(uint8_t)(90+(50*y)/fb->height);
                uint32_t c=gfx_rgba(r,gv,b,255);
                for(uint32_t x=0;x<fb->width;x++) g->wallpaper[y*fb->width+x]=c;
            }
        }
    }
    gui_open_app(g, GUI_APP_ABOUT);
}

void gui_handle_mouse(struct gui_state* g, int x, int y, uint8_t buttons) {
    if(!g) return;
    g->mouse_x=x; g->mouse_y=y;
    uint8_t pressed = buttons & ~g->prev_buttons;
    uint8_t released = ~buttons & g->prev_buttons;
    if (pressed & MOUSE_BTN_LEFT) {
        /* dock click */
        struct gfx_fb* fb = gfx_fb_get();
        int sw = fb ? (int)fb->width : 1920;
        int sh = fb ? (int)fb->height : 1080;
        int count=4, icon_w=56;
        int dy = sh - 64 - 8;
        for(int i=0;i<count;i++) {
            int ix=dock_icon_x(i,count,sw);
            if(x>=ix && x<ix+icon_w && y>=dy && y<dy+64) {
                gui_open_app(g, (enum gui_app)(i+1));
                g->prev_buttons=buttons; g->mouse_buttons=buttons; return;
            }
        }
        uint32_t hit = comp_hit_test_id(&g->comp, x, y);
        if (hit) {
            g->focused_id = hit;
            comp_bring_to_front(&g->comp, hit);
            struct comp_window* cw = comp_find(&g->comp, hit);
            if (cw && gui_hit_test_close(cw, x, y)) {
                gui_remove_window(g, hit);
                g->prev_buttons=buttons; g->mouse_buttons=buttons; return;
            }
            if (cw && gui_hit_test_titlebar(cw, x, y)) {
                g->drag_win = (int)hit;
                g->drag_off_x = x - cw->x;
                g->drag_off_y = y - cw->y;
            }
        } else {
            g->focused_id = 0;
        }
    }
    if ((buttons & MOUSE_BTN_LEFT) && g->drag_win > 0) {
        struct comp_window* cw = comp_find(&g->comp, (uint32_t)g->drag_win);
        if (cw) { cw->x = x - g->drag_off_x; cw->y = y - g->drag_off_y; }
    }
    if (released & MOUSE_BTN_LEFT) g->drag_win = -1;
    g->prev_buttons = buttons;
    g->mouse_buttons = buttons;
}

void gui_tick(struct gui_state* g, struct gfx_fb* fb) {
    if(!g||!fb||!fb->pixels) return;
    g->frame++;
    g->fps_frames++;
    /* fps estimate: every 100 ticks ~1s */
    if (g->frame - g->fps_last_sec >= 100) { g->fps = g->fps_frames; g->fps_frames=0; g->fps_last_sec=g->frame; }

    struct mouse_state ms = mouse_get_state();
    gui_handle_mouse(g, ms.x, ms.y, ms.buttons);

    draw_wallpaper(fb, g->wallpaper);
    /* redraw window contents (simple) */
    for (int i=0;i<g->win_count;i++) {
        struct comp_window* cw = comp_find(&g->comp, g->wins[i].comp_id);
        if (cw) gui_draw_window_content(&g->wins[i], cw, g);
    }
    /* cached 9-slice shadows behind windows */
    for (int zi = 0; zi < g->comp.zcount; zi++) {
        int idx = g->comp.zorder[zi];
        struct comp_window* cw = &g->comp.windows[idx];
        if (cw->used && (cw->flags & COMP_WIN_VISIBLE))
            draw_window_shadow(fb, cw->x, cw->y, cw->w, cw->h);
    }
    struct gfx_rect full = {0,0,(int)fb->width,(int)fb->height};
    comp_composite(&g->comp, fb, &full);
    draw_menu_bar(fb, g);
    draw_dock(fb, g);
    if (g->cursor_visible) draw_cursor_simple(fb, g->mouse_x, g->mouse_y);
    gfx_present(&full);
}

static struct gui_state g_gui;
static int g_gui_active = 0;

void gui_start(struct gfx_fb* fb) {
    gui_init(&g_gui, fb);
    g_gui_active = 1;
}

int gui_is_active(void) { return g_gui_active; }

void gui_frame_tick(void) {
    if (g_gui_active) gui_tick(&g_gui, gfx_fb_get());
}

struct gui_state* gui_get_state(void) { return &g_gui; }

int gui_handle_key(char c) {
    if (!g_gui_active) return 0;
    struct gui_state* g = &g_gui;
    if (!g->focused_id) return 0;
    struct gui_window* gw = 0;
    for (int i = 0; i < g->win_count; i++) if (g->wins[i].comp_id == g->focused_id) gw = &g->wins[i];
    if (!gw || gw->app != GUI_APP_NOTES) return 0;
    if (c == '\b' || c == 127) {
        if (gw->text_len > 0) gw->text[--gw->text_len] = 0;
        return 1;
    }
    if (c == '\n' || c == '\r') c = ' ';
    if (c >= 32 && c < 127 && gw->text_len < (int)sizeof(gw->text) - 1) {
        gw->text[gw->text_len++] = c;
        gw->text[gw->text_len] = 0;
    }
    return 1;
}
