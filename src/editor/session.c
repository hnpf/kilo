#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "editor.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char *session_file_path(void) {
	const char *home = getenv("HOME");
	static char path[PATH_MAX];
	if (!home) return NULL;
	int n = snprintf(path, sizeof(path), "%s/.config/kilo/session", home);
	return n >= 0 && n < (int)sizeof(path) ? path : NULL;
}

static char *absolute_path(const char *path) {
	if (!path || !path[0]) return strdup("");
	if (path[0] == '/') return strdup(path);
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof(cwd))) return strdup(path);
	size_t cwd_len = strlen(cwd), path_len = strlen(path);
	if (cwd_len > SIZE_MAX - path_len - 2) return NULL;
	char *absolute = malloc(cwd_len + path_len + 2);
	if (!absolute) return NULL;
	snprintf(absolute, cwd_len + path_len + 2, "%s/%s", cwd, path);
	return absolute;
}

static char *encode_path(const char *path) {
	static const char hex[] = "0123456789abcdef";
	if (!path || !path[0]) return strdup("-");
	size_t len = strlen(path);
	if (len > PATH_MAX || len > (SIZE_MAX - 1) / 2) return NULL;
	char *encoded = malloc(len * 2 + 1);
	if (!encoded) return NULL;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)path[i];
		encoded[i * 2] = hex[c >> 4];
		encoded[i * 2 + 1] = hex[c & 15];
	}
	encoded[len * 2] = '\0';
	return encoded;
}

static int hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static char *decode_path(const char *encoded) {
	if (!strcmp(encoded, "-")) return strdup("");
	size_t len = strlen(encoded);
	if (!len || len % 2 || len / 2 > PATH_MAX) return NULL;
	char *path = malloc(len / 2 + 1);
	if (!path) return NULL;
	for (size_t i = 0; i < len; i += 2) {
		int hi = hex_digit(encoded[i]), lo = hex_digit(encoded[i + 1]);
		if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) { free(path); return NULL; }
		path[i / 2] = (char)((hi << 4) | lo);
	}
	path[len / 2] = '\0';
	return path;
}

void session_state_free(struct session_state *state) {
	if (!state) return;
	if (state->tabs)
		for (int i = 0; i < state->tab_count; i++) free(state->tabs[i].path);
	free(state->tabs);
	memset(state, 0, sizeof(*state));
}

int session_restore(int *sidebar_open, int *sidebar_focused) {
	char *path = session_file_path();
	if (!path) return 0;
	FILE *fp = fopen(path, "r");
	if (!fp) return 0;
	struct session_state state = {0};
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t got;
	int version = 0, count = 0;
	int restored = 0;
	if ((got = getline(&line, &line_cap, fp)) < 0 || sscanf(line, "KILO_SESSION %d", &version) != 1 || version != 1) goto done;
	if ((got = getline(&line, &line_cap, fp)) < 0 || sscanf(line, "active %d", &state.active) != 1) goto done;
	if ((got = getline(&line, &line_cap, fp)) < 0 ||
		sscanf(line, "sidebar %d %d", &state.sidebar_open, &state.sidebar_focused) != 2) goto done;
	if ((got = getline(&line, &line_cap, fp)) < 0 || sscanf(line, "tabs %d", &count) != 1 || count < 1 || count > 10000) goto done;
	state.tab_count = count;
	state.tabs = calloc((size_t)count, sizeof(*state.tabs));
	if (!state.tabs) goto done;
	for (int i = 0; i < count; i++) {
		char encoded[PATH_MAX * 2 + 2];
		struct session_tab_state *tab = &state.tabs[i];
		if ((got = getline(&line, &line_cap, fp)) < 0 ||
			sscanf(line, "tab %d %d %d %d %8192s", &tab->cx, &tab->cy,
				&tab->rowoff, &tab->coloff, encoded) != 5) goto done;
		tab->path = decode_path(encoded);
		if (!tab->path) goto done;
	}
	if (tabs_session_import(&state) > 0) {
		*sidebar_open = state.sidebar_open != 0;
		*sidebar_focused = *sidebar_open && state.sidebar_focused != 0;
		restored = 1;
	}
done:
	free(line);
	fclose(fp);
	session_state_free(&state);
	return restored;
}

void session_save(void) {
	char *path = session_file_path();
	const char *home = getenv("HOME");
	if (!path || !home) return;
	struct session_state state = {0};
	if (!tabs_session_export(&state)) return;
	state.sidebar_open = sidebar_is_open();
	state.sidebar_focused = sidebar_has_focus();
	char config_dir[PATH_MAX], kilo_dir[PATH_MAX], temp_path[PATH_MAX];
	if (snprintf(config_dir, sizeof(config_dir), "%s/.config", home) >= (int)sizeof(config_dir) ||
		snprintf(kilo_dir, sizeof(kilo_dir), "%s/.config/kilo", home) >= (int)sizeof(kilo_dir) ||
		snprintf(temp_path, sizeof(temp_path), "%s.tmp.XXXXXX", path) >= (int)sizeof(temp_path)) {
		session_state_free(&state);
		return;
	}
	mkdir(config_dir, 0755);
	mkdir(kilo_dir, 0755);
	int fd = mkstemp(temp_path);
	if (fd == -1) { session_state_free(&state); return; }
	FILE *fp = fdopen(fd, "w");
	if (!fp) {
		close(fd); unlink(temp_path); session_state_free(&state); return;
	}
	int okay = fprintf(fp, "KILO_SESSION 1\nactive %d\nsidebar %d %d\ntabs %d\n",
		state.active, state.sidebar_open, state.sidebar_focused, state.tab_count) >= 0;
	for (int i = 0; okay && i < state.tab_count; i++) {
		char *absolute = absolute_path(state.tabs[i].path);
		char *encoded = absolute ? encode_path(absolute) : NULL;
		free(absolute);
		if (!encoded) { okay = 0; break; }
		struct session_tab_state *tab = &state.tabs[i];
		if (fprintf(fp, "tab %d %d %d %d %s\n", tab->cx, tab->cy,
			tab->rowoff, tab->coloff, encoded) < 0) okay = 0;
		free(encoded);
	}
	int flushed = okay && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	int closed = fclose(fp) == 0;
	if (flushed && closed && rename(temp_path, path) == 0) {
		session_state_free(&state);
		return;
	}
	unlink(temp_path);
	session_state_free(&state);
}
