#include "searcher.h"
#include "simd_search.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

#include <re2/re2.h>

static constexpr size_t MMAP_THRESHOLD    = 65536;
static constexpr size_t BINARY_PROBE_SIZE = 8192;

static void scan_lines(const char* data, size_t file_size,
                       const std::string& pattern,
                       const SearchOptions& opts,
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
        int  match_start = -1;
        int  match_len   = 0;

        if (opts.use_regex) {
            re2::StringPiece input(line_view.data(), line_view.size());
            re2::StringPiece submatch;
            if (opts.re->Match(input, 0, input.size(),
                               re2::RE2::UNANCHORED, &submatch, 1)) {
                matched     = true;
                match_start = static_cast<int>(submatch.data() - line_view.data());
                match_len   = static_cast<int>(submatch.size());
            }
        } else if (opts.case_insensitive) {
            // Thread-local buffer avoids per-line allocation.
            thread_local std::string lower_buf;
            lower_buf.resize(line_view.size());
            std::transform(line_view.begin(), line_view.end(),
                           lower_buf.begin(), ::tolower);
            auto pos = lower_buf.find(lower_pattern);
            if (pos != std::string::npos) {
                matched     = true;
                match_start = static_cast<int>(pos);
                match_len   = static_cast<int>(lower_pattern.size());
            }
        } else {
            const char* found = simd_find(line_view.data(), line_view.size(),
                                          pattern.data(), pattern.size());
            if (found) {
                matched     = true;
                match_start = static_cast<int>(found - line_view.data());
                match_len   = static_cast<int>(pattern.size());
            }
        }

        if (matched) {
            // When files_only is set, we only need match counts.
            // Skip allocating line content entirely.
            if (opts.files_only) {
                result.matches.push_back({line_num, "", 0, 0});
            } else {
                result.matches.push_back(
                    {line_num, std::string(line_view), match_start, match_len});
            }
        }

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

    // Regex should be pre-compiled by the caller and passed via opts.re.
    if (opts.use_regex && !opts.re) {
        close(fd);
        return false;
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

        scan_lines(buf.data(), actual, pattern, opts, lower_pattern, result);
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

        scan_lines(data, file_size, pattern, opts, lower_pattern, result);
        munmap(mapped, file_size);
    }

    return true;
}
