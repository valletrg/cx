#include "index.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// On-disk layout inside <root>/.cx/
//
//   files    — newline-separated file paths; line N (0-based) == file ID N
//   postings — concatenated posting lists: [uint32_t count, uint32_t id, ...]
//   lookup   — sorted array of LookupEntry (trigram_hash → offset in postings)
// ---------------------------------------------------------------------------

static constexpr const char* kIndexDir     = ".cx";
static constexpr const char* kFilesName    = "files";
static constexpr const char* kPostingsName = "postings";
static constexpr const char* kLookupName   = "lookup";

#pragma pack(push, 1)
struct LookupEntry {
    uint32_t trigram_hash;
    uint64_t offset;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// In-memory index (kept alive for the inotify watcher)
// ---------------------------------------------------------------------------

struct InMemoryIndex {
    std::vector<std::string>                          files;
    std::unordered_map<uint32_t, std::vector<uint32_t>> trigram_to_files;
    std::mutex                                        mtx;
};

// Single global instance; populated by build_index, updated by the watcher.
static InMemoryIndex g_index; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::unordered_set<uint32_t> extract_trigrams(std::string_view content) {
    std::unordered_set<uint32_t> out;
    if (content.size() < 3) return out;
    for (size_t i = 0; i + 2 < content.size(); ++i) {
        uint32_t t = (static_cast<uint32_t>(static_cast<uint8_t>(content[i]))     << 16) |
                     (static_cast<uint32_t>(static_cast<uint8_t>(content[i + 1])) <<  8) |
                      static_cast<uint32_t>(static_cast<uint8_t>(content[i + 2]));
        out.insert(t);
    }
    return out;
}

static std::string read_file_content(const fs::path& p) {
    int fd = open(p.c_str(), O_RDONLY);
    if (fd < 0) return {};
    struct stat st{};
    if (fstat(fd, &st) < 0 || st.st_size == 0) { close(fd); return {}; }
    std::string buf(static_cast<size_t>(st.st_size), '\0');
    ssize_t remaining = st.st_size;
    char* ptr = buf.data();
    while (remaining > 0) {
        ssize_t n = read(fd, ptr, static_cast<size_t>(remaining));
        if (n <= 0) break;
        ptr       += n;
        remaining -= n;
    }
    close(fd);
    return buf;
}

// Write g_index to disk. Caller must hold g_index.mtx.
static void write_index(const fs::path& index_dir) {
    // --- files list ---
    {
        std::ofstream f(index_dir / kFilesName, std::ios::trunc);
        for (const auto& p : g_index.files)
            f << p << '\n';
    }

    // Sort trigrams by hash for binary-searchable lookup
    std::vector<std::pair<uint32_t, std::vector<uint32_t>>> sorted;
    sorted.reserve(g_index.trigram_to_files.size());
    for (const auto& kv : g_index.trigram_to_files) {
        if (!kv.second.empty())
            sorted.push_back(kv);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // --- postings + lookup ---
    std::ofstream pf(index_dir / kPostingsName, std::ios::binary | std::ios::trunc);
    std::ofstream lf(index_dir / kLookupName,   std::ios::binary | std::ios::trunc);

    uint64_t offset = 0;
    for (const auto& [hash, ids] : sorted) {
        LookupEntry entry{hash, offset};
        lf.write(reinterpret_cast<const char*>(&entry), sizeof(entry));

        auto count = static_cast<uint32_t>(ids.size());
        pf.write(reinterpret_cast<const char*>(&count), sizeof(count));
        pf.write(reinterpret_cast<const char*>(ids.data()),
                 static_cast<std::streamsize>(ids.size() * sizeof(uint32_t)));

        offset += sizeof(uint32_t) + ids.size() * sizeof(uint32_t);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool build_index(const fs::path& root, const WalkOptions& opts) {
    const fs::path index_dir = root / kIndexDir;
    std::error_code ec;
    fs::create_directories(index_dir, ec);
    if (ec) {
        std::cerr << "[cx] failed to create index directory: " << ec.message() << '\n';
        return false;
    }

    auto files = collect_files(root, opts);

    // Filter out files that live inside our own .cx directory
    const std::string prefix = (root / kIndexDir).string();
    files.erase(std::remove_if(files.begin(), files.end(), [&](const fs::path& p) {
        return p.string().starts_with(prefix);
    }), files.end());

    std::lock_guard<std::mutex> lock(g_index.mtx);
    g_index.files.clear();
    g_index.trigram_to_files.clear();

    for (uint32_t id = 0; id < static_cast<uint32_t>(files.size()); ++id) {
        g_index.files.push_back(files[id].string());
        auto content  = read_file_content(files[id]);
        auto trigrams = extract_trigrams(content);
        for (uint32_t t : trigrams)
            g_index.trigram_to_files[t].push_back(id); // ids stay sorted (inserted ascending)
    }

    write_index(index_dir);
    return true;
}

std::vector<fs::path> query_index(const fs::path& root,
                                  const std::string& pattern) {
    const fs::path index_dir  = root / kIndexDir;
    const fs::path lookup_path   = index_dir / kLookupName;
    const fs::path postings_path = index_dir / kPostingsName;
    const fs::path files_path    = index_dir / kFilesName;

    if (!fs::exists(lookup_path)) return {};

    // Load file list
    std::vector<std::string> file_list;
    {
        std::ifstream f(files_path);
        std::string line;
        while (std::getline(f, line))
            if (!line.empty())
                file_list.push_back(line);
    }
    if (file_list.empty()) return {};

    // Short patterns can't be trigrammed — return everything
    if (pattern.size() < 3) {
        std::vector<fs::path> all;
        all.reserve(file_list.size());
        for (const auto& p : file_list) all.emplace_back(p);
        return all;
    }

    // Collect unique trigrams from the (literal) pattern
    std::vector<uint32_t> pat_trigrams;
    for (size_t i = 0; i + 2 < pattern.size(); ++i) {
        uint32_t t = (static_cast<uint32_t>(static_cast<uint8_t>(pattern[i]))     << 16) |
                     (static_cast<uint32_t>(static_cast<uint8_t>(pattern[i + 1])) <<  8) |
                      static_cast<uint32_t>(static_cast<uint8_t>(pattern[i + 2]));
        pat_trigrams.push_back(t);
    }
    std::sort(pat_trigrams.begin(), pat_trigrams.end());
    pat_trigrams.erase(std::unique(pat_trigrams.begin(), pat_trigrams.end()),
                       pat_trigrams.end());

    // mmap the lookup table
    int lfd = open(lookup_path.c_str(), O_RDONLY);
    if (lfd < 0) return {};
    struct stat lst{};
    fstat(lfd, &lst);
    if (lst.st_size == 0) { close(lfd); return {}; }
    const auto* lookup = static_cast<const LookupEntry*>(
        mmap(nullptr, static_cast<size_t>(lst.st_size), PROT_READ, MAP_PRIVATE, lfd, 0));
    close(lfd);
    if (lookup == MAP_FAILED) return {};

    const size_t n_entries = static_cast<size_t>(lst.st_size) / sizeof(LookupEntry);

    std::ifstream postings_f(postings_path, std::ios::binary);

    std::optional<std::vector<uint32_t>> candidates;

    for (uint32_t trigram : pat_trigrams) {
        // Binary search lookup
        size_t lo = 0, hi = n_entries;
        bool found = false;
        uint64_t poffset = 0;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (lookup[mid].trigram_hash == trigram) {
                poffset = lookup[mid].offset;
                found   = true;
                break;
            }
            if (lookup[mid].trigram_hash < trigram) lo = mid + 1;
            else                                    hi = mid;
        }

        if (!found) {
            // No file contains this trigram → intersection is empty
            munmap(const_cast<LookupEntry*>(lookup), static_cast<size_t>(lst.st_size));
            return {};
        }

        // Read posting list
        postings_f.seekg(static_cast<std::streamoff>(poffset));
        uint32_t count = 0;
        postings_f.read(reinterpret_cast<char*>(&count), sizeof(count));
        std::vector<uint32_t> posting(count);
        postings_f.read(reinterpret_cast<char*>(posting.data()),
                        static_cast<std::streamsize>(count * sizeof(uint32_t)));

        if (!candidates.has_value()) {
            candidates = std::move(posting);
        } else {
            std::vector<uint32_t> intersected;
            intersected.reserve(std::min(candidates->size(), posting.size()));
            std::set_intersection(candidates->begin(), candidates->end(),
                                  posting.begin(), posting.end(),
                                  std::back_inserter(intersected));
            candidates = std::move(intersected);
        }

        if (candidates->empty()) break;
    }

    munmap(const_cast<LookupEntry*>(lookup), static_cast<size_t>(lst.st_size));

    if (!candidates.has_value()) return {};

    std::vector<fs::path> result;
    result.reserve(candidates->size());
    for (uint32_t id : *candidates) {
        if (id < static_cast<uint32_t>(file_list.size()))
            result.emplace_back(file_list[id]);
    }
    return result;
}

void start_index_watcher(const fs::path& root, const WalkOptions& opts) {
    std::thread([root, opts]() {
        int ifd = inotify_init1(0); // blocking reads
        if (ifd < 0) return;

        // Watch root and all subdirectories
        std::unordered_map<int, fs::path> wd_to_dir;
        auto add_watch = [&](const fs::path& dir) {
            int wd = inotify_add_watch(ifd, dir.c_str(),
                                       IN_MODIFY | IN_CREATE | IN_DELETE);
            if (wd >= 0) wd_to_dir[wd] = dir;
        };

        add_watch(root);
        std::error_code ec;
        for (const auto& entry : fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec)) {
            if (entry.is_directory())
                add_watch(entry.path());
        }

        const fs::path index_dir = root / kIndexDir;
        // Buffer sized for up to 16 events
        constexpr size_t kBufSize =
            16 * (sizeof(struct inotify_event) + NAME_MAX + 1);
        char buf[kBufSize];

        while (true) {
            ssize_t n = read(ifd, buf, sizeof(buf));
            if (n <= 0) break;

            ssize_t i = 0;
            while (i < n) {
                const auto* ev =
                    reinterpret_cast<const struct inotify_event*>(buf + i);

                if ((ev->mask & IN_ISDIR) == 0 && ev->len > 0) {
                    auto it = wd_to_dir.find(ev->wd);
                    if (it != wd_to_dir.end()) {
                        fs::path changed = it->second / ev->name;

                        // Ignore events inside .cx itself
                        if (changed.string().starts_with(index_dir.string())) {
                            i += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
                            continue;
                        }

                        // Check extension filter
                        bool include = opts.extensions.empty();
                        if (!include) {
                            std::string ext = changed.extension().string();
                            for (const auto& e : opts.extensions)
                                if (ext == e) { include = true; break; }
                        }

                        if (include) {
                            std::lock_guard<std::mutex> lock(g_index.mtx);
                            std::string path_str = changed.string();

                            if (ev->mask & IN_DELETE) {
                                // Remove file from index
                                auto fit = std::find(g_index.files.begin(),
                                                     g_index.files.end(), path_str);
                                if (fit != g_index.files.end()) {
                                    auto fid = static_cast<uint32_t>(
                                        fit - g_index.files.begin());
                                    for (auto& [t, ids] : g_index.trigram_to_files) {
                                        ids.erase(std::remove(ids.begin(), ids.end(), fid),
                                                  ids.end());
                                    }
                                    // Keep file slot as empty string (reusing IDs
                                    // is complex; we just leave a tombstone)
                                    *fit = "";
                                }
                            } else if (fs::is_regular_file(changed)) {
                                // Find or assign file ID
                                uint32_t fid;
                                auto fit = std::find(g_index.files.begin(),
                                                     g_index.files.end(), path_str);
                                if (fit != g_index.files.end()) {
                                    fid = static_cast<uint32_t>(
                                        fit - g_index.files.begin());
                                    // Remove stale trigram entries
                                    for (auto& [t, ids] : g_index.trigram_to_files) {
                                        ids.erase(std::remove(ids.begin(), ids.end(), fid),
                                                  ids.end());
                                    }
                                } else {
                                    fid = static_cast<uint32_t>(g_index.files.size());
                                    g_index.files.push_back(path_str);
                                }

                                // Re-index file content
                                auto content  = read_file_content(changed);
                                auto trigrams = extract_trigrams(content);
                                for (uint32_t t : trigrams) {
                                    auto& ids = g_index.trigram_to_files[t];
                                    // Insert maintaining sorted order
                                    auto pos = std::lower_bound(ids.begin(), ids.end(), fid);
                                    if (pos == ids.end() || *pos != fid)
                                        ids.insert(pos, fid);
                                }

                                write_index(index_dir);
                            }
                        }
                    }
                }

                i += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
            }
        }

        close(ifd);
    }).detach();
}
