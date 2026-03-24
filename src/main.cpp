#include <print>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <format>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <CLI/CLI.hpp>

#include "walker.h"
#include "searcher.h"
#include "index.h"
#include "threadpool.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Gitignore parsing
// ---------------------------------------------------------------------------

static std::vector<std::string> parse_gitignore(const fs::path& root) {
    std::vector<std::string> patterns;
    std::ifstream f(root / ".gitignore");
    if (!f) return patterns;
    std::string line;
    while (std::getline(f, line)) {
        // Trim trailing whitespace
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                                  line.back() == '\r'))
            line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        patterns.push_back(std::move(line));
    }
    return patterns;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20 || c >= 0x80) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

static void print_human(const std::vector<FileResult>& results, bool files_only) {
    for (const auto& r : results) {
        if (r.matches.empty()) continue;
        if (files_only) {
            std::println("{}: {} matches", r.file.string(), r.matches.size());
        } else {
            for (const auto& m : r.matches)
                std::println("{}:{}:{}", r.file.string(), m.line, m.content);
        }
    }
}

static void print_json(const std::vector<FileResult>& results, bool files_only) {
    std::string out = "[\n";
    bool first = true;
    for (const auto& r : results) {
        if (r.matches.empty()) continue;
        if (!first) out += ",\n";
        first = false;

        out += "  {\n";
        out += std::format("    \"file\": \"{}\",\n",
                           json_escape(r.file.string()));
        out += std::format("    \"match_count\": {},\n", r.matches.size());
        out += std::format("    \"density\": {:.6f}", r.density);

        if (!files_only) {
            out += ",\n    \"matches\": [\n";
            bool fm = true;
            for (const auto& m : r.matches) {
                if (!fm) out += ",\n";
                fm = false;
                out += std::format(
                    "      {{ \"line\": {}, \"content\": \"{}\" }}",
                    m.line, json_escape(m.content));
            }
            out += "\n    ]";
        }

        out += "\n  }";
    }
    out += "\n]\n";
    std::print("{}", out);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    CLI::App app{"cx — fast code search"};

    std::string pattern;
    app.add_option("pattern", pattern, "Search pattern")->required();

    std::string search_path = ".";
    app.add_option("-p,--path", search_path, "Directory to search (default: .)");

    std::vector<std::string> extensions;
    app.add_option("-t,--type", extensions,
                   "File extensions to include (e.g. .cpp .h)")
        ->allow_extra_args()
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);

    bool case_insensitive = false;
    app.add_flag("--ignore-case,-i", case_insensitive, "Case-insensitive search");

    bool use_regex = false;
    app.add_flag("--regex,-r", use_regex,
                 "Treat pattern as RE2 regular expression");

    bool json_output = false;
    app.add_flag("--json", json_output, "Output results as JSON");

    bool files_only = false;
    app.add_flag("--files-only", files_only,
                 "Output file paths and match counts only");

    int limit = 0; // 0 = no limit
    app.add_option("--limit", limit,
                   "Maximum number of files to return (by density)");

    bool reindex = false;
    app.add_flag("--reindex", reindex,
                 "Build or rebuild the trigram index, then search");

    bool no_index = false;
    app.add_flag("--no-index", no_index,
                 "Skip the trigram index and scan all files directly");

    const unsigned int hw = std::thread::hardware_concurrency();
    int n_threads = static_cast<int>(hw > 0 ? hw : 1);
    app.add_option("--threads", n_threads,
                   std::format("Worker thread count (default: {})", n_threads));

    CLI11_PARSE(app, argc, argv);

    if (n_threads < 1) n_threads = 1;

    const fs::path root(search_path);

    // Parse .gitignore from the search root (if present)
    const auto gitignore = parse_gitignore(root);

    // --- Phase 3: index management ---
    if (reindex) {
        WalkOptions idx_opts{.extensions = extensions,
                             .gitignore_patterns = gitignore};
        if (!build_index(root, idx_opts)) {
            std::cerr << "[cx] index build failed\n";
            return 1;
        }
        start_index_watcher(root, idx_opts);
    }

    // --- Collect candidate files ---
    std::vector<fs::path> files;

    // Regex patterns can't be narrowed by trigrams — scan all files.
    // Literal patterns use the trigram index when available.
    const fs::path lookup_path = root / ".cx" / "lookup";
    if (!use_regex && !no_index && fs::exists(lookup_path)) {
        files = filter_gitignored(query_index(root, pattern), root, gitignore);
        // Apply extension filter: index may have been built with different -t
        // options, so we must filter here too.
        if (!extensions.empty()) {
            files.erase(std::remove_if(files.begin(), files.end(),
                [&](const fs::path& p) {
                    const std::string ext = p.extension().string();
                    return std::find(extensions.begin(), extensions.end(), ext)
                           == extensions.end();
                }), files.end());
        }
    } else {
        if (!use_regex && !no_index && !reindex && !fs::exists(lookup_path))
            std::cerr << "[cx] no index found, run with --reindex to build one\n";
        WalkOptions walk_opts{.extensions = extensions,
                              .gitignore_patterns = gitignore};
        files = collect_files(root, walk_opts);
    }

    // --- Search files in parallel ---
    SearchOptions sopts{.use_regex = use_regex,
                        .case_insensitive = case_insensitive};
    std::vector<FileResult> results;
    results.reserve(files.size());
    std::mutex results_mtx;

    {
        // Each worker owns a FileResult whose Match vector is reused across
        // files (Opt 2). thread_local gives each thread its own copy.
        ThreadPool pool(static_cast<size_t>(n_threads),
                        [&](const fs::path& f, const fs::path* next) {
                            // Opt 5: prefetch next file into page cache while
                            // we scan the current one.
                            if (next) {
                                int pfd = open(next->c_str(), O_RDONLY);
                                if (pfd >= 0) {
                                    posix_fadvise(pfd, 0, 0, POSIX_FADV_WILLNEED);
                                    close(pfd);
                                }
                            }

                            thread_local FileResult tl_result;
                            search_file(f, pattern, sopts, tl_result);
                            if (!tl_result.matches.empty()) {
                                // Move file+density fields; steal matches
                                // vector's heap allocation in O(1), leaving
                                // tl_result.matches empty to reuse next file.
                                FileResult out;
                                out.file        = tl_result.file;
                                out.total_lines = tl_result.total_lines;
                                out.density     = tl_result.density;
                                out.matches     = std::move(tl_result.matches);
                                std::lock_guard<std::mutex> lock(results_mtx);
                                results.push_back(std::move(out));
                            }
                        });

        for (const auto& f : files)
            pool.enqueue(f);

        pool.wait();
    } // pool destructor joins all workers before we sort

    // Sort by density descending (most dense first)
    std::sort(results.begin(), results.end(),
              [](const FileResult& a, const FileResult& b) {
                  return a.density > b.density;
              });

    // Apply limit
    if (limit > 0 && static_cast<int>(results.size()) > limit)
        results.resize(static_cast<size_t>(limit));

    // --- Output ---
    if (json_output)
        print_json(results, files_only);
    else
        print_human(results, files_only);

    return 0;
}
