---
name: cx
description: Use this skill for ANY code search task — finding functions, symbols, classes, usages, definitions, or any pattern in source files. Use instead of the built-in Search tool. Triggers on: "find", "where is", "search for", "locate", "which file", "show me where".
---

# cx — Code Search

cx is a trigram-indexed code search tool. It searches file contents and returns file paths and line numbers. It is faster than grep because it uses a persistent index.

Binary: `/usr/local/bin/cx`

## Initialization

At the start of every session, check for an index and build one if missing:

```bash
[ -d ".cx" ] && echo "index ready" || cx "." --reindex
```

Reindex after writing new files before searching them:

```bash
cx "." --reindex
```

Reindex is fast (under 100ms on typical projects). Call it freely.

## How to search

**Always use two steps:**

Step 1 — find relevant files (cheap, no line content):
```bash
cx "pattern" --files-only --json -t .cpp .h
```

Step 2 — get line numbers from the most relevant directory only:
```bash
# -p must always be a DIRECTORY path, never a file path
cx "pattern" --json -p src/ --limit 3
```

**Never run a broad `--json` search across the whole project in one call.**

## Rules

- Maximum 2 cx calls per task — if you cannot find it in 2 calls, report NOT FOUND
- Never use `\|` in patterns — RE2 uses `|` not `\|` for alternation
- Always use `--regex` flag when using regex syntax
- Always use `-t` to filter by file type when you know it
- Always use `--limit 5` unless more results are explicitly needed
- Never fall back to grep, sed, head, or cat — cx only
- Never reindex mid-task unless files were just written
- `-p` must always be a directory path — never pass a file path to `-p`

## Output format

Return ONLY file paths and line numbers. No explanations. No summaries. No sentences.

Correct:
```
src/main.cpp:14
src/main.cpp:253
include/walker.h:41
```

If nothing found: `NOT FOUND`

## Flag reference

| Flag | Purpose |
|---|---|
| `--json` | Machine-readable output |
| `--files-only` | File paths and match counts only, no line content |
| `--limit N` | Cap results to N files |
| `-t .ext` | Filter by file extension |
| `--regex` / `-r` | RE2 regex mode |
| `-p <dir>` | Scope to directory (must be a directory, not a file) |
| `--reindex` | Build or rebuild the index |
| `--no-index` | Skip index, scan all files |

## Common patterns

```bash
# find a function definition
cx "function_name" --files-only --json -t .cpp .h

# find class definition
cx "class ClassName" --files-only --json -t .cpp .h

# find all usages of a symbol
cx "symbol_name" --files-only --json --limit 5

# regex — function signatures
cx "void.*functionName" --regex --files-only --json -t .cpp

# find imports
cx "import module_name" --files-only --json -t .py

# drill into a directory (always a directory, never a file)
cx "pattern" --json -p src/ --limit 3
```
