#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static struct termios original_termios;

void
tui_init(void)
{
	tcgetattr(STDIN_FILENO, &original_termios);
	struct termios raw = original_termios;
	raw.c_lflag &= ~(ECHO | ICANON);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
	printf("\033[?25l"); // Hide cursor
}

void
tui_cleanup(void)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
	printf("\033[?25h"); // Show cursor
	printf("\033[0m\n"); // Reset color
}

void
tui_clear(void)
{
	printf("\033[2J\033[H");
}

void
tui_move_cursor(int x, int y)
{
	printf("\033[%d;%dH", y, x);
}

void
tui_set_color(int color)
{
	printf("\033[%dm", color);
}

void
tui_reset_color(void)
{
	printf("\033[0m");
}

tui_key_t
tui_get_key(void)
{
	char c;
	if (read(STDIN_FILENO, &c, 1) <= 0) {
		return KEY_OTHER;
	}

	if (c == '\033') {
		char seq[3];
		if (read(STDIN_FILENO, &seq[0], 1) <= 0) {
			return KEY_ESC;
		}
		if (read(STDIN_FILENO, &seq[1], 1) <= 0) {
			return KEY_ESC;
		}

		if (seq[0] == '[') {
			switch (seq[1]) {
			case 'A':
				return KEY_UP;
			case 'B':
				return KEY_DOWN;
			}
		}
		return KEY_ESC;
	} else if (c == '\n' || c == '\r') {
		return KEY_ENTER;
	} else if (c == ' ') {
		return KEY_SPACE;
	}
	return KEY_OTHER;
}

void
tui_draw_header(const char* title)
{
	tui_clear();
	tui_set_color(COLOR_BLUE);
	tui_set_color(COLOR_BOLD);
	printf("╔════════════════════════════════════════════════════════════════╗\n");
	printf("║ %-62s ║\n", title);
	printf("╚════════════════════════════════════════════════════════════════╝\n");
	tui_reset_color();
	printf("\n");
}

void
tui_draw_menu(const char** options, int count, int selected, const char* title)
{
	tui_draw_header(title);
	for (int i = 0; i < count; i++) {
		if (i == selected) {
			tui_set_color(COLOR_CYAN);
			printf("  > %s\n", options[i]);
			tui_reset_color();
		} else {
			printf("    %s\n", options[i]);
		}
	}
	printf("\n(Use arrow keys to move, ENTER to select)\n");
}

void
tui_draw_checklist(
    const char** options, bool* selected_flags, int count, int current, const char* title)
{
	tui_draw_header(title);
	for (int i = 0; i < count; i++) {
		if (i == current) {
			tui_set_color(COLOR_CYAN);
			printf("  > [%s] %s\n", selected_flags[i] ? "x" : " ", options[i]);
			tui_reset_color();
		} else {
			printf("    [%s] %s\n", selected_flags[i] ? "x" : " ", options[i]);
		}
	}
	printf("\n(SPACE to toggle, arrow keys to move, ENTER to confirm)\n");
}
