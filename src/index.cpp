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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include <fastpfor.h>
#include <compositecodec.h>
#include <variablebyte.h>
#pragma GCC diagnostic pop

// On-disk layout: files, postings, lookup

static constexpr const char* kIndexDir        = ".cx";
static constexpr size_t      kBinaryProbeSize = 8192;
static constexpr const char* kFilesName    = "files";
static constexpr const char* kPostingsName = "postings";
static constexpr const char* kLookupName   = "lookup";

// Index format version — increment when on-disk format changes.
static constexpr uint32_t kIndexVersion = 2;

// Posting lists smaller than this are stored uncompressed (FastPFor overhead).
static constexpr size_t kMinCompressSize = 4;

// Max (trigram, file_id) pairs to buffer per thread before flushing to disk.
// 4M pairs × 8 bytes = 32MB per thread. With 8 threads = 256MB peak.
static constexpr size_t kFlushThreshold = 4 * 1024 * 1024;

// Lines longer than this are skipped during trigram extraction (avoids
// bloating the index with minified JS, generated protobuf output, etc.).
static constexpr size_t kMaxIndexLineLen = 512;

#pragma pack(push, 1)
struct LookupEntry {
    uint32_t trigram_hash;
    uint64_t offset;
};
#pragma pack(pop)

// Shared helpers

