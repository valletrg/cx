#include "index.h"
#include "threadpool.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
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

static constexpr const char* kIndexDir        = ".cx";
static constexpr size_t      kBinaryProbeSize = 8192;
static constexpr const char* kFilesName    = "files";
static constexpr const char* kPostingsName = "postings";
static constexpr const char* kLookupName   = "lookup";

// Max (trigram, file_id) pairs to buffer per thread before flushing to disk.
// 4M pairs × 8 bytes = 32MB per thread. With 8 threads = 256MB peak.
static constexpr size_t kFlushThreshold = 4 * 1024 * 1024;

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
// Batched external-sort index builder
//
// Workers extract trigrams in parallel, buffering (trigram, file_id) pairs in
// per-thread vectors. When a buffer exceeds kFlushThreshold, it is sorted and
// written to a temporary "run" file on disk. After all files are processed,
// remaining buffers are flushed, then run files are K-way merged and streamed
// directly into the postings + lookup files.
//
// Peak RAM is bounded by (kFlushThreshold × n_threads) regardless of codebase
// size.
// ---------------------------------------------------------------------------

// Write a sorted run of (trigram, file_id) pairs to a binary temp file.
// Returns the path of the written file, or empty on failure.
static fs::path flush_run(std::vector<std::pair<uint32_t, uint32_t>>& buf,
                          const fs::path& tmp_dir,
                          std::atomic<uint32_t>& run_counter) {
    if (buf.empty()) return {};
    std::sort(buf.begin(), buf.end());

    uint32_t id = run_counter.fetch_add(1, std::memory_order_relaxed);
    fs::path run_path = tmp_dir / ("run_" + std::to_string(id));
    int fd = open(run_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return {};
    const char* ptr = reinterpret_cast<const char*>(buf.data());
    size_t remaining = buf.size() * sizeof(buf[0]);
    while (remaining > 0) {
        ssize_t n = write(fd, ptr, remaining);
        if (n < 0) { close(fd); return {}; }
        ptr       += n;
        remaining -= static_cast<size_t>(n);
    }
    close(fd);
    buf.clear();
    return run_path;
}

// A reader for one sorted run file, providing streaming access for K-way merge.
struct RunReader {
    int fd = -1;
    size_t file_size = 0;
    size_t pos = 0;                  // byte offset into file
    static constexpr size_t kBufPairs = 16384; // 128KB buffer
    std::vector<std::pair<uint32_t, uint32_t>> buf;
    size_t buf_idx = 0;
    size_t buf_len = 0;

    bool open_file(const fs::path& path) {
        fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) < 0) { close(fd); fd = -1; return false; }
        file_size = static_cast<size_t>(st.st_size);
        buf.resize(kBufPairs);
        return refill();
    }

    bool refill() {
        if (pos >= file_size) { buf_len = 0; return false; }
        size_t to_read = std::min(kBufPairs * sizeof(buf[0]), file_size - pos);
        ssize_t n = pread(fd, buf.data(), to_read, static_cast<off_t>(pos));
        if (n <= 0) { buf_len = 0; return false; }
        pos     += static_cast<size_t>(n);
        buf_len  = static_cast<size_t>(n) / sizeof(buf[0]);
        buf_idx  = 0;
        return buf_len > 0;
    }

    bool has_next() const { return buf_idx < buf_len; }

    const std::pair<uint32_t, uint32_t>& peek() const { return buf[buf_idx]; }

    void advance() {
        ++buf_idx;
        if (buf_idx >= buf_len) refill();
    }

    void close_file() {
        if (fd >= 0) { close(fd); fd = -1; }
        buf.clear();
        buf.shrink_to_fit();
    }
};

