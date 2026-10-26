### Project Identity: **Kilo** (`kl`)
> **The Ergonomic Win:** `k` and `l` sit side-by-side on home row. Launching via `kl main.c` or `kl ~/.config/kilo/kilo.conf` is a single, effortless two-finger roll.

---

# The Master Implementation Checklist
*(Ordered from inception & setup to deep architectural details)*

```
[ ] Phase 0: Identity, Repo & Workspace Inception
[ ] Phase 1: Build System & Project Skeleton
[ ] Phase 2: Terminal Engine & Raw Mode (Zero-Dep VT100/ANSI)
[ ] Phase 3: Core Buffer Engine (Piece Table + Undo Tree)
[ ] Phase 4: Non-Blocking Save Engine (Zero-Lag Atomic Writes)
[ ] Phase 5: Modern Non-Modal Keybinds & OSC 52 Clipboard
[ ] Phase 6: Double-Buffered Diff Renderer
[ ] Phase 7: Configuration Parser (`~/.config/kilo/kilo.conf`)
[ ] Phase 8: Palette Engine & Theming (Transparent Base16 + Helix)
[ ] Phase 9: Contextual Help Overlay (`F1`) & Syntax Tokens
[ ] Phase 10: Validation, Memory Audits & Benchmarks
```

---

## [Phase 0] Identity, Repo & Workspace Inception

- [ ] **Repository Setup**: Initialize `github.com/<user>/kilo`.
- [ ] **License**: MIT or BSD-2-Clause (permissive, minimal friction).
- [ ] **Target OS**: Linux strictly (`_GNU_SOURCE`, Linux `kernel >= 3.15` for atomic renames and non-blocking syscalls).
- [ ] **Configuration Path Standard**: `~/.config/kilo/kilo.conf` and `~/.config/kilo/themes/`.

---

## [Phase 1] Build System & Project Skeleton

- [ ] **Create Modular Project Tree**:
  ```text
  kilo/
  ├── Makefile
  ├── themes/
  │   ├── base16_transparent.conf
  │   ├── helix_default.conf
  │   └── unix_amber.conf
  └── src/
      ├── main.c           # Entry point, signals, runloop
      ├── buffer.c/h       # Piece Table data structure
      ├── editor.c/h       # Viewport, cursor, selection state
      ├── input.c/h        # Keycode parser & mouse decoding
      ├── render.c/h       # Frame diffing & ANSI output
      ├── io_worker.c/h    # Threaded non-blocking file saver
      ├── config.c/h       # Flat INI-style config reader
      ├── theme.c/h        # Color palettes & ANSI emission
      ├── clipboard.c/h    # OSC 52 escape sequences & fallbacks
      ├── syntax.c/h       # State-machine token scanner
      └── help.c/h         # Dynamic F1 help overlay
  ```
- [ ] **Configure the Makefile**:
  * Zero external libraries needed; link only standard `libc` and `pthread`.
  * Support auto-generating the `kl` symlink on `make install`.
  ```makefile
  CC ?= gcc
  CFLAGS ?= -O3 -std=c11 -Wall -Wextra -pedantic -D_GNU_SOURCE
  LDFLAGS ?= -lpthread
  PREFIX ?= /usr/local

  SRCS = $(wildcard src/*.c)
  OBJS = $(SRCS:.c=.o)
  BIN = kilo

  all: $(BIN)

  $(BIN): $(OBJS)
  	$(CC) $(OBJS) -o $@ $(LDFLAGS)

  %.o: %.c
  	$(CC) $(CFLAGS) -c $< -o $@

  install: $(BIN)
  	install -d $(DESTDIR)$(PREFIX)/bin
  	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/kilo
  	ln -sf $(DESTDIR)$(PREFIX)/bin/kilo $(DESTDIR)$(PREFIX)/bin/kl
  	install -d $(DESTDIR)/etc/kilo/themes
  	install -m 644 themes/* $(DESTDIR)/etc/kilo/themes/
  ```

---

## [Phase 2] Terminal Engine & Raw Mode (Zero-Dep VT100/ANSI)

- [ ] **Raw Mode Initialization (`src/input.c`)**:
  * Save original terminal settings via `tcgetattr(STDIN_FILENO, &orig_termios)`.
  * Disable canonical mode, echo, extended input processing, and flow control:
    `raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);`
    `raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);`
    `raw.c_cflag |= (CS8);`
  * Set read timeout: `raw.c_cc[VMIN] = 0; raw.c_cc[VTIME] = 1;` (non-blocking 100ms poll).
  * Register cleanup via `atexit(restore_terminal)` and signal handlers (`SIGINT`, `SIGTERM`, `SIGWINCH`).
