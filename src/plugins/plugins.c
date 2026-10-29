#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "editor.h"

static lua_State *g_lua;
struct lua_binding { int key; int ref; };
static struct lua_binding g_lua_bindings[256];
static int g_lua_binding_count;

static int lua_bind(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	int key = key_from_name(name);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (key < 0) return luaL_error(L, "unknown key name '%s'", name);
	for (int i = 0; i < g_lua_binding_count; i++) {
		if (g_lua_bindings[i].key == key) {
			luaL_unref(L, LUA_REGISTRYINDEX, g_lua_bindings[i].ref);
			lua_pushvalue(L, 2);
			g_lua_bindings[i].ref = luaL_ref(L, LUA_REGISTRYINDEX);
			return 0;
		}
	}
	if (g_lua_binding_count >= (int)(sizeof(g_lua_bindings) / sizeof(g_lua_bindings[0])))
		return luaL_error(L, "too many plugin keybindings");
	lua_pushvalue(L, 2);
	g_lua_bindings[g_lua_binding_count].key = key;
	g_lua_bindings[g_lua_binding_count].ref = luaL_ref(L, LUA_REGISTRYINDEX);
	g_lua_binding_count++;
	return 0;
}

static int lua_unbind(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	int key = key_from_name(name);
	for (int i = 0; i < g_lua_binding_count; i++) {
		if (g_lua_bindings[i].key == key) {
			luaL_unref(L, LUA_REGISTRYINDEX, g_lua_bindings[i].ref);
			memmove(&g_lua_bindings[i], &g_lua_bindings[i + 1],
				sizeof(g_lua_bindings[0]) * (size_t)(g_lua_binding_count - i - 1));
			g_lua_binding_count--;
			break;
		}
	}
	return 0;
}

static int api_status(lua_State *L) {
	msg("%s", luaL_checkstring(L, 1));
	return 0;
}

static int lua_line_count(lua_State *L) {
	lua_pushinteger(L, g.nr);
	return 1;
}

static int lua_get_line(lua_State *L) {
	int line = (int)luaL_checkinteger(L, 1) - 1;
	if (line < 0 || line >= g.nr) { lua_pushnil(L); return 1; }
	lua_pushlstring(L, g.r[line].s, (size_t)g.r[line].l);
	return 1;
}

static int lua_get_text(lua_State *L) {
	struct b b = {0};
	for (int i = 0; i < g.nr; i++) {
		b_add(&b, g.r[i].s, g.r[i].l);
		if (i + 1 < g.nr) b_add(&b, "\n", 1);
	}
	lua_pushlstring(L, b.p ? b.p : "", (size_t)b.l);
	free(b.p);
	return 1;
}

static int lua_cursor(lua_State *L) {
	lua_pushinteger(L, g.cy + 1);
	lua_pushinteger(L, g.cx + 1);
	return 2;
}

static int lua_set_cursor(lua_State *L) {
	int y = (int)luaL_checkinteger(L, 1) - 1;
	int x = (int)luaL_optinteger(L, 2, 1) - 1;
	if (y < 0) y = 0;
	if (y > g.nr) y = g.nr;
	if (x < 0) x = 0;
	int maxx = y < g.nr ? g.r[y].l : 0;
	if (x > maxx) x = maxx;
	g.cy = y; g.cx = x; g.pref_cx = -1;
	g.sx = g.sy = -1;
	return 0;
}

static int lua_selection(lua_State *L) {
	char *s = get_sel();
	if (!s) lua_pushnil(L);
	else { lua_pushstring(L, s); free(s); }
	return 1;
}

static int lua_set_selection(lua_State *L) {
	int y1 = (int)luaL_checkinteger(L, 1) - 1;
	int x1 = (int)luaL_checkinteger(L, 2) - 1;
	int y2 = (int)luaL_checkinteger(L, 3) - 1;
	int x2 = (int)luaL_checkinteger(L, 4) - 1;
	if (y1 < 0) y1 = 0;
	if (y1 > g.nr) y1 = g.nr;
	if (y2 < 0) y2 = 0;
	if (y2 > g.nr) y2 = g.nr;
	if (x1 < 0) x1 = 0;
	if (x2 < 0) x2 = 0;
	if (y1 < g.nr && x1 > g.r[y1].l) x1 = g.r[y1].l;
	else if (y1 == g.nr) x1 = 0;
	if (y2 < g.nr && x2 > g.r[y2].l) x2 = g.r[y2].l;
	else if (y2 == g.nr) x2 = 0;
	g.sy = y1; g.sx = x1;
	g.cy = y2; g.cx = x2;
	g.pref_cx = -1; g.in_type = 0;
	return 0;
}

