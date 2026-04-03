#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// WalkOptions drives file collection: which extensions to include and
// which gitignore-style patterns to skip.
struct WalkOptions {
    std::vector<std::string> extensions;
    std::vector<std::string> gitignore_patterns;
};

// Parse a .gitignore file at `root/.gitignore` and return its raw patterns.
// Returns an empty vector if the file doesn't exist or is unreadable.
std::vector<std::string> load_gitignore(const fs::path& root);

// Collect files recursively under `root`, applying extension filters and
// gitignore rules.  Used both for searching (no index) and for index building.
std::vector<fs::path> collect_files(const fs::path& root, const WalkOptions& opts);

// Given a list of absolute file paths, remove any that match the provided
// gitignore patterns.  Used after query_index returns candidate files that
// may have been indexed under different rules.
std::vector<fs::path> filter_gitignored(std::vector<fs::path> files,
                                         const fs::path& root,
                                         const std::vector<std::string>& patterns);
