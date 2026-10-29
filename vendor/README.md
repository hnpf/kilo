# Vendored Tree-sitter sources

Kilo embeds the Tree-sitter C runtime and generated language parsers. Language
highlight queries are installed under `$(PREFIX)/share/kilo/queries` and are
also loaded from the repository checkout during development.

Included languages: Markdown, Rust, C, C++, Go, Python, JavaScript, TypeScript,
TSX, HTML, CSS, JSON, QML (including Quickshell configs), INI-style config
files, YAML, TOML, Bash, Java, Ruby, PHP, and Lua. `.conf`, `.ini`, `.cfg`,
`.desktop`, and common systemd unit extensions use INI highlighting.
Parser updates must keep the grammar ABI compatible with the runtime. Source
licenses are retained beside each vendored project.

Pinned source revisions:

- Tree-sitter runtime: `ad067fdd`
- Markdown grammars: `41ff24a6`
- Rust: `77a3747`
- C: `b780e47`
- C++: `c009222`
- Go: `2346a3a`
- Python: `26855ea`
- JavaScript: `58404d8`
- JSON: `254c42a`
- TypeScript and TSX: `75b3874`
- HTML: `73a3947`
- CSS: `dda5cfc`
- QML: `de96ed6`
- INI: `e4018b5`
- YAML: `a1c4812`
- TOML: `64b5683`
- Bash: `a06c2e4`
- Java: `e10607b`
- Ruby: `ad907a6`
- PHP: `58d5643`
- Lua: `10fe005`
