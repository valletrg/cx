#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "walker.h"

namespace fs = std::filesystem;

// Build (or rebuild) the trigram index under <root>/.cx/.
// Returns true on success.
bool build_index(const fs::path& root, const WalkOptions& opts);

// Query the trigram index for candidate files that may contain pattern.
// Returns an empty vector if the index does not exist.
// Short patterns (<3 chars) return all indexed files.
std::vector<fs::path> query_index(const fs::path& root,
                                  const std::string& pattern);

// Start an inotify watcher in a detached background thread.
// On file create/modify, incrementally updates the on-disk index.
void start_index_watcher(const fs::path& root, const WalkOptions& opts);
