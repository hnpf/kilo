#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tree_sitter/api.h>
#include "editor.h"

#define SYNTAX_MAX_BYTES (8u * 1024u * 1024u)

static unsigned char *g_syntax_styles;
static size_t g_syntax_len;
static unsigned long g_syntax_gen = ~0ul;
extern const TSLanguage *tree_sitter_markdown(void);
extern const TSLanguage *tree_sitter_markdown_inline(void);
extern const TSLanguage *tree_sitter_rust(void);
extern const TSLanguage *tree_sitter_c(void);
extern const TSLanguage *tree_sitter_cpp(void);
extern const TSLanguage *tree_sitter_go(void);
extern const TSLanguage *tree_sitter_python(void);
extern const TSLanguage *tree_sitter_javascript(void);
extern const TSLanguage *tree_sitter_json(void);
extern const TSLanguage *tree_sitter_typescript(void);
extern const TSLanguage *tree_sitter_tsx(void);
extern const TSLanguage *tree_sitter_html(void);
extern const TSLanguage *tree_sitter_css(void);
extern const TSLanguage *tree_sitter_qmljs(void);
extern const TSLanguage *tree_sitter_ini(void);
extern const TSLanguage *tree_sitter_yaml(void);
extern const TSLanguage *tree_sitter_toml(void);
extern const TSLanguage *tree_sitter_bash(void);
extern const TSLanguage *tree_sitter_java(void);
extern const TSLanguage *tree_sitter_ruby(void);
extern const TSLanguage *tree_sitter_php(void);
extern const TSLanguage *tree_sitter_lua(void);

static const TSLanguage *syntax_language(const char **name) {
	if (!g.fn) return NULL;
	const char *dot = strrchr(g.fn, '.');
	if (!dot) return NULL;
	if (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".markdown") || !strcasecmp(dot, ".mdown") || !strcasecmp(dot, ".mkd")) {
		*name = "markdown"; return tree_sitter_markdown();
	}
	if (!strcasecmp(dot, ".rs")) { *name = "rust"; return tree_sitter_rust(); }
	if (!strcasecmp(dot, ".c") || !strcasecmp(dot, ".h")) { *name = "c"; return tree_sitter_c(); }
	if (!strcasecmp(dot, ".cc") || !strcasecmp(dot, ".cpp") || !strcasecmp(dot, ".cxx") ||
	    !strcasecmp(dot, ".hpp") || !strcasecmp(dot, ".hh") || !strcasecmp(dot, ".hxx")) {
		*name = "cpp"; return tree_sitter_cpp();
	}
	if (!strcasecmp(dot, ".go")) { *name = "go"; return tree_sitter_go(); }
	if (!strcasecmp(dot, ".py") || !strcasecmp(dot, ".pyw")) { *name = "python"; return tree_sitter_python(); }
	if (!strcasecmp(dot, ".ts") || !strcasecmp(dot, ".mts") || !strcasecmp(dot, ".cts")) {
		*name = "typescript"; return tree_sitter_typescript();
	}
	if (!strcasecmp(dot, ".tsx")) { *name = "tsx"; return tree_sitter_tsx(); }
	if (!strcasecmp(dot, ".js") || !strcasecmp(dot, ".jsx") || !strcasecmp(dot, ".mjs") || !strcasecmp(dot, ".cjs")) {
		*name = "javascript"; return tree_sitter_javascript();
	}
	if (!strcasecmp(dot, ".json") || !strcasecmp(dot, ".jsonc")) { *name = "json"; return tree_sitter_json(); }
	if (!strcasecmp(dot, ".html") || !strcasecmp(dot, ".htm") || !strcasecmp(dot, ".xhtml")) {
		*name = "html"; return tree_sitter_html();
	}
	if (!strcasecmp(dot, ".css")) { *name = "css"; return tree_sitter_css(); }
	if (!strcasecmp(dot, ".qml")) { *name = "qml"; return tree_sitter_qmljs(); }
	if (!strcasecmp(dot, ".conf") || !strcasecmp(dot, ".ini") || !strcasecmp(dot, ".cfg") ||
	    !strcasecmp(dot, ".desktop") || !strcasecmp(dot, ".service") || !strcasecmp(dot, ".target") ||
	    !strcasecmp(dot, ".mount") || !strcasecmp(dot, ".env")) {
		*name = "ini"; return tree_sitter_ini();
	}
	if (!strcasecmp(dot, ".yaml") || !strcasecmp(dot, ".yml")) { *name = "yaml"; return tree_sitter_yaml(); }
	if (!strcasecmp(dot, ".toml")) { *name = "toml"; return tree_sitter_toml(); }
	if (!strcasecmp(dot, ".sh") || !strcasecmp(dot, ".bash") || !strcasecmp(dot, ".zsh")) {
		*name = "bash"; return tree_sitter_bash();
	}
	if (!strcasecmp(dot, ".java")) { *name = "java"; return tree_sitter_java(); }
	if (!strcasecmp(dot, ".rb") || !strcasecmp(dot, ".rake") || !strcasecmp(dot, ".gemspec")) {
		*name = "ruby"; return tree_sitter_ruby();
	}
	if (!strcasecmp(dot, ".php") || !strcasecmp(dot, ".phtml")) { *name = "php"; return tree_sitter_php(); }
	if (!strcasecmp(dot, ".lua")) { *name = "lua"; return tree_sitter_lua(); }
	return NULL;
}