- [ ] **Alternate Screen & Mouse Tracking**:
  * Enter: write `\033[?1049h` (alt screen) + `\033[?1006h\033[?1002h` (SGR extended mouse tracking).
  * Exit: write `\033[?1006l\033[?1002l` + `\033[?1049l` + show cursor `\033[?25h`.
- [ ] **Escape Sequence State Machine**:
  * Decode multi-byte sequences into clean internal enum IDs:
    `Ctrl + Arrows`, `Shift + Arrows`, `Home`, `End`, `PageUp`, `PageDown`, `Delete`, `Mouse Click/Drag/Scroll`.

---

## [Phase 3] Core Buffer Engine (Piece Table + Undo Tree)

- [ ] **Implement Piece Table (`src/buffer.c`)**:
  * Avoid reallocating large strings. Keep two memory buffers:
    1. **Original Buffer**: Read-only `mmap` of the loaded file.
    2. **Add Buffer**: Append-only buffer for newly typed characters.
  * Define piece node descriptors:
    ```c
    typedef enum { BUFFER_ORIGINAL, BUFFER_ADD } BufferType;
    typedef struct Piece {
        BufferType type;
        size_t offset;
        size_t length;
        struct Piece *prev;
        struct Piece *next;
    } Piece;
    ```
- [ ] **Instant $O(1)$ Undo/Redo**:
  * Maintain a stack of piece-node pointers.
  * Undo simply rewinds the active pointer head without copying file contents.
- [ ] **Line-Offset Cache**:
  * Maintain an array of piece + offset pointers corresponding to line beginnings for instant scrolling navigation.

---

## [Phase 4] Non-Blocking Save Engine (Zero-Lag Atomic Writes)

- [ ] **Prevent the "Micro Freeze" (`src/io_worker.c`)**:
  * Never run disk writes on the UI/input thread.
  * Use a dedicated POSIX thread pool worker triggered by a condition variable:
    ```c
    typedef struct {
        const char *filepath;
        Piece *buffer_snapshot; // Immutable snapshot of piece chain
        int is_saving;
    } SaveTask;
    ```
- [ ] **Atomic Swap Pattern**:
  1. Background thread writes sequentially across the Piece chain to `.filename.tmp.XXXXXX`.
  2. Issue `fsync()` on the temporary descriptor.
  3. Perform atomic swap via `renameat2(AT_FDCWD, tmp, AT_FDCWD, target, RENAME_NOREPLACE)` or `rename()`.
  4. Post an asynchronous completion message to the UI event loop (e.g., `[Saved in 1.2ms]`).

---

## [Phase 5] Modern Non-Modal Keybinds & OSC 52 Clipboard

- [ ] **Map Standard GUI Keybinds (`src/input.c`, `src/editor.c`)**:
  * Standard text navigation—zero vi/emacs modes.
  
  | Keybind | Function |
  | :--- | :--- |
  | `Ctrl + S` | Trigger background atomic save |
  | `Ctrl + Q` | Safe quit (double-press if unsaved changes exist) |
  | `Ctrl + Z` / `Ctrl + Y` | Instant Undo / Redo |
  | `Ctrl + C` / `Ctrl + X` | Copy / Cut selection to system clipboard |
  | `Ctrl + V` | Paste from system clipboard |
  | `Ctrl + A` | Select full buffer |
  | `Ctrl + F` | Incremental search prompt |
  | `Ctrl + H` | Find and Replace prompt |
  | `Ctrl + G` | Go-to line number |
  | `Ctrl + /` | Toggle single-line comment (`#`, `//`, `--`) |
  | `Shift + Arrows` | Create / adjust text selection highlight |
  | `Mouse Drag` | Visual selection by dragging |
  | `F1` | Show dynamic Help Cheat-sheet |

- [ ] **Clipboard System (`src/clipboard.c`)**:
  * Primary: Emit OSC 52 escape codes (`\033]52;c;<base64-data>\a`). Enables native clipboard over SSH, tmux, and local terminal emulators.
  * Fallback: Pipe into `wl-copy` (Wayland) or `xclip -selection clipboard` (X11) if OSC 52 is unacknowledged.

---

## [Phase 6] Double-Buffered Diff Renderer

- [ ] **Off-Screen Render Buffer (`src/render.c`)**:
  * Never call `printf` directly per character. Construct an in-memory frame buffer (`struct FrameBuffer`).
  * Compute dirty rows: Compare the newly constructed frame against the previous frame; only emit ANSI sequences for modified lines.
  * Append full payload to an internal string builder and flush to `STDOUT_FILENO` with a single `write()` call to eliminate flicker.
- [ ] **UI Layout Composition**:
  * Top: Optional file tab strip.
  * Left: Line numbers (dimmed color, auto-sizing width based on line count).
  * Center: Viewport buffer text + selection background highlighting.
  * Bottom: Status line (File path, dirty marker `*`, cursor line:col, file encoding, async save status toast).

