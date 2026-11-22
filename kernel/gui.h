#ifndef ARC_GUI_H
#define ARC_GUI_H

#include <stdint.h>
#include "gfx.h"
#include "compositor.h"
#include "mouse.h"

#define GUI_MAX_WINDOWS 16
#define GUI_TITLE_H 28
#define GUI_MENU_H 28
#define GUI_DOCK_H 72

enum gui_app {
    GUI_APP_NONE = 0,
    GUI_APP_ABOUT,
    GUI_APP_NOTES,
    GUI_APP_FILES,
    GUI_APP_SYSMON,
};

struct gui_window {
    uint32_t comp_id;
    enum gui_app app;
    char title[32];
    char text[256];
    int text_len;
    int closing;
    int focused;
};

struct gui_state {
    struct compositor comp;
    struct gui_window wins[GUI_MAX_WINDOWS];
    int win_count;
    uint32_t focused_id;
    int mouse_x, mouse_y;
    uint8_t mouse_buttons, prev_buttons;
    int drag_win;
    int drag_off_x, drag_off_y;
    int cursor_visible;
    uint32_t frame;
    uint32_t fps;
    uint32_t fps_frames;
    uint32_t fps_last_sec;
    /* cached wallpaper */
    uint32_t* wallpaper;
};

void gui_init(struct gui_state* g, struct gfx_fb* fb);
void gui_start(struct gfx_fb* fb);
int gui_is_active(void);
void gui_frame_tick(void);
struct gui_state* gui_get_state(void);
void gui_tick(struct gui_state* g, struct gfx_fb* fb);
void gui_handle_mouse(struct gui_state* g, int x, int y, uint8_t buttons);
int gui_hit_test_titlebar(const struct comp_window* win, int px, int py);
int gui_hit_test_close(const struct comp_window* win, int px, int py);
int gui_handle_key(char c);
/* easing: t in 0..256 fixed point, returns 0..256 */
int gui_ease_out(int t);
int gui_spring(int t);

#endif
