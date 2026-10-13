#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include "windows/browser.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs = std::filesystem;
using namespace stale::win;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void put(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path()); std::ofstream out(file, std::ios::binary); out << contents;
    require(bool(out), "fixture write failed");
}
std::wstring extended(const fs::path& path) { return L"\\\\?\\" + path.wstring(); }
int main() {
    const auto fixture = fs::temp_directory_path() / (L"stale-browser-fixture-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    try {
        put(fixture / L"日本語" / L"nested" / L"hidden.txt", "not an immediate child");
        put(fixture / L"猫.txt", "seven!!");
        fs::create_directory(fixture / L"empty");
        auto listing = list_directory(fixture.u8string());
        require(listing.error.empty() && listing.snapshot.errors == 0, "ordinary directory listing must succeed");
        require(listing.snapshot.entries.size() == 4 && listing.snapshot.files == 1, "listing must include only immediate children");
        require(listing.size_known.size() == listing.snapshot.entries.size(), "size-known flags must align with entries");
        require(listing.snapshot.entries[0].parent == -1 && !listing.size_known[0], "root directory size must be unknown");
        bool saw_unicode = false;
        for (size_t i = 1; i < listing.snapshot.entries.size(); ++i) {
            const auto& e = listing.snapshot.entries[i]; require(e.parent == 0, "all listing children must point to root");
            if (e.directory) require(!listing.size_known[i] && e.logical_bytes == 0 && e.allocated_bytes == 0, "directory contents must not be scanned for size");
            else { require(e.name == u8"猫.txt" && e.logical_bytes == 7 && listing.size_known[i], "Unicode file size must come from real metadata"); saw_unicode = true; }
        }
        require(saw_unicode, "Unicode filename must remain visible");
        require(!list_directory((fixture / L"missing").u8string()).error.empty(), "missing directory must report an error");
        require(!list_directory((fixture / L"猫.txt").u8string()).error.empty(), "file path cannot be listed as a directory");
        const auto denied_path = (fixture / L"empty").wstring(); DWORD security_bytes = 0;
        GetFileSecurityW(denied_path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &security_bytes);
        std::vector<unsigned char> security(security_bytes);
        require(GetFileSecurityW(denied_path.c_str(), DACL_SECURITY_INFORMATION, security.data(), security_bytes, &security_bytes), "fixture security read failed");
        PSECURITY_DESCRIPTOR denied = nullptr;
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;OW)", SDDL_REVISION_1, &denied, nullptr), "fixture security conversion failed");
        const bool applied = SetFileSecurityW(denied_path.c_str(), DACL_SECURITY_INFORMATION, denied) != FALSE; LocalFree(denied);
        require(applied, "fixture access denial failed");
        auto denied_listing = list_directory((fixture / L"empty").u8string());
        require(SetFileSecurityW(denied_path.c_str(), DACL_SECURITY_INFORMATION, security.data()), "fixture security restoration failed");
        require(!denied_listing.error.empty() && denied_listing.snapshot.errors > 0, "access denial must produce an explicit listing error");
        auto deep = fixture / L"long";
        for (int i = 0; i < 8; ++i) deep /= std::wstring(40, wchar_t(L'a' + i));
        put(fs::path(extended(deep)) / L"長い.txt", "long");
        auto long_listing = list_directory(deep.u8string());
        require(long_listing.error.empty() && long_listing.snapshot.entries.size() == 2 && long_listing.snapshot.entries[1].logical_bytes == 4, "long Unicode paths must list correctly");
        auto link = fixture / L"directory-link";
        if (CreateSymbolicLinkW(link.c_str(), (fixture / L"日本語").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            auto parent = list_directory(fixture.u8string()); bool visible = false;
            for (size_t i = 0; i < parent.snapshot.entries.size(); ++i) if (parent.snapshot.entries[i].path == link.u8string()) {
                visible = true; require(parent.snapshot.entries[i].reparse && !parent.size_known[i], "reparse directory must be visible with unknown size");
            }
            require(visible, "directory link must be listed without traversing it");
            require(!list_directory(link.u8string()).error.empty(), "selected reparse directory must not be followed");
            require(!list_directory((link / L"nested").u8string()).error.empty(), "reparse ancestors must not be followed");
        } else std::cout << "SKIP symbolic link fixture: developer mode/privilege unavailable\n";
        auto cloud_file = fixture / L"offline.txt"; put(cloud_file, "metadata only");
        require(SetFileAttributesW(cloud_file.c_str(), FILE_ATTRIBUTE_OFFLINE), "offline fixture metadata failed");
        auto offline = list_directory(fixture.u8string());
        require(SetFileAttributesW(cloud_file.c_str(), FILE_ATTRIBUTE_NORMAL), "offline fixture metadata restoration failed");
        bool saw_offline = false;
        for (size_t i = 0; i < offline.snapshot.entries.size(); ++i) if (offline.snapshot.entries[i].path == cloud_file.u8string()) {
            saw_offline = true; require(offline.snapshot.entries[i].cloud && !offline.size_known[i], "offline file must not be read or imply known disk size");
        }
        require(saw_offline, "offline metadata entry must remain visible");
        wchar_t drive_name[] = L"Z:"; const DWORD existing = GetLogicalDrives();
        while (drive_name[0] > L'D' && (existing & (1u << (drive_name[0] - L'A')))) --drive_name[0];
        require(!(existing & (1u << (drive_name[0] - L'A'))), "unused fixture drive letter required");
        const auto mapped_target = L"\\??\\" + fixture.wstring();
        require(DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, drive_name, mapped_target.c_str()), "fixture drive mapping failed");
        auto drives = list_drives();
        const std::string root = std::string(1, char(drive_name[0])) + ":\\";
        auto root_listing = list_directory(root);
        require(DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM, drive_name, mapped_target.c_str()), "fixture drive mapping cleanup failed");
        auto mapped = std::find_if(drives.begin(), drives.end(), [&](const Drive& d) { return d.path == root; });
        require(mapped != drives.end() && mapped->ready && mapped->capacity_known && !mapped->kind.empty() && !mapped->label.empty() && mapped->total_bytes >= mapped->free_bytes, "drive inventory must include mapped ready drive and capacity");
        require(root_listing.error.empty(), "drive-root browsing must succeed");
        for (const auto& e : root_listing.snapshot.entries) require(e.path.find("\\\\") == std::string::npos, "drive-root children must use canonical separators");
        require(drives.size() >= 2, "inventory must retain existing drives beside the fixture drive");
        const auto absent_target = L"\\??\\" + (fixture / L"disconnected-drive-target").wstring();
        require(DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, drive_name, absent_target.c_str()), "unavailable drive mapping fixture failed");
        auto unavailable_drives = list_drives();
        require(DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE | DDD_NO_BROADCAST_SYSTEM, drive_name, absent_target.c_str()), "unavailable drive mapping cleanup failed");
        auto unavailable = std::find_if(unavailable_drives.begin(), unavailable_drives.end(), [&](const Drive& d) { return d.path == root; });
        require(unavailable != unavailable_drives.end() && !unavailable->ready && !unavailable->capacity_known, "unavailable drive roots must remain in inventory with unknown capacity");
        for (const auto& d : drives) if (d.kind == "network" || d.kind == "removable" || d.kind == "optical") require(!d.capacity_known, "inventory must not probe remote or removable media capacity");
        require(fs::equivalent(fixture.parent_path(), fs::temp_directory_path()) && fixture.filename().wstring().rfind(L"stale-browser-fixture-", 0) == 0, "fixture cleanup boundary failed");
        fs::remove_all(fs::path(extended(fixture)));
        std::cout << "Windows browser tests passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\nFixture: " << fixture.u8string() << '\n'; return 1; }
}
