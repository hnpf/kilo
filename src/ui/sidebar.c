#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "editor.h"

struct tree_node {
	char *name;
	char *path;
	int directory;
	int loaded;
	int expanded;
	struct tree_node *parent;
	struct tree_node **children;
	int child_count;
	int child_cap;
};

struct visible_node { struct tree_node *node; int depth; };
struct search_file { char *path; char *relative; };
struct search_result { int file; size_t line, column; int content; char snippet[96]; };

#define SIDEBAR_SEARCH_FILE_LIMIT 20000
#define SIDEBAR_SEARCH_RESULT_LIMIT 100
#define SIDEBAR_SEARCH_FILE_SIZE_LIMIT (32u * 1024u * 1024u)

static struct tree_node *root;
static struct visible_node *visible;
static int visible_count, visible_cap;
static int selected, top, is_open, is_focused;
static struct search_file *search_files;
static int search_file_count, search_file_cap, search_index_built;
static struct search_result *search_results;
static int search_result_count, search_result_cap;
static int *search_candidates;
static int search_candidate_count, search_candidate_cap;
static int search_candidates_complete = 1;
static int search_results_truncated;
static char search_query[128];
static size_t search_cursor;
static struct timespec last_click_time;
static struct tree_node *last_clicked_node;
static int last_clicked_search_file = -1;

static void free_node(struct tree_node *node) {
	if (!node) return;
	for (int i = 0; i < node->child_count; i++) free_node(node->children[i]);
	free(node->children);
	free(node->name);
	free(node->path);
	free(node);
}

static struct tree_node *new_node(const char *name, const char *path, int is_dir, struct tree_node *parent) {
	struct tree_node *node = calloc(1, sizeof(*node));
	if (!node) return NULL;
	node->name = strdup(name);
	node->path = strdup(path);
	node->directory = is_dir;
	node->parent = parent;
	if (!node->name || !node->path) { free_node(node); return NULL; }
	return node;
}

static int node_compare(const void *a, const void *b) {
	const struct tree_node *na = *(const struct tree_node *const *)a;
	const struct tree_node *nb = *(const struct tree_node *const *)b;
	if (na->directory != nb->directory) return nb->directory - na->directory;
	int order = strcasecmp(na->name, nb->name);
	return order ? order : strcmp(na->name, nb->name);
}

static int append_child(struct tree_node *parent, struct tree_node *child) {
	if (parent->child_count == parent->child_cap) {
		int cap = parent->child_cap ? parent->child_cap * 2 : 32;
		struct tree_node **children = realloc(parent->children, sizeof(*children) * (size_t)cap);
		if (!children) return 0;
		parent->children = children;
		parent->child_cap = cap;
	}
	parent->children[parent->child_count++] = child;
	return 1;
}

static int search_file_compare(const void *a, const void *b) {
	const struct search_file *fa = a;
	const struct search_file *fb = b;
	int order = strcasecmp(fa->relative, fb->relative);
	return order ? order : strcmp(fa->relative, fb->relative);
}

static int add_search_file(const char *path, const char *relative) {
	if (search_file_count >= SIDEBAR_SEARCH_FILE_LIMIT) return 0;
	if (search_file_count == search_file_cap) {
		int cap = search_file_cap ? search_file_cap * 2 : 256;
		if (cap > SIDEBAR_SEARCH_FILE_LIMIT) cap = SIDEBAR_SEARCH_FILE_LIMIT;
		struct search_file *files = realloc(search_files, sizeof(*files) * (size_t)cap);
		if (!files) return 0;
		search_files = files;
		search_file_cap = cap;
	}
	struct search_file *file = &search_files[search_file_count];
	file->path = strdup(path);
	file->relative = strdup(relative);
	if (!file->path || !file->relative) {
		free(file->path);
		free(file->relative);
		return 0;
	}
	search_file_count++;
	return 1;
}