---

## [Phase 7] Configuration Parser (`~/.config/kilo/kilo.conf`)

- [ ] **Zero-Dependency Flat Parser (`src/config.c`)**:
  * Parse standard `key = value` options without external libraries.
- [ ] **Default `kilo.conf` Template**:
  ```ini
  # ~/.config/kilo/kilo.conf

  [editor]
  theme = "base16_transparent"
  tab_size = 4
  expand_tabs = true
  line_numbers = true
  highlight_current_line = true
  scroll_offset = 3
  mouse_support = true

  [save]
  atomic_save = true
  trim_trailing_whitespace = true

  [clipboard]
  use_osc52 = true
  ```

---

## [Phase 8] Palette Engine & Theming

- [ ] **Transparent Terminal Integration (`src/theme.c`)**:
  * When a color value is `"none"`, emit `\033[49m` (default/transparent background), letting terminal opacity, acrylic, and blur show through cleanly.
- [ ] **Theme Definitions (`~/.config/kilo/themes/`)**:

  * **`base16_transparent.conf`**:
    ```ini
    [theme]
    name = "base16_transparent"
    background      = "none"
    foreground      = "#d8d8d8"
    cursor          = "#e8e8e8"
    selection       = "#383838"
    line_number     = "#585858"
    current_line_nr = "#f7ca88"
    keyword         = "#ba8baf"
    function        = "#7cafc2"
    string          = "#a1b56c"
    number          = "#d28445"
    comment         = "#747369"
    ```

  * **`helix_default.conf`**:
    ```ini
    [theme]
    name = "helix_default"
    background      = "#282a36"
    foreground      = "#f8f8f2"
    cursor          = "#ff79c6"
    selection       = "#44475a"
    line_number     = "#6272a4"
    current_line_nr = "#f1fa8c"
    keyword         = "#ff79c6"
    function        = "#50fa7b"
    string          = "#f1fa8c"
    number          = "#bd93f9"
    comment         = "#6272a4"
    ```

  * **`unix_amber.conf`**:
    ```ini
    [theme]
    name = "unix_amber"
    background      = "#120a00"
    foreground      = "#ffb000"
    cursor          = "#ffd700"
    selection       = "#4a3000"
    line_number     = "#805800"
    current_line_nr = "#ffc04d"
    keyword         = "#ffb000"
    string          = "#e69d00"
    comment         = "#664600"
    ```

---

## [Phase 9] Contextual Help Overlay (`F1`) & Syntax Tokens

- [ ] **Modal Floating Overlay Window (`src/help.c`)**:
  * Intercepts render stage and blits a centered ASCII frame without corrupting the underlying piece table buffer:
  ```text
  ┌─── Kilo Help [kl] (Press ESC or F1 to close) ──────────────────────┐
  │                                                                    │
  │  Navigation                  Editing                               │
  │  Arrows        Move cursor   Ctrl+S       Save (non-blocking)      │
  │  Ctrl+Home/End Top / Bottom  Ctrl+Z / Y   Undo / Redo              │
  │  Ctrl+G        Go to line    Ctrl+C / V   Copy / Paste (OSC 52)    │
  │                              Ctrl+/       Toggle comment           │
  │  Selection                                                         │
  │  Shift+Arrows  Select text   Search                                │
  │  Mouse Drag    Highlight     Ctrl+F       Find                     │
  │  Ctrl+A        Select All    Ctrl+H       Find & Replace           │
  │                                                                    │
  │  Config: ~/.config/kilo/kilo.conf                                  │
  │  Themes: ~/.config/kilo/themes/                                    │
  └────────────────────────────────────────────────────────────────────┘
  ```
- [ ] **Streamlined Syntax Engine (`src/syntax.c`)**:
  * Avoid multi-megabyte Tree-sitter dependencies.
  * Use state-driven line tokenizers (Keywords, String Literals, Numerics, Block & Line Comments) for fast per-screen-line passes.

---

## [Phase 10] Validation, Memory Audits & Benchmarks

- [ ] **Latency Audit**:
  * Keystroke-to-screen render budget: **$<1.5\text{ ms}$**.
- [ ] **Memory Verification**:
  * Run `valgrind --leak-check=full --show-leak-kinds=all ./kilo test.c` to guarantee zero memory leaks on exit.
- [ ] **Stress Testing**:
  * Open a 100MB log file: verify file load is instantaneous via piece-table pointers and memory usage remains minimal.
  * Hold down typing keys inside the 100MB file while saving repeatedly (`Ctrl+S`): verify UI framerate remains locked at full speed without cursor stutter.
