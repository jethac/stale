#include "browser.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace stale::win {
namespace {
namespace fs = std::filesystem;
struct QuietErrors {
    DWORD previous = 0; bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
    ~QuietErrors() { if (changed) SetThreadErrorMode(previous, nullptr); }
};
struct FileHandle {
    HANDLE value;
    explicit FileHandle(HANDLE h) : value(h) {}
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct FindHandle {
    HANDLE value;
    explicit FindHandle(HANDLE h) : value(h) {}
    ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
};
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) throw std::runtime_error("The path is not valid UTF-8.");
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("A filename contains unsupported Unicode.");
    std::string result(count, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring normal(const std::string& path) {
    auto value = wide(path);
    if (value.empty() || value.find(L'\0') != std::wstring::npos) throw std::runtime_error("Enter a folder path.");
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.rfind(L"\\\\?\\UNC\\", 0) == 0) value = L"\\\\" + value.substr(8);
    else if (value.rfind(L"\\\\?\\", 0) == 0) {
        value.erase(0, 4);
        if (value.size() < 3 || value[1] != L':' || value[2] != L'\\') throw std::runtime_error("Unsupported device path.");
    }
    if (value.rfind(L"\\\\.\\", 0) == 0 || value.find(L':', 2) != std::wstring::npos) throw std::runtime_error("Device paths and alternate data streams cannot be browsed.");
    const DWORD needed = GetFullPathNameW(value.c_str(), 0, nullptr, nullptr);
    if (!needed) throw std::runtime_error("Windows could not resolve this path.");
    std::wstring result(needed, 0);
    const DWORD count = GetFullPathNameW(value.c_str(), needed, result.data(), nullptr);
    if (!count || count >= needed) throw std::runtime_error("Windows could not resolve this path.");
    result.resize(count);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}
std::wstring extended(const std::wstring& path) { return path.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path; }
std::wstring child(const std::wstring& parent, const wchar_t* name) { return parent + (parent.back() == L'\\' ? L"" : L"\\") + name; }
std::string windows_error(DWORD code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    auto message = text ? utf8(text) : "Windows error " + std::to_string(code);
    LocalFree(text);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) message.pop_back();
    return message;
}
double timestamp(FILETIME value) {
    ULARGE_INTEGER integer; integer.LowPart = value.dwLowDateTime; integer.HighPart = value.dwHighDateTime;
    return (static_cast<double>(integer.QuadPart) - 116444736000000000.0) / 10000000.0;
}
bool cloud(DWORD attrs) { return (attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_UNPINNED)) != 0; }
void error(DirectoryListing& listing, const std::string& path, const std::string& message) {
    ++listing.snapshot.errors;
    if (listing.error.empty()) listing.error = path.empty() ? message : path + ": " + message;
    listing.snapshot.error = listing.error;
}
// A UNC server alone is not a filesystem directory: stop at the share root.
std::wstring root_boundary(const std::wstring& path) {
    if (path.rfind(L"\\\\", 0) != 0) return path.substr(0, 3);
    const auto server_end = path.find(L'\\', 2);
    if (server_end == std::wstring::npos || server_end + 1 == path.size()) throw std::runtime_error("A network folder requires a share name.");
    const auto share_end = path.find(L'\\', server_end + 1);
    return path.substr(0, share_end);
}
bool safe_ancestors(const std::wstring& path, DirectoryListing& listing) {
    const auto boundary = root_boundary(path); auto current = fs::path(path);
    while (!current.empty()) {
        const auto value = current.wstring();
        const auto attrs = GetFileAttributesW(extended(value).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) { error(listing, utf8(value), windows_error(GetLastError())); return false; }
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || cloud(attrs)) {
            error(listing, utf8(value), "This folder or an ancestor is a link, reparse point, or cloud placeholder; its contents were not accessed."); return false;
        }
        if (value == boundary) return true;
        const auto parent = current.parent_path();
        if (parent == current || parent.wstring().size() < boundary.size()) break;
        current = parent;
    }
    return true;
}
Entry metadata(const std::wstring& path, const WIN32_FIND_DATAW& data, int parent, DirectoryListing& listing, bool& size_known) {
    Entry e; e.path = utf8(path); e.name = parent < 0 ? utf8(fs::path(path).filename().wstring()) : utf8(data.cFileName);
    if (e.name.empty()) e.name = e.path;
    e.parent = parent; e.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; e.cloud = cloud(data.dwFileAttributes); e.modified = timestamp(data.ftLastWriteTime);
    size_known = false;
    if (e.directory) return e;
    e.logical_bytes = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    // Enumeration metadata suffices for links/placeholders. Do not open them.
    if (e.reparse || e.cloud) return e;
    FileHandle file(CreateFileW(extended(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{}; FILE_STANDARD_INFO standard{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) || !GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof standard)) {
        e.metadata_ok = false; error(listing, e.path, windows_error(GetLastError())); return e;
    }
    // A file may have changed between enumeration and its attribute-only open.
    e.reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; e.cloud = cloud(info.dwFileAttributes);
    e.directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (e.directory) { e.logical_bytes = 0; return e; }
    if (e.reparse || e.cloud) return e;
    e.volume_id = info.dwVolumeSerialNumber; e.file_id = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow; e.link_count = info.nNumberOfLinks;
    e.modified = timestamp(info.ftLastWriteTime); e.logical_bytes = static_cast<uint64_t>(standard.EndOfFile.QuadPart); e.allocated_bytes = static_cast<uint64_t>(standard.AllocationSize.QuadPart);
    size_known = true; return e;
}
}