static void index_directory(const char *path, const char *relative, int depth) {
	if (depth > 64 || search_file_count >= SIDEBAR_SEARCH_FILE_LIMIT) return;
	DIR *dir = opendir(path);
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL && search_file_count < SIDEBAR_SEARCH_FILE_LIMIT) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		char *child_path = NULL, *child_relative = NULL;
		if (asprintf(&child_path, "%s%s%s", path,
			path[strlen(path) - 1] == '/' ? "" : "/", entry->d_name) < 0) continue;
		if (relative[0]) {
			if (asprintf(&child_relative, "%s/%s", relative, entry->d_name) < 0) child_relative = NULL;
		} else if (asprintf(&child_relative, "%s", entry->d_name) < 0) child_relative = NULL;
		if (!child_relative) { free(child_path); continue; }
		struct stat st;
		if (lstat(child_path, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				if (strcmp(entry->d_name, ".git") && strcmp(entry->d_name, "node_modules"))
					index_directory(child_path, child_relative, depth + 1);
			} else if (S_ISREG(st.st_mode)) {
				add_search_file(child_path, child_relative);
			}
		}
		free(child_path);
		free(child_relative);
	}
	closedir(dir);
}

static void build_search_index(void) {
	if (search_index_built) return;
	search_index_built = 1;
	if (root) index_directory(root->path, "", 0);
	if (search_file_count > 1)
		qsort(search_files, (size_t)search_file_count, sizeof(*search_files), search_file_compare);
}

static void clear_search_index(void) {
	for (int i = 0; i < search_file_count; i++) {
		free(search_files[i].path);
		free(search_files[i].relative);
	}
	free(search_files);
	search_files = NULL;
	search_file_count = search_file_cap = search_index_built = 0;
	search_candidate_count = 0;
	search_result_count = 0;
	search_candidates_complete = 1;
	search_results_truncated = 0;
	free(search_candidates);
	search_candidates = NULL;
	search_candidate_cap = 0;
}

static int contains_ci(const char *text, size_t length, const char *query, size_t query_length,
	size_t *match_at) {
	if (!query_length || query_length > length) return 0;
	for (size_t i = 0; i <= length - query_length; i++) {
		size_t j = 0;
		while (j < query_length && tolower((unsigned char)text[i + j]) ==
			tolower((unsigned char)query[j])) j++;
		if (j == query_length) {
			if (match_at) *match_at = i;
			return 1;
		}
	}
	return 0;
}

static int search_file_contents(const struct search_file *file, const char *query,
	size_t query_length, size_t *line_number, size_t *column, char snippet[96]) {
	int fd = open(file->path, O_RDONLY);
	if (fd < 0) return 0;
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size <= 0 ||
		(uintmax_t)st.st_size > SIDEBAR_SEARCH_FILE_SIZE_LIMIT) {
		close(fd);
		return 0;
	}
	size_t size = (size_t)st.st_size;
	const unsigned char *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED) return 0;
	size_t sample = size < 4096 ? size : 4096;
	if (memchr(data, '\0', sample)) { munmap((void *)data, size); return 0; }
	size_t match_at = 0;
	if (!contains_ci((const char *)data, size, query, query_length, &match_at)) {
		munmap((void *)data, size);
		return 0;
	}
	size_t line = 1;
	for (size_t i = 0; i < match_at; i++) if (data[i] == '\n') line++;
	*line_number = line;
	size_t line_start = match_at;
	while (line_start && data[line_start - 1] != '\n') line_start--;
	*column = match_at - line_start;
	size_t line_end = match_at;
	while (line_end < size && data[line_end] != '\n' && data[line_end] != '\r') line_end++;
	size_t snippet_start = line_start;
	if (line_end - line_start > 88) {
		snippet_start = match_at > line_start + 28 ? match_at - 28 : line_start;
		if (snippet_start + 88 > line_end) snippet_start = line_end - 88;
	}
	size_t snippet_length = line_end - snippet_start;
	if (snippet_length > 88) snippet_length = 88;
	for (size_t i = 0; i < snippet_length; i++) {
		unsigned char c = data[snippet_start + i];
		snippet[i] = c == '\t' || c >= 32 ? (char)c : ' ';
	}
	snippet[snippet_length] = '\0';
	munmap((void *)data, size);
	return 1;
}

static void append_search_result(int file, int content, size_t line, size_t column, const char *snippet) {
	if (search_result_count == search_result_cap) {
		int cap = search_result_cap ? search_result_cap * 2 : 32;
		struct search_result *results = realloc(search_results, sizeof(*results) * (size_t)cap);
		if (!results) return;
		search_results = results;
		search_result_cap = cap;
	}
	struct search_result *result = &search_results[search_result_count++];
	result->file = file;
	result->content = content;
	result->line = line;
	result->column = column;
	snprintf(result->snippet, sizeof(result->snippet), "%s", snippet ? snippet : "");
}

