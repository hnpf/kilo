#ifndef KILO_EDITOR_H
#define KILO_EDITOR_H

#include <stddef.h>
#include <stdint.h>
#include <termios.h>
#include <time.h>

enum k {
	K_ESC = 27,
	K_BACKSPACE = 127,
	K_UP = 1000,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_PGUP,
	K_PGDN,
	K_DEL,
	K_INS,
	K_S_UP,
	K_S_DOWN,
	K_S_LEFT,
	K_S_RIGHT,
	K_CMD,
	K_HELP,
	K_REDO,
	K_CTRL_LEFT,
	K_CTRL_RIGHT,
	K_CTRL_UP,
	K_CTRL_DOWN,
	K_CTRL_SHIFT_LEFT,
	K_CTRL_SHIFT_RIGHT,
	K_CTRL_ENTER,
	K_CTRL_PGUP,
	K_CTRL_PGDN,
	K_CTRL_BACKSPACE,
	K_ALT_BACKSPACE,
	K_PLUGIN_CTRL_SHIFT_BASE = 1200,
	K_PLUGIN_ALT_BASE = 1300,
	K_PLUGIN_CTRL_ALT_BASE = 1400
};

struct b { char *p; int l; };
struct r { int l; char *s; };
struct th {
	char name[32], bg[32], fg[32], sel[32], ln[32], cur_ln[32];
	char keyword[32], function[32], string[32], number[32], comment[32];
};
struct snap { int nr; struct r *r; char *data; int cx, cy; size_t bytes; };

struct s {
	struct termios o;
	int cx, cy, sx, sy, pref_cx, rowoff, coloff, w, h, nr, r_cap;
	struct r *r;
	char *fn, *clip;
	char search[128];
	int search_on;
	char msg[80];
	time_t msg_t;
	int dirty, overwrite, word_wrap, plugins_enabled;
	char plugin_dir[512];
	int quit_warn, in_type;
	size_t file_sz;
	struct th th;
	struct snap u_stack[128]; int u_len;
	struct snap r_stack[128]; int r_len;
	size_t history_bytes;
	int history_disabled;
	unsigned long text_gen;
};

extern struct s g;
extern char g_themes[64][32];
extern int g_nthemes;

void msg(const char *fmt, ...);
void err(const char *message);
void b_add(struct b *b, const char *s, int len);
void b_free(struct b *b);
int hex_rgb(const char *hex, int *r, int *g, int *b);
void th_bg(struct b *b);
void th_fg(struct b *b);
void wsz(void);
void rfsh(void);
void snap_save(void);
void add_r(int at, const char *s, size_t len);
char *get_sel(void);
void del_sel(void);
void ins_c(int c);
void ins_nl(void);
void load_th(const char *name);
void load_cfg(void);
void save_cfg(void);
void scan_themes(void);
int str_contains_ci(const char *haystack, const char *needle);
char *doc_text(void);
void syntax_prepare(void);
int syntax_style_at(size_t offset);
int gutter_width(void);
void cursor_from_screen(int screen_row, int screen_col, int width, int gutter);
int r_key(void);
int key_from_name(const char *name);
void sav(void);
void mv(int key);
void word_left(void);
void word_right(void);
int mouse_word_bounds_at_cursor(int *start, int *end, int *clicked);
void mouse_select_word(int start, int end);
void mouse_extend_word_selection(int start, int end, int clicked);
void show_help(void);
void cmd_menu(void);

void plugins_load(void);
void plugins_close(void);
int plugin_dispatch(int key);
void sidebar_init(int visible);
int sidebar_width(void);
int sidebar_is_open(void);
int sidebar_has_focus(void);
void sidebar_blur(void);
int sidebar_handle_key(int key);
int sidebar_mouse_click(int screen_row);
int sidebar_mouse_drag(int screen_row);
void sidebar_render_row(struct b *b, int screen_row);
int editor_open_file(const char *path);

#endif
