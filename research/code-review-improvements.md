# cx — Code Review & Improvements Research

**Review Date:** April 2, 2026
**Last Updated:** April 2, 2026 (post implementation)
**Project:** ~/projects/cx
**Language:** C++23

---

## Implementation Summary ✅

The following quick wins from this document have been **implemented** (commit `73b0700`):

| # | Quick Win | Status | Notes |
|---|-----------|--------|-------|
| 1 | Pre-compile regex once | ✅ Done | Moved from per-file `SearchOptions` to `main()`, pass via `.re` pointer |
| 2 | TLS scratch buffer for case-insensitive search | ✅ Done | Replaced per-line `std::string` allocation with `thread_local` buffer |
| 3 | Skip content capture in files-only mode | ✅ Done | Added `.files_only` to `SearchOptions`, avoids `std::string(line_view)` |
| 4 | Centralize gitignore parsing | ✅ Done | Moved `parse_gitignore()` from `main.cpp` → `load_gitignore()` in `walker.cpp/h` |
| 5 | TLS invariant assert | ✅ Done | Added `assert(tl_result.matches.empty())` before `search_file()` |

**Files modified:** `include/searcher.h`, `include/walker.h`, `src/main.cpp`, `src/searcher.cpp`, `src/walker.cpp`

## Remaining Quick Wins (not implemented)

| # | Improvement | Effort | Reason deferred |
|---|-------------|--------|----------------|
| 6 | UTF-8 JSON escaping fix | Low | Needs downstream validation (Claude handles raw UTF-8 but worth testing) |
| 7 | RE2 version bump | Medium | Risky — may need Abseil dependency update, test suite changes |
| 8 | Dead code cleanup | Low | Cosmetic, won't merge cleanly without coordination |

## P1/P2 items (not done)
- SIMD character classification for trigram extraction
- Replace `unordered_set` with sorted vectors for trigram dedup
- More sophisticated SIMD find for longer patterns

---

## Project Overview

**What it is:** Fast trigram-indexed code search engine built as a Claude Code skill.

**Architecture:**
- Trigram index stored in `.cx/` directory
- Posting lists compressed with FastPFor (VariableByte + FastPFor)
- SIMD-optimized string search (AVX2/SSE2 fallback)
- Parallel file scanning via thread pool with prefetching
- External-sort index builder for bounded memory usage

**Key Files:**
- `src/main.cpp` (407 lines) - CLI, output formatting, orchestration
- `src/index.cpp` (897 lines) - Index build, K-way merge, inotify watcher
- `src/searcher.cpp` (169 lines) - Per-file search with line scanning
- `src/walker.cpp` (161 lines) - File collection, gitignore filtering
- `src/simd_search.h` (100 lines) - Header-only SIMD find implementation
- `src/threadpool.cpp` - Thread pool with work distribution

---

## 🔍 Search Speed Improvements

### 1. Regex Recompile Per File [HIGH IMPACT]
**Location:** `searcher.cpp:120-130`
```cpp
std::unique_ptr<re2::RE2> re;
if (opts.use_regex) {
    re2::RE2::Options re_opts;
    re_opts.set_case_sensitive(!opts.case_insensitive);
    re_opts.set_log_errors(false);
    re = std::make_unique<re2::RE2>(pattern, re_opts);
    if (!re->ok()) return false;
}
```

**Problem:** RE2 compiled fresh for every single file. RE2 compilation involves NFA/DFA construction which isn't cheap. On a project with 10k files, that's 10k compilations of the same pattern.

**Fix:** Pre-compile once in `main()`, compile into the `SearchOptions` struct, pass to worker threads. `SearchOptions` should own the optional `std::unique_ptr<re2::RE2>`.

**Expected improvement:** 10-30% faster regex searches, proportional to file count.

### 2. Case-Insensitive Search Allocates Per Line [HIGH IMPACT]
**Location:** `searcher.cpp:68-70`
```cpp
std::string lower_line(line_view);
std::transform(lower_line.begin(), lower_line.end(),
               lower_line.begin(), ::tolower);
auto pos = lower_line.find(lower_pattern);
```

**Problem:** Allocates a new `std::string` for every single line during case-insensitive search. On a 1000-line file, that's 1000 heap allocations.

**Fix:** Use a `thread_local` string buffer that's reused across files. Clear and resize instead of reallocating. Pre-convert the pattern to lowercase once in `main()`.

```cpp
// In worker lambda:
thread_local std::string scratch_buffer;
scratch_buffer.assign(line_view);
std::transform(scratch_buffer.begin(), scratch_buffer.end(),
               scratch_buffer.begin(), ::tolower);
```