static int append_candidate(int **items, int *count, int *capacity, int file) {
	if (*count == *capacity) {
		int cap = *capacity ? *capacity * 2 : 64;
		int *next = realloc(*items, sizeof(*next) * (size_t)cap);
		if (!next) return 0;
		*items = next;
		*capacity = cap;
	}
	(*items)[(*count)++] = file;
	return 1;
}

static void rebuild_search_results(const char *previous_query) {
	build_search_index();
	search_result_count = 0;
	search_results_truncated = 0;
	if (!search_query[0]) { search_candidate_count = 0; return; }
	size_t query_length = strlen(search_query);
	size_t previous_length = strlen(previous_query);
	int incremental = search_candidates_complete && previous_length >= 2 && query_length == previous_length + 1 &&
		!strncasecmp(search_query, previous_query, previous_length);
	int source_count = incremental ? search_candidate_count : search_file_count;
	int *next_candidates = NULL, next_count = 0, next_capacity = 0;
	int next_complete = 1;
	unsigned char *name_matches = calloc((size_t)search_file_count, 1);
	if (!name_matches && search_file_count) return;
	for (int i = 0; i < source_count; i++) {
		int file = incremental ? search_candidates[i] : i;
		if (file < 0 || file >= search_file_count) continue;
		const char *relative = search_files[file].relative;
		if (!contains_ci(relative, strlen(relative), search_query, query_length, NULL)) continue;
		name_matches[file] = 1;
		if (!append_candidate(&next_candidates, &next_count, &next_capacity, file)) break;
		if (search_result_count < SIDEBAR_SEARCH_RESULT_LIMIT)
			append_search_result(file, 0, 0, 0, NULL);
		else search_results_truncated = 1;
	}
	if (query_length >= 2) {
		if (search_result_count >= SIDEBAR_SEARCH_RESULT_LIMIT) {
			search_results_truncated = 1;
			next_complete = 0;
		}
		for (int i = 0; i < source_count && search_result_count < SIDEBAR_SEARCH_RESULT_LIMIT; i++) {
			int file = incremental ? search_candidates[i] : i;
			if (file < 0 || file >= search_file_count || name_matches[file]) continue;
			size_t line = 0, column = 0;
			char snippet[96] = {0};
			if (search_file_contents(&search_files[file], search_query, query_length, &line, &column, snippet)) {
				if (!append_candidate(&next_candidates, &next_count, &next_capacity, file)) break;
				if (search_result_count < SIDEBAR_SEARCH_RESULT_LIMIT)
					append_search_result(file, 1, line, column, snippet);
				else {
					search_results_truncated = 1;
					next_complete = 0;
					break;
				}
				if (search_result_count == SIDEBAR_SEARCH_RESULT_LIMIT) {
					search_results_truncated = 1;
					next_complete = 0;
					break;
				}
			}
		}
	}
	if (search_result_count == SIDEBAR_SEARCH_RESULT_LIMIT) search_results_truncated = 1;
	free(name_matches);
	free(search_candidates);
	search_candidates = next_candidates;
	search_candidate_count = next_count;
	search_candidate_cap = next_capacity;
	search_candidates_complete = next_complete;
	selected = top = 0;
}

static void load_children(struct tree_node *node) {
	if (!node || !node->directory || node->loaded) return;
	node->loaded = 1;
	DIR *dir = opendir(node->path);
	if (!dir) return;
	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		size_t plen = strlen(node->path), nlen = strlen(entry->d_name);
		char *path = malloc(plen + nlen + 2);
		if (!path) continue;
		snprintf(path, plen + nlen + 2, "%s%s%s", node->path,
			node->path[plen - 1] == '/' ? "" : "/", entry->d_name);
		struct stat st;
		int is_dir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
		struct tree_node *child = new_node(entry->d_name, path, is_dir, node);
		free(path);
		if (!child || !append_child(node, child)) free_node(child);
	}
	closedir(dir);
	qsort(node->children, (size_t)node->child_count, sizeof(*node->children), node_compare);
}