static int lua_clear_selection(lua_State *L) {
	(void)L;
	g.sx = g.sy = -1;
	return 0;
}

static int lua_select_all(lua_State *L) {
	(void)L;
	g.in_type = 0;
	g.sx = 0; g.sy = 0;
	if (g.nr > 0) { g.cy = g.nr - 1; g.cx = g.r[g.cy].l; }
	return 0;
}

static int lua_delete_selection(lua_State *L) {
	(void)L;
	if (g.sx != -1) { g.in_type = 0; snap_save(); del_sel(); }
	return 0;
}

static int api_insert(lua_State *L) {
	size_t len;
	const char *s = luaL_checklstring(L, 1, &len);
	if (memchr(s, '\0', len)) return luaL_error(L, "insert text cannot contain NUL bytes");
	if (!len) return 0;
	g.in_type = 0;
	snap_save();
	if (g.sx != -1) del_sel();
	for (size_t i = 0; i < len; i++) {
		if (s[i] == '\n') ins_nl();
		else if (s[i] != '\r') ins_c((unsigned char)s[i]);
	}
	g.in_type = 0;
	return 0;
}

static int lua_set_text(lua_State *L) {
	size_t len;
	const char *s = luaL_checklstring(L, 1, &len);
	if (memchr(s, '\0', len)) return luaL_error(L, "buffer text cannot contain NUL bytes");
	snap_save();
	for (int i = 0; i < g.nr; i++) free(g.r[i].s);
	free(g.r); g.r = NULL; g.nr = 0; g.r_cap = 0;
	size_t start = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i == len || s[i] == '\n') {
			size_t end = i;
			if (end > start && s[end - 1] == '\r') end--;
			if (i < len || end > start) add_r(g.nr, s + start, end - start);
			start = i + 1;
		}
	}
	g.cx = g.cy = 0; g.sx = g.sy = -1; g.pref_cx = -1; g.in_type = 0;
	g.dirty = 1; g.text_gen++;
	return 0;
}

static int lua_set_line(lua_State *L) {
	int line = (int)luaL_checkinteger(L, 1) - 1;
	size_t len;
	const char *s = luaL_checklstring(L, 2, &len);
	if (memchr(s, '\0', len)) return luaL_error(L, "line text cannot contain NUL bytes");
	if (memchr(s, '\n', len) || memchr(s, '\r', len)) return luaL_error(L, "set_line accepts one line of text");
	if (line < 0 || line >= g.nr) return luaL_error(L, "line out of range");
	g.in_type = 0; snap_save();
	free(g.r[line].s);
	g.r[line].s = malloc(len + 1);
	memcpy(g.r[line].s, s, len); g.r[line].s[len] = '\0'; g.r[line].l = (int)len;
	if (g.cy == line && g.cx > g.r[line].l) g.cx = g.r[line].l;
	g.dirty = 1; g.text_gen++;
	return 0;
}

static int lua_move(lua_State *L) {
	const char *dir = luaL_checkstring(L, 1);
	g.sx = g.sy = -1; g.in_type = 0;
	int key = -1;
	if (!strcmp(dir, "up")) key = K_UP;
	else if (!strcmp(dir, "down")) key = K_DOWN;
	else if (!strcmp(dir, "left")) key = K_LEFT;
	else if (!strcmp(dir, "right")) key = K_RIGHT;
	else if (!strcmp(dir, "home")) key = K_HOME;
	else if (!strcmp(dir, "end")) key = K_END;
	else if (!strcmp(dir, "pageup")) key = K_PGUP;
	else if (!strcmp(dir, "pagedown")) key = K_PGDN;
	else if (!strcmp(dir, "top")) { g.cy = g.cx = 0; g.pref_cx = -1; return 0; }
	else if (!strcmp(dir, "bottom")) {
		if (g.nr > 0) { g.cy = g.nr - 1; g.cx = g.r[g.cy].l; }
		g.pref_cx = -1; return 0;
	} else if (!strcmp(dir, "word_left")) { word_left(); return 0; }
	else if (!strcmp(dir, "word_right")) { word_right(); return 0; }
	if (key < 0) return luaL_error(L, "unknown movement '%s'", dir);
	mv(key);
	return 0;
}

static int lua_save(lua_State *L) { (void)L; sav(); return 0; }
static int lua_filename(lua_State *L) {
	if (g.fn) lua_pushstring(L, g.fn); else lua_pushnil(L);
	return 1;
}
static int lua_theme(lua_State *L) {
	load_th(luaL_checkstring(L, 1));
	save_cfg();
	return 0;
}
static int lua_overwrite(lua_State *L) {
	g.overwrite = lua_toboolean(L, 1);
	save_cfg();
	return 0;
}

