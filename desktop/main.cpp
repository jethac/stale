#include "app.h"
#include "windows/core.h"
#include "windows/browser.h"
#include "agent_bridge.h"
#include "vendor/json.hpp"
#include "recommendations.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <unordered_map>

namespace {
namespace backend = stale::win;
using Ui = slint::ComponentHandle<App>;
using Candidates = std::vector<backend::Candidate>;
using Json = nlohmann::json;

bool saved_dark_mode() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Stale", L"DarkMode",
        RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS && value == 1;
}
void change_theme(const Ui& ui, bool dark) {
    ui->invoke_apply_theme(dark);
    const DWORD value = dark ? 1 : 0;
    if (RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stale", L"DarkMode",
        REG_DWORD, &value, sizeof(value)) != ERROR_SUCCESS)
        ui->set_status("Appearance changed, but the preference could not be saved.");
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::wstring folded(const std::string& value) {
    auto result = wide(value);
    if (!result.empty()) CharLowerBuffW(result.data(), static_cast<DWORD>(result.size()));
    return result;
}
std::string string(const slint::SharedString& value) { return std::string(value.data(), value.size()); }
std::string bytes(uint64_t value) {
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double amount = static_cast<double>(value);
    size_t unit = 0;
    while (amount >= 1024 && unit < 5) { amount /= 1024; ++unit; }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : amount >= 100 ? 1 : 2) << amount << ' ' << units[unit];
    return out.str();
}
std::string date(double seconds, bool time = false) {
    if (seconds <= 0) return "Unknown";
    const auto stamp = static_cast<time_t>(seconds);
    tm local{};
    if (localtime_s(&local, &stamp)) return "Unknown";
    char result[40]{};
    strftime(result, sizeof(result), time ? "%Y-%m-%d %H:%M" : "%Y-%m-%d", &local);
    return result;
}
std::string basename(const std::string& path) {
    const auto separator = path.find_last_of("/\\");
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

// One owned worker serializes expensive operations. Its queue contains immutable
// inputs and weak UI continuations; shutdown cancels work and joins before exit.
class Worker {
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::thread thread_;
public:
    Worker() : thread_([this] {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (stopping_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }) {}
    void push(std::function<void()> job) {
        { std::lock_guard lock(mutex_); if (stopping_) return; jobs_.push_back(std::move(job)); }
        ready_.notify_one();
    }
    void stop() {
        { std::lock_guard lock(mutex_); stopping_ = true; jobs_.clear(); }
        ready_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    ~Worker() { stop(); }
};

struct Outcome { std::string path, message; bool success; };
struct Source {
    std::shared_ptr<const backend::Snapshot> snapshot;
    std::shared_ptr<const Candidates> candidates;
    std::shared_ptr<const std::vector<Outcome>> outcomes;
    std::shared_ptr<const std::vector<bool>> size_known;
    bool live = false;
    std::shared_ptr<const std::vector<backend::Drive>> drives;
};

// Slint's ListView instantiates only visible rows. Keep just source indexes here;
// UTF-8 formatting happens on demand, never for millions of rows up front.
class Rows final : public slint::Model<FileRow> {
public:
    Source source;
    std::vector<size_t> indexes;
    std::shared_ptr<std::unordered_set<std::string>> selected;
    size_t row_count() const override { return indexes.size(); }
    std::optional<FileRow> row_data(size_t row) const override {
        if (row >= indexes.size()) return std::nullopt;
        const auto index = indexes[row];
        FileRow result{};
        if (source.candidates) {
            const auto& item = source.candidates->at(index);
            const auto parent = utf8(std::filesystem::path(wide(item.path)).parent_path().filename().wstring());
            result.name = parent.empty() ? basename(item.path) : parent + " / " + basename(item.path);
            result.detail = item.deletion_error.empty() ? item.path : "Could not delete: " + item.deletion_error;
            result.size = bytes(item.bytes);
            result.modified = date(item.modified);
            result.selectable = item.deletion_error.empty() && !item.validation_token.empty();
            result.checkable = item.deletion_error.empty() && item.duplicate_group < 0;
            result.selected = selected->count(item.path) != 0;
        } else if (source.outcomes) {
            const auto& item = source.outcomes->at(index);
            result.name = item.success ? "Deleted: " + basename(item.path) : "Could not delete";
            result.detail = item.success ? item.path : item.message;
        } else if (source.snapshot) {
            const auto& item = source.snapshot->entries.at(index);
            result.name = item.name.empty() ? item.path : item.name;
            result.detail = item.cloud ? "Cloud placeholder — not read" : item.reparse ? "Link / reparse point — not followed" : item.path;
            result.size = source.size_known && !source.size_known->at(index) ? "—" : bytes(item.allocated_bytes);
            result.modified = date(item.modified);
            result.directory = item.directory && !item.reparse && !item.cloud;
            result.measured = item.metadata_ok && (!source.size_known || source.size_known->at(index));
            if (result.measured && item.parent >= 0) {
                const auto total = source.snapshot->entries[item.parent].allocated_bytes;
                result.size_fraction = total ? static_cast<float>(std::min(1.0, double(item.allocated_bytes) / double(total))) : 0;
            }
            if (source.drives) {
                result.drive = true;
                result.capacity_label = "Capacity unavailable";
                for (const auto& drive : *source.drives) if (drive.path == item.path) {
                    result.capacity_known = drive.capacity_known && drive.total_bytes > 0;
                    if (result.capacity_known) {
                        const auto free = std::min(drive.free_bytes, drive.total_bytes);
                        const auto used = drive.total_bytes - free;
                        result.used_fraction = static_cast<float>(double(used) / double(drive.total_bytes));
                        result.capacity_label = bytes(used) + " used of " + bytes(drive.total_bytes);
                        result.free_label = bytes(free) + " free";
                        result.usage_label = std::to_string(static_cast<int>(result.used_fraction * 100 + 0.5f)) + "% used";
                    } else result.free_label = "Not measured";
                    break;
                }
            }
        }
        return result;
    }
    void selection_changed(size_t row) { notify_row_changed(row); }
    void selection_reset() { notify_reset(); }
};

class Controller final : public std::enable_shared_from_this<Controller> {
    slint::ComponentWeakHandle<App> ui_;
    Worker worker_;
    Worker browser_worker_;
    Worker query_worker_;
    struct TreeNode {
        std::string path, label, detail;
        std::vector<std::wstring> children;
        std::shared_ptr<backend::DirectoryListing> listing;
        bool expanded = false, loading = false, blocked = false;
        uint64_t request = 0;
    };
    std::map<std::wstring, TreeNode> tree_;
    std::vector<std::wstring> drive_roots_;
    std::vector<backend::Drive> drives_;
    std::vector<BrowserTreeRow> visible_tree_;
    bool tree_root_expanded_ = true, drives_loading_ = false, browse_loading_ = false;
    uint64_t drive_request_ = 0, tree_request_ = 0;
    std::string browse_path_ = "this-pc";
    std::shared_ptr<const backend::Snapshot> browse_snapshot_;
    std::shared_ptr<const std::vector<bool>> browse_sizes_;
    slint::Timer progress_, debounce_;
    std::shared_ptr<backend::ScanControl> control_;
    std::shared_ptr<std::atomic<uint64_t>> query_generation_ = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<const backend::Snapshot> snapshot_;
    std::shared_ptr<const Candidates> cleanup_, duplicates_, report_;
    std::unordered_map<std::string, size_t> agent_row_ids_;
    std::string report_root_, scan_label_ = "No scan yet";
    std::shared_ptr<const Candidates> grouped_source_;
    std::vector<stale::desktop::RecommendationGroup> groups_;
    std::string active_group_, active_project_, targeted_path_;
    std::unordered_set<std::string> expanded_groups_;
    int group_source_page_ = 2;
    uint64_t drive_total_ = 0, drive_free_ = 0;
    bool detail_generated_ = false;
    std::shared_ptr<const std::vector<Outcome>> outcomes_;
    std::shared_ptr<std::unordered_set<std::string>> selected_ = std::make_shared<std::unordered_set<std::string>>();
    std::shared_ptr<Rows> rows_;
    std::string detail_path_, activity_;
    int folder_ = 0;
    bool busy_ = false;
    uint64_t navigation_generation_ = 0, scan_navigation_ = 0;
    bool summary_only_ = false;
    bool smoke_ = false;
    bool smoke_finished_ = false;
    uint64_t selected_bytes_ = 0;
    int smoke_page_ = 0;
    std::string screenshot_;
    int* exit_code_;

    static std::string browser_path(const std::string& path) {
        auto native = std::filesystem::path(wide(path)).lexically_normal();
        if (!native.is_absolute()) throw std::runtime_error("Choose an absolute drive or folder path");
        return stale::desktop::recommendation_detail::path(utf8(native.wstring()));
    }
    static std::wstring browser_key(const std::string& path) {
        return folded(stale::desktop::recommendation_detail::path(path));
    }
    void publish_tree(const Ui& ui) {
        visible_tree_.clear();
        BrowserTreeRow root{};
        root.id = "this-pc"; root.label = "This PC";
        root.detail = slint::SharedString(std::to_string(drives_.size()) + " drives");
        root.expanded = tree_root_expanded_; root.expandable = true; root.available = true;
        root.loading = drives_loading_; root.selected = browse_path_ == "this-pc";
        visible_tree_.push_back(root);
        std::function<void(const std::wstring&, int)> append = [&](const std::wstring& key, int depth) {
            const auto found = tree_.find(key);
            if (found == tree_.end()) return;
            const auto& node = found->second;
            BrowserTreeRow row{};
            row.id = slint::SharedString(node.path); row.label = slint::SharedString(node.label);
            row.detail = slint::SharedString(node.detail); row.depth = depth;
            row.expanded = node.expanded; row.expandable = !node.blocked && (!node.listing || !node.children.empty());
            row.available = !node.blocked; row.loading = node.loading;
            row.selected = browse_path_ != "this-pc" && key == browser_key(browse_path_);
            visible_tree_.push_back(row);
            if (node.expanded) for (const auto& child : node.children) append(child, depth + 1);
        };
        if (tree_root_expanded_) for (const auto& key : drive_roots_) append(key, 1);
        ui->set_tree_rows(std::make_shared<slint::VectorModel<BrowserTreeRow>>(visible_tree_));
        ui->set_tree_loading(drives_loading_ || browse_loading_ || std::any_of(tree_.begin(), tree_.end(), [](const auto& pair) { return pair.second.loading; }));
    }
    void show_this_pc(const Ui& ui) {
        auto snapshot = std::make_shared<backend::Snapshot>();
        snapshot->root = "this-pc";
        backend::Entry root{}; root.path = "this-pc"; root.name = "This PC"; root.directory = true;
        snapshot->entries.push_back(root);
        auto known = std::make_shared<std::vector<bool>>(1, false);
        for (const auto& drive : drives_) {
            backend::Entry entry{}; entry.path = drive.path; entry.name = drive.label; entry.parent = 0; entry.directory = true;
            entry.allocated_bytes = drive.total_bytes >= drive.free_bytes ? drive.total_bytes - drive.free_bytes : 0;
            entry.logical_bytes = entry.allocated_bytes;
            snapshot->entries.push_back(entry); known->push_back(drive.capacity_known);
        }
        browse_path_ = "this-pc"; browse_loading_ = false; browse_snapshot_ = snapshot; browse_sizes_ = known;
        ui->set_browser_can_scan(false);
        publish_tree(ui);
        if (ui->get_page() < 2) { ui->set_status("Choose a drive or folder. Browsing does not start a scan."); refresh_rows(ui); }
    }
    void refresh_drives(const Ui& ui) {
        const auto ticket = ++drive_request_;
        drives_loading_ = true; publish_tree(ui);
        auto weak = weak_from_this();
        browser_worker_.push([weak, ticket] {
            std::vector<backend::Drive> drives;
            std::string error;
            try { drives = backend::list_drives(); } catch (const std::exception& ex) { error = ex.what(); }
            post(weak, [ticket, drives = std::move(drives), error](Controller& self, const Ui& ui) {
                if (ticket != self.drive_request_) return;
                self.drives_loading_ = false;
                if (!error.empty()) { ui->set_status(slint::SharedString(error)); self.publish_tree(ui); return; }
                self.drives_ = drives; self.drive_roots_.clear();
                for (const auto& drive : drives) {
                    const auto key = browser_key(drive.path);
                    auto& node = self.tree_[key]; node.path = drive.path;
                    node.label = drive.path.substr(0, 2) + "  " + drive.label.substr(0, drive.label.rfind(" ("));
                    node.detail = drive.capacity_known ? bytes(drive.free_bytes) + " free of " + bytes(drive.total_bytes) : drive.kind + " drive";
                    self.drive_roots_.push_back(key);
                }
                if (self.browse_path_ == "this-pc") self.show_this_pc(ui);
                else self.publish_tree(ui);
            });
        });
    }
    void ensure_browser_node(const std::string& path) {
        // Seed ancestors for typed paths so opening a deep folder still has a
        // visible, stable route back to its drive. Children are loaded lazily.
        std::vector<std::string> chain;
        auto current = std::filesystem::path(wide(path));
        while (!current.empty()) {
            chain.push_back(stale::desktop::recommendation_detail::path(utf8(current.wstring())));
            auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        std::reverse(chain.begin(), chain.end());
        std::wstring previous;
        for (const auto& part : chain) {
            const auto key = browser_key(part);
            auto& node = tree_[key];
            if (node.path.empty()) { node.path = part; node.label = basename(part); if (node.label.empty()) node.label = part; }
            if (!previous.empty()) {
                auto& parent = tree_[previous]; parent.expanded = true;
                if (std::find(parent.children.begin(), parent.children.end(), key) == parent.children.end()) parent.children.push_back(key);
            } else if (std::find(drive_roots_.begin(), drive_roots_.end(), key) == drive_roots_.end()) drive_roots_.push_back(key);
            previous = key;
        }
        tree_root_expanded_ = true;
    }
    void accept_directory(const Ui& ui, const std::wstring& key, const std::shared_ptr<backend::DirectoryListing>& listing) {
        auto& node = tree_.at(key);
        node.loading = false; node.listing = listing;
        node.children.clear();
        for (size_t i = 1; i < listing->snapshot.entries.size(); ++i) {
            const auto& entry = listing->snapshot.entries[i];
            if (!entry.directory) continue;
            const auto child_key = browser_key(entry.path);
            auto& child = tree_[child_key]; child.path = entry.path; child.label = entry.name;
            child.blocked = entry.reparse || entry.cloud;
            child.detail = entry.cloud ? "Cloud folder — not opened" : entry.reparse ? "Link — not followed" : listing->size_known.at(i) ? bytes(entry.allocated_bytes) + " (scan)" : "";
            node.children.push_back(child_key);
        }
        std::sort(node.children.begin(), node.children.end());
        if (!listing->error.empty()) node.detail = listing->error;
        if (browse_path_ != "this-pc" && browser_key(browse_path_) == key) {
            browse_loading_ = false;
            browse_snapshot_ = std::shared_ptr<const backend::Snapshot>(listing, &listing->snapshot);
            browse_sizes_ = std::shared_ptr<const std::vector<bool>>(listing, &listing->size_known);
            const bool readable = !listing->snapshot.entries.empty() && listing->snapshot.entries.front().metadata_ok && listing->error.empty();
            ui->set_browser_can_scan(readable && !node.blocked);
            if (ui->get_page() < 2) {
                ui->set_status(slint::SharedString(listing->error.empty() ? "Folder contents loaded." : listing->error));
                refresh_rows(ui);
            }
        }
        publish_tree(ui);
    }
    void load_directory(const Ui& ui, const std::wstring& key) {
        auto& node = tree_.at(key);
        if (node.loading || node.blocked) return;
        node.loading = true;
        const auto request = node.request = ++tree_request_;
        const auto path = node.path;
        const auto measured = snapshot_;
        publish_tree(ui);
        auto weak = weak_from_this();
        browser_worker_.push([weak, key, request, path, measured] {
            auto listing = std::make_shared<backend::DirectoryListing>();
            try {
                *listing = backend::list_directory(path);
                if (measured) {
                    std::map<std::wstring, size_t> wanted;
                    for (size_t i = 0; i < listing->snapshot.entries.size(); ++i) {
                        const auto& entry = listing->snapshot.entries[i];
                        if (entry.directory && !entry.reparse && !entry.cloud) wanted[browser_key(entry.path)] = i;
                    }
                    for (const auto& entry : measured->entries) {
                        if (!entry.directory) continue;
                        const auto found = wanted.find(browser_key(entry.path));
                        if (found == wanted.end()) continue;
                        auto& target = listing->snapshot.entries[found->second];
                        target.allocated_bytes = entry.allocated_bytes; target.logical_bytes = entry.logical_bytes;
                        listing->size_known[found->second] = entry.metadata_ok;
                    }
                }
            }
            catch (const std::exception& ex) { listing->snapshot.root = path; listing->snapshot.error = listing->error = ex.what(); }
            post(weak, [key, request, listing](Controller& self, const Ui& ui) {
                auto found = self.tree_.find(key);
                if (found == self.tree_.end() || found->second.request != request) return;
                self.accept_directory(ui, key, listing);
            });
        });
    }
    void open_browser(const Ui& ui, const std::string& requested) {
        if (ui->get_review_open()) return;
        ++navigation_generation_;
        clear_selection(ui); clear_detail(ui);
        ui->set_page(1); ui->set_navigation_index(1); ui->set_search("");
        if (requested == "this-pc") { show_this_pc(ui); return; }
        const auto path = browser_path(requested);
        ensure_browser_node(path);
        const auto key = browser_key(path);
        auto& node = tree_.at(key);
        if (node.blocked) { ui->set_status("Links and cloud placeholders are not opened automatically."); publish_tree(ui); return; }
        browse_path_ = path; ui->set_root_path(slint::SharedString(path));
        ui->set_browser_can_scan(false);
        if (node.listing) accept_directory(ui, key, node.listing);
        else {
            browse_loading_ = true;
            auto pending = std::make_shared<backend::Snapshot>(); pending->root = path;
            browse_snapshot_ = pending; browse_sizes_ = std::make_shared<std::vector<bool>>();
            ui->set_status("Loading folder contents…");
            refresh_rows(ui); load_directory(ui, key);
        }
        publish_tree(ui);
    }
    void expand_tree(const Ui& ui, const std::string& id, bool expanded) {
        if (id == "this-pc") { tree_root_expanded_ = expanded; publish_tree(ui); return; }
        const auto key = browser_key(id);
        const auto found = tree_.find(key);
        if (found == tree_.end()) throw std::runtime_error("Enumerate the tree before expanding a folder");
        auto& node = found->second;
        if (node.blocked) return;
        node.expanded = expanded;
        if (expanded && !node.listing) load_directory(ui, key);
        publish_tree(ui);
    }

    void update_drive(const Ui& ui) {
        if (ui->get_page() < 2 && browse_snapshot_ && browse_path_ == "this-pc") {
            drive_total_ = drive_free_ = 0;
            ui->set_capacity_known(false); ui->set_free_fraction(0); ui->set_drive_free("—");
            ui->set_drive_capacity(""); ui->set_drive_title("This PC — choose a drive");
            return;
        }
        const auto location = report_ && ui->get_page() == 5 ? report_root_ : string(ui->get_root_path());
        const auto volume = std::filesystem::path(wide(location)).root_path();
        ULARGE_INTEGER available{}, total{}, free{};
        const auto kind = volume.empty() ? DRIVE_UNKNOWN : GetDriveTypeW(volume.c_str());
        const bool known = !volume.empty() && (kind == DRIVE_FIXED || kind == DRIVE_RAMDISK) && GetDiskFreeSpaceExW(volume.c_str(), &available, &total, &free) && total.QuadPart;
        drive_total_ = known ? total.QuadPart : 0; drive_free_ = known ? free.QuadPart : 0;
        ui->set_capacity_known(known);
        ui->set_free_fraction(known ? static_cast<float>(double(drive_free_) / double(drive_total_)) : 0);
        ui->set_drive_free(slint::SharedString(known ? bytes(drive_free_) : "—"));
        ui->set_drive_capacity(slint::SharedString(known ? bytes(drive_total_) : ""));
        ui->set_drive_title(slint::SharedString(known ? utf8(volume.wstring()) + " has " + bytes(drive_free_) + " free of " + bytes(drive_total_) : "Choose a drive or folder"));
    }
    void clear_detail(const Ui& ui) {
        detail_path_.clear(); detail_generated_ = false;
        ui->set_detail_title(""); ui->set_detail_name(""); ui->set_detail_size(""); ui->set_detail_text("");
        ui->set_detail_state(""); ui->set_detail_metadata(""); ui->set_detail_can_scan(false); ui->set_detail_reveal(false);
    }
    void update_groups(const Ui& ui) {
        const int page = ui->get_page();
        if (page < 2) group_source_page_ = 2;
        else if (page == 2 || page == 3 || page == 5) group_source_page_ = page;
        const auto candidates = source(group_source_page_).candidates;
        if (candidates != grouped_source_) {
            grouped_source_ = candidates;
            groups_ = candidates ? stale::desktop::group_recommendations(*candidates) : std::vector<stale::desktop::RecommendationGroup>{};
            for (auto& group : groups_) std::sort(group.projects.begin(), group.projects.end(), [](const auto& a, const auto& b) { return a.bytes != b.bytes ? a.bytes > b.bytes : a.id < b.id; });
            if (std::none_of(groups_.begin(), groups_.end(), [&](const auto& g) { return g.id == active_group_; })) { active_group_.clear(); active_project_.clear(); }
        }
        std::vector<Opportunity> values;
        std::string title = page < 2 ? "Everything in this scan" : page == 4 ? "Deleted this session" : page == 3 ? "Identical file contents" : "All recommendations";
        for (const auto& group : groups_) {
            Opportunity item{}; item.id = slint::SharedString(group.id); item.title = slint::SharedString(group.label); item.size = slint::SharedString(bytes(group.bytes));
            item.detail = slint::SharedString(std::to_string(group.item_indices.size()) + (group.item_indices.size() == 1 ? " item" : " items")); item.expanded = expanded_groups_.count(group.id) != 0;
            item.state = group.id == "models" || group.id == "review" ? 2 : 0;
            values.push_back(item);
            if (active_group_ == group.id && (page == 2 || page == 3 || page == 5)) title = group.label;
            for (const auto& project : group.projects) {
                if (active_project_ == project.id && (page == 2 || page == 3 || page == 5)) title = project.label;
                if (!item.expanded) continue;
                Opportunity child{}; child.id = slint::SharedString(project.id); child.title = slint::SharedString(project.label); child.size = slint::SharedString(bytes(project.bytes)); child.detail = slint::SharedString(std::to_string(project.item_indices.size()) + (project.item_indices.size() == 1 ? " item" : " items")); child.depth = 1; child.state = item.state;
                values.push_back(std::move(child));
            }
        }
        ui->set_opportunities(std::make_shared<slint::VectorModel<Opportunity>>(values));
        ui->set_active_group(slint::SharedString(active_project_.empty() ? active_group_ : active_project_));
        ui->set_group_title(slint::SharedString(title)); ui->set_has_report(bool(report_));
    }
    void open_group(const Ui& ui, const std::string& id) {
        for (const auto& group : groups_) {
            if (group.id == id) { active_group_ = group.id; active_project_.clear(); ui->set_page(group_source_page_); ui->set_search(""); clear_detail(ui); refresh_rows(ui); return; }
            for (const auto& project : group.projects) if (project.id == id) { active_group_ = group.id; active_project_ = project.id; ui->set_page(group_source_page_); ui->set_search(""); clear_detail(ui); refresh_rows(ui); return; }
        }
        throw std::runtime_error("Unknown recommendation group or project");
    }
    void scan_detail(const Ui& ui) {
        if (busy_ || detail_path_.empty() || !ui->get_detail_can_scan()) return;
        targeted_path_ = detail_path_;
        const auto path = detail_generated_ ? utf8(std::filesystem::path(wide(detail_path_)).parent_path().wstring()) : detail_path_;
        ui->set_root_path(slint::SharedString(path));
        scan(ui, false);
    }

    // Check saved observations as part of selection, preserving the current
    // report, filter and previous selections instead of replacing the scan.
    void check_select(const Ui& ui, int row = -1) {
        if (busy_ || ui->get_review_open() || !rows_ || !rows_->source.candidates) return;
        if (row < -1 || (row >= 0 && static_cast<size_t>(row) >= rows_->indexes.size())) return;
        const int page = ui->get_page();
        if (page != 2 && page != 5) return;
        const auto indexes = row < 0 ? rows_->indexes : std::vector<size_t>{rows_->indexes[row]};
        if (indexes.empty()) return;
        auto candidates = std::make_shared<Candidates>(*rows_->source.candidates);
        auto selected = std::make_shared<std::unordered_set<std::string>>(*selected_);
        start(ui, "Checking and selecting candidates…", true);
        auto control = control_;
        auto weak = weak_from_this();
        enqueue([weak, candidates, selected, indexes, page, control] {
            size_t skipped = 0, added = 0;
            for (const auto index : indexes) {
                if (control->cancel) break;
                auto& item = candidates->at(index);
                if (selected->count(item.path)) continue;
                if (item.validation_token.empty()) {
                    const auto groups = stale::desktop::group_recommendations(Candidates{item});
                    const auto kind = groups.empty() ? std::string("review") : groups.front().id;
                    if (kind == "models" || kind == "review" || kind == "duplicates") { ++skipped; continue; }
                    auto snapshot = backend::scan(item.path, control.get());
                    if (control->cancel) break;
                    const auto fresh = backend::cleanup_candidates(snapshot, true);
                    const auto match = std::find_if(fresh.begin(), fresh.end(), [&](const auto& candidate) { return folded(candidate.path) == folded(item.path); });
                    if (match == fresh.end()) { ++skipped; continue; }
                    item = *match;
                }
                selected->insert(item.path); ++added;
            }
            post(weak, [candidates, selected, page, skipped, added, control](Controller& self, const Ui& ui) {
                self.finish(ui);
                if (page == 5) self.report_ = candidates; else self.cleanup_ = candidates;
                *self.selected_ = *selected;
                self.selected_bytes_ = 0;
                for (const auto& item : *candidates) if (selected->count(item.path)) self.selected_bytes_ += item.bytes;
                self.clear_detail(ui);
                self.selection_status(ui);
                std::string status = control->cancel ? "Cancelled. Completed selections are kept." : std::to_string(added) + " candidates selected. Choose Delete selected when ready.";
                if (skipped) status += " " + std::to_string(skipped) + " could not be selected; inspect them for details.";
                ui->set_status(slint::SharedString(status));
                self.refresh_rows(ui);
            });
        });
    }

    template<class F> static void post(std::weak_ptr<Controller> weak, F action) {
        slint::invoke_from_event_loop([weak, action = std::move(action)]() mutable {
            if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) action(*self, *ui);
        });
    }
    template<class F> void enqueue(F job) {
        auto weak = weak_from_this();
        worker_.push([weak, job = std::move(job)]() mutable {
            try { job(); }
            catch (const std::exception& ex) {
                post(weak, [message = std::string(ex.what())](Controller& self, const Ui& ui) {
                    self.finish(ui);
                    ui->set_status(slint::SharedString("Operation failed: " + message));
                    if (self.smoke_) self.end_smoke(ui, 1);
                });
            } catch (...) {
                post(weak, [](Controller& self, const Ui& ui) {
                    self.finish(ui);
                    ui->set_status(slint::SharedString("Operation failed. Try a smaller folder or restart Stale."));
                    if (self.smoke_) self.end_smoke(ui, 1);
                });
            }
        });
    }
    void start(const Ui& ui, std::string activity, bool cancellable) {
        busy_ = true;
        activity_ = std::move(activity);
        control_ = std::make_shared<backend::ScanControl>();
        ui->set_busy(true);
        ui->set_can_cancel(cancellable);
        ui->set_status(slint::SharedString(activity_));
    }
    void finish(const Ui& ui) {
        busy_ = false;
        ui->set_busy(false);
        ui->set_can_cancel(false);
        control_.reset();
    }
    Source source(int page) const {
        if (page == 2) return { {}, cleanup_, {} };
        if (page == 3) return { {}, duplicates_, {} };
        if (page == 4) return { {}, {}, outcomes_ };
        if (page == 5) return { {}, report_, {} };
        if (browse_snapshot_ && !smoke_) return { browse_snapshot_, {}, {}, browse_sizes_, true,
            browse_path_ == "this-pc" ? std::make_shared<const std::vector<backend::Drive>>(drives_) : nullptr };
        return { snapshot_, {}, {} };
    }
    void selection_status(const Ui& ui) {
        ui->set_selection_count(static_cast<int>(selected_->size()));
        ui->set_selection_label(slint::SharedString(selected_->empty() ? "" : std::to_string(selected_->size()) + " selected for deletion · " + bytes(selected_bytes_) + " estimated"));
        std::vector<std::string> paths(selected_->begin(), selected_->end());
        std::sort(paths.begin(), paths.end());
        std::string listed;
        for (const auto& path : paths) { if (!listed.empty()) listed += "\n"; listed += path; }
        ui->set_selection_paths(slint::SharedString(listed));
    }
    void clear_selection(const Ui& ui) {
        selected_->clear();
        selected_bytes_ = 0;
        if (rows_) rows_->selection_reset();
        selection_status(ui);
        if (rows_ && ui->get_focused_row() >= 0) inspect(ui, ui->get_focused_row());
    }
    void accept_snapshot(const Ui& ui, std::shared_ptr<backend::Snapshot> snapshot, std::shared_ptr<Candidates> candidates, bool cached, std::string cache_error) {
        finish(ui);
        const bool navigated = !smoke_ && scan_navigation_ != navigation_generation_;
        const bool targeted = !targeted_path_.empty();
        const auto target = targeted_path_; targeted_path_.clear();
        if (targeted && snapshot->cancelled && report_) {
            ui->set_page(5); ui->set_status("Scan cancelled. Showing the saved map."); refresh_rows(ui); return;
        }
        summary_only_ = false;
        ui->set_summary_only(false);
        snapshot_ = std::move(snapshot);
        cleanup_ = std::move(candidates);
        if (report_ && !snapshot_->cancelled) {
            auto refreshed = std::make_shared<Candidates>(*report_);
            std::unordered_map<std::wstring, size_t> report_paths;
            for (size_t index = 0; index < refreshed->size(); ++index) report_paths.emplace(browser_key(refreshed->at(index).path), index);
            std::unordered_map<std::wstring, const backend::Candidate*> current_paths;
            for (const auto& item : *cleanup_) current_paths.emplace(browser_key(item.path), &item);
            for (const auto& measured : snapshot_->entries) {
                if (!measured.metadata_ok) continue;
                const auto key = browser_key(measured.path);
                const auto found = report_paths.find(key);
                if (found == report_paths.end()) continue;
                auto& item = refreshed->at(found->second);
                item.bytes = measured.allocated_bytes; item.modified = measured.modified;
                item.deletion_error.clear(); item.validation_token.clear();
                const auto current = current_paths.find(key);
                if (current != current_paths.end()) item = *current->second;
            }
            report_ = refreshed;
        }
        duplicates_.reset();
        ui->set_duplicates_ready(false);
        folder_ = 0;
        clear_selection(ui);
        ui->set_has_snapshot(!snapshot_->entries.empty());
        if (!navigated) ui->set_root_path(slint::SharedString(snapshot_->root));
        ui->set_allocated(slint::SharedString(snapshot_->entries.empty() ? "—" : bytes(snapshot_->entries[0].allocated_bytes)));
        ui->set_logical(slint::SharedString(snapshot_->entries.empty() ? "—" : bytes(snapshot_->entries[0].logical_bytes)));
        ui->set_file_count(slint::SharedString(std::to_string(snapshot_->files)));
        ui->set_error_count(slint::SharedString(std::to_string(snapshot_->errors)));
        std::string label = (cached ? "Saved scan from " : "Scanned ") + date(snapshot_->scanned_at, true);
        label += snapshot_->cancelled ? " · Incomplete (cancelled)" : " · Refresh to see later changes";
        ui->set_snapshot_label(slint::SharedString(label));
        scan_label_ = label;
        const bool failed_root = snapshot_->entries.empty() || !snapshot_->entries.front().metadata_ok;
        std::string status = snapshot_->cancelled ? "Scan cancelled. Showing the files discovered so far." : failed_root ? "Could not scan this folder." : cached ? "Saved scan loaded. These sizes may have changed; refresh when ready." : "Scan complete.";
        if (snapshot_->errors) status += " " + std::to_string(snapshot_->errors) + (snapshot_->errors == 1 ? " item could not be fully read." : " items could not be fully read.");
        if (!snapshot_->error.empty()) status += " " + snapshot_->error;
        if (!cache_error.empty()) status += " Saved index: " + cache_error;
        ui->set_status(slint::SharedString(status));
        clear_detail(ui);
        if (targeted && report_) {
            auto merged = std::make_shared<Candidates>(*report_);
            const auto target_key = folded(target);
            for (auto& item : *merged) {
                if (folded(item.path) != target_key) continue;
                item.validation_token.clear();
                const auto found = std::find_if(cleanup_->begin(), cleanup_->end(), [&](const auto& candidate) { return folded(candidate.path) == target_key; });
                if (found != cleanup_->end()) { item = *found; ui->set_status("Folder refreshed. Select it using its checkbox."); }
                else ui->set_status("Folder scanned. No current cleanup candidate was found for this saved recommendation.");
            }
            report_ = merged; ui->set_page(5);
        } else if (!targeted && !navigated && !smoke_ && (ui->get_page() < 2 || ui->get_page() == 2 || ui->get_page() == 5)) {
            const bool from_cleanup = ui->get_page() == 2 || ui->get_page() == 5;
            ui->set_page(from_cleanup ? 2 : 1);
            active_group_.clear(); active_project_.clear(); ui->set_search("");
        }
        // Invalidate only loaded live listings; their next open reattaches
        // directory totals from this scan without throwing away expansion state.
        for (auto& pair : tree_) pair.second.listing.reset();
        if (!targeted && !navigated && !smoke_ && ui->get_page() < 2 && !failed_root) open_browser(ui, snapshot_->root);
        update_drive(ui);
        if (smoke_) ui->set_page(smoke_page_);
        if (smoke_ && smoke_page_ == 3 && !snapshot_->entries.empty()) find_duplicates(ui);
        refresh_rows(ui);
    }
    void scan(const Ui& ui, bool cached) {
        if (busy_) return;
        const auto path = string(ui->get_root_path());
        if (path.empty()) { ui->set_status(slint::SharedString("Enter a folder path or choose a folder first.")); return; }
        ++*query_generation_;
        scan_navigation_ = navigation_generation_;
        start(ui, cached ? "Loading saved index…" : "Scanning files…", !cached);
        const auto control = control_;
        auto weak = weak_from_this();
        enqueue([weak, path, cached, control] {
            auto snapshot = std::make_shared<backend::Snapshot>();
            std::string error;
            if (cached) {
                if (!backend::load_snapshot(backend::cache_path(path), *snapshot, &error)) {
                    post(weak, [error](Controller& self, const Ui& ui) {
                        self.finish(ui);
                        ui->set_status(slint::SharedString("No readable saved scan for this folder. Scan it to create one. " + error));
                        if (self.smoke_) self.end_smoke(ui, 1);
                    });
                    return;
                }
            } else {
                *snapshot = backend::scan(path, control.get());
                if (!snapshot->cancelled && !snapshot->entries.empty())
                    backend::save_snapshot(*snapshot, backend::cache_path(path), &error);
            }
            auto candidates = std::make_shared<Candidates>(backend::cleanup_candidates(*snapshot));
            post(weak, [snapshot, candidates, cached, error](Controller& self, const Ui& ui) {
                self.accept_snapshot(ui, snapshot, candidates, cached, error);
            });
        });
    }
    void find_duplicates(const Ui& ui) {
        if (busy_ || !snapshot_ || summary_only_) return;
        start(ui, "Comparing file contents…", true);
        auto weak = weak_from_this();
        const auto snapshot = snapshot_;
        const auto control = control_;
        enqueue([weak, snapshot, control] {
            auto candidates = std::make_shared<Candidates>(backend::find_duplicates(*snapshot, control.get()));
            post(weak, [candidates, control](Controller& self, const Ui& ui) {
                self.finish(ui);
                self.duplicates_ = candidates;
                self.clear_selection(ui);
                ui->set_duplicates_ready(!control->cancel);
                ui->set_status(slint::SharedString(control->cancel ? "Duplicate check cancelled. Review any completed groups below." : "Content comparison complete. One copy in each group is retained."));
                self.refresh_rows(ui);
            });
        });
    }
    void refresh_rows(const Ui& ui) {
        update_groups(ui);
        update_drive(ui);
        const auto generation = ++*query_generation_;
        const auto serial = query_generation_;
        const int page = ui->get_page(), sort = ui->get_sort_order();
        ui->set_snapshot_label(page == 5 ? ui->get_report_label() : slint::SharedString(scan_label_));
        ui->set_summary_only(page == 5 || summary_only_);
        const bool only_folders = ui->get_folders_only();
        const auto query = folded(string(ui->get_search()));
        auto data = source(page);
        const bool overview = page < 2 && snapshot_ && !summary_only_ && !snapshot_->entries.empty()
            && (data.live ? browse_path_ != "this-pc" && browser_key(browse_path_) == browser_key(snapshot_->root) : folder_ == 0);
        ui->set_scan_overview(overview);
        ui->set_scope_label(slint::SharedString(page < 2 ? (data.live ? browse_path_ == "this-pc" ? "This PC" : browse_path_ : snapshot_ ? snapshot_->root : "Choose a folder") : page == 4 ? "Deletion activity" : page == 5 ? report_root_ : snapshot_ ? snapshot_->root : "Choose a folder"));
        std::vector<Opportunity> summaries;
        if (overview && cleanup_) {
            for (const auto& group : stale::desktop::group_recommendations(*cleanup_)) {
                Opportunity item{}; item.id = group.id; item.title = group.label; item.size = bytes(group.bytes);
                item.detail = std::to_string(group.item_indices.size()) + (group.item_indices.size() == 1 ? " candidate" : " candidates");
                summaries.push_back(item);
                if (summaries.size() == 4) break;
            }
        }
        ui->set_scan_summaries(std::make_shared<slint::VectorModel<Opportunity>>(summaries));
        ui->set_cleanup_candidate_count(data.candidates ? static_cast<int>(data.candidates->size()) : 0);
        ui->set_cleanup_ready_count(data.candidates ? static_cast<int>(std::count_if(data.candidates->begin(), data.candidates->end(), [](const auto& item) { return !item.validation_token.empty(); })) : 0);
        const int folder = data.live ? 0 : folder_;
        if (data.live) ui->set_group_title(slint::SharedString(browse_path_ == "this-pc" ? "This PC" : basename(browse_path_).empty() ? browse_path_ : basename(browse_path_)));
        std::vector<bool> allowed;
        if (data.candidates) {
            allowed.resize(data.candidates->size(), false);
            for (const auto& group : groups_) {
                if (!active_group_.empty() && group.id != active_group_) continue;
                if (active_project_.empty()) for (const auto index : group.item_indices) allowed[index] = true;
                else for (const auto& project : group.projects) if (project.id == active_project_) for (const auto index : project.item_indices) allowed[index] = true;
            }
        }
        // Old rows belong to a different query/page and cannot remain actionable.
        rows_.reset();
        agent_row_ids_.clear();
        ui->set_rows(std::make_shared<slint::VectorModel<FileRow>>());
        ui->set_result_label(slint::SharedString("Updating list…"));
        ui->set_location(slint::SharedString(data.live ? browse_path_ == "this-pc" ? "This PC" : browse_path_ : snapshot_ && !snapshot_->entries.empty() ? snapshot_->entries[folder_].path : ""));
        ui->set_can_up(data.live ? browse_path_ != "this-pc" : snapshot_ && folder_ > 0);
        ui->set_empty_message(slint::SharedString(busy_ ? "Working… Results will appear here." : page == 3 && !duplicates_ ? "Choose Find duplicates to compare file contents. This can take a while." : page == 4 ? "Deletion results will appear here, one outcome per item." : !snapshot_ ? "Choose a folder, then scan to explore its contents." : "No matching items. Try another search or folder."));
        if (!busy_ && (page == 2 || page == 5)) {
            ui->set_empty_message(!query.empty() || !active_group_.empty() ? "No candidates match this view. Clear the filter or choose another category."
                : !snapshot_ ? "Choose a folder in the title bar, then scan to find cleanup candidates."
                : "No cleanup candidates found in this scan. Choose another folder to scan; ordinary files are not automatically cleanup candidates.");
        }
        if (data.live) ui->set_empty_message(slint::SharedString(browse_loading_ ? "Loading folder contents…" : !data.snapshot->error.empty() ? data.snapshot->error : "This folder is empty, or no items match your filter."));
        else if (snapshot_ && (snapshot_->entries.empty() || !snapshot_->entries.front().metadata_ok))
            ui->set_empty_message("This folder could not be read. Check the path and permissions, or choose another folder.");
        auto weak = weak_from_this();
        query_worker_.push([weak, generation, serial, data, page, folder, sort, only_folders, query, allowed = std::move(allowed)] {
            try {
            if (serial->load() != generation) return;
            std::vector<size_t> indexes;
            const size_t count = data.candidates ? data.candidates->size() : data.outcomes ? data.outcomes->size() : data.snapshot ? data.snapshot->entries.size() : 0;
            for (size_t i = 0; i < count; ++i) {
                if ((i & 4095) == 0 && serial->load() != generation) return;
                if (data.candidates && !allowed[i]) continue;
                std::string path;
                if (data.candidates) path = data.candidates->at(i).path;
                else if (data.outcomes) path = data.outcomes->at(i).path + data.outcomes->at(i).message;
                else {
                    const auto& item = data.snapshot->entries[i];
                    if (i == 0 || (query.empty() && item.parent != folder) || (only_folders && !item.directory)) continue;
                    path = item.path;
                }
                if (!query.empty() && folded(path).find(query) == std::wstring::npos) continue;
                indexes.push_back(i);
            }
            auto less = [&](size_t a, size_t b) {
                if (data.candidates) {
                    const auto& x = data.candidates->at(a); const auto& y = data.candidates->at(b);
                    if (sort == 0 && x.bytes != y.bytes) return x.bytes > y.bytes;
                    if (sort == 2 && x.modified != y.modified) return x.modified < y.modified;
                    return x.path < y.path;
                }
                if (data.outcomes) return a < b;
                const auto& x = data.snapshot->entries[a]; const auto& y = data.snapshot->entries[b];
                if (data.drives) return folded(x.path) < folded(y.path);
                if (sort == 0 && x.allocated_bytes != y.allocated_bytes) return x.allocated_bytes > y.allocated_bytes;
                if (sort == 2 && x.modified != y.modified) return x.modified < y.modified;
                return x.name < y.name;
            };
            std::sort(indexes.begin(), indexes.end(), less);
            if (serial->load() != generation) return;
            post(weak, [generation, data, indexes = std::move(indexes)](Controller& self, const Ui& ui) mutable {
                if (self.query_generation_->load() != generation) return;
                auto model = std::make_shared<Rows>();
                model->source = data;
                model->indexes = std::move(indexes);
                model->selected = self.selected_;
                self.rows_ = model;
                ui->set_rows(model);
                ui->set_focused_row(-1);
                for (size_t row = 0; row < model->indexes.size(); ++row) {
                    const auto index = model->indexes[row];
                    const auto path = data.candidates ? data.candidates->at(index).path : data.snapshot ? data.snapshot->entries[index].path : data.outcomes ? data.outcomes->at(index).path : std::string{};
                    if (!self.detail_path_.empty() && path == self.detail_path_) { ui->set_focused_row(static_cast<int>(row)); break; }
                }
                ui->set_result_label(slint::SharedString(std::to_string(model->row_count()) + (model->row_count() == 1 ? " item" : " items")));
                self.selection_status(ui);
                if (self.smoke_ && !self.busy_ && !self.smoke_finished_) self.end_smoke(ui, self.snapshot_ && !self.snapshot_->entries.empty() && self.snapshot_->error.empty() ? 0 : 1);
            });
            } catch (const std::exception& error) {
                post(weak, [generation, message = std::string(error.what())](Controller& self, const Ui& ui) {
                    if (self.query_generation_->load() != generation) return;
                    ui->set_status(slint::SharedString("Could not display this view: " + message));
                });
            }

        });
    }
    void inspect(const Ui& ui, int row, bool activate = false) {
        if (!rows_ || row < 0 || static_cast<size_t>(row) >= rows_->indexes.size()) return;
        ui->set_focused_row(row);
        const auto index = rows_->indexes[row];
        auto data = rows_->source;
        std::string title, detail;
        detail_path_.clear();
        detail_generated_ = false;
        ui->set_detail_checked(false); ui->set_detail_review(false); ui->set_detail_can_scan(false);
        ui->set_detail_size(""); ui->set_detail_state(""); ui->set_detail_metadata("");
        if (data.candidates) {
            const auto& item = data.candidates->at(index);
            detail_path_ = item.path;
            title = item.path;
            detail = item.deletion_error.empty() ? item.reason : item.deletion_error;
            const auto classification = stale::desktop::group_recommendations(Candidates{item});
            const auto kind = classification.empty() ? std::string("review") : classification.front().id;
            detail_generated_ = kind == "builds" || kind == "dependencies" || kind == "caches";
            const bool checked = !item.validation_token.empty();
            ui->set_detail_checked(selected_->count(item.path) != 0); ui->set_detail_review(kind == "models" || kind == "review");
            ui->set_detail_size(slint::SharedString(bytes(item.bytes)));
            ui->set_detail_state(slint::SharedString(!item.deletion_error.empty() ? "Deletion failed. Rescan before retrying; some contents may already be gone." : selected_->count(item.path) ? "Selected for deletion" : kind == "models" ? "Review this model before selecting it." : "Not selected"));
            std::string metadata = item.modified > 0 ? "Modified " + date(item.modified, true) + "." : "";
            if (kind == "models") metadata += " Manage model downloads in the application that downloaded them.";
            ui->set_detail_metadata(slint::SharedString(metadata));
            ui->set_detail_can_scan(!checked && kind != "models");
        } else if (data.outcomes) {
            const auto& item = data.outcomes->at(index);
            detail_path_ = item.path;
            title = item.path;
            detail = item.success ? item.path + " — permanently deleted." : item.message;
        } else if (data.snapshot) {
            const auto& item = data.snapshot->entries[index];
            if (activate && item.directory && !item.reparse && !item.cloud) {
                if (data.live) { open_browser(ui, item.path); return; }
                folder_ = static_cast<int>(index);
                ui->set_page(1);
                ui->set_search(slint::SharedString(""));
                refresh_rows(ui);
                return;
            }
            detail_path_ = item.path;
            title = item.path;
            const bool known = !data.size_known || data.size_known->at(index);
            detail = known ? bytes(item.logical_bytes) + " logical · " + bytes(item.allocated_bytes) + " on disk." : "Folder size has not been measured. Scan this folder to measure its contents.";
            if (item.modified > 0) detail += " Last modified " + date(item.modified, true) + ".";
            if (item.cloud) detail += " Cloud content was not downloaded.";
            if (item.reparse) detail += " Reparse points are not followed.";
            ui->set_detail_size(slint::SharedString(known ? bytes(item.allocated_bytes) : "Not scanned"));
            ui->set_detail_state(data.live ? (item.directory && known ? "Live listing; folder size from saved scan" : "Live folder listing") : summary_only_ ? "Saved summary. Folder contents are not indexed." : "From the current scan");
            ui->set_detail_can_scan(item.directory && !item.cloud && !item.reparse);
            if (data.live && data.snapshot->root == "this-pc") {
                const auto drive = std::find_if(drives_.begin(), drives_.end(), [&](const auto& candidate) { return browser_key(candidate.path) == browser_key(item.path); });
                if (drive != drives_.end()) {
                    detail = drive->capacity_known ? bytes(drive->free_bytes) + " free of " + bytes(drive->total_bytes) + ". Open the drive to browse its folders." : "Open this " + drive->kind + " drive to check availability and browse its folders.";
                    ui->set_detail_state("Drive");
                    ui->set_detail_size(slint::SharedString(drive->capacity_known ? bytes(item.allocated_bytes) + " used" : "Capacity not checked"));
                }
            }
        }
        ui->set_detail_name(slint::SharedString(basename(title)));
        ui->set_detail_title(slint::SharedString(title));
        ui->set_detail_text(slint::SharedString(detail));
        ui->set_detail_reveal(!detail_path_.empty());
    }
    void toggle(const Ui& ui, int row) {
        if (busy_ || !rows_ || !rows_->source.candidates || row < 0 || static_cast<size_t>(row) >= rows_->indexes.size()) return;
        const auto& item = rows_->source.candidates->at(rows_->indexes[row]);
        if (!item.deletion_error.empty() || (item.validation_token.empty() && !rows_->row_data(row)->checkable)) return;
        if (selected_->erase(item.path)) selected_bytes_ -= item.bytes;
        else { selected_->insert(item.path); selected_bytes_ += item.bytes; }
        rows_->selection_changed(row);
        inspect(ui, row);
        selection_status(ui);
    }
    void recycle(const Ui& ui) {
        if (busy_) return;
        const auto data = source(ui->get_page()).candidates;
        if (!data || selected_->empty()) return;
        Candidates items;
        for (const auto& item : *data) if (selected_->count(item.path)) items.push_back(item);
        if (items.empty()) return;
        start(ui, "Deleting selected items permanently…", false);
        auto weak = weak_from_this();
        enqueue([weak, items = std::move(items)] {
            const auto result = backend::delete_permanently(items, [weak](const std::string& message) {
                post(weak, [message](Controller& self, const Ui& ui) {
                    self.activity_ = message;
                    ui->set_status(slint::SharedString(message));
                });
            });
            auto outcomes = std::make_shared<std::vector<Outcome>>();
            for (const auto& path : result.recycled) outcomes->push_back({path, {}, true});
            for (const auto& failure : result.failure_details) outcomes->push_back({failure.path, failure.reason, false});
            post(weak, [outcomes, good = result.recycled.size(), failed = outcomes->size() - result.recycled.size()](Controller& self, const Ui& ui) {
                self.finish(ui);
                self.outcomes_ = outcomes;
                self.clear_selection(ui);
                auto retain = [&](std::shared_ptr<const Candidates>& source) {
                    if (!source) return;
                    auto remaining = std::make_shared<Candidates>();
                    for (auto candidate : *source) {
                        const auto outcome = std::find_if(outcomes->begin(), outcomes->end(), [&](const auto& value) { return value.path == candidate.path; });
                        if (outcome != outcomes->end()) {
                            if (outcome->success) continue;
                            candidate.deletion_error = outcome->message;
                            candidate.validation_token.clear();
                        }
                        remaining->push_back(std::move(candidate));
                    }
                    source = remaining;
                };
                retain(self.cleanup_); retain(self.report_);
                self.duplicates_.reset();
                ui->set_duplicates_ready(false);
                self.clear_detail(ui);
                ui->set_status(slint::SharedString(std::to_string(good) + " deleted; " + std::to_string(failed) + " failed." + (failed ? " Failed folders may be partly removed. Rescan before retrying." : " Remaining candidates are ready to review.")));
                self.scan_label_ = "Sizes are from before deletion. Rescan to update totals.";
                ui->set_report_label("Saved report · Sizes are from before deletion. Rescan to update totals.");
                self.refresh_rows(ui);
            });
        });
    }
    void end_smoke(const Ui&, int code) {
        if (smoke_finished_) return;
        smoke_finished_ = true;
        *exit_code_ = code;
        auto weak = weak_from_this();
        slint::Timer::single_shot(std::chrono::milliseconds(600), [weak] {
            if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) {
                if (!self->screenshot_.empty()) {
                    auto pixels = (*ui)->window().take_snapshot();
                    if (pixels) {
                        std::ofstream out(std::filesystem::path(wide(self->screenshot_)), std::ios::binary);
                        out << "P6\n" << pixels->width() << ' ' << pixels->height() << "\n255\n";
                        for (const auto& pixel : *pixels) { const char rgb[] = {static_cast<char>(pixel.r), static_cast<char>(pixel.g), static_cast<char>(pixel.b)}; out.write(rgb, 3); }
                        if (!out) *self->exit_code_ = 2;
                    } else *self->exit_code_ = 2;
                }
                std::printf("Stale smoke: %s; %zu visible-model rows\n", *self->exit_code_ == 0 ? "PASS" : "FAIL", self->rows_ ? self->rows_->row_count() : 0);
                slint::quit_event_loop();
            }
        });
    }
public:
    // Called only by AgentBridge on the GUI thread. The pipe is opt-in, and
    // commands share the same controller callbacks as the visible controls.
    std::string agent_command(const std::string& request) noexcept {
        try {
            const auto input = Json::parse(request);
            if (!input.is_object()) throw std::runtime_error("Request must be a JSON object");
            auto locked = ui_.lock();
            if (!locked) throw std::runtime_error("Window is closed");
            const auto& ui = *locked;
            const auto action = input.at("action").get<std::string>();
            static const std::vector<std::string> pages = {"overview", "browse", "cleanup", "duplicates", "activity", "recommendations"};
            auto state = [&]() -> Json {
                return {{"ok", true}, {"page", pages.at(ui->get_page())}, {"root", string(ui->get_root_path())},
                    {"busy", busy_}, {"theme", ui->get_dark_mode() ? "dark" : "light"}, {"rows_ready", bool(rows_) && !(ui->get_page() < 2 && browse_loading_)}, {"view_revision", query_generation_->load()},
                    {"browse_path", browse_path_}, {"browse_busy", browse_loading_}, {"tree_loading", ui->get_tree_loading()},
                    {"browser_error", browse_snapshot_ ? browse_snapshot_->error : ""}, {"browser_can_scan", ui->get_browser_can_scan()},
                    {"navigation_index", ui->get_navigation_index()}, {"navigation_expanded", ui->get_navigation_expanded()},
                    {"window_maximized", ui->get_window_maximized()}, {"window_minimized", ui->get_window_minimized()},
                    {"status", string(ui->get_status())}, {"progress_files", control_ ? control_->files.load() : 0},
                    {"can_cancel", ui->get_can_cancel()}, {"row_count", rows_ ? rows_->row_count() : 0},
                    {"selection_count", selected_->size()}, {"selection_bytes", selected_bytes_},
                    {"selection_label", string(ui->get_selection_label())}, {"selection_bar_visible", ui->get_selection_count() > 0 || ui->get_page() == 2 || ui->get_page() == 5},
                    {"cleanup_candidate_count", ui->get_cleanup_candidate_count()}, {"cleanup_ready_count", ui->get_cleanup_ready_count()},
                    {"search", string(ui->get_search())}, {"sort", ui->get_sort_order()},
                    {"detail_path", detail_path_}, {"detail", string(ui->get_detail_text())},
                    {"summary_only", ui->get_summary_only()}, {"snapshot_label", string(ui->get_snapshot_label())},
                    {"snapshot_files", snapshot_ ? snapshot_->files : 0}, {"snapshot_errors", snapshot_ ? snapshot_->errors : 0},
                    {"snapshot_scanned_at", snapshot_ ? snapshot_->scanned_at : 0.0},
                    {"allocated_bytes", snapshot_ && !snapshot_->entries.empty() ? snapshot_->entries.front().allocated_bytes : 0},
                    {"logical_bytes", snapshot_ && !snapshot_->entries.empty() ? snapshot_->entries.front().logical_bytes : 0},
                    {"drive_total_bytes", drive_total_}, {"drive_free_bytes", drive_free_},
                    {"storage_slices", drive_total_ ? Json::array({{{"id", "used"}, {"label", "Used"}, {"start", 0.0}, {"fraction", 1.0 - double(drive_free_) / double(drive_total_)}}, {{"id", "free"}, {"label", "Free"}, {"start", 1.0 - double(drive_free_) / double(drive_total_)}, {"fraction", double(drive_free_) / double(drive_total_)}}}) : Json::array()},
                    {"active_group", active_group_}, {"active_project", active_project_}, {"detail_can_scan", ui->get_detail_can_scan()}, {"detail_checked", ui->get_detail_checked()},
                    {"review_open", ui->get_review_open()}, {"agent_enabled", ui->get_agent_enabled()},
                    {"scan_overview", ui->get_scan_overview()}, {"scope", string(ui->get_scope_label())},
                    {"inspector_visible", !string(ui->get_detail_name()).empty()},
                    {"detail_selected", !detail_path_.empty() && selected_->count(detail_path_) != 0}};
            };
            if (action == "capabilities") return Json({{"ok", true}, {"protocol", 1},
                {"actions", {"state", "theme", "navigate", "navigation-menu", "window", "drives", "tree", "tree-expand", "tree-open", "browse", "refresh-drives", "groups", "open-group", "open-project", "expand-group", "scan-detail", "check-select", "select-visible", "resize", "rows", "scan", "load-cache", "load-report", "cancel", "page", "filter", "sort", "inspect", "open", "up", "root", "select", "clear-selection", "find-duplicates", "review", "dismiss-review", "close-inspector", "overview-group", "screenshot", "capabilities"}},
                {"pages", pages}, {"max_rows", 100}, {"row_ids", "Full paths; enumerate rows before inspect/select/open. Pass view_revision to reject stale requests."},
                {"recycle", "No agent deletion-confirmation command. review opens the visible confirmation only."},
                {"async", "Poll state until busy=false and rows_ready=true after scan/filter/page/report."}}).dump();
            if (action == "state") return state().dump();
            if (action == "drives") {
                Json list = Json::array();
                for (const auto& drive : drives_) list.push_back({{"id", drive.path}, {"path", drive.path}, {"label", drive.label}, {"kind", drive.kind}, {"ready", drive.ready}, {"capacity_known", drive.capacity_known}, {"total_bytes", drive.total_bytes}, {"free_bytes", drive.free_bytes}});
                return Json({{"ok", true}, {"loading", drives_loading_}, {"drives", list}}).dump();
            }
            if (action == "tree") {
                const auto offset = input.value("offset", 0), limit = input.value("limit", 50);
                if (offset < 0 || limit < 1 || limit > 100) throw std::runtime_error("Tree pagination requires offset >= 0 and limit 1-100");
                Json list = Json::array();
                for (size_t i = static_cast<size_t>(offset); i < visible_tree_.size() && list.size() < static_cast<size_t>(limit); ++i) {
                    const auto& row = visible_tree_[i];
                    list.push_back({{"id", string(row.id)}, {"label", string(row.label)}, {"detail", string(row.detail)}, {"depth", row.depth}, {"expanded", row.expanded}, {"expandable", row.expandable}, {"loading", row.loading}, {"available", row.available}, {"selected", row.selected}});
                }
                return Json({{"ok", true}, {"offset", offset}, {"total", visible_tree_.size()}, {"loading", ui->get_tree_loading()}, {"rows", list}}).dump();
            }
            if (action == "theme") {
                const auto mode = input.at("mode").get<std::string>();
                if (mode != "light" && mode != "dark") throw std::runtime_error("Theme mode must be light or dark");
                change_theme(ui, mode == "dark");
                return state().dump();
            }
            if (action == "window") {
                const auto operation = input.at("operation").get<std::string>();
                if (operation == "maximize") ui->invoke_maximize_window();
                else if (operation == "restore") ui->invoke_restore_window();
                else if (operation == "minimize") ui->invoke_minimize_window();
                else throw std::runtime_error("Window operation must be minimize, maximize, or restore");
                return state().dump();
            }
            if (action == "groups") {
                Json list = Json::array();
                for (const auto& group : groups_) {
                    Json projects = Json::array();
                    for (const auto& project : group.projects) {
                        if (projects.size() >= 100) break;
                        projects.push_back({{"id", project.id}, {"label", project.label}, {"path", project.path}, {"bytes", project.bytes}, {"count", project.item_indices.size()}});
                    }
                    list.push_back({{"id", group.id}, {"label", group.label}, {"explanation", group.explanation}, {"bytes", group.bytes}, {"count", group.item_indices.size()}, {"projects", projects}, {"project_count", group.projects.size()}, {"expanded", expanded_groups_.count(group.id) != 0}});
                }
                return Json({{"ok", true}, {"view_revision", query_generation_->load()}, {"groups", list}}).dump();
            }
            if (action == "resize") {
                const int width = input.at("width").get<int>(), height = input.at("height").get<int>();
                if (width < 1000 || width > 3840 || height < 660 || height > 2160) throw std::runtime_error("Window size must be between 1000x660 and 3840x2160");
                ui->window().set_size(slint::LogicalSize{{static_cast<float>(width), static_cast<float>(height)}});
                return state().dump();
            }
            if (action == "rows") {
                if (!rows_) throw std::runtime_error("Rows are updating; poll state until rows_ready");
                const int offset = input.value("offset", 0), limit = input.value("limit", 50);
                if (offset < 0 || limit < 1 || limit > 100) throw std::runtime_error("offset must be nonnegative; limit must be 1..100");
                if (agent_row_ids_.size() > 1000) agent_row_ids_.clear();
                Json values = Json::array();
                const auto end = std::min(rows_->row_count(), static_cast<size_t>(offset) + limit);
                for (size_t row = static_cast<size_t>(offset); row < end; ++row) {
                    const auto index = rows_->indexes[row];
                    Json value = {{"row", row}};
                    std::string id;
                    if (rows_->source.candidates) {
                        const auto& item = rows_->source.candidates->at(index); id = item.path;
                        value.update({{"id", id}, {"path", item.path}, {"name", basename(item.path)}, {"bytes", item.bytes},
                            {"modified", item.modified}, {"reason", item.reason}, {"selected", selected_->count(id) != 0},
                            {"selectable", rows_->row_data(row)->selectable || rows_->row_data(row)->checkable},
                            {"validated", !item.validation_token.empty()}, {"deletion_error", item.deletion_error}, {"duplicate_keeper", item.duplicate_keeper}});
                    } else if (rows_->source.snapshot) {
                        const auto& item = rows_->source.snapshot->entries[index]; id = item.path;
                        value.update({{"id", id}, {"path", item.path}, {"name", item.name}, {"bytes", item.allocated_bytes},
                            {"logical_bytes", item.logical_bytes}, {"modified", item.modified}, {"directory", item.directory},
                            {"reparse", item.reparse}, {"cloud", item.cloud}, {"selectable", false},
                            {"size_known", !rows_->source.size_known || rows_->source.size_known->at(index)}, {"live", rows_->source.live}});
                        if (rows_->source.drives) {
                            const auto visual = rows_->row_data(row).value();
                            value.update({{"drive", true}, {"capacity_known", visual.capacity_known}, {"used_fraction", visual.used_fraction},
                                {"capacity_label", string(visual.capacity_label)}, {"free_label", string(visual.free_label)}});
                        }
                    } else if (rows_->source.outcomes) {
                        const auto& item = rows_->source.outcomes->at(index); id = item.path;
                        value.update({{"id", id}, {"path", item.path}, {"success", item.success}, {"message", item.message}, {"selectable", false}});
                    }
                    agent_row_ids_[id] = row;
                    values.push_back(std::move(value));
                }
                return Json({{"ok", true}, {"view_revision", query_generation_->load()}, {"total", rows_->row_count()}, {"offset", offset}, {"rows", values}}).dump();
            }
            if (action == "screenshot") {
                auto pixels = ui->window().take_snapshot();
                if (!pixels) throw std::runtime_error("Renderer could not capture this window");
                const auto path = input.at("path").get<std::string>();
                std::ofstream out(std::filesystem::path(wide(path)), std::ios::binary);
                out << "P6\n" << pixels->width() << ' ' << pixels->height() << "\n255\n";
                for (const auto& pixel : *pixels) { const char rgb[] = {static_cast<char>(pixel.r), static_cast<char>(pixel.g), static_cast<char>(pixel.b)}; out.write(rgb, 3); }
                if (!out) throw std::runtime_error("Could not write screenshot");
                return Json({{"ok", true}, {"path", path}, {"format", "ppm"}, {"width", pixels->width()}, {"height", pixels->height()}}).dump();
            }
            if (action == "cancel") { if (!ui->get_can_cancel()) throw std::runtime_error("No cancellable operation is running"); ui->invoke_cancel_work(); return state().dump(); }
            if (action == "dismiss-review") { ui->invoke_dismiss_review(); return state().dump(); }
            if (ui->get_review_open()) throw std::runtime_error("Dismiss the visible review before changing the selection or view");
            if (action == "close-inspector") { clear_detail(ui); return state().dump(); }
            if (action == "overview-group") {
                if (busy_ || !ui->get_scan_overview()) throw std::runtime_error("Open a completed scan overview first");
                ui->invoke_overview_group(slint::SharedString(input.at("id").get<std::string>())); return state().dump();
            }
            if (busy_ && action != "browse" && action != "tree-open" && action != "tree-expand" && action != "navigate" && action != "page" && action != "filter" && action != "sort" && action != "inspect" && action != "open" && action != "up" && action != "root") throw std::runtime_error("An operation is running; poll state or cancel it first");
            if (action == "browse") {
                open_browser(ui, input.at("path").get<std::string>());
            } else if (action == "tree-open") {
                const auto id = input.at("id").get<std::string>();
                if (id != "this-pc" && !tree_.count(browser_key(id))) throw std::runtime_error("Enumerate the tree before opening a folder");
                open_browser(ui, id);
            } else if (action == "tree-expand") {
                expand_tree(ui, input.at("id").get<std::string>(), input.at("open").get<bool>());
            } else if (action == "refresh-drives") {
                refresh_drives(ui);
            } else if (action == "navigate") {
                static const std::vector<std::string> sections = {"cleanup", "files", "duplicates", "activity"};
                const auto section = input.at("section").get<std::string>();
                const auto found = std::find(sections.begin(), sections.end(), section);
                if (found == sections.end()) throw std::runtime_error("Unknown navigation section");
                ui->invoke_navigate(static_cast<int>(found - sections.begin()));
            } else if (action == "navigation-menu") {
                ui->set_navigation_expanded(input.at("open").get<bool>());
            } else if (action == "scan" || action == "load-cache") {
                if (input.contains("path")) ui->set_root_path(slint::SharedString(input.at("path").get<std::string>()));
                scan(ui, action == "load-cache");
            } else if (action == "load-report") {
                load_report(ui, input.at("path").get<std::string>());
            } else if (action == "open-group" || action == "open-project") {
                open_group(ui, input.at("id").get<std::string>());
            } else if (action == "expand-group") {
                const auto id = input.at("id").get<std::string>();
                if (std::none_of(groups_.begin(), groups_.end(), [&](const auto& group) { return group.id == id; })) throw std::runtime_error("Unknown group");
                if (input.value("open", true)) expanded_groups_.insert(id); else expanded_groups_.erase(id);
                update_groups(ui);
            } else if (action == "select-visible") {
                if (input.contains("view_revision") && input.at("view_revision").get<uint64_t>() != query_generation_->load()) throw std::runtime_error("Stale view_revision; enumerate current rows");
                if (!rows_ || (ui->get_page() != 2 && ui->get_page() != 5)) throw std::runtime_error("Open a cleanup view first");
                ui->invoke_select_visible();
            } else if (action == "scan-detail") {
                if (!ui->get_detail_can_scan()) throw std::runtime_error("Inspect an unchecked folder that supports scanning first");
                scan_detail(ui);
            } else if (action == "page") {
                const auto name = input.at("name").get<std::string>();
                const auto found = std::find(pages.begin(), pages.end(), name);
                if (found == pages.end()) throw std::runtime_error("Unknown page");
                ui->invoke_change_page(static_cast<int>(found - pages.begin()));
            } else if (action == "filter") {
                ui->set_search(slint::SharedString(input.at("text").get<std::string>()));
                if (input.contains("folders_only")) ui->set_folders_only(input.at("folders_only").get<bool>());
                refresh_rows(ui);
            } else if (action == "sort") {
                const auto order = input.at("order").get<std::string>();
                if (order != "largest" && order != "name" && order != "oldest") throw std::runtime_error("Sort order must be largest, name, or oldest");
                ui->set_sort_order(order == "largest" ? 0 : order == "name" ? 1 : 2); refresh_rows(ui);
            } else if (action == "clear-selection") clear_selection(ui);
            else if (action == "find-duplicates") {
                if (!snapshot_ || summary_only_ || ui->get_page() == 5) throw std::runtime_error("Open Duplicates with a current folder scan to compare contents; a saved report is not duplicate evidence");
                find_duplicates(ui);
            } else if (action == "up") ui->invoke_go_up();
            else if (action == "root") ui->invoke_go_root();
            else if (action == "review") {
                if (selected_->empty()) throw std::runtime_error("Select candidates using their checkboxes first");
                ui->invoke_show_review();
            } else if (action == "inspect" || action == "open" || action == "select" || action == "check-select") {
                if (input.contains("view_revision") && input.at("view_revision").get<uint64_t>() != query_generation_->load()) throw std::runtime_error("Stale view_revision; enumerate current rows");
                const auto id = input.at("id").get<std::string>();
                const auto found = agent_row_ids_.find(id);
                if (!rows_ || found == agent_row_ids_.end()) throw std::runtime_error("Enumerate rows in the current view before using this ID");
                const int row = static_cast<int>(found->second);
                if (action == "check-select") {
                    if (ui->get_page() != 2 && ui->get_page() != 5) throw std::runtime_error("Open a cleanup view first");
                    check_select(ui, row);
                } else if (action == "select") {
                    if (!rows_->source.candidates || (!rows_->row_data(row)->selectable && !rows_->row_data(row)->checkable)) throw std::runtime_error("This row does not support deletion");
                    const bool selected = input.at("selected").get<bool>();
                    if (selected != (selected_->count(id) != 0)) toggle(ui, row);
                } else { ui->set_focused_row(row); inspect(ui, row, action == "open"); }
            } else throw std::runtime_error("Unknown action; use capabilities");
            return state().dump();
        } catch (const std::exception& ex) { return Json({{"ok", false}, {"error", ex.what()}}).dump(); }
        catch (...) { return "{\"ok\":false,\"error\":\"Command failed\"}"; }
    }

    void load_report(const Ui& ui, const std::string& path) {
        if (busy_) return;
        start(ui, "Loading saved scan summary…", false);
        auto weak = weak_from_this();
        enqueue([weak, path] {
            std::ifstream file(std::filesystem::path(wide(path)), std::ios::binary | std::ios::ate);
            if (!file || file.tellg() < 0 || file.tellg() > 16 * 1024 * 1024) throw std::runtime_error("Report is unavailable or exceeds 16 MiB");
            file.seekg(0);
            const auto report = Json::parse(file);
            if (report.value("version", 0) != 1) throw std::runtime_error("Unsupported report version");
            const auto entries = report.at("entries"), candidates = report.at("candidates");
            if (!entries.is_array() || !candidates.is_array() || entries.empty() || entries.size() > 100000 || candidates.size() > 100000) throw std::runtime_error("Invalid report row count");
            auto number = [](const Json& object, const char* key) -> uint64_t {
                const auto& value = object.at(key);
                if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0)) throw std::runtime_error("Invalid nonnegative report number");
                return value.get<uint64_t>();
            };
            auto snapshot = std::make_shared<backend::Snapshot>();
            snapshot->root = report.at("root").get<std::string>();
            const auto normalized_root = std::filesystem::path(wide(snapshot->root)).lexically_normal();
            if (!normalized_root.is_absolute()) throw std::runtime_error("Report root must be an absolute Windows path");
            auto valid_path = [&](const std::string& value) {
                if (value.empty() || value.find('\0') != std::string::npos) throw std::runtime_error("Invalid report path");
                const auto path = std::filesystem::path(wide(value)).lexically_normal();
                auto root_text = folded(utf8(normalized_root.wstring())), path_text = folded(utf8(path.wstring()));
                if (root_text.back() != L'\\') root_text += L'\\';
                if (path != normalized_root && path_text.rfind(root_text, 0) != 0) throw std::runtime_error("Report row is outside its root");
            };
            snapshot->scanned_at = report.at("scanned_at").get<double>();
            snapshot->files = number(report, "files"); snapshot->errors = number(report, "errors");
            for (const auto& value : entries) {
                backend::Entry entry;
                entry.path = value.at("path").get<std::string>(); valid_path(entry.path);
                entry.name = value.value("name", basename(entry.path)); entry.parent = value.at("parent").get<int>();
                entry.directory = value.at("directory").get<bool>();
                entry.logical_bytes = number(value, "logical_bytes"); entry.allocated_bytes = number(value, "allocated_bytes");
                entry.modified = value.value("modified", 0.0);
                if ((snapshot->entries.empty() && entry.parent != -1) || (!snapshot->entries.empty() && (entry.parent < 0 || static_cast<size_t>(entry.parent) >= snapshot->entries.size()))) throw std::runtime_error("Invalid report parent ordering");
                if (entry.parent >= 0) {
                    const auto& parent = snapshot->entries[entry.parent];
                    auto parent_path = folded(utf8(std::filesystem::path(wide(parent.path)).lexically_normal().wstring()));
                    if (parent_path.back() != L'\\') parent_path += L'\\';
                    if (!parent.directory || folded(utf8(std::filesystem::path(wide(entry.path)).lexically_normal().wstring())).rfind(parent_path, 0) != 0)
                        throw std::runtime_error("Report parent is not an ancestor of its child");
                }
                snapshot->entries.push_back(std::move(entry));
            }
            if (folded(snapshot->entries.front().path) != folded(snapshot->root)) throw std::runtime_error("First report entry must be its root");
            auto items = std::make_shared<Candidates>();
            std::unordered_set<std::wstring> candidate_paths;
            for (const auto& value : candidates) {
                backend::Candidate item;
                item.path = value.at("path").get<std::string>(); valid_path(item.path);
                const auto issue = backend::deletion_path_issue(item.path);
                if (!issue.empty()) throw std::runtime_error("Unsafe report candidate " + item.path + ": " + issue);
                item.path = utf8(std::filesystem::path(wide(item.path)).lexically_normal().wstring());
                if (!candidate_paths.insert(folded(item.path)).second) throw std::runtime_error("Duplicate report candidate: " + item.path);
                item.reason = value.at("reason").get<std::string>(); item.bytes = number(value, "bytes"); item.modified = value.value("modified", 0.0);
                // Imported claims are display data, never backend validation evidence.
                items->push_back(std::move(item));
            }
            post(weak, [snapshot, items](Controller& self, const Ui& ui) {
                self.finish(ui);
                self.clear_selection(ui); self.clear_detail(ui);
                self.summary_only_ = !self.snapshot_; self.report_ = items; self.report_root_ = snapshot->root;
                ui->set_root_path(slint::SharedString(snapshot->root));
                ui->set_summary_only(true);
                const auto label = "Saved report from " + date(snapshot->scanned_at, true) + ". Sizes may have changed.";
                ui->set_snapshot_label(slint::SharedString(label)); ui->set_report_label(slint::SharedString(label));
                ui->set_status("Saved report loaded.");
                ui->set_page(5); ui->set_search(""); self.refresh_rows(ui);
            });
        });
    }
    Controller(const Ui& ui, int* exit_code) : ui_(ui), exit_code_(exit_code) {}
    ~Controller() {
        if (control_) control_->cancel = true;
        ++*query_generation_;
        browser_worker_.stop();
        query_worker_.stop();
        worker_.stop();
    }
    void bind(const Ui& ui) {
        const auto weak = weak_from_this();
        ui->invoke_apply_theme(saved_dark_mode());
        ui->on_change_theme([weak](bool dark) { if (auto self = weak.lock()) if (auto app = self->ui_.lock()) change_theme(*app, dark); });
        ui->on_tree_open([weak](slint::SharedString id) { if (auto self = weak.lock()) if (auto app = self->ui_.lock()) { try { self->open_browser(*app, string(id)); } catch (const std::exception& ex) { (*app)->set_status(ex.what()); } } });
        ui->on_tree_expand([weak](slint::SharedString id, bool expanded) { if (auto self = weak.lock()) if (auto app = self->ui_.lock()) { try { self->expand_tree(*app, string(id), expanded); } catch (const std::exception& ex) { (*app)->set_status(ex.what()); } } });
        ui->on_refresh_drives([weak] { if (auto self = weak.lock()) if (auto app = self->ui_.lock()) self->refresh_drives(*app); });
        refresh_drives(ui);
        ui->set_root_path(slint::SharedString(backend::default_root()));
        update_drive(ui);
        ui->on_open_group([weak](slint::SharedString id) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) { try { self->open_group(*ui, string(id)); } catch (const std::exception& error) { (*ui)->set_status(error.what()); } } });
        ui->on_expand_group([weak](slint::SharedString id, bool open) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) { if (open) self->expanded_groups_.insert(string(id)); else self->expanded_groups_.erase(string(id)); self->update_groups(*ui); } });
        ui->on_scan_detail([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->scan_detail(*ui); });
        ui->on_select_visible([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) {
            if (self->busy_ || !self->rows_) return;
            for (size_t i = 0; i < self->rows_->row_count(); ++i) {
                const auto row = self->rows_->row_data(i);
                if (row && !row->selected && (row->selectable || row->checkable)) self->toggle(*ui, static_cast<int>(i));
            }
        } });
        ui->on_check_select([weak](int row) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->check_select(*ui, row); });
        ui->on_choose_folder([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) {
            if (self->busy_) return;
            // Native modal dialogs run their own Windows message loop. Keeping
            // this on the UI thread avoids an uncancellable dialog on the worker.
            const auto path = backend::pick_folder();
            if (!path.empty()) { (*ui)->set_root_path(slint::SharedString(path)); self->scan(*ui, false); }
            else (*ui)->set_status(slint::SharedString("Folder selection cancelled."));
        }});
        ui->on_scan_folder([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->scan(*ui, false); });
        ui->on_load_cache([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->scan(*ui, true); });
        ui->on_cancel_work([weak] { if (auto self = weak.lock()) if (self->control_) { self->control_->cancel = true; if (auto ui = self->ui_.lock()) (*ui)->set_status(slint::SharedString("Cancelling… Finishing the current filesystem operation.")); } });
        ui->on_change_page([weak](int page) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) {
            ++self->navigation_generation_;
            (*ui)->set_page(page); (*ui)->set_search(slint::SharedString("")); (*ui)->set_detail_title(slint::SharedString(""));
            self->active_group_.clear(); self->active_project_.clear(); self->clear_detail(*ui);
            if (page < 2 && self->browse_path_ != "this-pc") (*ui)->set_root_path(slint::SharedString(self->browse_path_));
            else if (page == 5) (*ui)->set_root_path(slint::SharedString(self->report_root_));
            else if (page == 2 && self->snapshot_) (*ui)->set_root_path(slint::SharedString(self->snapshot_->root));
            self->clear_selection(*ui); self->refresh_rows(*ui);
        }});
        ui->on_query_changed([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->refresh_rows(*ui); });
        ui->on_inspect_row([weak](int row) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->inspect(*ui, row); });
        ui->on_activate_row([weak](int row) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->inspect(*ui, row, true); });
        ui->on_toggle_row([weak](int row) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->toggle(*ui, row); });
        ui->on_go_up([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) {
            if (self->source((*ui)->get_page()).live) {
                if (self->browse_path_ != "this-pc") {
                    const auto path = std::filesystem::path(wide(self->browse_path_));
                    const auto parent = path.parent_path();
                    self->open_browser(*ui, parent == path || parent.empty() ? "this-pc" : utf8(parent.wstring()));
                }
                return;
            }
            if (self->snapshot_ && self->folder_ > 0) self->folder_ = std::max(0, self->snapshot_->entries[self->folder_].parent);
            (*ui)->set_search(slint::SharedString("")); self->refresh_rows(*ui);
        }});
        ui->on_go_root([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) { if (self->source((*ui)->get_page()).live) { self->open_browser(*ui, "this-pc"); return; } self->folder_ = 0; (*ui)->set_search(slint::SharedString("")); self->refresh_rows(*ui); } });
        ui->on_find_copies([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->find_duplicates(*ui); });
        ui->on_overview_group([weak](slint::SharedString id) { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) {
            (*ui)->set_page(2); self->update_groups(*ui); self->open_group(*ui, string(id));
        } });
        ui->on_close_inspector([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->clear_detail(*ui); });
        ui->on_clear_selection([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->clear_selection(*ui); });
        ui->on_recycle_selected([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) self->recycle(*ui); });
        ui->on_reveal_detail([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) if (!self->detail_path_.empty() && !backend::reveal(self->detail_path_)) (*ui)->set_status(slint::SharedString("Windows could not open this location. It may have moved since the scan.")); });
        ui->on_recover([weak] { if (auto self = weak.lock()) if (auto ui = self->ui_.lock()) if (!backend::open_recycle_bin()) (*ui)->set_status(slint::SharedString("Windows could not open Recycle Bin. Open it from the desktop to restore files.")); });
        progress_.start(slint::TimerMode::Repeated, std::chrono::milliseconds(250), [weak] {
            if (auto self = weak.lock()) if (self->control_ && self->busy_ && !self->control_->cancel) if (auto ui = self->ui_.lock()) {
                const auto files = self->control_->files.load();
                (*ui)->set_status(slint::SharedString(self->activity_ + (files ? " " + std::to_string(files) + " files examined" : "")));
            }
        });
    }
    void smoke(const Ui& ui, const std::string& path, const std::string& screenshot, int page, bool cached) {
        smoke_ = true;
        smoke_page_ = std::clamp(page, 0, 4);
        screenshot_ = screenshot;
        if (path.empty()) { end_smoke(ui, 0); return; }
        ui->set_root_path(slint::SharedString(path));
        scan(ui, cached);
    }
};
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool smoke = false;
    bool cached = false;
    std::string path, screenshot, agent_pipe, report_path;
    int page = 0;
    int cancel_after = -1, close_after = -1;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--smoke-test") smoke = true;
        else if (arg == L"--root" && i + 1 < argc) path = utf8(argv[++i]);
        else if (arg == L"--screenshot" && i + 1 < argc) { screenshot = utf8(argv[++i]); smoke = true; }
        else if (arg == L"--page" && i + 1 < argc) page = _wtoi(argv[++i]);
        else if (arg == L"--load-cache") cached = true;
        else if (arg == L"--cancel-after-ms" && i + 1 < argc) cancel_after = _wtoi(argv[++i]);
        else if (arg == L"--close-after-ms" && i + 1 < argc) close_after = _wtoi(argv[++i]);
        else if (arg == L"--agent-pipe" && i + 1 < argc) agent_pipe = utf8(argv[++i]);
        else if (arg == L"--report" && i + 1 < argc) report_path = utf8(argv[++i]);
    }
    LocalFree(argv);
    if (smoke && AttachConsole(ATTACH_PARENT_PROCESS)) { FILE* stream; freopen_s(&stream, "CONOUT$", "w", stdout); freopen_s(&stream, "CONOUT$", "w", stderr); }
    try {
        auto ui = App::create();
        int exit_code = 0;
        auto controller = std::make_shared<Controller>(ui, &exit_code);
        controller->bind(ui);
        std::unique_ptr<AgentBridge> bridge;
        if (!agent_pipe.empty()) {
            bridge = std::make_unique<AgentBridge>(agent_pipe, [weak = std::weak_ptr<Controller>(controller)](const std::string& request) {
                if (auto self = weak.lock()) return self->agent_command(request);
                return std::string("{\"ok\":false,\"error\":\"Application is closing\"}");
            });
            ui->set_agent_enabled(true);
        }
        if (!path.empty()) ui->set_root_path(slint::SharedString(path));
        ui->show();
        if (!report_path.empty() && !smoke) controller->load_report(ui, report_path);
        if (smoke) controller->smoke(ui, path, screenshot, page, cached);
        if (cancel_after >= 0) slint::Timer::single_shot(std::chrono::milliseconds(cancel_after), [weak = slint::ComponentWeakHandle<App>(ui)] { if (auto app = weak.lock()) (*app)->invoke_cancel_work(); });
        if (close_after >= 0) slint::Timer::single_shot(std::chrono::milliseconds(close_after), [] { slint::quit_event_loop(); });
        if (smoke) slint::Timer::single_shot(std::chrono::minutes(2), [&exit_code] { exit_code = 3; slint::quit_event_loop(); });
        slint::run_event_loop();
        bridge.reset();
        controller.reset();
        return exit_code;
    } catch (const std::exception& ex) {
        if (!smoke) MessageBoxW(nullptr, wide(ex.what()).c_str(), L"Stale could not start", MB_OK | MB_ICONERROR);
        else std::fprintf(stderr, "Stale: %s\n", ex.what());
        return 1;
    }
}