static void push_visible(struct tree_node *node, int depth) {
	if (visible_count == visible_cap) {
		int cap = visible_cap ? visible_cap * 2 : 64;
		struct visible_node *items = realloc(visible, sizeof(*items) * (size_t)cap);
		if (!items) return;
		visible = items;
		visible_cap = cap;
	}
	visible[visible_count++] = (struct visible_node){node, depth};
	if (node->directory && node->expanded) {
		load_children(node);
		for (int i = 0; i < node->child_count; i++) push_visible(node->children[i], depth + 1);
	}
}

static void keep_selection_visible_count(int count) {
	if (selected < 0) selected = 0;
	if (selected >= count) selected = count - 1;
	if (selected < 0) selected = 0;
	int rows = g.h > 1 ? g.h - 1 : 1;
	if (selected < top) top = selected;
	if (selected >= top + rows) top = selected - rows + 1;
	int max_top = count - rows;
	if (max_top < 0) max_top = 0;
	if (top > max_top) top = max_top;
	if (top < 0) top = 0;
}

static void keep_selection_visible(void) {
	keep_selection_visible_count(visible_count);
}

static void rebuild_visible(void) {
	visible_count = 0;
	if (root) {
		load_children(root);
		for (int i = 0; i < root->child_count; i++) push_visible(root->children[i], 0);
	}
	keep_selection_visible();
}

static struct tree_node *selected_node(void) {
	return selected >= 0 && selected < visible_count ? visible[selected].node : NULL;
}

static void activate_selected(void) {
	if (search_query[0]) {
		if (selected >= 0 && selected < search_result_count) {
			struct search_result *result = &search_results[selected];
			int file = result->file;
			if (file >= 0 && file < search_file_count && editor_open_file(search_files[file].path) && result->content) {
				g.cy = result->line ? (int)(result->line - 1) : 0;
				if (g.cy >= g.nr) g.cy = g.nr ? g.nr - 1 : 0;
				g.cx = g.cy < g.nr ? (int)result->column : 0;
				if (g.cy < g.nr && g.cx > g.r[g.cy].l) g.cx = g.r[g.cy].l;
				g.rowoff = g.coloff = 0;
				g.pref_cx = -1;
			}
		}
		return;
	}
	struct tree_node *node = selected_node();
	if (!node) return;
	if (node->directory) {
		load_children(node);
		node->expanded = !node->expanded;
		rebuild_visible();
	} else {
		editor_open_file(node->path);
	}
}

void sidebar_init(int visible_at_start) {
	free_node(root);
	free(visible);
	clear_search_index();
	search_query[0] = '\0';
	root = NULL;
	visible = NULL;
	visible_count = visible_cap = selected = top = 0;
	char cwd[4096];
	if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
	const char *name = strrchr(cwd, '/');
	name = name && name[1] ? name + 1 : cwd;
	root = new_node(name, cwd, 1, NULL);
	if (root) { root->expanded = 1; load_children(root); }
	is_open = visible_at_start;
	is_focused = visible_at_start;
	rebuild_visible();
}

int sidebar_width(void) {
	if (!is_open || g.w < 28) return 0;
	int width = search_query[0] ? g.w / 2 : g.w / 3;
	int max_width = search_query[0] ? 48 : 28;
	int min_width = search_query[0] ? 18 : 12;
	if (width > max_width) width = max_width;
	if (width < min_width) width = min_width;
	return width;
}

int sidebar_is_open(void) { return sidebar_width() > 0; }
int sidebar_has_focus(void) { return sidebar_is_open() && is_focused; }
void sidebar_blur(void) { is_focused = 0; }

static void change_search_query(const char *query) {
	char previous[sizeof(search_query)];
	snprintf(previous, sizeof(previous), "%s", search_query);
	snprintf(search_query, sizeof(search_query), "%s", query);
	last_clicked_node = NULL;
	last_clicked_search_file = -1;
	if (search_query[0]) {
		rebuild_search_results(previous);
		keep_selection_visible_count(search_result_count);
	} else {
		search_candidate_count = 0;
		search_candidates_complete = 1;
		search_result_count = 0;
		search_results_truncated = 0;
		search_cursor = 0;
		selected = top = 0;
		rebuild_visible();
	}
}

static int query_word_char(unsigned char c) {
	return isalnum(c) || c == '_';
}

static size_t query_word_left(size_t cursor) {
	while (cursor && !query_word_char((unsigned char)search_query[cursor - 1])) cursor--;
	while (cursor && query_word_char((unsigned char)search_query[cursor - 1])) cursor--;
	return cursor;
}

