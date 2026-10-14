#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace stale::win {

enum class Category { None, Build, Dependencies, Cache, Agent, Downloads, Model };
struct Entry {
    std::string path;
    std::string name;
    int parent = -1;
    bool directory = false;
    bool reparse = false;
    bool cloud = false;
    uint64_t logical_bytes = 0;
    uint64_t allocated_bytes = 0;
    double modified = 0;
    uint64_t volume_id = 0;
    uint64_t file_id = 0;
    Category category = Category::None;
    uint32_t link_count = 1;
    bool metadata_ok = true;
};
struct Snapshot {
    std::string root;
    std::vector<Entry> entries; // Root first; parent precedes children; directories have rolled-up totals.
    uint64_t files = 0;
    uint64_t errors = 0;
    double scanned_at = 0;
    bool cancelled = false;
    std::string error;
};
struct ScanControl {
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> files{0};
};
struct Candidate {
    std::string path;
    std::string reason;
    uint64_t bytes = 0;
    double modified = 0;
    bool suggested = false;
    int duplicate_group = -1;
    std::string validation_token; // Opaque scan metadata fingerprint; preserve for recycle().
    std::string duplicate_keeper; // This independently verified copy must remain present.
    Category category = Category::None; // Classification evidence; never a deletion guarantee.
};
struct RecycleResult {
    std::vector<std::string> recycled;
    std::vector<std::string> failures;
};
Snapshot scan(const std::string& root, ScanControl* control = nullptr);
std::vector<Candidate> cleanup_candidates(const Snapshot& snapshot, bool include_scan_root = false);
std::vector<Candidate> find_duplicates(const Snapshot& snapshot, ScanControl* control = nullptr);
std::string default_root();
std::string cache_path(const std::string& root);
bool save_snapshot(const Snapshot& snapshot, const std::string& file, std::string* error = nullptr);
bool load_snapshot(const std::string& file, Snapshot& snapshot, std::string* error = nullptr);
RecycleResult recycle(const std::vector<Candidate>& candidates);
std::string deletion_path_issue(const std::string& path);
RecycleResult delete_permanently(const std::vector<Candidate>& candidates, const std::function<void(const std::string&)>& progress = {});
std::string pick_folder(); // Empty on cancel.
bool reveal(const std::string& path);
bool open_recycle_bin();
const char* category_name(Category category);
}