std::vector<Drive> list_drives() {
    QuietErrors quiet;
    std::vector<Drive> result;
    const DWORD needed = GetLogicalDriveStringsW(0, nullptr);
    if (!needed) return result;
    std::vector<wchar_t> roots(static_cast<size_t>(needed) + 1, 0);
    const DWORD count = GetLogicalDriveStringsW(static_cast<DWORD>(roots.size()), roots.data());
    if (!count || count >= roots.size()) return result;
    for (const wchar_t* root = roots.data(); *root; root += wcslen(root) + 1) {
        Drive drive; drive.path = utf8(root); const auto type = GetDriveTypeW(root);
        const char* fallback = "Drive";
        switch (type) {
        case DRIVE_FIXED: drive.kind = "fixed"; fallback = "Local disk"; break;
        case DRIVE_REMOVABLE: drive.kind = "removable"; fallback = "Removable drive"; break;
        case DRIVE_REMOTE: drive.kind = "network"; fallback = "Network drive"; break;
        case DRIVE_CDROM: drive.kind = "optical"; fallback = "Optical drive"; break;
        case DRIVE_RAMDISK: drive.kind = "ramdisk"; fallback = "RAM disk"; break;
        default: drive.kind = "unknown"; break;
        }
        drive.label = fallback;
        // Do not reconnect mapped shares, spin optical media, or probe unavailable
        // removable devices just to populate This PC. They remain navigable.
        if (type == DRIVE_FIXED || type == DRIVE_RAMDISK) {
            ULARGE_INTEGER available{}, total{}, unused{};
            if (GetDiskFreeSpaceExW(root, &available, &total, &unused)) {
                drive.ready = true; drive.capacity_known = true; drive.total_bytes = total.QuadPart; drive.free_bytes = available.QuadPart;
                wchar_t label[MAX_PATH + 1]{};
                if (GetVolumeInformationW(root, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0) && *label) drive.label = utf8(label);
            }
        }
        drive.label += " (" + drive.path.substr(0, drive.path.size() - 1) + ")";
        result.push_back(std::move(drive));
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return result;
}
DirectoryListing list_directory(const std::string& requested) {
    QuietErrors quiet; DirectoryListing result;
    FILETIME now; GetSystemTimeAsFileTime(&now); result.snapshot.scanned_at = timestamp(now);
    try {
        const auto path = normal(requested); result.snapshot.root = utf8(path);
        if (!safe_ancestors(path, result)) return result;
        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (!GetFileAttributesExW(extended(path).c_str(), GetFileExInfoStandard, &attrs)) { error(result, result.snapshot.root, windows_error(GetLastError())); return result; }
        if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || cloud(attrs.dwFileAttributes)) { error(result, result.snapshot.root, "This folder became a link, reparse point, or cloud placeholder; its contents were not accessed."); return result; }
        if (!(attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { error(result, result.snapshot.root, "This location is a file, not a folder."); return result; }
        WIN32_FIND_DATAW root{}; root.dwFileAttributes = attrs.dwFileAttributes; root.ftLastWriteTime = attrs.ftLastWriteTime;
        bool known = false;
        result.snapshot.entries.push_back(metadata(path, root, -1, result, known)); result.size_known.push_back(false);
        WIN32_FIND_DATAW found{};
        const auto pattern = child(extended(path), L"*");
        FindHandle enumeration(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (enumeration.value == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND) { result.snapshot.entries[0].metadata_ok = false; error(result, result.snapshot.root, windows_error(code)); }
            return result;
        }
        for (;;) {
            if (wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) {
                auto entry = metadata(child(path, found.cFileName), found, 0, result, known);
                if (!entry.directory) ++result.snapshot.files;
                result.snapshot.entries.push_back(std::move(entry)); result.size_known.push_back(known);
            }
            if (!FindNextFileW(enumeration.value, &found)) {
                const auto code = GetLastError();
                if (code != ERROR_NO_MORE_FILES) { result.snapshot.entries[0].metadata_ok = false; error(result, result.snapshot.root, windows_error(code)); }
                break;
            }
        }
    } catch (const std::exception& e) { error(result, result.snapshot.root, e.what()); }
    return result;
}
}