static size_t query_word_right(size_t cursor) {
	size_t length = strlen(search_query);
	while (cursor < length && query_word_char((unsigned char)search_query[cursor])) cursor++;
	while (cursor < length && !query_word_char((unsigned char)search_query[cursor])) cursor++;
	return cursor;
}

static void edit_query(size_t start, size_t end, int inserted_char) {
	size_t length = strlen(search_query);
	if (start > length) start = length;
	if (end > length) end = length;
	if (end < start) end = start;
	char next[sizeof(search_query)];
	size_t suffix_length = length - end;
	memcpy(next, search_query, start);
	if (inserted_char >= 0) next[start++] = (char)inserted_char;
	memcpy(next + start, search_query + end, suffix_length + 1);
	change_search_query(next);
	search_cursor = start;
}

static void delete_query_word_left(void) {
	if (!search_cursor) return;
	edit_query(query_word_left(search_cursor), search_cursor, -1);
}

static int sidebar_item_count(void) {
	return search_query[0] ? search_result_count : visible_count;
}

int sidebar_handle_key(int key) {
	if (key == ('b' & 0x1f)) {
		is_open = !is_open;
		is_focused = is_open;
		return 1;
	}
	if (!sidebar_is_open()) return 0;
	if (key == K_ESC || key == '\x1b') {
		if (search_query[0]) change_search_query("");
		is_open = 0;
		is_focused = 0;
		return 1;
	}
	if (key == '\t') {
		if (is_focused) { is_focused = 0; return 1; }
		return 0;
	}
	if (!is_focused) return 0;
	if (key == (('u' & 0x1f)) && search_query[0]) {
		change_search_query("");
		return 1;
	}
	if (search_query[0]) {
		if (key == K_CTRL_BACKSPACE || key == K_ALT_BACKSPACE || key == 23) {
			delete_query_word_left();
			return 1;
		}
		if (key == K_BACKSPACE || key == 8 || key == 127) {
			if (search_cursor) edit_query(search_cursor - 1, search_cursor, -1);
			return 1;
		}
		if (key == K_DEL) {
			if (search_cursor < strlen(search_query)) edit_query(search_cursor, search_cursor + 1, -1);
			return 1;
		}
		if (key == K_LEFT) { if (search_cursor) search_cursor--; return 1; }
		if (key == K_RIGHT) { if (search_cursor < strlen(search_query)) search_cursor++; return 1; }
		if (key == K_CTRL_LEFT) { search_cursor = query_word_left(search_cursor); return 1; }
		if (key == K_CTRL_RIGHT) { search_cursor = query_word_right(search_cursor); return 1; }
		if (key == K_HOME || key == 1) { search_cursor = 0; return 1; }
		if (key == K_END || key == 5) { search_cursor = strlen(search_query); return 1; }
		if (key >= 32 && key < 127) { edit_query(search_cursor, search_cursor, key); return 1; }
		if (key == K_UP) selected--;
		else if (key == K_DOWN) selected++;
		else if (key == K_PGUP) selected -= g.h > 2 ? g.h - 2 : 1;
		else if (key == K_PGDN) selected += g.h > 2 ? g.h - 2 : 1;
		else if (key == '\r' || key == '\n') activate_selected();
		else { is_focused = 0; return 0; }
		keep_selection_visible_count(search_result_count);
		return 1;
	}
	if (key == K_CTRL_BACKSPACE || key == K_ALT_BACKSPACE || key == 23 ||
		key == K_BACKSPACE || key == K_DEL || key == 8 || key == 127) return 1;
	if (key >= 32 && key < 127) { edit_query(0, 0, key); return 1; }
	rebuild_visible();
	if (key == K_UP && visible_count) selected--;
	else if (key == K_DOWN && visible_count) selected++;
	else if (key == K_HOME) selected = 0;
	else if (key == K_END) selected = visible_count - 1;
	else if (key == K_PGUP) selected -= g.h > 2 ? g.h - 2 : 1;
	else if (key == K_PGDN) selected += g.h > 2 ? g.h - 2 : 1;
	else if (key == '\r' || key == '\n' || key == K_RIGHT) {
		struct tree_node *node = selected_node();
		if (node && node->directory && key == K_RIGHT) {
			if (node->expanded) {
				if (node->child_count) selected++;
			} else {
				load_children(node);
				node->expanded = 1;
			}
		} else activate_selected();
	} else if (key == K_LEFT) {
		struct tree_node *node = selected_node();
		if (node && node->directory && node->expanded) node->expanded = 0;
		else if (node && node->parent && node->parent != root) {
			node->parent->expanded = 0;
			for (int i = 0; i < visible_count; i++) if (visible[i].node == node->parent) { selected = i; break; }
		}
	} else {
		is_focused = 0;
		return 0;
	}
	rebuild_visible();
	return 1;
}

