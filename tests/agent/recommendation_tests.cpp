#include "desktop/recommendations.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <limits>
using stale::win::Candidate;
using stale::win::Category;
using namespace stale::desktop;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Candidate candidate(std::string path, uint64_t bytes, Category category = Category::None, std::string reason = {}) {
    Candidate c; c.path = std::move(path); c.bytes = bytes; c.category = category; c.reason = std::move(reason); return c;
}
const RecommendationGroup& group(const std::vector<RecommendationGroup>& groups, const std::string& id) {
    auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.id == id; });
    if (found == groups.end()) throw std::runtime_error("Missing recommendation group: " + id); return *found;
}
int main() {
    try {
        std::vector<Candidate> input{
            candidate("B:\\dev\\alpha\\target", 100, Category::Build),
            candidate("B:\\dev\\alpha\\target-fast", 200, Category::Build),
            candidate("B:\\dev\\alpha\\node_modules", 80, Category::Dependencies),
            candidate("C:\\Users\\person\\.codex\\log", 30, Category::Agent),
            candidate("B:\\models\\model.gguf", 900, Category::Model),
            candidate("B:\\photos\\copy.jpg", 60),
            candidate("B:\\photos\\family", 70),
            candidate("B:\\SDK\\target", 40),
            candidate("B:\\work\\legacy\\target", 50, Category::None, "Cargo build tree identified by .rustc_info.json and the canonical CACHEDIR.TAG signature."),
            candidate("B:\\other\\alpha\\target", 25, Category::Build)
        };
        input[5].duplicate_group = 1;
        auto groups = group_recommendations(input);
        require(groups.size() == 6, "all six independent kinds must be represented");
        require(groups[0].id == "builds" && groups[1].id == "dependencies" && groups[2].id == "caches" && groups[3].id == "models" && groups[4].id == "duplicates" && groups[5].id == "review", "group ordering must remain deterministic");
        const auto& builds = group(groups, "builds");
        require(builds.bytes == 375 && builds.projects.size() == 3, "build output must aggregate by distinct full project roots");
        require(builds.projects[0].path == "B:\\dev\\alpha" && builds.projects[0].bytes == 300 && builds.projects[0].item_indices.size() == 2, "target variants must share their containing project");
        require(builds.projects[0].id != builds.projects[1].id, "projects with the same short name must have distinct IDs");
        require(group(groups, "review").bytes == 110, "arbitrary target folders and personal photos must require review");
        require(group(groups, "models").explanation.find("review") != std::string::npos, "model descriptions must require review");
        require(group(groups, "models").explanation.find("safe") == std::string::npos, "model groups must not claim safe removal");
        require(group(groups, "duplicates").bytes == 60, "duplicate marker must determine duplicate kind");
        require(group_recommendations({}).empty(), "empty candidates must produce no groups");
        std::vector<Candidate> overlap{
            candidate("B:/project/target/debug/output.bin", 50),
            candidate("B:\\project\\target", 100, Category::Build),
            candidate("B:\\project\\target-fast", 200, Category::Build),
            candidate("B:\\project\\target\\", 100, Category::Build)
        };
        overlap[0].duplicate_group = 9;
        auto overlapped = group_recommendations(overlap);
        require(overlapped.size() == 1 && overlapped[0].bytes == 300 && overlapped[0].item_indices.size() == 2, "ancestor must suppress descendants and aliases without swallowing prefix siblings");
        require(overlap.size() == 4, "view grouping must preserve source candidates");
        auto reversed = input; std::reverse(reversed.begin(), reversed.end());
        auto reordered = group_recommendations(reversed);
        require(reordered.size() == groups.size(), "input order must not change kinds");
        for (size_t i = 0; i < groups.size(); ++i) {
            require(groups[i].id == reordered[i].id && groups[i].bytes == reordered[i].bytes && groups[i].projects.size() == reordered[i].projects.size(), "input order must not change group identities or totals");
            for (size_t j = 0; j < groups[i].projects.size(); ++j) require(groups[i].projects[j].id == reordered[i].projects[j].id, "project order and IDs must remain stable");
        }
        auto worktree = group_recommendations({candidate("B:\\repo\\.worktrees\\feature\\target", 1, Category::Build)});
        require(worktree[0].projects[0].path == "B:\\repo\\.worktrees\\feature", "worktree grouping must retain its distinct checkout root");
        require(worktree[0].projects[0].label.find("feature") != std::string::npos, "worktree project label must identify the checkout");
        auto uncertain = group_recommendations({candidate("B:\\SDK\\target", 1, Category::None, "This looks like build output"), candidate("B:\\photo.jpg", 1, Category::None, "Cargo might use this")});
        require(uncertain.size() == 1 && uncertain[0].id == "review", "uncontrolled reasons must never infer build safety");
        auto imported = group_recommendations({
            candidate("B:\\legacy\\target-stale", 11, Category::None, "Cargo build output: .rustc_info.json and CACHEDIR.TAG found."),
            candidate("B:\\models\\download.gguf", 12, Category::None, "Downloaded model. Review whether it is still used."),
            candidate("B:\\photos\\maybe-copy.jpg", 13, Category::None, "Potential photo copies, NOT fully verified by content."),
            candidate("B:\\scratch\\converted", 14, Category::None, "Review conversion scratch before removal."),
            candidate("B:\\$Recycle.Bin", 15, Category::None, "Review Recycle Bin contents before emptying.")
        });
        require(group(imported, "builds").bytes == 11 && group(imported, "models").bytes == 12 && group(imported, "review").bytes == 42, "legacy report prefixes must classify only supported evidence and leave uncertainty in review");
        require(imported.size() == 3, "unverified photo report must not invent a duplicates group");
        auto overflow = group_recommendations({candidate("B:\\a\\target", UINT64_MAX, Category::Build), candidate("B:\\b\\target", 10, Category::Build)});
        require(overflow[0].bytes == UINT64_MAX, "aggregation must saturate rather than wrap byte totals");
        std::cout << "Recommendation grouping tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
