#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#include "core.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <algorithm>
namespace fs = std::filesystem;
using namespace stale::win;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void put(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path()); std::ofstream stream(file, std::ios::binary); stream << contents;
    require(bool(stream), "fixture write failed");
}
std::wstring extended(const fs::path& p) { return L"\\\\?\\" + p.wstring(); }
void age_tree(const fs::path& root) {
    auto old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
    for (auto& entry : fs::recursive_directory_iterator(root)) fs::last_write_time(entry.path(), old);
    fs::last_write_time(root, old);
}
int main(int argc, char** argv) {
    auto fixture = fs::temp_directory_path() / (L"stale-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        // Model a personal-data root without writing outside the disposable temp
        // tree. Environment changes affect this test process only.
        require(SetEnvironmentVariableW(L"LOCALAPPDATA", (fixture / L"local-app-data").c_str()), "fixture LOCALAPPDATA override failed");
        put(fixture / L"日本語" / L"猫.txt", "abc");
        put(fixture / L"plain.txt", "12345");
        auto s = scan(fixture.u8string());
        require(s.error.empty(), "scan unexpectedly failed");
        require(s.files == 2, "scan must enumerate both Unicode and plain files");
        require(s.entries.at(0).logical_bytes == 8, "root logical total must equal fixture bytes");
        require(s.entries.at(0).allocated_bytes >= 8, "root allocation must use filesystem allocation");
        // Map only our disposable fixture as a drive; never scan a real drive here.
        wchar_t test_drive[] = L"Z:";
        DWORD drives = GetLogicalDrives();
        while (test_drive[0] > L'D' && (drives & (1u << (test_drive[0] - L'A')))) --test_drive[0];
        require(!(drives & (1u << (test_drive[0] - L'A'))), "no unused fixture drive letter");
        auto drive_target = L"\\??\\" + fixture.wstring();
        require(DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, test_drive, drive_target.c_str()), "fixture drive mapping failed");
        auto drive_scan = scan(std::string(1, char(test_drive[0])) + ":\\");
        require(DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, test_drive, drive_target.c_str()), "fixture drive mapping cleanup failed");
        require(drive_scan.errors == 0 && drive_scan.files == 2, "drive-root fixture must scan");
        for (const auto& e : drive_scan.entries) require(e.path.find("\\\\") == std::string::npos, "drive-root children must not contain doubled separators");
        auto longdir = fixture / L"long";
        for (int i = 0; i < 8; ++i) longdir /= std::wstring(40, L'a' + i);
        put(fs::path(extended(longdir)) / L"長い.txt", "long");
        auto longscan = scan(longdir.u8string());
        require(longscan.errors == 0 && longscan.files == 1 && longscan.entries[0].logical_bytes == 4, "paths longer than MAX_PATH must scan");
        auto links = fixture / L"links";
        put(links / L"first.bin", std::string(16384, 'x'));
        require(CreateHardLinkW((links / L"second.bin").c_str(), (links / L"first.bin").c_str(), nullptr), "hardlink fixture creation failed");
        auto linked = scan(links.u8string());
        require(linked.files == 2 && linked.entries[0].logical_bytes == 32768, "hardlinks count directory entries and logical sizes");
        require(linked.entries[0].allocated_bytes == 16384, "hardlink allocation must count physical storage once");
        require(find_duplicates(linked).empty(), "hardlinks must not be reported as reclaimable copies");
        put(links / L"build" / L"CMakeCache.txt", "generated");
        require(CreateHardLinkW((links / L"build" / L"linked-output.bin").c_str(), (links / L"first.bin").c_str(), nullptr), "linked build fixture creation failed");
        require(cleanup_candidates(scan(links.u8string())).empty(), "cleanup must exclude a directory with externally hardlinked data");
        auto sparse = fixture / L"sparse";
        put(sparse / L"normal.bin", std::string(1024 * 1024, '\0'));
        HANDLE sparse_file = CreateFileW((sparse / L"sparse.bin").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(sparse_file != INVALID_HANDLE_VALUE, "sparse fixture create failed");
        DWORD returned = 0; bool sparse_supported = DeviceIoControl(sparse_file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
        LARGE_INTEGER length{}; length.QuadPart = 1024 * 1024;
        require(SetFilePointerEx(sparse_file, length, nullptr, FILE_BEGIN) && SetEndOfFile(sparse_file), "sparse fixture resize failed");
        CloseHandle(sparse_file);
        auto sparse_scan = scan(sparse.u8string());
        auto sparse_dupes = find_duplicates(sparse_scan);
        require(sparse_dupes.size() == 1, "equal contents with differing allocation must still match");
        if (sparse_supported) require(sparse_scan.entries[0].allocated_bytes == 1024 * 1024, "sparse holes must not inflate allocated bytes");
        auto compressed_path = fixture / L"compressed.bin";
        put(compressed_path, std::string(1024 * 1024, 'c'));
        HANDLE compressed_file = CreateFileW(compressed_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(compressed_file != INVALID_HANDLE_VALUE, "compression fixture open failed");
        USHORT compression = COMPRESSION_FORMAT_DEFAULT;
        bool compression_supported = DeviceIoControl(compressed_file, FSCTL_SET_COMPRESSION, &compression, sizeof compression, nullptr, 0, &returned, nullptr) != FALSE;
        CloseHandle(compressed_file);
        if (compression_supported) {
            DWORD high = 0; DWORD low = GetCompressedFileSizeW(compressed_path.c_str(), &high);
            auto compressed_scan = scan(compressed_path.u8string());
            require(compressed_scan.entries.at(0).allocated_bytes == ((uint64_t(high) << 32) | low), "compressed allocation must reflect actual on-disk bytes");
        } else std::cout << "SKIP compression fixture: filesystem compression unavailable\n";
        auto target = fixture / L"target"; put(target / L"secret.txt", "must not traverse");
        auto symlink = fixture / L"directory-link";
        if (CreateSymbolicLinkW(symlink.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            auto linked_root = scan(symlink.u8string());
            require(linked_root.entries.empty() && linked_root.errors > 0, "selected reparse root must not be followed");
            auto whole = scan(fixture.u8string()); bool saw_link = false;
            for (const auto& e : whole.entries) {
                if (e.path == symlink.u8string()) { saw_link = e.reparse; require(e.logical_bytes == 0, "reparse directory must not roll up its target"); }
                require(e.path != (symlink / L"secret.txt").u8string(), "directory reparse point was traversed");
            }
            require(saw_link, "reparse directory entry must remain visible");
        } else { std::cout << "SKIP symbolic-link fixture: Windows privilege/developer mode unavailable (" << GetLastError() << ")\n"; }
        ScanControl stopped; stopped.cancel = true;
        require(scan(fixture.u8string(), &stopped).cancelled, "pre-cancelled scan must report cancellation");
        auto cancel_root = fixture / L"cancellation";
        for (int i = 0; i < 800; ++i) put(cancel_root / (std::to_wstring(i) + L".txt"), "cancel");
        ScanControl running; Snapshot interrupted; std::atomic<bool> finished{false};
        std::thread worker([&] { interrupted = scan(cancel_root.u8string(), &running); finished = true; });
        while (!finished && running.files == 0) std::this_thread::yield();
        running.cancel = true; worker.join();
        require(interrupted.cancelled && interrupted.files < 800, "active scan cancellation must stop enumeration and mark partial results");
        auto index = fixture / L"index.stale"; std::string error;
        require(save_snapshot(s, index.u8string(), &error), "complete index must save");
        Snapshot restored;
        require(load_snapshot(index.u8string(), restored, &error), "saved index must load");
        require(restored.files == 2 && restored.entries.at(0).logical_bytes == 8, "index roundtrip must preserve totals");
        require(save_snapshot(drive_scan, index.u8string(), &error) && load_snapshot(index.u8string(), restored, &error), "canonical drive-root snapshot must roundtrip after drive is disconnected");
        auto legacy_drive_scan = drive_scan;
        for (size_t i = 1; i < legacy_drive_scan.entries.size(); ++i) legacy_drive_scan.entries[i].path.insert(3, "\\");
        require(save_snapshot(legacy_drive_scan, index.u8string(), &error) && load_snapshot(index.u8string(), restored, &error), "legacy v3 drive-root indexes must migrate doubled separators when loaded");
        for (const auto& e : restored.entries) require(e.path.find("\\\\") == std::string::npos, "legacy drive-root load must produce canonical paths");
        put(index, "corrupt");
        require(!load_snapshot(index.u8string(), restored, &error), "corrupt index must be rejected");
        require(save_snapshot(s, index.u8string(), &error), "index rewrite must work");
        { std::fstream stream(index, std::ios::binary | std::ios::in | std::ios::out); stream.seekp(-1, std::ios::end); stream.put('z'); }
        require(!load_snapshot(index.u8string(), restored, &error), "tampered payload must fail checksum verification");
        Snapshot incomplete = s; incomplete.cancelled = true;
        require(!save_snapshot(incomplete, index.u8string(), &error), "incomplete scan must not replace complete index");
        require(scan((fixture / L"missing").u8string()).errors > 0, "unavailable root must report an error");
        put(fixture / L"project" / L"build" / L"CMakeCache.txt", "generated");
        put(fixture / L"project" / L"build" / L"result.obj", "0123456789");
        put(fixture / L"unmarked" / L"build" / L"keep.txt", "user files");
        put(fixture / L"Windows" / L"build" / L"CMakeCache.txt", "protected");
        auto clean = cleanup_candidates(scan(fixture.u8string()));
        require(clean.size() == 1, "only positively identified unprotected build trees may be suggested");
        require(clean[0].category == Category::Build, "native cleanup candidates must carry their verified category for grouping");
        put(fixture / L"locked-subtree" / L"private.txt", "unavailable enumeration");
        DWORD needed = 0; auto locked_path = (fixture / L"locked-subtree").wstring();
        GetFileSecurityW(locked_path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
        std::vector<unsigned char> original_security(needed);
        require(GetFileSecurityW(locked_path.c_str(), DACL_SECURITY_INFORMATION, original_security.data(), needed, &needed), "fixture security read failed");
        PSECURITY_DESCRIPTOR denied = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;OW)", SDDL_REVISION_1, &denied, nullptr), "fixture security conversion failed");
        bool denied_applied = SetFileSecurityW(locked_path.c_str(), DACL_SECURITY_INFORMATION, denied) != FALSE; LocalFree(denied);
        require(denied_applied, "fixture metadata denial failed");
        auto partial = scan(fixture.u8string());
        require(SetFileSecurityW(locked_path.c_str(), DACL_SECURITY_INFORMATION, original_security.data()), "fixture security restoration failed");
        require(partial.errors > 0, "inaccessible file metadata must report errors");
        require(cleanup_candidates(partial).size() == 1, "unrelated access errors must not suppress a fully scanned safe candidate");
        require(save_snapshot(partial, index.u8string(), &error) && load_snapshot(index.u8string(), restored, &error), "partial index must persist honestly");
        require(restored.errors == partial.errors && restored.error == partial.error, "partial index must preserve its error state");
        auto unsafe = scan((fixture / L"project").u8string());
        for (auto& e : unsafe.entries) if (!e.directory) e.cloud = true;
        require(cleanup_candidates(unsafe).empty(), "cloud descendants must exclude a whole cleanup tree");
        for (auto& e : unsafe.entries) { e.cloud = false; if (!e.directory) e.reparse = true; }
        require(cleanup_candidates(unsafe).empty(), "reparse descendants must exclude a whole cleanup tree");
        put(fixture / L"project" / L"build" / L"changed.txt", "new work");
        auto rejected = recycle(clean);
        require(rejected.recycled.empty() && rejected.failures.size() == 1, "changed candidate must be rejected before recycling");
        require(fs::exists(fixture / L"project" / L"build" / L"changed.txt"), "rejected candidate must stay on disk");
        put(fixture / L"dupes" / L"a.txt", "same contents");
        put(fixture / L"dupes" / L"b.txt", "same contents");
        put(fixture / L"dupes" / L"c.txt", "other content");
        auto duplicates = find_duplicates(scan((fixture / L"dupes").u8string()));
        require(duplicates.size() == 1, "duplicates must omit a retained copy and nonmatching same-size content");
        require(!duplicates[0].duplicate_keeper.empty(), "duplicates must remember a retained copy");
        auto managed = fixture / L"managed";
        for (auto relative : {L"node_modules/a.txt", L"node_modules/b.txt", L".codex/sessions/a.jsonl", L".codex/sessions/b.jsonl", L".cache/a.bin", L".cache/b.bin", L"build/a.obj", L"build/b.obj"}) put(managed / relative, "intentional identical copies");
        put(managed / L"build" / L"CMakeCache.txt", "generated");
        require(find_duplicates(scan(managed.u8string())).empty(), "generated dependency and agent state copies must not become duplicate recommendations");
        put(fixture / L"local-app-data" / L"app" / L"a.bin", "intentional app copies");
        put(fixture / L"local-app-data" / L"app" / L"b.bin", "intentional app copies");
        require(find_duplicates(scan((fixture / L"local-app-data").u8string())).empty(), "application-managed data must not become duplicate recommendations");
        put(fs::u8path(duplicates[0].duplicate_keeper), "changed copy!");
        rejected = recycle(duplicates);
        require(rejected.recycled.empty() && rejected.failures.size() == 1, "changed duplicate keeper must prevent recycling");
        Candidate fabricated; fabricated.path = (fixture / L"plain.txt").u8string();
        rejected = recycle({fabricated});
        require(rejected.recycled.empty() && rejected.failures.size() == 1 && fs::exists(fixture / L"plain.txt"), "unvalidated candidate must never be recycled");
        auto agents = fixture / L"agent-fixtures";
        for (auto relative : {L".codex/log/old.log", L".claude/debug/old.txt", L".claude/paste-cache/old.txt", L".codex/sessions/keep.jsonl", L".codex/worktrees/keep.txt", L".claude/models/keep.bin"}) put(agents / relative, "old agent fixture");
        require(cleanup_candidates(scan(agents.u8string())).empty(), "recent agent caches/logs must not be suggested");
        age_tree(agents);
        auto agent_items = cleanup_candidates(scan(agents.u8string()));
        require(agent_items.size() == 3, "known old agent log/cache paths should be suggested but sessions/worktrees/models retained");
        for (const auto& c : agent_items) require(c.category == Category::Agent, "native agent candidates must carry the agent category");
        for (const auto& c : agent_items) require(c.path.find("sessions") == std::string::npos && c.path.find("worktrees") == std::string::npos && c.path.find("models") == std::string::npos, "persistent agent data must stay excluded");
        auto roaming_fixture = fixture / L"roaming-fixture";
        put(roaming_fixture / L"Cursor" / L"Code Cache" / L"old.bin", "cache");
        put(roaming_fixture / L"Windsurf" / L"logs" / L"old.log", "logs");
        put(roaming_fixture / L"Cursor" / L"User" / L"workspaceStorage" / L"keep.json", "state");
        age_tree(roaming_fixture);
        auto unrelated_editor = cleanup_candidates(scan(roaming_fixture.u8string()));
        require(unrelated_editor.empty(), "editor paths outside known roaming root must not be suggested");
        wchar_t old_roaming[32768]{}; DWORD old_roaming_length = GetEnvironmentVariableW(L"APPDATA", old_roaming, 32768);
        require(SetEnvironmentVariableW(L"APPDATA", roaming_fixture.c_str()), "fixture APPDATA override failed");
        auto editor_items = cleanup_candidates(scan(roaming_fixture.u8string()));
        require(SetEnvironmentVariableW(L"APPDATA", old_roaming_length ? old_roaming : nullptr), "fixture APPDATA restoration failed");
        require(editor_items.size() == 2, "known old Cursor/Windsurf transient paths must be recognized");
        auto cargo = fixture / L"cargo-fixtures";
        const std::string cargo_tag = "Signature: 8a477f597d28d172789f06886806bc55\n# This file is a cache directory tag created by cargo.\n";
        for (auto folder : {L"crate/target", L"crate/target-fast"}) {
            put(cargo / folder / L".rustc_info.json", "{\"rustc_fingerprint\":1}");
            put(cargo / folder / L"CACHEDIR.TAG", cargo_tag);
            put(cargo / folder / L"debug" / L"app.exe", "generated fixture");
        }
        put(cargo / L"Sony SDK" / L"target" / L"keep.txt", "SDK data");
        put(cargo / L"fake" / L"target" / L".rustc_info.json", "{}");
        put(cargo / L"fake" / L"target" / L"CACHEDIR.TAG", "not a canonical cache tag");
        put(cargo / L"tag-only" / L"target" / L"CACHEDIR.TAG", cargo_tag);
        auto cargo_items = cleanup_candidates(scan(cargo.u8string()));
        require(cargo_items.size() == 2, "only Cargo target trees with both canonical markers may be suggested");
        require(find_duplicates(scan(cargo.u8string())).empty(), "verified Cargo outputs must not become intentional-copy duplicate recommendations");
        for (auto& c : cargo_items) require(c.reason.find("Cargo") != std::string::npos, "Cargo candidates must explain their evidence");
        put(cargo / L"crate" / L"target" / L"CACHEDIR.TAG", "changed marker");
        auto changed_cargo = std::find_if(cargo_items.begin(), cargo_items.end(), [&](const Candidate& c) { return c.path == (cargo / L"crate" / L"target").u8string(); });
        require(changed_cargo != cargo_items.end(), "marked Cargo target must be present");
        auto cargo_rejected = recycle({*changed_cargo});
        require(cargo_rejected.recycled.empty() && cargo_rejected.failures.size() == 1, "changed Cargo marker must prevent recycling");
        if (argc == 2 && std::string(argv[1]) == "--large-index") {
            // A synthetic index, not a filesystem rescan. Crosses both old limits
            // (5 million entries and 512 MiB) while keeping ordinary CTest fast.
            constexpr uint64_t count = 5000002;
            Snapshot large; large.root = (fixture / L"synthetic-large-root").u8string(); large.files = count - 1; large.scanned_at = s.scanned_at;
            Entry root; root.path = large.root; root.name = "synthetic-large-root"; root.directory = true; root.logical_bytes = root.allocated_bytes = count - 1;
            large.entries.reserve(count); large.entries.push_back(root);
            for (uint64_t i = 1; i < count; ++i) {
                Entry e; e.name = "synthetic-" + std::to_string(i) + ".bin"; e.path = large.root + "\\" + e.name; e.parent = 0; e.logical_bytes = e.allocated_bytes = 1;
                large.entries.push_back(std::move(e));
            }
            auto large_index = fixture / L"large-index.stale";
            std::cout << "Writing synthetic " << count << "-entry index...\n" << std::flush;
            require(save_snapshot(large, large_index.u8string(), &error), "large streaming snapshot save failed");
            auto saved_size = fs::file_size(large_index);
            require(saved_size > 512ull * 1024 * 1024, "large fixture must exceed former file-size limit");
            large = Snapshot{};
            std::cout << "Loading synthetic " << saved_size << "-byte index...\n" << std::flush;
            require(load_snapshot(large_index.u8string(), large, &error), "large streaming snapshot load failed");
            require(large.entries.size() == count && large.files == count - 1 && large.entries[0].logical_bytes == count - 1, "large snapshot must preserve every entry and total");
            std::cout << "Large index streaming roundtrip passed.\n" << std::flush;
        }
        if (argc == 2 && std::string(argv[1]) == "--exercise-recycle") {
            auto recycle_fixture = fixture / L"recycle-only-created-test-data";
            put(recycle_fixture / L"build" / L"CMakeCache.txt", "Stale disposable recycle integration test\n");
            auto recycle_items = cleanup_candidates(scan(recycle_fixture.u8string()));
            require(recycle_items.size() == 1, "recycle fixture must yield exactly one candidate");
            auto actual = recycle(recycle_items);
            for (const auto& failure : actual.failures) std::cerr << failure << '\n';
            require(actual.recycled.size() == 1 && actual.failures.empty(), "real disposable fixture must reach the Recycle Bin");
            require(!fs::exists(recycle_fixture / L"build"), "recycled test tree must leave its source path");
            std::cout << "Disposable recycle operation succeeded; small test tree remains recoverable in Windows Recycle Bin.\n";
        }
        // fixture is constructed under the OS temp directory, never supplied by the user.
        require(fs::equivalent(fixture.parent_path(), fs::temp_directory_path()) && fixture.filename().wstring().rfind(L"stale-fixture-", 0) == 0, "fixture cleanup boundary check failed");
        fs::remove_all(fs::path(extended(fixture)));
        std::cout << "All Windows core tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\nFixture: " << fixture.u8string() << '\n'; return 1; }
}
