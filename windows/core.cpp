#include "core.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace stale::win {
namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    operator bool() const { return h != INVALID_HANDLE_VALUE; }
};
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 path");
    std::wstring result(count, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), count);
    return result;
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode path");
    std::string result(count, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring normal(const std::string& p) {
    auto w = wide(p);
    if (w.empty() || w.find(L'\0') != std::wstring::npos) throw std::runtime_error("Empty or invalid path");
    std::replace(w.begin(), w.end(), L'/', L'\\');
    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) w = L"\\\\" + w.substr(8);
    else if (w.rfind(L"\\\\?\\", 0) == 0) w = w.substr(4);
    if (w.rfind(L"\\\\.\\", 0) == 0 || w.find(L':', 2) != std::wstring::npos)
        throw std::runtime_error("Device paths and alternate data streams are not supported");
    DWORD size = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Cannot resolve path");
    std::wstring out(size, 0);
    DWORD length = GetFullPathNameW(w.c_str(), size, out.data(), nullptr);
    if (!length || length >= size) throw std::runtime_error("Cannot resolve path");
    out.resize(length);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}
std::wstring extended(const std::wstring& w) {
    return w.rfind(L"\\\\", 0) == 0 ? L"\\\\?\\UNC\\" + w.substr(2) : L"\\\\?\\" + w;
}
std::wstring child_path(const std::wstring& parent, const wchar_t* name) {
    return parent + (parent.back() == L'\\' ? L"" : L"\\") + name;
}
std::string winerror(DWORD code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                  nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string result = buffer ? utf8(buffer) : "Windows error " + std::to_string(code);
    LocalFree(buffer);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n')) result.pop_back();
    return result;
}
double stamp(FILETIME t) { ULARGE_INTEGER n; n.LowPart = t.dwLowDateTime; n.HighPart = t.dwHighDateTime; return (static_cast<double>(n.QuadPart) - 116444736000000000.0) / 10000000.0; }
double now() { FILETIME t; GetSystemTimeAsFileTime(&t); return stamp(t); }
bool cloud(DWORD a) { return (a & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_UNPINNED)) != 0; }
bool cancelled(ScanControl* c) { return c && c->cancel.load(); }
std::wstring lower(std::wstring s) { std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); }); return s; }
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); }); return s; }
bool is_under(const std::wstring& path, const std::wstring& parent) {
    auto p = lower(path), r = lower(parent);
    return p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && (r.back() == L'\\' || p[r.size()] == L'\\'));
}
std::wstring environment(const wchar_t* name) {
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0); if (!size) return {};
    std::wstring value(size, 0); auto len = GetEnvironmentVariableW(name, value.data(), size); value.resize(len); return value;
}
bool protected_path(const std::string& path) {
    auto p = normal(path);
    for (auto name : {L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"}) {
        auto v = environment(name); if (!v.empty() && is_under(p, v)) return true;
    }
    auto folded = lower(p);
    for (auto component : {L"\\windows", L"\\program files", L"\\program files (x86)", L"\\$recycle.bin", L"\\system volume information", L"\\.git", L"\\onedrive", L"\\dropbox", L"\\icloud drive"}) {
        size_t at = 0;
        while ((at = folded.find(component, at)) != std::wstring::npos) {
            size_t end = at + wcslen(component);
            if (end == folded.size() || folded[end] == L'\\' || (std::wstring(component) == L"\\onedrive" && folded[end] == L' ')) return true;
            at = end;
        }
    }
    return p.size() <= 3;
}
bool agent_directory(const Entry& e) {
    if (!e.directory) return false;
    auto name = lower(e.name), parent = lower(fs::u8path(e.path).parent_path().filename().u8string());
    if ((parent == ".codex" && name == "log") || (parent == ".claude" && (name == "debug" || name == "paste-cache" || name == "cache"))) return true;
    auto roaming = environment(L"APPDATA");
    if (roaming.empty() || (parent != "cursor" && parent != "windsurf")) return false;
    auto expected = roaming + L"\\" + wide(parent) + L"\\" + wide(name);
    return lower(normal(e.path)) == lower(expected) && (name == "cache" || name == "code cache" || name == "gpucache" || name == "logs");
}
// Never follow a reparse ancestor, including a selected junction used as the root.
bool safe_ancestors(const std::wstring& p) {
    auto current = fs::path(p);
    while (!current.empty()) {
        DWORD a = GetFileAttributesW(extended(current.wstring()).c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT) || cloud(a)) return false;
        auto parent = current.parent_path(); if (parent == current) break; current = parent;
    }
    return true;
}
void add_error(Snapshot& s, const std::string& path, DWORD code) {
    ++s.errors;
    if (s.error.empty()) s.error = path + ": " + winerror(code);
}
Entry metadata(const std::wstring& p, const WIN32_FIND_DATAW& d, int parent, Snapshot& s) {
    Entry e;
    e.path = utf8(p); e.name = utf8(fs::path(p).filename().wstring());
    if (e.name.empty()) e.name = e.path;
    e.parent = parent; e.directory = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.reparse = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0; e.cloud = cloud(d.dwFileAttributes);
    e.modified = stamp(d.ftLastWriteTime);
    if (!e.directory) e.logical_bytes = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    // Attribute-only handles never read contents or recall cloud data.
    Handle h(CreateFileW(extended(p).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{}; FILE_STANDARD_INFO standard{};
    if (!h || !GetFileInformationByHandle(h.h, &info) || !GetFileInformationByHandleEx(h.h, FileStandardInfo, &standard, sizeof standard)) {
        e.metadata_ok = false; add_error(s, e.path, GetLastError()); return e;
    }
    e.volume_id = info.dwVolumeSerialNumber; e.file_id = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    // Directory enumeration may return a cached last-write time. Use the same
    // handle metadata for discovery and candidate-only revalidation.
    e.modified = stamp(info.ftLastWriteTime);
    e.link_count = info.nNumberOfLinks;
    if (!e.directory) {
        e.logical_bytes = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
        e.allocated_bytes = static_cast<uint64_t>(standard.AllocationSize.QuadPart);
    }
    return e;
}
uint64_t add_saturated(uint64_t a, uint64_t b) { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
bool canonical_cache_tag(const Entry& e);
}

Snapshot scan(const std::string& root, ScanControl* control) {
    Snapshot s; s.scanned_at = now(); if (control) control->files.store(0);
    try {
        auto base = normal(root); s.root = utf8(base);
        if (cancelled(control)) { s.cancelled = true; return s; }
        if (!safe_ancestors(base)) { s.errors = 1; s.error = "Root or ancestor is unavailable, a reparse point, or cloud-only; scanning was skipped."; return s; }
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(extended(base).c_str(), GetFileExInfoStandard, &a)) { add_error(s, s.root, GetLastError()); return s; }
        WIN32_FIND_DATAW d{}; d.dwFileAttributes = a.dwFileAttributes; d.ftLastWriteTime = a.ftLastWriteTime; d.nFileSizeHigh = a.nFileSizeHigh; d.nFileSizeLow = a.nFileSizeLow;
        s.entries.push_back(metadata(base, d, -1, s));
        std::vector<int> pending;
        if (s.entries[0].directory) pending.push_back(0); else { s.files = 1; if (control) control->files.store(1); }
        std::set<std::pair<uint64_t, uint64_t>> identities;
        while (!pending.empty() && !cancelled(control)) {
            int parent = pending.back(); pending.pop_back();
            auto dir = wide(s.entries[parent].path);
            DWORD attrs = GetFileAttributesW(extended(dir).c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || attrs & FILE_ATTRIBUTE_REPARSE_POINT || cloud(attrs)) { s.entries[parent].metadata_ok = false; add_error(s, s.entries[parent].path, ERROR_CANT_ACCESS_FILE); continue; }
            auto pattern = child_path(extended(dir), L"*");
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find == INVALID_HANDLE_VALUE) { if (GetLastError() != ERROR_FILE_NOT_FOUND) { s.entries[parent].metadata_ok = false; add_error(s, s.entries[parent].path, GetLastError()); } continue; }
            do {
                if (cancelled(control)) break;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                auto e = metadata(child_path(dir, fd.cFileName), fd, parent, s);
                int index = static_cast<int>(s.entries.size());
                if (e.directory && !e.reparse && !e.cloud && e.metadata_ok) pending.push_back(index);
                if (!e.directory) {
                    ++s.files; if (control) control->files.store(s.files);
                    if (e.metadata_ok && e.file_id && !identities.emplace(e.volume_id, e.file_id).second) e.allocated_bytes = 0;
                }
                s.entries.push_back(std::move(e));
            } while (FindNextFileW(find, &fd));
            DWORD last = GetLastError(); FindClose(find);
            if (!cancelled(control) && last != ERROR_NO_MORE_FILES) { s.entries[parent].metadata_ok = false; add_error(s, utf8(dir), last); }
        }
        s.cancelled = cancelled(control);
        std::map<int, unsigned> cargo_markers;
        for (size_t i = s.entries.size(); i-- > 1;) {
            auto& e = s.entries[i]; auto& p = s.entries[e.parent];
            p.logical_bytes = add_saturated(p.logical_bytes, e.logical_bytes); p.allocated_bytes = add_saturated(p.allocated_bytes, e.allocated_bytes);
            auto name = lower(e.name), parent_name = lower(p.name);
            if (!e.directory && name == "cmakecache.txt" && (parent_name == "build" || parent_name == "out" || parent_name.rfind("cmake-build-", 0) == 0)) p.category = Category::Build;
            if (!e.directory && name == ".package-lock.json" && parent_name == "node_modules") p.category = Category::Dependencies;
            if (!e.directory && parent_name == "__pycache__" && lower(fs::u8path(e.path).extension().u8string()) == ".pyc") p.category = Category::Cache;
            if (agent_directory(e)) e.category = Category::Agent;
            if (!e.directory && e.metadata_ok && !e.reparse && !e.cloud && (parent_name == "target" || parent_name.rfind("target-", 0) == 0)) {
                if (e.name == ".rustc_info.json") cargo_markers[e.parent] |= 1;
                if (e.name == "CACHEDIR.TAG" && canonical_cache_tag(e)) cargo_markers[e.parent] |= 2;
            }
        }
        for (const auto& marker : cargo_markers) if (marker.second == 3) s.entries[marker.first].category = Category::Build;
    } catch (const std::exception& e) { ++s.errors; s.error = e.what(); }
    return s;
}
}

