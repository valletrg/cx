#include "walker.h"

#include <fnmatch.h>

// Returns true if path should be excluded based on gitignore patterns.
// root is used to compute relative paths for slash-containing patterns.
static bool is_gitignored(const fs::path& path,
                           const fs::path& root,
                           const std::vector<std::string>& patterns) {
    if (patterns.empty()) return false;

    std::error_code ec;
    const fs::path rel     = fs::relative(path, root, ec);
    if (ec) return false;
    const std::string rel_str  = rel.string();
    const std::string basename = path.filename().string();

    for (const auto& pat : patterns) {
        if (pat.empty() || pat.front() == '#') continue;

        if (pat.back() == '/') {
            const std::string dir_name = pat.substr(0, pat.size() - 1);
            if (dir_name.find('/') != std::string::npos) {
                // Path-relative directory pattern e.g. "packaging/pkg/" —
                // match rel_str against the pattern path or any prefix of it.
                if (rel_str == dir_name || rel_str.starts_with(dir_name + "/"))
                    return true;
                if (fnmatch(dir_name.c_str(), rel_str.c_str(), FNM_PATHNAME) == 0)
                    return true;
            } else {
                // Simple name pattern e.g. "build/" — matches any component
                for (const auto& comp : rel)
                    if (fnmatch(dir_name.c_str(), comp.string().c_str(), 0) == 0)
                        return true;
            }
        } else if (pat.find('/') != std::string::npos) {
            // Slash in pattern → match against relative path
            if (fnmatch(pat.c_str(), rel_str.c_str(), FNM_PATHNAME) == 0)
                return true;
        } else {
            // No slash → match against basename only
            if (fnmatch(pat.c_str(), basename.c_str(), 0) == 0)
                return true;
        }
    }
    return false;
}

std::vector<fs::path> filter_gitignored(std::vector<fs::path> files,
                                         const fs::path& root,
                                         const std::vector<std::string>& patterns) {
    if (patterns.empty()) return files;
    files.erase(std::remove_if(files.begin(), files.end(), [&](const fs::path& p) {
        return is_gitignored(p, root, patterns);
    }), files.end());
    return files;
}

std::vector<fs::path> collect_files(const fs::path& root, const WalkOptions& opts) {
    std::vector<fs::path> files;
    const bool has_gitignore = !opts.gitignore_patterns.empty();

    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);

    for (; it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            // Skip gitignored directories entirely (prunes the recursion)
            if (has_gitignore &&
                is_gitignored(it->path(), root, opts.gitignore_patterns))
                it.disable_recursion_pending();
            continue;
        }

        if (!it->is_regular_file()) continue;

        if (has_gitignore &&
            is_gitignored(it->path(), root, opts.gitignore_patterns))
            continue;

        if (opts.extensions.empty()) {
            files.push_back(it->path());
            continue;
        }

        const std::string ext = it->path().extension().string();
        for (const auto& allowed : opts.extensions) {
            if (ext == allowed) {
                files.push_back(it->path());
                break;
            }
        }
    }

    return files;
}
