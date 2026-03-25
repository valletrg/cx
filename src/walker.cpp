#include "walker.h"

#include <fnmatch.h>
#include <string_view>
#include <unordered_set>

// Pre-classified gitignore patterns for fast matching.
struct GitignoreRules {
    std::unordered_set<std::string> dir_names;      // "build", ".git" — exact match
    std::vector<std::string>        dir_globs;       // "*.d" (directory pattern with glob chars)
    std::vector<std::string>        dir_paths;       // "packaging/pkg" (multi-component dir)
    std::vector<std::string>        path_patterns;   // patterns with / (non-dir)
    std::vector<std::string>        name_patterns;   // "*.o", "*.a" — basename match

    explicit GitignoreRules(const std::vector<std::string>& patterns) {
        for (const auto& pat : patterns) {
            if (pat.empty() || pat.front() == '#') continue;

            if (pat.back() == '/') {
                std::string dir_name = pat.substr(0, pat.size() - 1);
                if (dir_name.find('/') != std::string::npos) {
                    dir_paths.push_back(std::move(dir_name));
                } else if (dir_name.find_first_of("*?[") != std::string::npos) {
                    dir_globs.push_back(std::move(dir_name));
                } else {
                    dir_names.insert(std::move(dir_name));
                }
            } else if (pat.find('/') != std::string::npos) {
                path_patterns.push_back(pat);
            } else {
                name_patterns.push_back(pat);
            }
        }
    }

    bool empty() const {
        return dir_names.empty() && dir_globs.empty() && dir_paths.empty()
            && path_patterns.empty() && name_patterns.empty();
    }
};

// Fast check: should this directory be pruned?
// Only needs the directory's basename and relative path string.
static bool is_dir_ignored(const std::string& dirname,
                            const std::string& rel_str,
                            const GitignoreRules& rules) {
    // Exact directory name match (e.g. "build", ".git")
    if (rules.dir_names.count(dirname)) return true;

    // Directory glob patterns (e.g. "*.d/")
    for (const auto& glob : rules.dir_globs) {
        if (fnmatch(glob.c_str(), dirname.c_str(), 0) == 0)
            return true;
    }

    // Path-relative directory patterns (e.g. "packaging/pkg/")
    for (const auto& dp : rules.dir_paths) {
        if (rel_str == dp || rel_str.starts_with(dp + "/"))
            return true;
        if (fnmatch(dp.c_str(), rel_str.c_str(), FNM_PATHNAME) == 0)
            return true;
    }

    // Name patterns can also match directory names (e.g. "*.cache")
    for (const auto& np : rules.name_patterns) {
        if (fnmatch(np.c_str(), dirname.c_str(), 0) == 0)
            return true;
    }

    return false;
}

// Fast check: should this file be skipped?
static bool is_file_ignored(const std::string& basename,
                             const std::string& rel_str,
                             const GitignoreRules& rules) {
    // Basename patterns (e.g. "*.o", "*.a", "PLANS.md")
    for (const auto& np : rules.name_patterns) {
        if (fnmatch(np.c_str(), basename.c_str(), 0) == 0)
            return true;
    }

    // Path patterns (e.g. "docs/generated/*.html")
    for (const auto& pp : rules.path_patterns) {
        if (fnmatch(pp.c_str(), rel_str.c_str(), FNM_PATHNAME) == 0)
            return true;
    }

    return false;
}

// Compute relative path by stripping the root prefix.
// Safe when path comes from recursive_directory_iterator rooted at root.
static std::string fast_relative(const fs::path& path, const std::string& root_str) {
    std::string_view ps = path.native();
    if (ps.starts_with(root_str)) {
        ps.remove_prefix(root_str.size());
        if (!ps.empty() && ps.front() == '/') ps.remove_prefix(1);
    }
    return std::string(ps);
}

std::vector<fs::path> filter_gitignored(std::vector<fs::path> files,
                                         const fs::path& root,
                                         const std::vector<std::string>& patterns) {
    if (patterns.empty()) return files;
    GitignoreRules rules(patterns);
    const std::string root_str = root.string();
    files.erase(std::remove_if(files.begin(), files.end(), [&](const fs::path& p) {
        std::string rel = fast_relative(p, root_str);
        std::string base = p.filename().string();
        return is_file_ignored(base, rel, rules);
    }), files.end());
    return files;
}

std::vector<fs::path> collect_files(const fs::path& root, const WalkOptions& opts) {
    std::vector<fs::path> files;
    GitignoreRules rules(opts.gitignore_patterns);
    const bool has_rules = !rules.empty();
    const std::string root_str = root.string();

    auto it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied);

    for (; it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (has_rules) {
                std::string dirname = it->path().filename().string();
                std::string rel = fast_relative(it->path(), root_str);
                if (is_dir_ignored(dirname, rel, rules))
                    it.disable_recursion_pending();
            }
            continue;
        }

        if (!it->is_regular_file()) continue;

        if (has_rules) {
            std::string basename = it->path().filename().string();
            std::string rel = fast_relative(it->path(), root_str);
            if (is_file_ignored(basename, rel, rules))
                continue;
        }

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