// Returns true if byte is alphanumeric [0-9A-Za-z].
[[gnu::always_inline]] inline bool is_alnum_byte(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// Encode three consecutive bytes as a 24-bit trigram hash.
// Input bytes must be valid (caller ensures this).
[[gnu::always_inline]] inline uint32_t trigram_hash_3(uint8_t a, uint8_t b, uint8_t c) {
    return (static_cast<uint32_t>(a) << 16) |
           (static_cast<uint32_t>(b) <<  8) |
            static_cast<uint32_t>(c);
}

// Write a posting list to `pf` (postings file) and emit a LookupEntry to `lf`.
// Handles both compressed and uncompressed formats.
// Caller provides `codec`, `deltas`, and `compressed` buffers for reuse.
static void write_one_posting(std::ofstream& pf, std::ofstream& lf,
                              uint32_t trigram_hash, uint64_t& offset,
                              const std::vector<uint32_t>& ids,
                              FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>,
                                                          FastPForLib::VariableByte>& codec,
                              std::vector<uint32_t>& deltas,
                              std::vector<uint32_t>& compressed,
                              size_t& total_original_size,
                              size_t& total_compressed_size) {
    LookupEntry entry{trigram_hash, offset};
    lf.write(reinterpret_cast<const char*>(&entry), sizeof(entry));

    auto original_count = static_cast<uint32_t>(ids.size());
    auto original_size = ids.size() * sizeof(uint32_t);
    total_original_size += original_size;

    if (original_count < kMinCompressSize) {
        uint32_t compressed_size = 0;
        pf.write(reinterpret_cast<const char*>(&original_count), sizeof(original_count));
        pf.write(reinterpret_cast<const char*>(&compressed_size), sizeof(compressed_size));
        pf.write(reinterpret_cast<const char*>(ids.data()),
                 static_cast<std::streamsize>(ids.size() * sizeof(uint32_t)));
        offset += 2 * sizeof(uint32_t) + ids.size() * sizeof(uint32_t);
    } else {
        deltas.resize(ids.size());
        deltas[0] = ids[0];
        for (size_t i = 1; i < ids.size(); ++i)
            deltas[i] = ids[i] - ids[i - 1];

        compressed.resize(ids.size() + 1024);
        size_t compressed_len = compressed.size();
        codec.encodeArray(deltas.data(), deltas.size(),
                          compressed.data(), compressed_len);

        auto comp_size = static_cast<uint32_t>(compressed_len);
        auto comp_size_bytes = compressed_len * sizeof(uint32_t);
        total_compressed_size += comp_size_bytes;

        pf.write(reinterpret_cast<const char*>(&original_count), sizeof(original_count));
        pf.write(reinterpret_cast<const char*>(&comp_size), sizeof(comp_size));
        pf.write(reinterpret_cast<const char*>(compressed.data()),
                 static_cast<std::streamsize>(compressed_len * sizeof(uint32_t)));
        offset += 2 * sizeof(uint32_t) + compressed_len * sizeof(uint32_t);
    }
}

// In-memory index (kept alive for the inotify watcher)

struct InMemoryIndex {
    std::vector<std::string>                          files;
    std::unordered_map<uint32_t, std::vector<uint32_t>> trigram_to_files;
    std::mutex                                        mtx;
};

// Single global instance; populated by build_index, updated by the watcher.
static InMemoryIndex g_index; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Helpers

static std::unordered_set<uint32_t> extract_trigrams(std::string_view content) {
    std::unordered_set<uint32_t> out;
    if (content.size() < 3) return out;

    // Process line-by-line, skipping long and non-alphanumeric lines.
    const char* line_start = content.data();
    const char* end = content.data() + content.size();
    while (line_start < end) {
        const char* line_end = static_cast<const char*>(
            memchr(line_start, '\n', static_cast<size_t>(end - line_start)));
        if (!line_end) line_end = end;
        const size_t line_len = static_cast<size_t>(line_end - line_start);

        if (line_len <= kMaxIndexLineLen && line_len >= 3) {
            bool has_alnum = false;
            for (size_t j = 0; j < line_len; ++j) {
                if (is_alnum_byte(line_start[j])) {
                    has_alnum = true;
                    break;
                }
            }
            if (has_alnum) {
                for (size_t i = 0; i + 2 < line_len; ++i) {
                    uint32_t t = trigram_hash_3(line_start[i], line_start[i + 1], line_start[i + 2]);
                    out.insert(t);
                }
            }
        }
        line_start = line_end + 1;
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

    // Write version header.
    lf.write(reinterpret_cast<const char*>(&kIndexVersion), sizeof(kIndexVersion));

    FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>,
                                FastPForLib::VariableByte> codec;
    std::vector<uint32_t> deltas;
    std::vector<uint32_t> compressed;

    uint64_t offset = 0;
    size_t total_original_size = 0;
    size_t total_compressed_size = 0;

    for (const auto& [hash, ids] : sorted) {
        write_one_posting(pf, lf, hash, offset, ids,
                         codec, deltas, compressed,
                         total_original_size, total_compressed_size);
    }

    if (total_original_size > 0) {
        double ratio = static_cast<double>(total_compressed_size) / total_original_size;
        std::cerr << "[cx] index compression: " << ratio
                  << " (" << total_compressed_size << " / " << total_original_size << " bytes)\n";
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

    // Write version header as first bytes of lookup file.
    lf.write(reinterpret_cast<const char*>(&kIndexVersion), sizeof(kIndexVersion));

    FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>,
                                FastPForLib::VariableByte> codec;

    uint64_t offset = 0;
    uint32_t cur_trigram = 0;
    bool     have_cur    = false;
    std::vector<uint32_t> cur_ids;   // posting list being accumulated
    std::vector<uint32_t> deltas;    // reusable delta buffer
    std::vector<uint32_t> compressed; // reusable compression buffer

    size_t total_original_size = 0;
    size_t total_compressed_size = 0;

    auto flush_posting = [&]() {
        if (cur_ids.empty()) return;
        write_one_posting(pf, lf, cur_trigram, offset, cur_ids,
                          codec, deltas, compressed,
                          total_original_size, total_compressed_size);
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

    if (total_original_size > 0) {
        double ratio = static_cast<double>(total_compressed_size) / total_original_size;
        std::cerr << "[cx] index compression: " << ratio
                  << " (" << total_compressed_size << " / " << total_original_size << " bytes)\n";
    }
}

// Public API

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

    // Per-thread buffers. Each pool thread claims a slot on its first task.
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

                // Extract unique trigrams line-by-line, skipping long lines
                // and lines with no alphanumeric characters.
                if (fsize >= 3) {
                    std::unordered_set<uint32_t> seen;
                    const char* line_start = data;
                    const char* file_end = data + fsize;
                    while (line_start < file_end) {
                        const char* line_end = static_cast<const char*>(
                            memchr(line_start, '\n',
                                   static_cast<size_t>(file_end - line_start)));
                        if (!line_end) line_end = file_end;
                        const size_t line_len =
                            static_cast<size_t>(line_end - line_start);

                        if (line_len <= kMaxIndexLineLen && line_len >= 3) {
                            // Check if line has at least one alphanumeric char.
                            bool has_alnum = false;
                            for (size_t j = 0; j < line_len; ++j) {
                                if (is_alnum_byte(line_start[j])) {
                                    has_alnum = true;
                                    break;
                                }
                            }
                            if (has_alnum) {
                                for (size_t i = 0; i + 2 < line_len; ++i) {
                                    uint32_t t = trigram_hash_3(line_start[i], line_start[i + 1], line_start[i + 2]);
                                    if (seen.insert(t).second)
                                        buf.emplace_back(t, fid);
                                }
                            }
                        }
                        line_start = line_end + 1;
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
    path_to_id.clear();
    // Force deallocation (unordered_map doesn't release on clear alone).
    { std::unordered_map<std::string, uint32_t> tmp; path_to_id.swap(tmp); }
    thread_bufs.clear();

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

    // Check index version.
    {
        std::ifstream vlf(lookup_path, std::ios::binary);
        uint32_t version = 0;
        vlf.read(reinterpret_cast<char*>(&version), sizeof(version));
        if (version != kIndexVersion) {
            std::cerr << "[cx] index format outdated, run --reindex\n";
            return {};
        }
    }

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
        uint32_t t = trigram_hash_3(pattern[i], pattern[i + 1], pattern[i + 2]);
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
    // Skip version header (uint32_t) at start of lookup file.
    const auto* lookup = reinterpret_cast<const LookupEntry*>(
        static_cast<const char*>(lookup_raw) + sizeof(uint32_t));
    const size_t n_entries = (lookup_size - sizeof(uint32_t)) / sizeof(LookupEntry);

    std::ifstream postings_f(postings_path, std::ios::binary);

    // Helper: binary-search the lookup table for a trigram, return its offset.
    // Returns nullopt if not found.
    auto find_trigram = [&](uint32_t trigram) -> std::optional<uint64_t> {
        size_t lo = 0, hi = n_entries;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (lookup[mid].trigram_hash == trigram) return lookup[mid].offset;
            if (lookup[mid].trigram_hash < trigram) lo = mid + 1;
            else                                    hi = mid;
        }
        return std::nullopt;
    };

    FastPForLib::CompositeCodec<FastPForLib::FastPFor<8>,
                                FastPForLib::VariableByte> codec;

    // Helper: read a posting list at a given offset.
    // Format: [uint32_t original_count, uint32_t compressed_size, data...]
    // If compressed_size == 0, data is raw IDs. Otherwise, delta+FastPFor compressed.
    auto read_posting = [&](uint64_t poffset) -> std::vector<uint32_t> {
        postings_f.seekg(static_cast<std::streamoff>(poffset));
        uint32_t original_count = 0;
        uint32_t compressed_size = 0;
        postings_f.read(reinterpret_cast<char*>(&original_count), sizeof(original_count));
        postings_f.read(reinterpret_cast<char*>(&compressed_size), sizeof(compressed_size));

        if (compressed_size == 0) {
            // Uncompressed: read raw IDs.
            std::vector<uint32_t> posting(original_count);
            postings_f.read(reinterpret_cast<char*>(posting.data()),
                            static_cast<std::streamsize>(original_count * sizeof(uint32_t)));
            return posting;
        }

        // Compressed: read compressed data, decompress, then delta-decode.
        std::vector<uint32_t> comp_buf(compressed_size);
        postings_f.read(reinterpret_cast<char*>(comp_buf.data()),
                        static_cast<std::streamsize>(compressed_size * sizeof(uint32_t)));

        std::vector<uint32_t> posting(original_count);
        size_t decoded_len = posting.size();
        codec.decodeArray(comp_buf.data(), compressed_size,
                          posting.data(), decoded_len);

        // Delta decode: [3, 4, 3, 5] → [3, 7, 10, 15]
        for (size_t i = 1; i < decoded_len; ++i)
            posting[i] += posting[i - 1];

        return posting;
    };

    std::optional<std::vector<uint32_t>> candidates;

    // Fast path: pattern is exactly one trigram — single lookup, no intersection.
    if (pat_trigrams.size() == 1) {
        auto off = find_trigram(pat_trigrams[0]);
        if (!off) { munmap(lookup_raw, lookup_size); return {}; }
        candidates = read_posting(*off);
    } else {
        // For longer patterns, select at most 4 rarest trigrams to intersect.
        // "Rarest" = smallest posting list. We probe each trigram's posting
        // list size, then pick the smallest ones.
        static constexpr size_t kMaxTrigrams = 4;

        // Filter out noisy trigrams (all common punctuation/whitespace).
        auto is_common_byte = [](uint8_t c) -> bool {
            return c == ' ' || c == '(' || c == ')' || c == '{' || c == '}' ||
                   c == '[' || c == ']' || c == ';' || c == '\t';
        };
        auto is_noisy_trigram = [&](uint32_t t) -> bool {
            uint8_t a = static_cast<uint8_t>(t >> 16);
            uint8_t b = static_cast<uint8_t>(t >> 8);
            uint8_t c = static_cast<uint8_t>(t);
            return is_common_byte(a) && is_common_byte(b) && is_common_byte(c);
        };

        // Gather (posting_count, trigram, offset) for each non-noisy trigram.
        struct TrigramInfo {
            uint32_t count;
            uint32_t trigram;
            uint64_t offset;
        };
        std::vector<TrigramInfo> infos;
        infos.reserve(pat_trigrams.size());

        for (uint32_t trigram : pat_trigrams) {
            if (is_noisy_trigram(trigram) && pat_trigrams.size() > kMaxTrigrams)
                continue;
            auto off = find_trigram(trigram);
            if (!off) { munmap(lookup_raw, lookup_size); return {}; }
            // Peek at count without reading full posting list.
            postings_f.seekg(static_cast<std::streamoff>(*off));
            uint32_t count = 0;
            postings_f.read(reinterpret_cast<char*>(&count), sizeof(count));
            infos.push_back({count, trigram, *off});
        }

        if (infos.empty()) {
            // All trigrams were noisy — fall back to using them all.
            for (uint32_t trigram : pat_trigrams) {
                auto off = find_trigram(trigram);
                if (!off) { munmap(lookup_raw, lookup_size); return {}; }
                postings_f.seekg(static_cast<std::streamoff>(*off));
                uint32_t count = 0;
                postings_f.read(reinterpret_cast<char*>(&count), sizeof(count));
                infos.push_back({count, trigram, *off});
            }
        }

        // Sort by posting list size (rarest first) and take at most kMaxTrigrams.
        std::sort(infos.begin(), infos.end(),
                  [](const TrigramInfo& a, const TrigramInfo& b) {
                      return a.count < b.count;
                  });
        if (infos.size() > kMaxTrigrams)
            infos.resize(kMaxTrigrams);

        // Intersect the selected posting lists (rarest first for early pruning).
        for (const auto& info : infos) {
            auto posting = read_posting(info.offset);

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