**Expected improvement:** 15-25% faster case-insensitive searches on moderately sized files.

### 3. SIMD Find Optimization [MEDIUM IMPACT]
**Location:** `include/simd_search.h`

**Current:** Only checks first and last byte in SIMD lanes:
```cpp
const __m256i first = _mm256_set1_epi8(needle[0]);
const __m256i last  = _mm256_set1_epi8(needle[needle_len - 1]);
```

**Observation:** For short patterns (2-4 chars), this is efficient. For longer patterns (>8 chars), many false positives pass the first+last byte check but fail in the expensive `memcmp`.

**Potential improvements:**
- For patterns ≥4: Check first two bytes AND last two bytes using `_mm256_packs_epi8`
- For patterns ≥8: Use a 4-byte SIMD check (first 4, last 4)
- Use `_mm256_loadu_si256` for aligned loads when possible

**Trade-off:** Adds complexity and register pressure. Profile before implementing — the first/last byte check already eliminates ~99.9% of positions for typical English text.

### 4. Files-Only Mode Still Captures Content [MEDIUM IMPACT]
**Location:** `searcher.cpp:88`
```cpp
result.matches.push_back({line_num, std::string(line_view), match_start, match_len});
```

**Problem:** Even with `--files-only`, the searcher still:
- Copies the entire line content into `std::string`
- Stores match offsets
- Then discards this data in the output phase

**Fix:** Add a flag to `SearchOptions` to skip content capture. Only record line numbers and match counts when `--files-only` is used. Consider not even tracking `match_start`/`match_len` in this mode.

**Expected improvement:** 20-40% less memory per file, faster output, potentially better cache utilization.

### 5. Line Splitting Overhead [LOW-MEDIUM IMPACT]
**Location:** `searcher.cpp:42-50`

**Current approach:** Processes line-by-line with `memchr`. This means:
- Multiple `memchr` calls per file
- No SIMD acceleration for line splitting
- For very large lines (>64KB), still processes them as one unit

**Fix:** Consider pre-splitting with SIMD `cmpeq` + `movemask` on newlines, then iterating over the resulting bitset. For most code files (short lines), the current approach is fine. Benefit only shows on minified/compiled files.

---

## ⚡ Indexing Speed Improvements

### 1. SIMD Character Classification [MEDIUM IMPACT]
**Location:** `index.cpp` lines 96-104
```cpp
for (size_t j = 0; j < line_len; ++j) {
    uint8_t c = static_cast<uint8_t>(line_start[j]);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z')) {
        has_alnum = true;
        break;
    }
}
```

**Problem:** Scalar character-by-character check to determine if line has alphanumeric characters. For typical code files with dense alphanumeric content, this returns quickly. But for files with lots of whitespace/comments, this loops needlessly.

**Fix:** SIMD character classification using `_mm256_cmpgt_epi8` with lookup table for `isalpha`/`isalnum`. Process 32 bytes at a time. Since you're already using AVX2/SSE2 for search, this fits naturally.

**Expected improvement:** 15-30% faster line classification in files with low alphanumeric density.

### 2. Replace unordered_set with Sorted Vector for Trigrams [MEDIUM IMPACT]
**Location:** 
- `index.cpp:82-118` - `extract_trigrams()`
- `index.cpp:71` - `trigram_to_files` map