namespace stale::win {
namespace {
struct Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error("SHA-256 initialization failed");
    }
    ~Sha256() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    void update(const void* data, size_t length) {
        if (length > ULONG_MAX || BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(length), 0) < 0)
            throw std::runtime_error("SHA-256 failed");
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw std::runtime_error("SHA-256 failed");
        std::ostringstream out; out << std::hex << std::setfill('0'); for (auto b : digest) out << std::setw(2) << int(b); return out.str();
    }
};
std::string digest(const std::string& data) { Sha256 hash; hash.update(data.data(), data.size()); return hash.finish(); }
template<class T> void append(std::string& out, const T& value) { out.append(reinterpret_cast<const char*>(&value), sizeof value); }
void append_string(std::string& out, const std::string& value) { append(out, uint32_t(value.size())); out += value; }
std::vector<const Entry*> subtree(const Snapshot& s, size_t root) {
    std::vector<const Entry*> result;
    const auto path = wide(s.entries.at(root).path);
    for (const auto& e : s.entries) if (is_under(wide(e.path), path)) result.push_back(&e);
    std::sort(result.begin(), result.end(), [](auto a, auto b) { return a->path < b->path; }); return result;
}
std::string fingerprint(const std::vector<const Entry*>& items) {
    Sha256 hash; std::string data; data.reserve(1024 * 1024);
    for (const auto* e : items) {
        append_string(data, e->path); append(data, e->file_id); append(data, e->volume_id); append(data, e->modified);
        append(data, e->directory); if (!e->directory) append(data, e->logical_bytes);
        append(data, e->link_count); append(data, e->reparse); append(data, e->cloud); append(data, e->metadata_ok);
        if (data.size() >= 1024 * 1024) { hash.update(data.data(), data.size()); data.clear(); }
    }
    if (!data.empty()) hash.update(data.data(), data.size());
    return hash.finish();
}
bool safe_tree(const std::vector<const Entry*>& items) {
    return std::all_of(items.begin(), items.end(), [](auto e) { return e->metadata_ok && !e->reparse && !e->cloud && (e->directory || e->link_count == 1) && !protected_path(e->path); });
}
std::string permanent_tree_issue(const std::vector<const Entry*>& items) {
    for (const auto* entry : items) {
        if (!entry->metadata_ok) return "Cannot read metadata: " + entry->path;
        if (entry->reparse) return "Contains a link or reparse point: " + entry->path;
        if (entry->cloud) return "Contains an unavailable cloud file: " + entry->path;
        if (protected_path(entry->path)) return "Contains a protected path: " + entry->path;
        // Explicit permanent deletion removes this directory entry only.
        // Other hard links remain valid; shared allocation may not be reclaimed.
    }
    return {};
}
bool safe_lineage(const Snapshot& s, const Entry& e) {
    for (auto current = &e; current;) {
        if (!current->metadata_ok || current->cloud || current->reparse) return false;
        if (current->parent < 0) break;
        if (size_t(current->parent) >= s.entries.size()) return false;
        current = &s.entries[current->parent];
    }
    return true;
}
bool duplicate_scope(const Snapshot& s, const Entry& e) {
    for (auto current = &e; current;) {
        if (current->category == Category::Build || current->category == Category::Dependencies || current->category == Category::Agent || current->category == Category::Cache) return false;
        if (current->parent < 0) break;
        current = &s.entries[current->parent];
    }
    auto path = normal(e.path);
    for (auto env : {L"APPDATA", L"LOCALAPPDATA"}) {
        auto root = environment(env);
        // Application data, including the OS temporary directory, is excluded
        // from personal duplicate recommendations.
        if (!root.empty() && is_under(path, root)) return false;
    }
    for (const auto& component : fs::path(path)) {
        auto name = lower(component.u8string());
        if (name == "node_modules" || name == "vendor" || name == ".venv" || name == "venv" || name == "__pycache__" ||
            name == ".cache" || name == ".npm" || name == ".cargo" || name == ".rustup" || name == ".nuget" || name == ".gradle" ||
            name == ".codex" || name == ".claude" || name == ".cursor" || name == ".windsurf" || name == ".vscode") return false;
    }
    return true;
}
bool canonical_cache_tag(const Entry& e) {
    if (e.directory || !e.metadata_ok || e.reparse || e.cloud || !safe_ancestors(normal(e.path))) return false;
    Handle h(CreateFileW(extended(normal(e.path)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION info{};
    if (!h || !GetFileInformationByHandle(h.h, &info) || info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT || cloud(info.dwFileAttributes) ||
        info.dwVolumeSerialNumber != e.volume_id || ((uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != e.file_id ||
        stamp(info.ftLastWriteTime) != e.modified) return false;
    constexpr char signature[] = "Signature: 8a477f597d28d172789f06886806bc55";
    char contents[sizeof(signature) - 1]{}; DWORD read = 0;
    return ReadFile(h.h, contents, sizeof(contents), &read, nullptr) && read == sizeof(contents) && std::memcmp(contents, signature, sizeof(contents)) == 0;
}
bool cargo_tree(const Entry& e, const std::vector<const Entry*>& items) {
    const auto name = lower(e.name);
    if (!e.directory || (name != "target" && name.rfind("target-", 0) != 0)) return false;
    const Entry* rustc = nullptr; const Entry* tag = nullptr;
    for (auto child : items) {
        if (child->directory || fs::u8path(child->path).parent_path().u8string() != e.path) continue;
        if (child->name == ".rustc_info.json") rustc = child;
        if (child->name == "CACHEDIR.TAG") tag = child;
    }
    return rustc && rustc->metadata_ok && !rustc->cloud && !rustc->reparse && tag && canonical_cache_tag(*tag);
}
Category classify(const Entry& e, const std::vector<const Entry*>& items) {
    if (!e.directory) return Category::None;
    auto name = lower(e.name);
    auto has = [&](const char* marker) { return std::any_of(items.begin(), items.end(), [&](auto child) { return child->parent >= 0 && lower(child->path) == lower(e.path + "\\" + marker); }); };
    if ((name == "build" || name == "out" || name.rfind("cmake-build-", 0) == 0) && has("CMakeCache.txt")) return Category::Build;
    if (cargo_tree(e, items)) return Category::Build;
    if (name == "node_modules" && has(".package-lock.json")) return Category::Dependencies;
    auto path = lower(e.path);
    if (name == "__pycache__" && std::all_of(items.begin(), items.end(), [&](auto item) { return item == &e || (!item->directory && lower(fs::u8path(item->path).extension().u8string()) == ".pyc"); })) return Category::Cache;
    if (name == "_cacache" && lower(fs::u8path(e.path).parent_path().filename().u8string()) == ".npm") return Category::Cache;
    if ((name == "pip" || name == "uv") && lower(fs::u8path(e.path).parent_path().filename().u8string()) == ".cache") return Category::Cache;
    // Known transient paths only. Agent sessions, worktrees, credentials and model
    // downloads are never recommendations. Recent activity suppresses the whole tree.
    if (agent_directory(e)) {
        const double cutoff = now() - 14 * 24 * 60 * 60;
        for (auto item : items) {
            auto child_name = lower(item->name);
            if (item->modified > cutoff || child_name == "sessions" || child_name == "worktrees" || child_name == "models" ||
                child_name == "transcripts" || child_name == "history" || child_name == "credentials") return Category::None;
        }
        return Category::Agent;
    }
    return Category::None;
}
std::string content_hash(const Entry& e, ScanControl* control) {
    if (e.directory || e.reparse || e.cloud || !e.metadata_ok || !safe_ancestors(normal(e.path))) return {};
    Handle h(CreateFileW(extended(normal(e.path)).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!h) return {};
    BY_HANDLE_FILE_INFORMATION before{}, after{};
    if (!GetFileInformationByHandle(h.h, &before) || before.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT || cloud(before.dwFileAttributes) ||
        before.dwVolumeSerialNumber != e.volume_id || ((uint64_t(before.nFileIndexHigh) << 32) | before.nFileIndexLow) != e.file_id ||
        ((uint64_t(before.nFileSizeHigh) << 32) | before.nFileSizeLow) != e.logical_bytes || stamp(before.ftLastWriteTime) != e.modified) return {};
    Sha256 hash; std::array<char, 256 * 1024> buffer{}; uint64_t total = 0;
    while (!cancelled(control)) {
        DWORD read = 0; if (!ReadFile(h.h, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return {};
        if (!read) break; hash.update(buffer.data(), read); total += read;
    }
    if (cancelled(control) || total != e.logical_bytes || !GetFileInformationByHandle(h.h, &after) ||
        CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime) || before.nFileSizeHigh != after.nFileSizeHigh || before.nFileSizeLow != after.nFileSizeLow) return {};
    return hash.finish();
}
}

namespace {
constexpr uint64_t max_index_bytes = 64ull * 1024 * 1024 * 1024;
constexpr uint64_t max_index_entries = 100000000;
constexpr size_t index_buffer_bytes = 1024 * 1024;
// The on-disk v3 layout stays compatible; only its I/O becomes streaming.
struct IndexWriter {
    std::ostream& stream; Sha256 hash; std::vector<char> buffer = std::vector<char>(index_buffer_bytes); size_t used = 0;
    uint64_t total = 0;
    explicit IndexWriter(std::ostream& output) : stream(output) {}
    void flush() {
        if (!used) return;
        hash.update(buffer.data(), used); stream.write(buffer.data(), static_cast<std::streamsize>(used));
        if (!stream) throw std::runtime_error("Cannot write index");
        used = 0;
    }
    void bytes(const void* source, size_t length) {
        if (length > max_index_bytes - total) throw std::runtime_error("Index exceeds 64 GiB format limit");
        total += length; auto data = static_cast<const char*>(source);
        while (length) {
            const size_t amount = std::min(length, buffer.size() - used);
            std::memcpy(buffer.data() + used, data, amount); used += amount; data += amount; length -= amount;
            if (used == buffer.size()) flush();
        }
    }
    template<class T> void value(const T& item) { bytes(&item, sizeof item); }
    void string(const std::string& item, size_t maximum = 131072) {
        if (item.size() > maximum) throw std::runtime_error("Index string exceeds supported path length");
        value(uint32_t(item.size())); bytes(item.data(), item.size());
    }
    std::string finish() { flush(); return hash.finish(); }
};
struct IndexReader {
    std::istream& stream; Sha256 hash; std::vector<char> buffer = std::vector<char>(index_buffer_bytes);
    uint64_t length, offset = 0; size_t position = 0, available = 0;
    IndexReader(std::istream& input, uint64_t payload_size) : stream(input), length(payload_size) {}
    void bytes(void* destination, size_t size) {
        if (size > length - offset) throw std::runtime_error("Truncated index");
        auto out = static_cast<char*>(destination);
        while (size) {
            if (position == available) {
                available = static_cast<size_t>(std::min<uint64_t>(buffer.size(), length - offset)); position = 0;
                stream.read(buffer.data(), static_cast<std::streamsize>(available));
                if (!stream) throw std::runtime_error("Cannot read index");
                hash.update(buffer.data(), available);
            }
            auto amount = std::min(size, available - position);
            std::memcpy(out, buffer.data() + position, amount); position += amount; offset += amount; out += amount; size -= amount;
        }
    }
    template<class T> T read() { T value; bytes(&value, sizeof value); return value; }
    std::string string(size_t maximum = 131072) {
        auto count = read<uint32_t>();
        if (count > maximum || count > length - offset) throw std::runtime_error("Invalid index string");
        std::string value(count, 0); bytes(value.data(), count);
        if (value.find('\0') != std::string::npos) throw std::runtime_error("Invalid index string");
        wide(value); return value;
    }
};
}
bool save_snapshot(const Snapshot& s, const std::string& file, std::string* error) {
    std::wstring temporary;
    try {
        if (s.cancelled || s.entries.empty()) throw std::runtime_error("Cancelled or empty indexes cannot be saved");
        if (s.entries.size() > max_index_entries) throw std::runtime_error("Index exceeds 100 million entry format limit");
        auto path = normal(file); auto parent = fs::path(path).parent_path();
        fs::create_directories(fs::path(extended(parent.wstring())));
        GUID guid{}; wchar_t unique[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, unique, 40)) throw std::runtime_error("Cannot create temporary index name");
        temporary = extended(path) + L".tmp-" + unique;
        {
            std::ofstream stream(fs::path(temporary), std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot create index");
            stream.write("STALEWIN3\n", 10); stream << std::string(64, '0');
            IndexWriter out(stream);
            out.string(s.root); out.value(s.files); out.value(s.scanned_at); out.value(s.errors); out.string(s.error, 1024 * 1024); out.value(uint64_t(s.entries.size()));
            for (const auto& e : s.entries) {
                out.string(e.path); out.string(e.name, 1024); out.value(int32_t(e.parent));
                out.value(uint8_t((e.directory ? 1 : 0) | (e.reparse ? 2 : 0) | (e.cloud ? 4 : 0) | (e.metadata_ok ? 8 : 0)));
                out.value(e.logical_bytes); out.value(e.allocated_bytes); out.value(e.modified); out.value(e.volume_id); out.value(e.file_id);
                out.value(uint32_t(e.category)); out.value(e.link_count);
            }
            const auto checksum = out.finish(); stream.seekp(10); stream.write(checksum.data(), checksum.size()); stream.flush();
            if (!stream) throw std::runtime_error("Cannot finish index");
        }
        if (!MoveFileExW(temporary.c_str(), extended(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error(winerror(GetLastError()));
        temporary.clear(); if (error) error->clear(); return true;
    } catch (const std::exception& e) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        if (error) *error = e.what(); return false;
    }
}
bool load_snapshot(const std::string& file, Snapshot& output, std::string* error) {
    try {
        std::ifstream stream(fs::path(extended(normal(file))), std::ios::binary | std::ios::ate);
        const auto size = stream.tellg();
        if (!stream || size < 74 || uint64_t(size) > max_index_bytes + 74) throw std::runtime_error("Missing, invalid, or oversized index (64 GiB limit)");
        stream.seekg(0); char header[74]{}; stream.read(header, sizeof header);
        if (!stream || std::memcmp(header, "STALEWIN3\n", 10) != 0) throw std::runtime_error("Unsupported index format");
        const std::string checksum(header + 10, 64);
        IndexReader in(stream, uint64_t(size) - 74);
        Snapshot s; s.root = in.string(); s.files = in.read<uint64_t>(); s.scanned_at = in.read<double>(); s.errors = in.read<uint64_t>(); s.error = in.string(1024 * 1024);
        const auto count = in.read<uint64_t>();
        if (!count || count > max_index_entries || count > (in.length - in.offset) / 65 || !std::isfinite(s.scanned_at) || s.root != utf8(normal(s.root)))
            throw std::runtime_error("Invalid index count, root or timestamp");
        s.entries.reserve(static_cast<size_t>(count)); uint64_t files = 0;
        for (uint64_t i = 0; i < count; ++i) {
            Entry e; e.path = in.string(); e.name = in.string(1024); e.parent = in.read<int32_t>(); const auto flags = in.read<uint8_t>();
            e.directory = flags & 1; e.reparse = flags & 2; e.cloud = flags & 4; e.metadata_ok = flags & 8;
            e.logical_bytes = in.read<uint64_t>(); e.allocated_bytes = in.read<uint64_t>(); e.modified = in.read<double>();
            e.volume_id = in.read<uint64_t>(); e.file_id = in.read<uint64_t>(); const auto category = in.read<uint32_t>(); e.link_count = in.read<uint32_t>();
            if (flags > 15 || category > uint32_t(Category::Model) || !std::isfinite(e.modified)) throw std::runtime_error("Invalid index entry");
            e.category = Category(category);
            if (i == 0) {
                if (e.parent != -1 || e.path != s.root) throw std::runtime_error("Invalid index root");
            } else {
                if (e.parent < 0 || uint64_t(e.parent) >= i || e.name.empty() || e.name == "." || e.name == ".." || e.name.find_first_of("\\/:") != std::string::npos)
                    throw std::runtime_error("Invalid index hierarchy");
                const auto& p = s.entries[e.parent];
                if (!p.directory || p.reparse || p.cloud) throw std::runtime_error("Invalid index parent");
                // Older v3 drive-root scans accidentally emitted C:\\child.
                // Canonicalize that one historical separator bug while retaining
                // exact (case-preserving) component validation for every record.
                if (e.path.size() > 3 && e.path[1] == ':' && e.path[2] == '\\' && e.path[3] == '\\') e.path.erase(3, 1);
                const auto expected = p.path + (p.path.back() == '\\' ? "" : "\\") + e.name;
                if (e.path != expected) throw std::runtime_error("Invalid index child path");
            }
            if (!e.directory) ++files; s.entries.push_back(std::move(e));
        }
        if (in.offset != in.length || files != s.files) throw std::runtime_error("Invalid index totals or trailing data");
        if (checksum != in.hash.finish()) throw std::runtime_error("Index checksum mismatch");
        // An index vector avoids copying every path into a hash set. Checksum and
        // structural checks finish before the caller's visible snapshot changes.
        std::vector<uint32_t> order(s.entries.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint32_t>(i);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return s.entries[a].path < s.entries[b].path; });
        for (size_t i = 1; i < order.size(); ++i) if (s.entries[order[i - 1]].path == s.entries[order[i]].path) throw std::runtime_error("Duplicate index path");
        std::vector<std::pair<uint64_t, uint64_t>> totals(s.entries.size());
        for (size_t i = s.entries.size(); i-- > 0;) {
            const auto& e = s.entries[i]; if (!e.directory) totals[i] = {e.logical_bytes, e.allocated_bytes};
            else if (totals[i] != std::make_pair(e.logical_bytes, e.allocated_bytes)) throw std::runtime_error("Invalid directory totals");
            if (e.parent >= 0) { auto& p = totals[e.parent]; p.first = add_saturated(p.first, totals[i].first); p.second = add_saturated(p.second, totals[i].second); }
        }
        output = std::move(s); if (error) error->clear(); return true;
    } catch (const std::exception& e) { if (error) *error = e.what(); return false; }
}
std::vector<Candidate> cleanup_candidates(const Snapshot& s, bool include_scan_root) {
    std::vector<Candidate> result; if (s.cancelled) return result;
    // Adjacency arrays keep collection proportional to the selected trees, rather than
    // rescanning a million-entry snapshot for every small cache directory.
    std::vector<int> first_child(s.entries.size(), -1), next_sibling(s.entries.size(), -1);
    std::vector<bool> covered(s.entries.size(), false);
    for (size_t i = 1; i < s.entries.size(); ++i) {
        int parent = s.entries[i].parent;
        if (parent < 0 || size_t(parent) >= i) return {};
        next_sibling[i] = first_child[parent]; first_child[parent] = static_cast<int>(i);
    }
    for (size_t i = include_scan_root ? 0 : 1; i < s.entries.size(); ++i) {
        const auto& e = s.entries[i];
        covered[i] = e.parent >= 0 ? covered[e.parent] : false;
        if (covered[i] || !e.directory) continue;
        auto name = lower(e.name);
        if (name != "build" && name != "out" && name.rfind("cmake-build-", 0) != 0 && name != "target" && name.rfind("target-", 0) != 0 && name != "node_modules" &&
            name != "__pycache__" && name != "_cacache" && name != "pip" && name != "uv" && name != "cache" &&
            name != "log" && name != "logs" && name != "debug" && name != "paste-cache" && name != "code cache" && name != "gpucache") continue;
        if (!safe_lineage(s, e) || protected_path(e.path)) continue;
        std::vector<const Entry*> items; std::vector<int> pending{static_cast<int>(i)};
        while (!pending.empty()) {
            int item = pending.back(); pending.pop_back(); items.push_back(&s.entries[item]);
            for (int child = first_child[item]; child >= 0; child = next_sibling[child]) pending.push_back(child);
        }
        std::sort(items.begin(), items.end(), [](auto a, auto b) { return a->path < b->path; });
        if (!safe_tree(items)) continue;
        auto category = classify(e, items); if (category == Category::None || items.size() <= 1) continue;
        Candidate c; c.path = e.path; c.modified = e.modified; c.suggested = true; c.validation_token = fingerprint(items); c.category = category;
        for (auto item : items) if (!item->directory && item->link_count == 1) c.bytes = add_saturated(c.bytes, item->allocated_bytes);
        if (category == Category::Build) c.reason = cargo_tree(e, items) ? "Cargo build tree identified by .rustc_info.json and the canonical CACHEDIR.TAG signature. Rebuildable; review for manually saved files." : "CMake-generated build tree identified by CMakeCache.txt. Rebuildable; review for manually saved files.";
        else if (category == Category::Dependencies) c.reason = "npm dependency tree identified by .package-lock.json. Reinstall required; review local modifications.";
        else if (category == Category::Agent) c.reason = "Known agent/editor cache or diagnostic log tree with no modifications in 14 days. Review cached pasted content before removal. Sessions, worktrees, models and credentials are excluded.";
        else c.reason = "Recognized generated cache. Its application may need to regenerate or download data.";
        c.reason += " Recycle Bin contents still occupy disk space until emptied."; result.push_back(std::move(c)); covered[i] = true;
    }
    return result;
}
std::vector<Candidate> find_duplicates(const Snapshot& s, ScanControl* control) {
    std::vector<Candidate> result; if (s.cancelled) return result;
    std::map<uint64_t, std::vector<const Entry*>> sizes;
    for (const auto& e : s.entries) if (!e.directory && safe_lineage(s, e) && duplicate_scope(s, e) && e.link_count == 1 && e.logical_bytes && !protected_path(e.path)) sizes[e.logical_bytes].push_back(&e);
    int group = 0; if (control) control->files.store(0);
    for (const auto& bucket : sizes) {
        if (cancelled(control)) break; if (bucket.second.size() < 2) continue;
        std::map<std::string, std::vector<const Entry*>> hashes;
        for (auto e : bucket.second) { if (cancelled(control)) break; auto hash = content_hash(*e, control); if (!hash.empty()) hashes[hash].push_back(e); if (control) ++control->files; }
        for (auto& pair : hashes) {
            auto& items = pair.second; if (items.size() < 2) continue;
            std::sort(items.begin(), items.end(), [](auto a, auto b) { return a->path < b->path; }); ++group;
            for (size_t i = 1; i < items.size(); ++i) {
                auto e = items[i]; Candidate c; c.path = e->path; c.modified = e->modified; c.bytes = e->allocated_bytes;
                c.duplicate_group = group; c.duplicate_keeper = items[0]->path; c.validation_token = fingerprint({e}) + ":" + pair.first;
                c.reason = "SHA-256 content match. Retain: " + c.duplicate_keeper + ". Review references before recycling; space remains used in Recycle Bin.";
                result.push_back(std::move(c));
            }
        }
    }
    if (cancelled(control)) result.clear(); return result;
}
const char* category_name(Category c) {
    switch (c) { case Category::Build: return "Build output"; case Category::Dependencies: return "Dependencies"; case Category::Cache: return "Cache";
    case Category::Agent: return "Agent cache"; case Category::Downloads: return "Downloads"; case Category::Model: return "Models"; default: return "Files"; }
}
std::string default_root() { auto p = environment(L"USERPROFILE"); return utf8(p.empty() ? L"C:\\" : p); }
std::string cache_path(const std::string& root) {
    auto base = environment(L"LOCALAPPDATA"); if (base.empty()) base = wide(default_root());
    return utf8(base + L"\\Stale\\Indexes\\" + wide(digest(utf8(normal(root)))) + L".stale");
}
}
namespace stale::win {
namespace {
struct ComApartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComApartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ready() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};
std::string validate_candidate(const Candidate& c, bool permanent = false) {
    if (c.validation_token.empty() || protected_path(c.path)) return "Unvalidated or protected candidate";
    auto current = scan(c.path);
    if (current.cancelled || current.errors || current.entries.empty()) return "Candidate is no longer fully accessible";
    auto items = subtree(current, 0);
    if (permanent) { const auto issue = permanent_tree_issue(items); if (!issue.empty()) return issue; }
    else if (!safe_tree(items)) return "Candidate contains protected, cloud, reparse, or hard-linked content";
    auto separator = c.validation_token.find(':');
    if (fingerprint(items) != c.validation_token.substr(0, separator)) return "Candidate changed since discovery; refresh first";
    if (c.duplicate_group >= 0) {
        if (separator == std::string::npos || c.duplicate_keeper.empty() || lower(c.duplicate_keeper) == lower(c.path)) return "Missing duplicate evidence";
        auto keeper = scan(c.duplicate_keeper);
        if (keeper.errors || keeper.entries.size() != 1 || keeper.entries[0].directory || current.entries.size() != 1 || current.entries[0].link_count != 1 || keeper.entries[0].link_count != 1)
            return "Retained duplicate is unavailable or linked";
        auto expected = c.validation_token.substr(separator + 1);
        if (content_hash(current.entries[0], nullptr) != expected || content_hash(keeper.entries[0], nullptr) != expected)
            return "Duplicate content changed; refresh first";
    } else if (!permanent && classify(current.entries[0], items) == Category::None) return "Generated-output evidence is missing";
    return {};
}
// Refuse the shell's permanent-delete fallback even if the bin is disabled/full.
class RecycleSink final : public IFileOperationProgressSink {
    LONG references = 1;
public:
    const Candidate& candidate; HRESULT outcome = E_PENDING; bool completed = false; bool permanent;
    explicit RecycleSink(const Candidate& c, bool permanently) : candidate(c), permanent(permanently) {}
    std::string failure;
    std::function<void(const std::string&)> progress;
    ULONGLONG last_progress = 0;
    uint64_t deleted_items = 0;
    IFACEMETHODIMP QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id == IID_IUnknown || id == IID_IFileOperationProgressSink) { *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK; } return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references); }
    IFACEMETHODIMP_(ULONG) Release() override { auto n = InterlockedDecrement(&references); if (!n) delete this; return n; }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD flags, IShellItem* item) override {
        if (!permanent && !(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) return E_ABORT;
        // Windows calls this during recursive deletion. Rescanning the selected
        // tree here is both quadratic and invalid once earlier files are gone.
        if (permanent) {
            PWSTR raw = nullptr;
            if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw))) { failure = "Cannot resolve the item being deleted"; return E_ABORT; }
            const std::wstring path(raw); CoTaskMemFree(raw);
            try {
                const auto p = normal(utf8(path));
                if (!is_under(p, normal(candidate.path)) || protected_path(utf8(p))) {
                    failure = "Path is outside the selection or protected: " + utf8(p); return E_ABORT;
                }
                const auto attributes = GetFileAttributesW(extended(p).c_str());
                // Shell callbacks can repeat for entries already removed by the
                // recursive operation. Missing entries are not a validation failure.
                if (attributes != INVALID_FILE_ATTRIBUTES && ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) || cloud(attributes))) {
                    failure = "Path became cloud-backed or linked: " + utf8(p); return E_ABORT;
                }
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    const auto error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                        failure = "Cannot inspect deletion path: " + utf8(p) + ": " + winerror(error); return E_ABORT;
                    }
                }
                // Recheck existing ancestors without rescanning contents. Missing
                // ancestors can result from the shell's recursive callback order.
                for (auto ancestor = fs::path(p).parent_path(); !ancestor.empty();) {
                    const auto attrs = GetFileAttributesW(extended(ancestor.wstring()).c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES && ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) || cloud(attrs))) {
                        failure = "Deletion ancestor became linked or cloud-backed: " + utf8(ancestor.wstring()); return E_ABORT;
                    }
                    if (attrs == INVALID_FILE_ATTRIBUTES) {
                        const auto error = GetLastError();
                        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) { failure = "Cannot inspect deletion ancestor: " + utf8(ancestor.wstring()); return E_ABORT; }
                    }
                    const auto next = ancestor.parent_path(); if (next == ancestor) break; ancestor = next;
                }
                return S_OK;
            } catch (const std::exception& ex) { failure = ex.what(); return E_ABORT; }
        }
        try { return validate_candidate(candidate, permanent).empty() ? S_OK : E_ABORT; } catch (...) { return E_ABORT; }
    }
    IFACEMETHODIMP PostDeleteItem(DWORD flags, IShellItem*, HRESULT hr, IShellItem* replacement) override {
        completed = true; if (outcome == E_PENDING || FAILED(hr)) outcome = hr;
        if (!permanent && SUCCEEDED(hr) && (!(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) || !replacement)) outcome = E_FAIL;
        if (SUCCEEDED(hr)) ++deleted_items;
        const auto tick = GetTickCount64();
        if (progress && tick - last_progress >= 250) {
            last_progress = tick;
            progress("Deleting " + candidate.path + " · " + std::to_string(deleted_items) + " items processed");
        }
        return S_OK;
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT total, UINT done) override {
        const auto tick = GetTickCount64();
        if (progress && (tick - last_progress >= 250 || done == total)) {
            last_progress = tick;
            progress("Deleting " + candidate.path + " · " + std::to_string(done) + " / " + std::to_string(total) + " items");
        }
        return S_OK;
    }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};
}
std::string deletion_path_issue(const std::string& path) {
    try {
        auto raw = wide(path);
        std::replace(raw.begin(), raw.end(), L'/', L'\\');
        const bool drive = raw.size() >= 3 && iswalpha(raw[0]) && raw[1] == L':' && raw[2] == L'\\';
        const bool unc = raw.rfind(L"\\\\", 0) == 0 && raw.rfind(L"\\\\?", 0) != 0 && raw.rfind(L"\\\\.", 0) != 0;
        if (!drive && !unc) return "Deletion requires an absolute drive or network path";
        size_t start = drive ? 3 : 2, components = 0;
        for (size_t at = start; at < raw.size();) {
            const auto end = raw.find(L'\\', at);
            const auto part = raw.substr(at, end == std::wstring::npos ? end : end - at);
            if (!part.empty()) {
                if (part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return "Ambiguous path components are not allowed for deletion";
                ++components;
            }
            if (end == std::wstring::npos) break;
            at = end + 1;
        }
        if ((drive && !components) || (unc && components <= 2)) return "Deleting an entire drive or network share is blocked";
        auto p = normal(path);
        // Expand existing short-name aliases before applying root protections.
        std::vector<wchar_t> long_path(32768);
        const auto length = GetLongPathNameW(p.c_str(), long_path.data(), static_cast<DWORD>(long_path.size()));
        if (length && length < long_path.size()) p = normal(utf8(std::wstring(long_path.data(), length)));
        if (protected_path(utf8(p))) return "Deleting a protected path is blocked";
        wchar_t volume[32768]{};
        if (GetVolumePathNameW(p.c_str(), volume, 32768) && lower(normal(utf8(volume))) == lower(p)) return "Deleting a volume root is blocked";
        for (auto name : {L"USERPROFILE", L"PUBLIC", L"SystemRoot", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramData"}) {
            const auto value = environment(name);
            if (!value.empty() && is_under(normal(utf8(value)), p)) return "Deleting a user/system root or its ancestor is blocked";
        }
        return {};
    } catch (const std::exception& ex) { return ex.what(); }
}
static RecycleResult remove_candidates(const std::vector<Candidate>& candidates, bool permanent, const std::function<void(const std::string&)>& progress = {}) {
    RecycleResult result; ComApartment apartment;
    for (size_t i = 0; i < candidates.size(); ++i) {
        auto c = candidates[i];
        try {
            if (progress) progress("Preparing " + std::to_string(i + 1) + " / " + std::to_string(candidates.size()) + ": " + c.path);
            if (!apartment.ready()) throw std::runtime_error("Shell initialization failed");
            const auto root_issue = deletion_path_issue(c.path);
            if (!root_issue.empty()) throw std::runtime_error(root_issue);
            auto p = normal(c.path);
            if (!permanent && p.size() >= MAX_PATH) throw std::runtime_error("Recycle for paths over 259 characters is unavailable; use Explorer");
            wchar_t volume[MAX_PATH]{};
            if (!permanent && (!GetVolumePathNameW(p.c_str(), volume, MAX_PATH) || GetDriveTypeW(volume) != DRIVE_FIXED)) throw std::runtime_error("Only local fixed-drive Recycle Bins are supported");
            for (size_t j = 0; j < candidates.size(); ++j) if (i != j) {
                auto other = normal(candidates[j].path);
                if (is_under(p, other) || is_under(other, p)) throw std::runtime_error("Overlapping selections; select each tree once");
                if (!c.duplicate_keeper.empty() && is_under(normal(c.duplicate_keeper), other)) throw std::runtime_error("The retained duplicate is also selected");
            }
            const bool fresh_selection = permanent && c.validation_token.empty();
            if (fresh_selection) {
                const auto current = scan(c.path);
                if (current.errors || current.cancelled || current.entries.empty()) throw std::runtime_error("Selected item is not fully accessible");
                const auto entries = subtree(current, 0);
                const auto issue = permanent_tree_issue(entries);
                if (!issue.empty()) throw std::runtime_error(issue);
                c.validation_token = fingerprint(entries);
            }
            if (!fresh_selection) { auto issue = validate_candidate(c, permanent); if (!issue.empty()) throw std::runtime_error(issue); }
            if (progress) progress("Deleting " + std::to_string(i + 1) + " / " + std::to_string(candidates.size()) + ": " + c.path);
            ComPtr<IFileOperation> operation; HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
            if (FAILED(hr)) throw std::runtime_error(winerror(hr));
            // The desktop already confirmed the chosen mode; never request elevation.
            hr = operation->SetOperationFlags(FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOFX_EARLYFAILURE | (permanent ? 0 : FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE));
            ComPtr<IShellItem> item;
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&item));
            ComPtr<RecycleSink> sink; sink.Attach(new RecycleSink(c, permanent));
            sink->progress = progress;
            if (SUCCEEDED(hr)) hr = operation->DeleteItem(item.Get(), sink.Get());
            if (SUCCEEDED(hr)) hr = operation->PerformOperations();
            BOOL aborted = TRUE; operation->GetAnyOperationsAborted(&aborted);
            if (FAILED(hr) || aborted || !sink->completed || FAILED(sink->outcome)) {
                const auto code = FAILED(hr) ? hr : (FAILED(sink->outcome) ? sink->outcome : HRESULT_FROM_WIN32(ERROR_CANCELLED));
                std::ostringstream message; message << "Deletion failed (0x" << std::hex << static_cast<unsigned long>(code) << "): " << (sink->failure.empty() ? winerror(code) : sink->failure);
                throw std::runtime_error(message.str());
            }
            const auto remaining = GetFileAttributesW(extended(p).c_str());
            const auto remaining_error = remaining == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
            if (remaining != INVALID_FILE_ATTRIBUTES || (remaining_error != ERROR_FILE_NOT_FOUND && remaining_error != ERROR_PATH_NOT_FOUND))
                throw std::runtime_error("Deletion was incomplete or could not be verified; some contents may already be gone");
            result.recycled.push_back(c.path);
        } catch (const std::exception& e) { result.failures.push_back(c.path + ": " + e.what()); }
    }
    return result;
}
RecycleResult recycle(const std::vector<Candidate>& candidates) { return remove_candidates(candidates, false); }
RecycleResult delete_permanently(const std::vector<Candidate>& candidates, const std::function<void(const std::string&)>& progress) { return remove_candidates(candidates, true, progress); }
std::string pick_folder() {
    ComApartment apartment; if (!apartment.ready()) return {};
    ComPtr<IFileOpenDialog> dialog; if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD flags = 0; dialog->GetOptions(&flags); dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"Choose a folder or drive to scan"); if (FAILED(dialog->Show(nullptr))) return {};
    ComPtr<IShellItem> item; if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR path = nullptr; if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    auto result = utf8(path); CoTaskMemFree(path); return result;
}
bool reveal(const std::string& path) {
    try { ComApartment apartment; if (!apartment.ready()) return false;
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(normal(path).c_str()); if (!item) return false;
        auto hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0); ILFree(item); return SUCCEEDED(hr);
    } catch (...) { return false; }
}
bool open_recycle_bin() { return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL)) > 32; }
}


