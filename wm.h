// 7OS desktop: compositor, window manager, taskbar and launcher
#pragma once
#include "kernel.h"
#include "gfx.h"
#include "assets.h"

typedef struct {
    bool dark;
    uint32_t accent, accent_text;
    uint32_t win_bg, title_bg, win_border;
    uint32_t text, text2, text3;
    uint32_t sidebar_bg, card_bg, card_border, hover, pressed, separator;
    uint32_t input_bg, input_border;
    uint32_t panel_tint;
    int panel_alpha, border_alpha, shadow_alpha;
} theme_t;
extern theme_t T;

enum { APP_NONE, APP_SETTINGS, APP_TERMINAL };
enum { M_DOWN, M_UP, M_MOVE, M_DRAG, M_LEAVE };

typedef struct window window_t;
struct window {
    char title[64];
    const image_t *icon;
    int app;
    rect_t r;      // outer frame, screen coordinates
    rect_t saved;  // geometry before maximise
    bool maximized, minimized;
    int min_w, min_h;
    void *data;
    void (*draw)(window_t *w, canvas_t *c, rect_t content);
    void (*mouse)(window_t *w, int type, int x, int y, int buttons);
    void (*key)(window_t *w, const event_t *e);
    void (*scroll)(window_t *w, int x, int y, int dz);
    void (*tick)(window_t *w, uint64_t now);
    void (*close)(window_t *w);
};

#define TITLE_H 40
#define WIN_RADIUS 12
#define TASKBAR_H 56

extern canvas_t g_screen;
extern int SCR_W, SCR_H;

void wm_init(void);
__attribute__((noreturn)) void wm_run(void);
window_t *wm_create(const char *title, int app, int w, int h);
void wm_close(window_t *w);
void wm_focus(window_t *w);
window_t *wm_focused(void);
window_t *wm_find_app(int app);
rect_t wm_content(window_t *w);
void wm_invalidate(rect_t r);
void wm_invalidate_all(void);
void wm_invalidate_window(window_t *w);
void wm_invalidate_content(window_t *w);
void wm_set_theme(bool dark, uint32_t accent);
void wm_set_wallpaper(int idx);
void wm_flush(void);
void wm_power_action(bool reboot);
extern int g_wallpaper;
int wallpaper_count(void);
const char *wallpaper_name(int idx);
bool wallpaper_is_light(int idx);
void wallpaper_render(int idx, uint32_t *out, int w, int h);

// boot splash
void splash_init(void);
void splash_progress(int pct, const char *msg);
void splash_finish(void);

// shared widgets
void ui_card(canvas_t *c, rect_t r);
void ui_bar(canvas_t *c, int x, int y, int w, int h, uint64_t num, uint64_t den, uint32_t col);
void ui_toggle(canvas_t *c, int x, int y, bool on);
void ui_button(canvas_t *c, rect_t r, const char *label, bool primary, bool hover);
void ui_text_center(canvas_t *c, const font_t *f, rect_t r, const char *s, uint32_t col);
int ui_text_vcenter(const font_t *f, int y, int h);

// apps
void app_settings_open(int page);
void app_terminal_open(void);
extern const char *g_accent_names[];
extern const uint32_t g_accents[];
extern const int g_accent_count;