static char *syntax_query_file(const char *name) {
	char path[256];
	snprintf(path, sizeof(path), "vendor/tree-sitter-languages/%s/queries/highlights.scm", name);
	FILE *fp = fopen(path, "r");
	if (!fp) {
		snprintf(path, sizeof(path), "/usr/share/kilo/queries/%s.scm", name);
		fp = fopen(path, "r");
	}
	if (!fp) {
		snprintf(path, sizeof(path), "/usr/local/share/kilo/queries/%s.scm", name);
		fp = fopen(path, "r");
	}
	if (!fp) return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
	long n = ftell(fp);
	if (n < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
	char *query = malloc((size_t)n + 1);
	if (!query) { fclose(fp); return NULL; }
	if (fread(query, 1, (size_t)n, fp) != (size_t)n) { free(query); fclose(fp); return NULL; }
	query[n] = '\0';
	fclose(fp);
	return query;
}

static int syntax_style_id(const char *name) {
	if (!strcmp(name, "heading")) return 1;
	if (!strcmp(name, "punctuation")) return 2;
	if (!strcmp(name, "literal")) return 3;
	if (!strcmp(name, "uri")) return 4;
	if (!strcmp(name, "emphasis")) return 5;
	if (!strcmp(name, "strong")) return 6;
	if (!strncmp(name, "keyword", 7)) return 7;
	if (!strncmp(name, "type", 4) || !strncmp(name, "constructor", 11)) return 8;
	if (!strncmp(name, "function", 8) || !strncmp(name, "method", 6)) return 9;
	if (!strncmp(name, "string", 6) || !strncmp(name, "character", 9)) return 10;
	if (!strncmp(name, "number", 6) || !strncmp(name, "constant", 8) || !strncmp(name, "boolean", 7)) return 11;
	if (!strncmp(name, "comment", 7)) return 12;
	if (!strncmp(name, "variable", 8) || !strncmp(name, "property", 8) || !strncmp(name, "parameter", 9)) return 13;
	if (!strncmp(name, "operator", 8) || !strncmp(name, "punctuation", 11)) return 14;
	if (!strncmp(name, "attribute", 9) || !strncmp(name, "tag", 3)) return 15;
	if (!strncmp(name, "text.title", 10)) return 1;
	if (!strncmp(name, "text.literal", 12)) return 3;
	if (!strncmp(name, "text.uri", 8)) return 4;
	if (!strncmp(name, "text.reference", 14)) return 13;
	if (!strncmp(name, "string.escape", 13)) return 14;
	return 0;
}

static int syntax_style_priority(int style) {
	switch (style) {
	case 2: return 6; // punctuation overrides enclosing constructs
	case 6: return 5;
	case 5: return 4;
	case 4: return 4;
	case 3: return 3;
	case 1: return 2;
	case 7: return 2;
	case 8: return 3;
	case 9: return 4;
	case 10: return 5;
	case 11: return 5;
	case 12: return 6;
	case 13: return 2;
	case 14: return 6;
	case 15: return 4;
	default: return 0;
	}
}

static void syntax_apply_query(TSTree *tree, const TSLanguage *language,
		const char *source, unsigned char *styles, size_t len) {
	uint32_t err_offset = 0;
	TSQueryError err_type;
	TSQuery *query = ts_query_new(language, source, strlen(source), &err_offset, &err_type);
	if (!query) return;
	TSQueryCursor *cursor = ts_query_cursor_new();
	ts_query_cursor_exec(cursor, query, ts_tree_root_node(tree));
	TSQueryMatch match;
	uint32_t capture_index;
	while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
		TSQueryCapture capture = match.captures[capture_index];
		uint32_t name_len = 0;
		const char *name = ts_query_capture_name_for_id(query, capture.index, &name_len);
		char label[32];
		if (name_len >= sizeof(label)) continue;
		memcpy(label, name, name_len);
		label[name_len] = '\0';
		int style = syntax_style_id(label);
		if (!style) continue;
		uint32_t start = ts_node_start_byte(capture.node);
		uint32_t end = ts_node_end_byte(capture.node);
		if (end > len) end = len;
		for (uint32_t i = start; i < end; i++) {
			if (syntax_style_priority(style) >= syntax_style_priority(styles[i]))
				styles[i] = style;
		}
	}
	ts_query_cursor_delete(cursor);
	ts_query_delete(query);
}

