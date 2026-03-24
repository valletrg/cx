# cx

**A fast trigram-indexed code search engine built for use as a Claude subagent**

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)
![Language: C++23](https://img.shields.io/badge/Language-C%2B%2B23-orange.svg)

---

## Why cx exists

AI coding agents like Claude Code default to grep or ripgrep when searching codebases. That works on small projects, but on anything large it scans every file on every query, wasting seconds per search and burning context tokens on irrelevant results.

cx maintains a persistent trigram index that narrows candidates before scanning. Instead of reading every file, cx intersects precomputed posting lists and scans only the handful of files that could possibly match. On a 180,000-file codebase like LLVM, that means results in 20ms instead of 445ms.

| Codebase | Files | cx (indexed) | ripgrep |
|---|---|---|---|
| Small project | ~20 | 8.6ms | 10.2ms |
| LLVM source | 180,000 | 20ms | ~445ms |

---

## Installation

**AUR (Arch Linux):**

```bash
yay -S cx-search
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

Requirements: CMake ≥ 3.20, Ninja, GCC 13+ or Clang 17+. RE2 and CLI11 are fetched automatically via CMake FetchContent.

---

## Quick start

```bash
# Initialize index in your project
cd your-project/
cx-init

# Search
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
| `--files-only` | Output file paths and match counts only (no line content) |
| `--limit N` | Return at most N results, sorted by density |
| `--threads N` | Worker thread count (default: CPU count) |
| `--reindex` | Build or rebuild the trigram index, then search |
| `--no-index` | Skip the index and scan all files directly |

---

## Claude Code subagent setup

This is the primary use case cx was built for.

1. Copy `cx.md` to `~/.claude/agents/cx.md`:

   ```bash
   cp /usr/share/doc/cx-search/cx.md ~/.claude/agents/cx.md
   # or from source:
   cp cx.md ~/.claude/agents/cx.md
   ```

2. Claude Code will automatically delegate code search tasks to the cx agent.

3. The agent auto-initializes the index at the start of each session using `cx-init`.

**What it looks like in practice:** when Claude Code needs to find a function or symbol, instead of grepping every file, it dispatches the cx subagent, which runs:

```bash
cx "MyFunction" --files-only --json
```

gets back a ranked list of candidate files, then drills into only those:

```bash
cx "MyFunction" --json -t .cpp .h --limit 5
```

The cx agent is read-only and only has access to the `Bash` tool, it cannot modify any files.

---

## How it works

**Indexing:** `cx --reindex` walks the directory and extracts every overlapping 3-character sequence (trigram) from each file, building an inverted index stored in `.cx/`. Each trigram maps to the sorted list of file IDs that contain it.

**Searching:** The query pattern is decomposed into trigrams, the posting lists are intersected to get a small set of candidate files, and only those candidates are scanned with mmap. Files that share no trigrams with the pattern are never opened.

**Result:** File count barely affects search time. cx searches 180,000 files in 20ms because it only reads a fraction of them.

---

## Contributing

PRs welcome. Open issues for bugs or feature requests at [https://github.com/valletrg/cx](https://github.com/valletrg/cx).
