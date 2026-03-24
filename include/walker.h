#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct WalkOptions {
    std::vector<std::string> extensions;
    // Gitignore patterns parsed from <root>/.gitignore.
    // Supports: exact paths, *.ext globs, and directory names ending in '/'.
    std::vector<std::string> gitignore_patterns;
};

std::vector<fs::path> collect_files(const fs::path& root, const WalkOptions& opts);

// Remove paths from `files` that match any of the gitignore patterns.
// Used to post-filter results that came from the trigram index.
std::vector<fs::path> filter_gitignored(std::vector<fs::path> files,
                                         const fs::path& root,
                                         const std::vector<std::string>& patterns);
