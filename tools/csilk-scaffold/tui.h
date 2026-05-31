#ifndef CSILK_TUI_H
#define CSILK_TUI_H

#include <termios.h>

typedef enum { KEY_UP = 1000, KEY_DOWN, KEY_ENTER, KEY_SPACE, KEY_ESC, KEY_OTHER } tui_key_t;

void tui_init(void);
void tui_cleanup(void);
void tui_clear(void);
void tui_move_cursor(int x, int y);
void tui_set_color(int color);
void tui_reset_color(void);
tui_key_t tui_get_key(void);

void tui_draw_header(const char* title);
void tui_draw_menu(const char** options, int count, int selected, const char* title);
void tui_draw_checklist(
    const char** options, bool* selected_flags, int count, int current, const char* title);
void
tui_draw_radiolist(const char** options, int count, int selected, int current, const char* title);

#define COLOR_BLUE 34
#define COLOR_GREEN 32
#define COLOR_RED 31
#define COLOR_CYAN 36
#define COLOR_YELLOW 33
#define COLOR_BOLD 1

#endif
