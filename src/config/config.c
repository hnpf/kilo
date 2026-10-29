#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "editor.h"

char g_themes[64][32];
int g_nthemes;

void load_th(const char *name) {
	char path[256];
	const char *home = getenv("HOME");
	FILE *fp = NULL;

	if (home) {
		snprintf(path, sizeof(path), "%s/.config/kilo/themes/%s.conf", home, name);
		fp = fopen(path, "r");
	}
	if (!fp) {
		snprintf(path, sizeof(path), "themes/%s.conf", name);
		fp = fopen(path, "r");
	}
	if (!fp) {
		snprintf(path, sizeof(path), "/etc/kilo/themes/%s.conf", name);
		fp = fopen(path, "r");
	}
	if (!fp) {
		msg("theme not found: %s", name);
		return;
	}

	strncpy(g.th.name, name, sizeof(g.th.name) - 1);
	g.th.name[sizeof(g.th.name) - 1] = '\0';
	g.th.keyword[0] = g.th.function[0] = g.th.string[0] = '\0';
	g.th.number[0] = g.th.comment[0] = '\0';
	char line[128];
	while (fgets(line, sizeof(line), fp)) {
		char key[32], val[32];
		if (sscanf(line, " %31[^= ] = \"%31[^\"]\"", key, val) == 2 ||
		    sscanf(line, " %31[^= ] = %31s", key, val) == 2) {
			if (!strcmp(key, "background")) snprintf(g.th.bg, sizeof(g.th.bg), "%s", val);
			else if (!strcmp(key, "foreground")) snprintf(g.th.fg, sizeof(g.th.fg), "%s", val);
			else if (!strcmp(key, "selection")) snprintf(g.th.sel, sizeof(g.th.sel), "%s", val);
			else if (!strcmp(key, "line_number")) snprintf(g.th.ln, sizeof(g.th.ln), "%s", val);
			else if (!strcmp(key, "current_line_nr")) snprintf(g.th.cur_ln, sizeof(g.th.cur_ln), "%s", val);
			else if (!strcmp(key, "keyword")) snprintf(g.th.keyword, sizeof(g.th.keyword), "%s", val);
			else if (!strcmp(key, "function")) snprintf(g.th.function, sizeof(g.th.function), "%s", val);
			else if (!strcmp(key, "string")) snprintf(g.th.string, sizeof(g.th.string), "%s", val);
			else if (!strcmp(key, "number")) snprintf(g.th.number, sizeof(g.th.number), "%s", val);
			else if (!strcmp(key, "comment")) snprintf(g.th.comment, sizeof(g.th.comment), "%s", val);
		}
	}
	fclose(fp);
	if (!g.th.keyword[0]) snprintf(g.th.keyword, sizeof(g.th.keyword), "%s", g.th.fg);
	if (!g.th.function[0]) snprintf(g.th.function, sizeof(g.th.function), "%s", g.th.fg);
	if (!g.th.string[0]) snprintf(g.th.string, sizeof(g.th.string), "%s", g.th.fg);
	if (!g.th.number[0]) snprintf(g.th.number, sizeof(g.th.number), "%s", g.th.fg);
	if (!g.th.comment[0]) snprintf(g.th.comment, sizeof(g.th.comment), "%s", g.th.ln[0] ? g.th.ln : g.th.fg);
	msg("theme: %s", name);
}

static char *cfg_path(void) {
	const char *home = getenv("HOME");
	if (!home) return NULL;
	static char path[256];
	snprintf(path, sizeof(path), "%s/.config/kilo/kilo.conf", home);
	return path;
}

void load_cfg(void) {
	char *p = cfg_path();
	if (!p) return;
	FILE *fp = fopen(p, "r");
	if (!fp) return;

	char line[512];
	while (fgets(line, sizeof(line), fp)) {
		char key[32], val[384];
		if (sscanf(line, " %31[^= ] = \"%383[^\"]\"", key, val) == 2 ||
		    sscanf(line, " %31[^= ] = %383s", key, val) == 2) {
			if (!strcmp(key, "theme")) {
				load_th(val);
			} else if (!strcmp(key, "overwrite")) {
				g.overwrite = !strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes");
			} else if (!strcmp(key, "plugins")) {
				g.plugins_enabled = strcmp(val, "false") && strcmp(val, "0") && strcmp(val, "no");
			} else if (!strcmp(key, "wrap")) {
				g.word_wrap = !strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes");
			} else if (!strcmp(key, "git")) {
				g.git_enabled = !strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes");
			} else if (!strcmp(key, "gutter_bright")) {
				g.gutter_bright = !strcmp(val, "false") || !strcmp(val, "0") || !strcmp(val, "no") ? 0 :
					(!strcmp(val, "true") || !strcmp(val, "1") || !strcmp(val, "yes"));
			} else if (!strcmp(key, "plugin_dir")) {
				snprintf(g.plugin_dir, sizeof(g.plugin_dir), "%s", val);
			}
		}
	}
	fclose(fp);
}

void save_cfg(void) {
	const char *home = getenv("HOME");
	if (!home) return;
	char dir[256];
	snprintf(dir, sizeof(dir), "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.config/kilo", home);
	mkdir(dir, 0755);

	char *p = cfg_path();
	if (!p) return;
	FILE *fp = fopen(p, "w");
	if (!fp) return;
	fprintf(fp, "[editor]\ntheme = \"%s\"\noverwrite = %s\nplugins = %s\nwrap = %s\ngit = %s\ngutter_bright = %s\n",
		g.th.name[0] ? g.th.name : "base16_transparent",
		g.overwrite ? "true" : "false",
		g.plugins_enabled ? "true" : "false",
		g.word_wrap ? "true" : "false",
		g.git_enabled ? "true" : "false",
		g.gutter_bright ? "true" : "false");
	if (g.plugin_dir[0]) fprintf(fp, "plugin_dir = \"%s\"\n", g.plugin_dir);
	fclose(fp);
}

void scan_themes(void) {
	g_nthemes = 0;
	char udir[256];
	const char *home = getenv("HOME");
	if (home) {
		snprintf(udir, sizeof(udir), "%s/.config/kilo/themes", home);
	}

	for (int d = 0; d < 3; d++) {
		const char *dirpath = (d == 0) ? "themes" : (d == 1 && home ? udir : "/etc/kilo/themes");
		DIR *dp = opendir(dirpath);
		if (!dp) continue;
		struct dirent *de;
		while ((de = readdir(dp)) != NULL && g_nthemes < 64) {
			char *ext = strstr(de->d_name, ".conf");
			if (ext && strlen(ext) == 5) {
				char name[32];
				size_t len = ext - de->d_name;
				if (len < sizeof(name)) {
					strncpy(name, de->d_name, len);
					name[len] = '\0';
					int exists = 0;
					for (int i = 0; i < g_nthemes; i++) {
						if (!strcmp(g_themes[i], name)) { exists = 1; break; }
					}
					if (!exists) {
						snprintf(g_themes[g_nthemes++], sizeof(g_themes[0]), "%s", name);
					}
				}
			}
		}
		closedir(dp);
	}
}

int str_contains_ci(const char *haystack, const char *needle) {
	if (!needle || !needle[0]) return 1;
	size_t nlen = strlen(needle);
	size_t hlen = strlen(haystack);
	if (nlen > hlen) return 0;
	for (size_t i = 0; i <= hlen - nlen; i++) {
		size_t j = 0;
		while (j < nlen && tolower((unsigned char)haystack[i + j]) == tolower((unsigned char)needle[j])) {
			j++;
		}
		if (j == nlen) return 1;
	}
	return 0;
}