// K-way merge all run files and write postings + lookup.
static void merge_runs(const std::vector<fs::path>& run_paths,
                       const fs::path& index_dir) {
    if (run_paths.empty()) {
        // No trigram data at all — write empty postings + lookup.
        std::ofstream(index_dir / kPostingsName, std::ios::binary | std::ios::trunc);
        std::ofstream(index_dir / kLookupName,   std::ios::binary | std::ios::trunc);
        return;
    }

    // Open all run readers.
    std::vector<RunReader> readers(run_paths.size());
    // Min-heap: (trigram, file_id, reader_index)
    using Entry = std::tuple<uint32_t, uint32_t, size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pq;

    for (size_t i = 0; i < run_paths.size(); ++i) {
        if (readers[i].open_file(run_paths[i]) && readers[i].has_next()) {
            auto [t, fid] = readers[i].peek();
            pq.emplace(t, fid, i);
        }
    }

    std::ofstream pf(index_dir / kPostingsName, std::ios::binary | std::ios::trunc);
    std::ofstream lf(index_dir / kLookupName,   std::ios::binary | std::ios::trunc);

    uint64_t offset = 0;
    uint32_t cur_trigram = 0;
    bool     have_cur    = false;
    std::vector<uint32_t> cur_ids;   // posting list being accumulated

    auto flush_posting = [&]() {
        if (cur_ids.empty()) return;
        LookupEntry entry{cur_trigram, offset};
        lf.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        auto count = static_cast<uint32_t>(cur_ids.size());
        pf.write(reinterpret_cast<const char*>(&count), sizeof(count));
        pf.write(reinterpret_cast<const char*>(cur_ids.data()),
                 static_cast<std::streamsize>(cur_ids.size() * sizeof(uint32_t)));
        offset += sizeof(uint32_t) + cur_ids.size() * sizeof(uint32_t);
        cur_ids.clear();
    };

    while (!pq.empty()) {
        auto [t, fid, ri] = pq.top();
        pq.pop();

        // Advance this reader and re-insert into heap.
        readers[ri].advance();
        if (readers[ri].has_next()) {
            auto [nt, nfid] = readers[ri].peek();
            pq.emplace(nt, nfid, ri);
        }

        if (!have_cur || t != cur_trigram) {
            flush_posting();
            cur_trigram = t;
            have_cur    = true;
        }
        // Deduplicate file IDs within the same trigram posting list.
        if (cur_ids.empty() || cur_ids.back() != fid)
            cur_ids.push_back(fid);
    }
    flush_posting();

    for (auto& r : readers) r.close_file();
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

    // Augment gitignore patterns with hardcoded skips for .git/ and .cx/
    auto patterns = opts.gitignore_patterns;
    for (const std::string& always_skip : {std::string(".git/"), std::string(kIndexDir) + "/"}) {
        if (std::find(patterns.begin(), patterns.end(), always_skip) == patterns.end())
            patterns.push_back(always_skip);
    }
    WalkOptions walk_opts{.extensions = opts.extensions, .gitignore_patterns = patterns};
    auto files = collect_files(root, walk_opts);

    // Write files list immediately. File ID = position in this list.
    {
        std::ofstream ff(index_dir / kFilesName, std::ios::trunc);
        for (const auto& f : files) ff << f.string() << '\n';
    }

    // Build a path->ID map so workers can look up their pre-assigned file ID.
    std::unordered_map<std::string, uint32_t> path_to_id;
    path_to_id.reserve(files.size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(files.size()); ++i)
        path_to_id[files[i].string()] = i;

    const unsigned hw = std::thread::hardware_concurrency();
    const size_t n_threads = hw > 0 ? static_cast<size_t>(hw) : 1;

    // Temp directory for sorted run files.
    const fs::path tmp_dir = index_dir / "tmp";
    fs::create_directories(tmp_dir, ec);

    // Shared state for run flushing.
    std::atomic<uint32_t> run_counter{0};
    std::vector<fs::path> run_paths;
    std::mutex             run_paths_mtx;

    // Per-thread buffers indexed by slot ID. Each pool thread claims a slot
    // on its first task via thread_local + atomic counter. After pool.wait()
    // returns (all threads joined or idle), we iterate these to flush leftovers.
    std::atomic<uint32_t> slot_counter{0};
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> thread_bufs(n_threads);

    // Phase 1: parallel trigram extraction with batched flush to disk.
    {
        ThreadPool pool(n_threads,
            [&](const fs::path& f, const fs::path*) {
                // Each pool thread claims a persistent slot on first invocation.
                thread_local uint32_t my_slot =
                    slot_counter.fetch_add(1, std::memory_order_relaxed);
                uint32_t slot = my_slot < static_cast<uint32_t>(n_threads)
                                    ? my_slot : 0;
                auto& buf = thread_bufs[slot];

                // Look up pre-assigned file ID.
                auto it = path_to_id.find(f.string());
                if (it == path_to_id.end()) return;
                uint32_t fid = it->second;

                int fd = open(f.c_str(), O_RDONLY);
                if (fd < 0) return;
                struct stat st{};
                if (fstat(fd, &st) < 0 || st.st_size <= 0) { close(fd); return; }
                void* addr = mmap(nullptr, static_cast<size_t>(st.st_size),
                                  PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (addr == MAP_FAILED) return;

                const size_t fsize = static_cast<size_t>(st.st_size);
                const char* data = static_cast<const char*>(addr);
                const size_t probe = std::min(fsize, kBinaryProbeSize);
                if (std::memchr(data, '\0', probe) != nullptr) {
                    munmap(addr, fsize);
                    return;
                }

                // Extract unique trigrams, then append (trigram, fid) to buffer.
                if (fsize >= 3) {
                    std::unordered_set<uint32_t> seen;
                    for (size_t i = 0; i + 2 < fsize; ++i) {
                        uint32_t t =
                            (static_cast<uint32_t>(static_cast<uint8_t>(data[i]))     << 16) |
                            (static_cast<uint32_t>(static_cast<uint8_t>(data[i + 1])) <<  8) |
                             static_cast<uint32_t>(static_cast<uint8_t>(data[i + 2]));
                        if (seen.insert(t).second)
                            buf.emplace_back(t, fid);
                    }
                }
                munmap(addr, fsize);

                // Flush to disk if buffer exceeds threshold.
                if (buf.size() >= kFlushThreshold) {
                    auto rp = flush_run(buf, tmp_dir, run_counter);
                    if (!rp.empty()) {
                        std::lock_guard<std::mutex> lk(run_paths_mtx);
                        run_paths.push_back(std::move(rp));
                    }
                }
            });

        for (const auto& f : files)
            pool.enqueue(f);
        pool.wait();
    }

    // Flush remaining data in per-thread buffers.
    for (auto& buf : thread_bufs) {
        if (!buf.empty()) {
            auto rp = flush_run(buf, tmp_dir, run_counter);
            if (!rp.empty())
                run_paths.push_back(std::move(rp));
        }
    }

    // Release memory from file list and path map before merge phase.
    files.clear();
    files.shrink_to_fit();
    path_to_id.clear();
    // Force deallocation (unordered_map doesn't release on clear alone).
    { std::unordered_map<std::string, uint32_t> tmp; path_to_id.swap(tmp); }
    thread_bufs.clear();
    thread_bufs.shrink_to_fit();

    // Phase 2: K-way merge all run files into final postings + lookup.
    merge_runs(run_paths, index_dir);

    // Clean up temp directory.
    fs::remove_all(tmp_dir, ec);

    // Populate in-memory index file list for the watcher (lightweight).
    // Don't load trigram_to_files — it would defeat the memory savings.
    std::lock_guard<std::mutex> lock(g_index.mtx);
    g_index.files.clear();
    g_index.trigram_to_files.clear();

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
    if (fstat(lfd, &lst) < 0 || lst.st_size == 0) { close(lfd); return {}; }
    const size_t lookup_size = static_cast<size_t>(lst.st_size);
    void* lookup_raw = mmap(nullptr, lookup_size, PROT_READ, MAP_PRIVATE, lfd, 0);
    close(lfd);
    if (lookup_raw == MAP_FAILED) return {};
    const auto* lookup = static_cast<const LookupEntry*>(lookup_raw);

    const size_t n_entries = lookup_size / sizeof(LookupEntry);

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
            // No file contains this trigram -> intersection is empty
            munmap(lookup_raw, lookup_size);
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

    munmap(lookup_raw, lookup_size);

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
