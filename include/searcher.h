#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct Match {
    int line;
    std::string content;
    int match_start{-1};  // byte offset of match in content, -1 if unknown
    int match_len{0};
};

struct FileResult {
    fs::path file;
    std::vector<Match> matches;
    int total_lines{0};
    double density{0.0};
};

struct SearchOptions {
    bool use_regex{false};
    bool case_insensitive{false};
};

// Search a single file. Clears and fills `result` in place so the caller can
// reuse the Match vector's allocation across files (one per worker thread).
// Returns false on I/O error or binary file (result.matches will be empty).
bool search_file(const fs::path& path,
                 const std::string& pattern,
                 const SearchOptions& opts,
                 FileResult& result);
