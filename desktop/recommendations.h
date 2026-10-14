#pragma once
#include "windows/core.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <limits>

namespace stale::desktop {
struct RecommendationProject {
    std::string id, label, path;
    uint64_t bytes = 0;
    std::vector<size_t> item_indices;
};
struct RecommendationGroup {
    std::string id, label, explanation;
    uint64_t bytes = 0;
    std::vector<size_t> item_indices;
    std::vector<RecommendationProject> projects;
};
namespace recommendation_detail {
inline std::string fold(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
    return value;
}
inline bool starts(const std::string& value, const std::string& prefix) { return value.compare(0, prefix.size(), prefix) == 0; }
inline std::string path(std::string value) {
    std::replace(value.begin(), value.end(), '/', '\\');
    if (starts(value, "\\\\?\\UNC\\")) value = "\\\\" + value.substr(8);
    else if (starts(value, "\\\\?\\")) value.erase(0, 4);
    std::string result; result.reserve(value.size());
    for (auto c : value) if (c != '\\' || result.empty() || result.back() != '\\' || result.size() == 1) result += c;
    while (result.size() > 1 && result.back() == '\\' && !(result.size() == 3 && result[1] == ':')) result.pop_back();
    return result;
}
inline std::string parent(const std::string& value) {
    if (value.size() == 3 && value[1] == ':' && value[2] == '\\') return value;
    const auto at = value.find_last_of('\\');
    if (at == std::string::npos) return {};
    if (at == 2 && value[1] == ':') return value.substr(0, 3);
    if (at == 0) return value.substr(0, 1);
    return value.substr(0, at);
}
inline std::string name(const std::string& value) {
    const auto at = value.find_last_of('\\');
    if (at == std::string::npos) return value;
    return at + 1 == value.size() ? value : value.substr(at + 1);
}
inline bool cargo_leaf(const std::string& value) { return value == "target" || starts(value, "target-"); }
inline int kind(const win::Candidate& c, const std::string& normalized) {
    if (c.duplicate_group >= 0) return 4;
    switch (c.category) {
    case win::Category::Build: return 0;
    case win::Category::Dependencies: return 1;
    case win::Category::Cache: case win::Category::Agent: return 2;
    case win::Category::Model: return 3;
    default: break;
    }
    // Compatibility with known report writers only. A directory name or a vague
    // sentence never upgrades personal data into a generated-output suggestion.
    const auto leaf = fold(name(normalized));
    if (cargo_leaf(leaf) && (starts(c.reason, "Cargo build tree identified by .rustc_info.json") || starts(c.reason, "Cargo build output:"))) return 0;
    if ((leaf == "build" || leaf == "out" || starts(leaf, "cmake-build-")) && starts(c.reason, "CMake-generated build tree identified by CMakeCache.txt.")) return 0;
    if (leaf == "node_modules" && starts(c.reason, "npm dependency tree identified by .package-lock.json.")) return 1;
    if (starts(c.reason, "Recognized generated cache.") || starts(c.reason, "Known agent/editor cache or diagnostic log tree with no modifications in 14 days.")) return 2;
    if (starts(c.reason, "Downloaded model.")) return 3;
    return 5;
}
inline bool project_leaf(int kind, const std::string& leaf) {
    if (kind == 0) return cargo_leaf(leaf) || leaf == "build" || leaf == "out" || starts(leaf, "cmake-build-");
    if (kind == 1) return leaf == "node_modules";
    if (kind == 2) return leaf == "cache" || leaf == "code cache" || leaf == "gpucache" || leaf == "logs" || leaf == "log" || leaf == "debug" || leaf == "paste-cache" || leaf == "__pycache__" || leaf == "_cacache";
    return false;
}
inline std::string project_path(int kind, const std::string& normalized) {
    auto current = normalized;
    while (!current.empty()) {
        auto above = parent(current);
        if (project_leaf(kind, fold(name(current)))) return above;
        if (above == current) break;
        current = std::move(above);
    }
    return parent(normalized);
}
inline std::string project_label(const std::string& root) {
    auto label = name(root);
    const auto lowered = fold(root);
    for (const auto& marker : {std::string("\\.worktrees\\"), std::string("\\worktrees\\")}) {
        const auto at = lowered.rfind(marker);
        if (at == std::string::npos) continue;
        const auto checkout = root.substr(at + marker.size());
        if (!checkout.empty() && checkout.find('\\') == std::string::npos) {
            auto owner = root.substr(0, at);
            if (name(owner) == ".claude" || name(owner) == ".codex") owner = parent(owner);
            const auto owner_name = name(owner);
            if (!owner_name.empty()) label = owner_name + " · " + checkout;
        }
        break;
    }
    return label.empty() ? "Unspecified location" : label;
}
inline uint64_t add(uint64_t a, uint64_t b) { return b > (std::numeric_limits<uint64_t>::max)() - a ? (std::numeric_limits<uint64_t>::max)() : a + b; }
struct Item { size_t index; int kind; std::string path, key; };
}

// Pure view transformation: indices always address the unchanged source vector.
// Ancestors (and repeated Windows path aliases) win globally, including across
// kinds, so selecting a displayed group does not select overlapping trees.
// Byte totals describe candidate metadata; they are never guaranteed savings.
inline std::vector<RecommendationGroup> group_recommendations(const std::vector<win::Candidate>& candidates) {
    namespace detail = recommendation_detail;
    constexpr std::array<const char*, 6> ids{"builds", "dependencies", "caches", "models", "duplicates", "review"};
    constexpr std::array<const char*, 6> labels{"Build outputs", "Dependencies", "Caches & logs", "Models", "Duplicates", "Needs review"};
    constexpr std::array<const char*, 6> explanations{
        "Generated project output. Review for manually saved files before rebuilding or removing it.",
        "Installed project dependencies. Review local changes; removing them requires reinstallation.",
        "Application and agent caches or logs. Review their contents; regeneration may require downloads.",
        "Downloaded model data: review what you still use. Removing it may require a large download later.",
        "Content-match candidates. Keep the verified original and review references before removing a copy.",
        "Personal or unclassified data. Review each item individually; no automatic cleanup recommendation."
    };
    std::vector<detail::Item> items; items.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto normalized = detail::path(candidates[i].path);
        items.push_back({i, detail::kind(candidates[i], normalized), normalized, detail::fold(normalized)});
    }
    std::sort(items.begin(), items.end(), [&](const auto& a, const auto& b) {
        if (a.key != b.key) return a.key < b.key; // Prefix ancestors precede descendants.
        const auto& x = candidates[a.index]; const auto& y = candidates[b.index];
        if ((x.category != win::Category::None) != (y.category != win::Category::None)) return x.category != win::Category::None;
        if (a.kind != b.kind) return a.kind < b.kind;
        if (x.bytes != y.bytes) return x.bytes > y.bytes;
        if (a.path != b.path) return a.path < b.path;
        if (x.reason != y.reason) return x.reason < y.reason;
        return a.index < b.index;
    });
    std::array<std::map<std::string, RecommendationProject>, 6> projects;
    std::set<std::string> included;
    for (const auto& item : items) {
        bool overlaps = false;
        for (auto ancestor = item.key; !ancestor.empty();) {
            if (included.find(ancestor) != included.end()) { overlaps = true; break; }
            auto above = detail::parent(ancestor); if (above == ancestor) break; ancestor = std::move(above);
        }
        if (overlaps) continue;
        if (!item.key.empty()) included.insert(item.key);
        auto root = detail::project_path(item.kind, item.path);
        auto& project = projects[item.kind][root];
        if (project.id.empty()) {
            project.id = std::string(ids[item.kind]) + ":" + root;
            project.path = root; project.label = detail::project_label(root);
        }
        project.bytes = detail::add(project.bytes, candidates[item.index].bytes);
        project.item_indices.push_back(item.index);
    }
    std::vector<RecommendationGroup> result;
    for (size_t k = 0; k < projects.size(); ++k) {
        if (projects[k].empty()) continue;
        RecommendationGroup group; group.id = ids[k]; group.label = labels[k]; group.explanation = explanations[k];
        for (auto& pair : projects[k]) {
            auto& project = pair.second;
            group.bytes = detail::add(group.bytes, project.bytes);
            group.item_indices.insert(group.item_indices.end(), project.item_indices.begin(), project.item_indices.end());
            group.projects.push_back(std::move(project));
        }
        result.push_back(std::move(group));
    }
    return result;
}
}