**Problem:** `std::unordered_set<uint32_t>` for collecting unique trigrams per file:
- Hash computation overhead (though it's the identity for uint32_t)
- Node allocations for each element
- Poor cache locality

**Fix:** For typical source files (<500 unique trigrams), use:
```cpp
std::vector<uint32_t> trigrams;
trigrams.reserve(line_len); // overestimate
// ... push all trigrams (with duplicates) ...
std::sort(trigrams.begin(), trigrams.end());
trigrams.erase(std::unique(trigrams.begin(), trigrams.end()), trigrams.end());
```

**Benchmark justification:** 
- Sorting N elements: O(N log N)
- unordered_set insertion: O(N) average, but with much higher constant due to allocations and cache misses
- For N < 1000, sort+unique wins 3-5x on modern CPUs

**Expected improvement:** 20-40% faster trigram extraction per file.

### 3. Chunked memchr for Large Files [LOW IMPACT]
**Location:** `index.cpp` lines 482-486

**Current:** Repeated `memchr` calls to find newlines:
```cpp
const char* line_end = static_cast<const char*>(
    memchr(line_start, '\n', static_cast<size_t>(file_end - line_start))));
```

**Observation:** For minified JS/TS files or very long generated files, the distance between newlines can be large. `memchr` is already highly optimized (glibc uses SIMD), so this is probably fine. But for extremely long lines (>1MB), consider processing in chunks to avoid scanning the same data multiple times.

**Trade-off:** Almost certainly not worth the complexity for code search workloads. Skip unless profiling shows otherwise.

### 4. Parallel File List Writing [LOW IMPACT]
**Location:** `index.cpp:140-144`
```cpp
std::ofstream f(index_dir / kFilesName, std::ios::trunc);
for (const auto& p : g_index.files)
    f << p << '\n';
```

**Observation:** Sequential file writing during index serialization. For very large projects (100k+ files), this dominates write time.

**Fix:** Collect file paths in thread-local buffers, then parallel write using `pwrite` to pre-allocated file. Use `posix_fadvise(POSIX_FADV_SEQUENTIAL)` on the output file.

**Expected improvement:** Only matters for projects with >50k files.

---

## 🧹 Code Cleanup & Quality

### 1. Centralize Gitignore Parsing [STRUCTURAL]
**Locations:** 
- `main.cpp:82-96` - `parse_gitignore()`
- `walker.cpp:8-40` - `GitignoreRules` constructor

**Problem:** Gitignore parsing logic split across two files. `main.cpp` parses lines, `walker.cpp` classifies patterns. This creates duplication and makes it hard to evolve the parsing logic.

**Fix:** Move all gitignore logic to `walker.cpp/h`:
```cpp
// walker.h
std::vector<std::string> load_gitignore(const fs::path& root);
GitignoreRules make_gitignore_rules(const std::vector<std::string>& patterns);
```

Remove `parse_gitignore` from `main.cpp`. This consolidates all path-matching intelligence in one place.

### 2. Dead Code & Comment Cleanup [MAINTENANCE]
**Locations:**
- `searcher.cpp:1-6` (stale optimization notes)
- `main.cpp:2-49` (review pass annotations)
- `simd_search.cpp` (3-line file pointing to header)

**Issues:**
1. `searcher.cpp` header mentions "opt4 (work-stealing pool): REVERTED" — the code isn't there, it's just noise
2. The review pass comments in `main.cpp` are valuable documentation but should be moved to `docs/REVIEW_NOTES.md` or similar to keep `main.cpp` focused
3. `simd_search.cpp` is a 3-line stub that adds compilation time without benefit

**Fix:**
- Move `searcher.cpp` header to `docs/OPTIMIZATION_HISTORY.md`
- Consider removing `simd_search.cpp` and including the header directly in searcher's compilation unit
- Keep review notes but separate from runtime code

### 3. Silent Error Handling [RELIABILITY]
**Patterns throughout:**
```cpp
if (fd < 0) return false;
if (fstat(fd, &st) < 0 || st.st_size == 0) { close(fd); return false; }
if (addr == MAP_FAILED) return false;
```

**Problem:** Silent failures make debugging difficult. When a file isn't found in results, it's unclear why:
- Doesn't exist? Permission denied? Binary file? mmap failed?

**Fix:** Add a debug logging mechanism (compile-time flag or environment variable):
```cpp
#ifdef CX_DEBUG
#define TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define TRACE(...)
#endif
```

Usage:
```cpp
if (fd < 0) {
    TRACE("[search] open failed: %s (%s)\n", path.c_str(), strerror(errno));
    return false;
}
```

**Alternative:** Return a `SearchResult` enum or error code instead of bool.

### 4. Thread-Local Storage Fragility [SAFETY]
**Location:** `main.cpp:368`
```cpp
thread_local FileResult tl_result;
```

**Concern:** The code relies on the invariant that `tl_result.matches` is empty after `search_file()` returns. This is true because of the `std::move()` on line 378, but it's fragile:
- If `search_file()` throws, the invariant breaks
- If the move doesn't happen (e.g., future code changes), matches accumulate

**Fix:** Add defensive assertion:
```cpp
thread_local FileResult tl_result;
assert(tl_result.matches.empty());  // Invariant check
search_file(f, pattern, sopts, tl_result);
```

### 5. JSON Escaping Over-Escapes UTF-8 [EFFICIENCY]
**Location:** `main.cpp:113-118`
```cpp
default:
    if (c < 0x20 || c >= 0x80) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
        out += buf;
    }
```

**Problem:** Escapes all bytes >= 0x80 as `\uXXXX`. For UTF-8 codepoints, this turns 2-4 bytes of raw UTF-8 into 6+ escaped bytes. JSON RFC 8259 explicitly states UTF-8 is valid.

**Fix:** Only escape control characters (< 0x20), pass through UTF-8:
```cpp
if (c < 0x20) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
    out += buf;
} else {
    out += static_cast<char>(c);
}
```

**Caveat:** This assumes downstream consumers handle UTF-8 properly. For Claude/LLM consumption, this is fine — they understand UTF-8.

### 6. RE2 Version Is Outdated [MAINTENANCE]
**Location:** `CMakeLists.txt:22-23`
```cmake
GIT_REPOSITORY https://github.com/google/re2.git
GIT_TAG        2022-12-01
```

**Problem:** That's ~3.5 years old. Since then, RE2 has received:
- Significant performance improvements
- Better error messages
- Security fixes
- API updates

**Fix:** Bump to `2024-07-02` or latest stable. Test build compatibility — RE2 started requiring Abseil in newer versions, so you may need to update your build configuration:
```cmake
set(RE2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    re2
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG        2024-07-02  # or latest
)
FetchContent_MakeAvailable(re2)
```

### 7. Magic Constants Without Documentation
**Locations:** Various

**Unexplained values:**
- `kFlushThreshold = 4M pairs` (index.cpp:52) — Why 4M? 256MB peak with 8 threads?
- `kMinCompressSize = 4` (index.cpp:48) — Why 4?
- `kMaxIndexLineLen = 512` (index.cpp:56) — Why 512?

**Fix:** Add comments explaining the reasoning:
```cpp
// Posting lists smaller than 4 entries are stored uncompressed.
// FastPFor has ~100 bytes of overhead per call, making compression
// counterproductive for tiny lists.
static constexpr size_t kMinCompressSize = 4;
```

### 8. Inotify File Descriptor Leak [ACCEPTABLE WITH WARNING]
**Location:** `index.cpp` (inotify watcher)

**Current state:** "thread is detached, ifd leaks on normal exit. Acceptable for a short-lived CLI tool — kernel reclaims on _exit()"

**Note:** This is documented as intentional and reviewed. Keep it, but consider adding a cleanup path for long-running usage:
```cpp
// In destructor or exit handler:
if (ifd >= 0) {
    close(ifd);
    ifd = -1;
}
```

---

## 🎯 Priority Matrix

### P0 - Quick Wins (1-2 hours each)
1. ✅ Pre-compile regex once
2. ✅ Thread-local buffers for case-insensitive search
3. ✅ Skip content capture in files-only mode
4. ✅ Centralize gitignore parsing

### P1 - Moderate Effort (4-8 hours each)
5. 🔄 SIMD character classification for trigram extraction
6. 🔄 Replace unordered_set with sorted vectors
7. 🔄 Update RE2 version + test
8. 🔄 Fix UTF-8 JSON escaping

### P2 - Architecture Decisions (weeks)
9. 📋 More sophisticated SIMD find for longer patterns
10. 📋 Error handling strategy (Result<T> vs debug logging)
11. 📋 Thread-local storage cleanup

### Won't Fix (not worth it)
- Chunked memchr for line splitting — glibc is already SIMD-optimized
- Parallel file list writing — only matters for 100k+ file projects
- SIMD posting list intersection — FastPFor is already efficient

---

## Benchmark Targets

If implementing these, measure:
1. **Reindex speed:** Current <100ms on small projects, measure improvement on 100k+ file projects
2. **Search speed:** Current 2.3ms - 13.7ms depending on codebase/pattern
3. **Memory usage:** Peak memory during indexing (currently ~256MB with 8 threads)
4. **Cache behavior:** L1/L2/L3 miss rates (perf stat)

**Tools:**
- `perf stat ./cx "pattern" -t .cpp .h`
- `hyperfine -w 3 -n 10 'cx "pattern" --files-only --json -t .cpp .h'`
- `valgrind --tool=massif ./cx "pattern" --reindex`

---

## Related Work

**Similar projects to study:**
- `ripgrep` — Rust-based, uses SIMD regex matching (regex-automata crate)
- `sift` — Go-based, very fast but less feature-rich
- `ag/the_silver_searcher` — C-based, optimized for code search
- `pg` — Rust code search with AST indexing

**Techniques from literature:**
- "Fast Substring Search" (Boyer-Moore-Horspool variants)
- "SIMD-friendly String Matching" (vectorized Boyer-Moore)
- "Compressed Inverted Indexes" (FastPFor is good, consider Simple8b)

---

*This document should be updated as improvements are implemented and benchmarked.*
