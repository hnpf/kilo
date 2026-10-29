# kilo

text editor. wip: not recommended for use yet. 
> [!NOTE]
> feel free to contribute and fork! todo will come soon.

## build

Kilo uses the Lua 5.4 development headers and library for plugins.

```sh
make
```

## Lua plugins

Lua plugins are enabled by default and load alphabetically from
`~/.config/kilo/plugins/*.lua` each time Kilo starts. Create that directory and
copy in a script, or change `plugin_dir` in `~/.config/kilo/kilo.conf`:

```ini
[editor]
plugins = true
plugin_dir = "~/my-kilo-plugins"
wrap = true
```

Word wrapping can also be toggled from the command menu (`ctrl+;` → `word wrap`);
the setting is saved in `kilo.conf`.

The file sidebar opens automatically when Kilo starts without a file. Use the
arrow keys to navigate, right/enter to expand a folder, left to collapse it, and
enter or double-click a file to open it. Type in the focused sidebar to search
file paths and file contents; content matches show their line number and snippet.
Use left/right to edit the query, up/down to choose a result, and enter to open it
at the match. Ctrl+left/right move by word, Ctrl+backspace deletes the previous
word, and Escape clears the search. Toggle the sidebar with `ctrl+b` or
`ctrl+;` → `sidebar`.

Tabs open from the top bar. Use `ctrl+t` for a new tab, `ctrl+w` or a tab's `x`
to close it, and `alt+left` / `alt+right` (or `alt+l` / `alt+r`) to switch tabs.
Opening a file from the sidebar selects its existing tab or opens it in a new one.

When a file is inside a Git repository, the gutter compares the current buffer
with `HEAD`: green `+` marks added lines, yellow `~` modified lines, and red `-`
marks deletions at the next line (or the empty row after the file). The current
branch appears in the status bar.

On exit, Kilo saves file-backed tabs, their cursor and viewport positions, and
the sidebar's open/focus state to `~/.config/kilo/session`. The next launch
restores that session, then opens any paths passed on the command line. Session
state stores file paths and positions; unsaved buffer contents are not checkpointed.

The command menu (`ctrl+;`) includes `plugins on` and `plugins off`, which change
the persistent setting. Plugin scripts are regular Lua 5.4 with
the standard libraries enabled.

Plugins use the global `kilo` table. For example, `kilo.bind("ctrl+shift+w",
function() ... end)` binds a callback; returning `false` lets Kilo's built-in
binding run, while any other return value consumes the key. Later-loaded
plugins replace earlier bindings for the same key. See
[`examples/plugins/word_count.lua`](examples/plugins/word_count.lua).

The API includes `get_text`, `set_text`, `get_line`, `set_line`, `line_count`,
`cursor`, `set_cursor`, `selection`, `set_selection`, `clear_selection`,
`select_all`, `delete_selection`, `insert`, `move`, `save`, `filename`, `status`,
`set_theme`, and `set_overwrite`. Rows and columns use 1-based coordinates; columns
follow the editor's UTF-8 byte indexing.

ci builds on push and nightly. artifacts are in github actions.

license: [MIT](/LICENSE), read for more.
