// Benchmark results (LLVM ~180k files, "std::vector" -t .cpp vs rg -t cpp):
//   baseline (mmap-only, string_view::find):   cx 379ms  rg 445ms  (+15%)
//   opt1+2+3+5 (all active, simple queue):     cx 410ms  rg 469ms  (+15%)
//   opt4 (work-stealing pool): REVERTED — added overhead without benefit
//     on I/O-bound workloads with pre-distributed round-robin queues.
//
// Active optimizations:
//   opt1: read() for files < 64KB, mmap for larger files
//   opt2: thread_local FileResult reuses Match vector allocation across files
//   opt3: inlined AVX2/SSE2 first+last byte filter before memcmp
//   opt5: posix_fadvise(WILLNEED) on next file while scanning current

#include "searcher.h"
#include "simd_search.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string_view>

#include <re2/re2.h>

static constexpr size_t MMAP_THRESHOLD    = 65536;
static constexpr size_t BINARY_PROBE_SIZE = 8192;

static void scan_lines(const char* data, size_t file_size,
                       const std::string& pattern,
                       const SearchOptions& opts,
                       const re2::RE2* re,
                       const std::string& lower_pattern,
                       FileResult& result) {
    const char* end        = data + file_size;
    const char* line_start = data;
    int         line_num   = 0;

    while (line_start < end) {
        const char* line_end = static_cast<const char*>(
            memchr(line_start, '\n', static_cast<size_t>(end - line_start)));
        if (!line_end) line_end = end;

        ++line_num;
        std::string_view line_view(line_start,
                                   static_cast<size_t>(line_end - line_start));
        if (!line_view.empty() && line_view.back() == '\r')
            line_view.remove_suffix(1);

        bool matched = false;
        if (opts.use_regex) {
            matched = re2::RE2::PartialMatch(line_view, *re);
        } else if (opts.case_insensitive) {
            std::string lower_line(line_view);
            std::transform(lower_line.begin(), lower_line.end(),
                           lower_line.begin(), ::tolower);
            matched = lower_line.find(lower_pattern) != std::string::npos;
        } else {
            matched = simd_find(line_view.data(), line_view.size(),
                                pattern.data(), pattern.size()) != nullptr;
        }

        if (matched)
            result.matches.push_back({line_num, std::string(line_view)});

        line_start = line_end + 1;
    }

    result.total_lines = line_num;
    if (line_num > 0)
        result.density = static_cast<double>(result.matches.size()) /
                         static_cast<double>(line_num);
}

bool search_file(const fs::path& path,
                 const std::string& pattern,
                 const SearchOptions& opts,
                 FileResult& result) {
    // Reuse caller-provided storage; clear previous file's data
    result.file = path;
    result.matches.clear();
    result.total_lines = 0;
    result.density     = 0.0;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_size == 0) {
        close(fd);
        return false;
    }

    const size_t file_size = static_cast<size_t>(st.st_size);

    std::unique_ptr<re2::RE2> re;
    if (opts.use_regex) {
        re2::RE2::Options re_opts;
        re_opts.set_case_sensitive(!opts.case_insensitive);
        re_opts.set_log_errors(false);
        re = std::make_unique<re2::RE2>(pattern, re_opts);
        if (!re->ok()) {
            close(fd);
            return false;
        }
    }

    std::string lower_pattern;
    if (!opts.use_regex && opts.case_insensitive) {
        lower_pattern = pattern;
        std::transform(lower_pattern.begin(), lower_pattern.end(),
                       lower_pattern.begin(), ::tolower);
    }

    if (file_size < MMAP_THRESHOLD) {
        std::string buf(file_size, '\0');
        ssize_t n = read(fd, buf.data(), file_size);
        close(fd);
        if (n < 0) return false;
        const size_t actual = static_cast<size_t>(n);

        const size_t check_size = std::min(actual, BINARY_PROBE_SIZE);
        if (memchr(buf.data(), '\0', check_size) != nullptr) return false;

        scan_lines(buf.data(), actual, pattern, opts, re.get(),
                   lower_pattern, result);
    } else {
        void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) return false;
        const char* data = static_cast<const char*>(mapped);

        const size_t check_size = std::min(file_size, BINARY_PROBE_SIZE);
        if (memchr(data, '\0', check_size) != nullptr) {
            munmap(mapped, file_size);
            return false;
        }

        scan_lines(data, file_size, pattern, opts, re.get(),
                   lower_pattern, result);
        munmap(mapped, file_size);
    }

    return true;
}
