#pragma once
#include "core.h"

namespace stale::win {
struct Drive {
    std::string path;
    std::string label;
    std::string kind; // fixed, removable, network, optical, ramdisk, or unknown.
    bool ready = false; // True only after a local probe; false may mean not yet checked.
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0; // Available to the current user.
    bool capacity_known = false; // Network/removable/optical capacity is not probed by inventory.
};
std::vector<Drive> list_drives();

struct DirectoryListing {
    Snapshot snapshot; // Root first; immediate children only, with parent == 0.
    std::vector<bool> size_known; // Matches entries. Directories/cloud/reparse entries are unknown.
    std::string error; // Also recorded in snapshot.errors/error; partial listings remain usable.
};
DirectoryListing list_directory(const std::string& path);
}