static const luaL_Reg kilo_api[] = {
	{"bind", lua_bind}, {"unbind", lua_unbind},
	{"status", api_status}, {"line_count", lua_line_count},
	{"get_line", lua_get_line}, {"get_text", lua_get_text},
	{"cursor", lua_cursor}, {"set_cursor", lua_set_cursor},
	{"selection", lua_selection}, {"set_selection", lua_set_selection},
	{"clear_selection", lua_clear_selection}, {"select_all", lua_select_all},
	{"delete_selection", lua_delete_selection}, {"insert", api_insert},
	{"set_text", lua_set_text}, {"set_line", lua_set_line},
	{"move", lua_move}, {"save", lua_save}, {"filename", lua_filename},
	{"set_theme", lua_theme}, {"set_overwrite", lua_overwrite},
	{NULL, NULL}
};

void plugins_close(void) {
	if (g_lua) lua_close(g_lua);
	g_lua = NULL; g_lua_binding_count = 0;
}

static int plugin_name_cmp(const void *a, const void *b) {
	return strcmp((const char *)a, (const char *)b);
}

void plugins_load(void) {
	plugins_close();
	if (!g.plugins_enabled) return;
	char dir[768];
	const char *home = getenv("HOME");
	if (g.plugin_dir[0]) {
		if (g.plugin_dir[0] == '~' && g.plugin_dir[1] == '/' && home) {
			size_t home_len = strlen(home), rest_len = strlen(g.plugin_dir + 2);
			if (home_len + rest_len + 2 > sizeof(dir)) { msg("plugin directory path is too long"); return; }
			memcpy(dir, home, home_len);
			dir[home_len] = '/';
			memcpy(dir + home_len + 1, g.plugin_dir + 2, rest_len + 1);
		} else {
			size_t path_len = strlen(g.plugin_dir);
			if (path_len >= sizeof(dir)) { msg("plugin directory path is too long"); return; }
			memcpy(dir, g.plugin_dir, path_len + 1);
		}
	} else if (home) snprintf(dir, sizeof(dir), "%s/.config/kilo/plugins", home);
	else { msg("Lua plugins need HOME to be set"); return; }
	if (mkdir(dir, 0755) == -1 && errno != EEXIST) { msg("plugin dir: %s", strerror(errno)); return; }
	DIR *dp = opendir(dir);
	if (!dp) { msg("plugin dir: %s", strerror(errno)); return; }
	char files[128][256]; int nfiles = 0;
	struct dirent *de;
	while ((de = readdir(dp)) && nfiles < 128) {
		size_t n = strlen(de->d_name);
		if (n > 4 && !strcmp(de->d_name + n - 4, ".lua") && n < sizeof(files[0]))
			snprintf(files[nfiles++], sizeof(files[0]), "%s", de->d_name);
	}
	closedir(dp);
	qsort(files, (size_t)nfiles, sizeof(files[0]), plugin_name_cmp);
	g_lua = luaL_newstate();
	if (!g_lua) { msg("could not create Lua state"); return; }
	luaL_openlibs(g_lua);
	lua_newtable(g_lua);
	luaL_setfuncs(g_lua, kilo_api, 0);
	lua_setglobal(g_lua, "kilo");
	for (int i = 0; i < nfiles; i++) {
		char path[1024];
		size_t dir_len = strlen(dir), file_len = strlen(files[i]);
		if (dir_len + file_len + 2 > sizeof(path)) continue;
		memcpy(path, dir, dir_len);
		path[dir_len] = '/';
		memcpy(path + dir_len + 1, files[i], file_len + 1);
		if (luaL_loadfile(g_lua, path) != LUA_OK || lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
			const char *error = lua_tostring(g_lua, -1);
			msg("plugin %.32s: %.42s", files[i], error ? error : "load failed");
			lua_pop(g_lua, 1);
		}
	}
}

int plugin_dispatch(int key) {
	if (!g_lua) return 0;
	for (int i = 0; i < g_lua_binding_count; i++) {
		if (g_lua_bindings[i].key != key) continue;
		lua_rawgeti(g_lua, LUA_REGISTRYINDEX, g_lua_bindings[i].ref);
		if (lua_pcall(g_lua, 0, 1, 0) != LUA_OK) {
			const char *error = lua_tostring(g_lua, -1);
			msg("plugin key error: %.52s", error ? error : "callback failed");
			lua_pop(g_lua, 1);
			return 1;
		}
		int handled = !lua_isboolean(g_lua, -1) || lua_toboolean(g_lua, -1);
		lua_pop(g_lua, 1);
		return handled;
	}
	return 0;
}