int sidebar_mouse_click(int screen_row) {
	if (!sidebar_is_open()) return 0;
	is_focused = 1;
	if (search_query[0]) keep_selection_visible_count(search_result_count);
	else rebuild_visible();
	if (screen_row > 0) {
		int index = top + screen_row - 1;
		if (index < sidebar_item_count()) {
			struct tree_node *node = search_query[0] ? NULL : visible[index].node;
			int search_file = search_query[0] ? search_results[index].file : -1;
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			long long elapsed = (long long)(now.tv_sec - last_click_time.tv_sec) * 1000000000LL +
				(now.tv_nsec - last_click_time.tv_nsec);
			int same_item = search_query[0] ? search_file == last_clicked_search_file : node == last_clicked_node;
			int twice = same_item && elapsed >= 0 && elapsed <= 450000000LL;
			selected = index;
			if (twice) activate_selected();
			last_clicked_node = twice || search_query[0] ? NULL : node;
			last_clicked_search_file = twice || !search_query[0] ? -1 : search_file;
			last_click_time = now;
			keep_selection_visible_count(sidebar_item_count());
		}
	}
	return 1;
}

int sidebar_mouse_drag(int screen_row) {
	if (!sidebar_is_open()) return 0;
	if (search_query[0]) keep_selection_visible_count(search_result_count);
	else rebuild_visible();
	int index = top + screen_row - 1;
	if (index >= 0 && index < sidebar_item_count()) selected = index;
	keep_selection_visible_count(sidebar_item_count());
	return 1;
}

static int decode_utf8(const unsigned char *text, unsigned *codepoint) {
	unsigned char c = text[0];
	if (c < 0x80) { *codepoint = c; return 1; }
	int length = c >= 0xc2 && c <= 0xdf ? 2 :
		c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
	if (!length) { *codepoint = '?'; return 1; }
	unsigned value = c & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
	for (int i = 1; i < length; i++) {
		if ((text[i] & 0xc0) != 0x80) { *codepoint = '?'; return 1; }
		value = (value << 6) | (text[i] & 0x3f);
	}
	if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
		(length == 4 && value < 0x10000) || value > 0x10ffff ||
		(value >= 0xd800 && value <= 0xdfff)) {
		*codepoint = '?';
		return 1;
	}
	*codepoint = value;
	return length;
}

