#include <print>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <thread>
#include <cassert>
#include <memory>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <re2/re2.h>

#include "walker.h"
#include "searcher.h"
#include "index.h"
#include "threadpool.h"

namespace fs = std::filesystem;

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

static void print_human(const std::vector<FileResult>& results, bool files_only,
                        const fs::path& root) {
    const bool is_tty = isatty(STDOUT_FILENO) != 0;

    int term_width = 120;
    if (is_tty) {
        struct winsize w{};
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        if (w.ws_col > 0) term_width = static_cast<int>(w.ws_col);
    }

    const char* COL_FILENAME = is_tty ? "\033[1;36m" : "";
    const char* COL_LINENUM  = is_tty ? "\033[0;33m" : "";
    const char* COL_MATCH    = is_tty ? "\033[1;31m" : "";
    const char* COL_DIM      = is_tty ? "\033[2m"    : "";
    const char* COL_RESET    = is_tty ? "\033[0m"    : "";

    bool first_file = true;
    for (const auto& r : results) {
        if (r.matches.empty()) continue;

        std::error_code ec;
        fs::path rel = fs::relative(r.file, root, ec);
        if (ec) rel = r.file;
        const std::string rel_str = rel.string();

        if (files_only) {
            const char* word = r.matches.size() == 1 ? "match" : "matches";
            std::print("{}{}{}", COL_FILENAME, rel_str, COL_RESET);
            std::println("  {}{} {}{}", COL_DIM, r.matches.size(), word, COL_RESET);
        } else {
            if (!first_file) std::println("");
            first_file = false;

            std::println("{}{}{}", COL_FILENAME, rel_str, COL_RESET);

            for (const auto& m : r.matches) {
                // Content — truncate if needed (TTY only)
                std::string content = m.content;
                int mstart = m.match_start;
                int mlen   = m.match_len;

                if (is_tty) {
                    const int max_content = std::max(1, term_width - 8);
                    if (static_cast<int>(content.size()) > max_content) {
                        content = content.substr(0, static_cast<size_t>(max_content));
                        content += "\xe2\x80\xa6"; // UTF-8 '…'
                        if (mstart >= max_content) {
                            mstart = -1;
                        } else if (mstart + mlen > max_content) {
                            mlen = max_content - mstart;
                        }
                    }
                }

                // Emit: "  <linenum>  <content with highlight>"
                if (is_tty && mstart >= 0 && mlen > 0 &&
                    mstart + mlen <= static_cast<int>(content.size())) {
                    const std::string before = content.substr(0, static_cast<size_t>(mstart));
                    const std::string match  = content.substr(static_cast<size_t>(mstart),
                                                              static_cast<size_t>(mlen));
                    const std::string after  = content.substr(static_cast<size_t>(mstart + mlen));
                    std::print("  {}{:4}{}  {}{}{}{}{}\n",
                        COL_LINENUM, m.line, COL_RESET,
                        before, COL_MATCH, match, COL_RESET, after);
                } else {
                    std::print("  {}{:4}{}  {}\n",
                        COL_LINENUM, m.line, COL_RESET, content);
                }
            }
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

    // Validate regex early so we never crash inside worker threads.
    if (use_regex) {
        re2::RE2::Options re_opts;
        re_opts.set_case_sensitive(!case_insensitive);
        re_opts.set_log_errors(false); // suppress RE2's own stderr noise
        re2::RE2 probe(pattern, re_opts);
        if (!probe.ok()) {
            std::cerr << "[cx] invalid regex: " << probe.error() << "\n";
            if (json_output) std::print("[]\n");
            return 0;
        }
    }

    const fs::path root(search_path);

    // Parse .gitignore from the search root (if present)
    const auto gitignore = load_gitignore(root);

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

    // --- Pre-compile regex (if needed) so we compile once, not per-file ---
    std::unique_ptr<re2::RE2> re_compiled;
    if (use_regex) {
        re2::RE2::Options re_opts;
        re_opts.set_case_sensitive(!case_insensitive);
        re_opts.set_log_errors(false);
        re_compiled = std::make_unique<re2::RE2>(pattern, re_opts);
        // Already validated above — re_compiled is guaranteed ok() here.
    }

    // --- Search files in parallel ---
    SearchOptions sopts{.use_regex = use_regex,
                        .case_insensitive = case_insensitive,
                        .files_only = files_only,
                        .re = re_compiled.get()};
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
                            assert(tl_result.matches.empty());
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
        print_human(results, files_only, root);

    return 0;
}
