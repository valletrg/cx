# cx

**A fast trigram-indexed code search engine built as a Claude Code skill**

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)
![Language: C++23](https://img.shields.io/badge/Language-C%2B%2B23-orange.svg)

---

## Why cx exists

AI coding agents like Claude Code default to grep or ripgrep when searching codebases. That works fine on small projects, but on anything larger it scans every file on every query, wasting seconds per search and burning context tokens on irrelevant results.

cx keeps a persistent trigram index that narrows candidates before scanning. Instead of reading every file, it intersects precomputed posting lists and scans only the files that could possibly match. For an agent searching for a specific function or class name, that usually means scanning a handful of files instead of thousands.

cx ships as a Claude Code skill. Install it once and Claude Code automatically knows how to use it for any code search task.

**How fast is it?** That depends on the pattern. The more specific the search, the more files the index eliminates and the bigger the win. Searching for a common term like `std::vector` gives a smaller advantage. Searching for a specific symbol like `MemoryBuffer::getFile` gives a much larger one, which is the typical agent use case.

| Codebase | Pattern | Files | cx | ripgrep | Speedup |
|---|---|---|---|---|---|
| cx project | `std::vector` | 21 | 2.3ms | 4.7ms | 2x |
| Godot engine | `std::vector` | 6,500 | 12.7ms | 58.6ms | 4.6x |
| LLVM/Clang | `std::vector` | 8,498 | 62ms | 52ms | 0.8x |
| LLVM/Clang | `MemoryBuffer::getFile` | 8,498 | 13.7ms | 48.3ms | 3.5x |

The LLVM `std::vector` result is intentionally included. cx is not always faster. It's faster when the pattern is specific enough for the index to do real filtering, which is almost always the case for agent workflows.

---

## Installation

**AUR (Arch Linux):**

```bash
paru -S cx-search
```

**Manual build:**

```bash
git clone https://github.com/valletrg/cx
cd cx
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo ln -sf "$(pwd)/build/cx" /usr/local/bin/cx
sudo cp cx-init /usr/local/bin/cx-init
sudo chmod +x /usr/local/bin/cx-init
```

Requirements: CMake >= 3.20, Ninja, GCC 13+ or Clang 17+. RE2 and CLI11 are fetched automatically via CMake FetchContent.

---

## Quick start

```bash
# initialize index in your project
cd your-project/
cx-init

# search
cx "pattern" --json
cx "pattern" --files-only --json
cx "MyClass" --regex --json -t .cpp .h
```

---

## All flags

| Flag | Description |
|------|-------------|
| `pattern` | Search pattern (required) |
| `-p, --path <dir>` | Directory to search (default: `.`) |
| `-t, --type <ext>...` | File extensions to include, e.g. `-t .cpp .h` |
| `-i, --ignore-case` | Case-insensitive search |
| `-r, --regex` | Treat pattern as RE2 regular expression |
| `--json` | Output results as JSON array |
| `--files-only` | Output file paths and match counts only, no line content |
| `--limit N` | Return at most N results, sorted by density |
| `--threads N` | Worker thread count (default: CPU count) |
| `--reindex` | Build or rebuild the trigram index, then search |
| `--no-index` | Skip the index and scan all files directly |

---

## Claude Code skill setup

This is what cx was built for.

1. Copy the skill into your Claude skills directory:

   ```bash
   mkdir -p ~/.claude/skills/cx
   cp cx-skill/SKILL.md ~/.claude/skills/cx/SKILL.md
   ```

2. Claude Code will automatically use cx when searching your codebase. No configuration needed.

3. Initialize the index in any project before searching:

   ```bash
   cx-init
   ```

When Claude Code needs to find a function or symbol, instead of grepping every file it calls cx like this:

```bash
# locate relevant files first
cx "MyFunction" --files-only --json

# then drill into just those files
cx "MyFunction" --json -t .cpp .h --limit 5
```

The skill is read-only and only has access to the Bash tool. It cannot modify any files.

---

## How it works

On `--reindex`, cx walks the directory and extracts every overlapping 3-character sequence (trigram) from each file, building an inverted index stored in `.cx/`. Each trigram maps to a sorted list of file IDs that contain it.

At search time, the pattern is decomposed into trigrams, the posting lists are intersected to get a small candidate set, and only those candidates are scanned with mmap. Files that share no trigrams with the pattern are never opened.

Reindexing is fast (under 100ms on typical projects) so the agent can call it freely at the start of a session or after writing new files.

---

## Contributing

PRs welcome. Open issues for bugs or feature requests at [https://github.com/valletrg/cx](https://github.com/valletrg/cx).