static int unicode_cells(unsigned cp) {
	if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x0483 && cp <= 0x0489) ||
		(cp >= 0x0591 && cp <= 0x05bd) || cp == 0x05bf || (cp >= 0x05c1 && cp <= 0x05c2) ||
		(cp >= 0x0610 && cp <= 0x061a) || (cp >= 0x064b && cp <= 0x065f) || cp == 0x0670 ||
		(cp >= 0x06d6 && cp <= 0x06ed) || cp == 0x200d || (cp >= 0xfe00 && cp <= 0xfe0f) ||
		(cp >= 0xfe20 && cp <= 0xfe2f) || (cp >= 0xe0100 && cp <= 0xe01ef) ||
		(cp >= 0x1f3fb && cp <= 0x1f3ff)) return 0;
	if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 1;
	if (cp >= 0x1100 && (cp <= 0x115f || cp == 0x2329 || cp == 0x232a ||
		(cp >= 0x2e80 && cp <= 0xa4cf && cp != 0x303f) ||
		(cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
		(cp >= 0xfe10 && cp <= 0xfe19) || (cp >= 0xfe30 && cp <= 0xfe6f) ||
		(cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
		(cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd))) return 2;
	return 1;
}

static int sidebar_label_cells(const char *label) {
	int cells = 0, join_next = 0;
	const unsigned char *p = (const unsigned char *)label;
	while (*p) {
		unsigned cp;
		int bytes = decode_utf8(p, &cp);
		if (cp >= 0x1f1e6 && cp <= 0x1f1ff && p[bytes]) {
			unsigned next_cp;
			int next_bytes = decode_utf8(p + bytes, &next_cp);
			if (next_cp >= 0x1f1e6 && next_cp <= 0x1f1ff) {
				cells += 2;
				p += bytes + next_bytes;
				continue;
			}
		}
		int char_cells = unicode_cells(cp);
		if (cp == 0x200d) join_next = 1;
		else if (join_next && char_cells == 2) { char_cells = 0; join_next = 0; }
		else if (char_cells > 0) join_next = 0;
		cells += char_cells;
		p += bytes;
	}
	return cells;
}

static void render_sidebar_label(struct b *b, const char *label, int width) {
	int total_cells = sidebar_label_cells(label);
	int truncated = total_cells > width;
	int text_width = truncated && width >= 3 ? width - 3 : width;
	int cells = 0, have_base = 0, join_next = 0;
	const unsigned char *p = (const unsigned char *)label;
	while (*p) {
		unsigned cp;
		int bytes = decode_utf8(p, &cp);
		int char_cells = unicode_cells(cp);
		if (cp >= 0x1f1e6 && cp <= 0x1f1ff && p[bytes]) {
			unsigned next_cp;
			int next_bytes = decode_utf8(p + bytes, &next_cp);
			if (next_cp >= 0x1f1e6 && next_cp <= 0x1f1ff) {
				bytes += next_bytes;
				char_cells = 2;
			}
		}
		if (cp == 0x200d) join_next = 1;
		else if (join_next && char_cells == 2) { char_cells = 0; join_next = 0; }
		else if (char_cells > 0) join_next = 0;
		if ((bytes == 1 && *p >= 0x80) || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) {
			b_add(b, "?", 1);
			cells++;
			have_base = 1;
		} else if (char_cells == 0) {
			if (have_base && cells <= text_width) b_add(b, (const char *)p, bytes);
		} else {
			if (cells + char_cells > text_width) break;
			b_add(b, (const char *)p, bytes);
			cells += char_cells;
			have_base = 1;
		}
		p += bytes;
	}
	if (truncated && width >= 3) {
		b_add(b, "...", 3);
		cells += 3;
	}
	while (cells < width) {
		b_add(b, " ", 1);
		cells++;
	}
}

void sidebar_render_row(struct b *b, int screen_row) {
	int width = sidebar_width();
	if (!width) return;
	th_bg(b);
	th_fg(b);
	int label_width = width - 1;
	char label[1024] = {0};
	if (screen_row == 0) {
		if (search_query[0]) {
			size_t length = strlen(search_query);
			if (search_cursor > length) search_cursor = length;
			snprintf(label, sizeof(label), " Search: %.*s|%s [%d%s]",
				(int)search_cursor, search_query, search_query + search_cursor,
				search_result_count, search_results_truncated ? "+" : "");
		} else
			snprintf(label, sizeof(label), " Files: %.900s", root ? root->path : "");
	} else {
		int index = top + screen_row - 1;
		if (search_query[0] && index >= 0 && index < search_result_count) {
			struct search_result *result = &search_results[index];
			struct search_file *file = &search_files[result->file];
			if (result->content)
				snprintf(label, sizeof(label), "%.400s:%zu: %.88s", file->relative, result->line, result->snippet);
			else
				snprintf(label, sizeof(label), "%.900s", file->relative);
			if (index == selected) b_add(b, "\x1b[7m", 4);
		} else if (!search_query[0] && index >= 0 && index < visible_count) {
			struct visible_node *item = &visible[index];
			char indent[128] = {0};
			int spaces = item->depth * 2;
			if (spaces > 40) spaces = 40;
			memset(indent, ' ', (size_t)spaces);
			const char *icon = item->node->directory ? (item->node->expanded ? "[-] " : "[+] ") : "    ";
			snprintf(label, sizeof(label), "%s%s%s", indent, icon, item->node->name);
			if (index == selected) b_add(b, "\x1b[7m", 4);
		}
	}
	render_sidebar_label(b, label, label_width);
	if (screen_row > 0 && top + screen_row - 1 == selected && selected < sidebar_item_count()) {
		b_add(b, "\x1b[0m", 4);
		th_bg(b);
		th_fg(b);
	}
	const char border[] = "\x1b[90m|\x1b[0m";
	b_add(b, border, sizeof(border) - 1);
}