void syntax_prepare(void) {
	if (g_syntax_gen == g.text_gen) return;
	free(g_syntax_styles);
	g_syntax_styles = NULL;
	g_syntax_len = 0;
	g_syntax_gen = g.text_gen;
	const char *language_name = NULL;
	const TSLanguage *language = syntax_language(&language_name);
	if (!language) return;
	size_t len = 0;
	for (int i = 0; i < g.nr; i++) {
		size_t row_bytes = (size_t)g.r[i].l + (i + 1 < g.nr);
		if (row_bytes > SYNTAX_MAX_BYTES - len) return;
		len += row_bytes;
	}
	char *text = doc_text();
	unsigned char *styles = calloc(len ? len : 1, 1);
	if (!styles) { free(text); return; }
	static const char *block_query =
		"(atx_heading) @heading\n"
		"(setext_heading) @heading\n"
		"[(atx_h1_marker) (atx_h2_marker) (atx_h3_marker) (atx_h4_marker) (atx_h5_marker) (atx_h6_marker) "
		"(setext_h1_underline) (setext_h2_underline) (fenced_code_block_delimiter) "
		"(list_marker_plus) (list_marker_minus) (list_marker_star) (list_marker_dot) "
		"(list_marker_parenthesis) (block_quote_marker) (thematic_break)] @punctuation\n"
		"[(fenced_code_block) (indented_code_block) (link_title)] @literal\n"
		"(link_destination) @uri\n"
		"(link_label) @literal\n";
	static const char *inline_query =
		"[(code_span) (link_title)] @literal\n"
		"(emphasis) @emphasis\n"
		"(strong_emphasis) @strong\n"
		"[(emphasis_delimiter) (code_span_delimiter)] @punctuation\n"
		"[(link_destination) (uri_autolink)] @uri\n"
		"[(link_label) (link_text) (image_description)] @literal\n"
		"(backslash_escape) @punctuation\n";
	TSParser *parser = ts_parser_new();
	if (ts_parser_set_language(parser, language)) {
		TSTree *tree = ts_parser_parse_string(parser, NULL, text, (uint32_t)len);
		if (tree) {
			if (!strcmp(language_name, "markdown")) {
				syntax_apply_query(tree, language, block_query, styles, len);
				TSParser *inline_parser = ts_parser_new();
				if (ts_parser_set_language(inline_parser, tree_sitter_markdown_inline())) {
					TSTree *inline_tree = ts_parser_parse_string(inline_parser, NULL, text, (uint32_t)len);
					if (inline_tree) {
						syntax_apply_query(inline_tree, tree_sitter_markdown_inline(), inline_query, styles, len);
						ts_tree_delete(inline_tree);
					}
				}
				ts_parser_delete(inline_parser);
			} else {
				char *query = syntax_query_file(language_name);
				if (query) {
					syntax_apply_query(tree, language, query, styles, len);
					free(query);
				}
			}
			ts_tree_delete(tree);
		}
	}
	ts_parser_delete(parser);
	free(text);
	g_syntax_styles = styles;
	g_syntax_len = len;
}

int syntax_style_at(size_t offset) { return offset < g_syntax_len && g_syntax_styles ? g_syntax_styles[offset] : 0; }
