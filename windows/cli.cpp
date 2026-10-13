#include "core.h"
#include <windows.h>
#include <iostream>
#include <string>

namespace {
stale::win::ScanControl control;
BOOL WINAPI cancel(DWORD signal) { if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) { control.cancel = true; return TRUE; } return FALSE; }
std::string utf8(const wchar_t* value) {
    int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size, 0); WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr); out.pop_back(); return out;
}
}
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8); SetConsoleCtrlHandler(cancel, TRUE);
    if (argc < 3) {
        std::cerr << "Usage: stale_windows_cli scan|cleanup|duplicates <folder> [--save <index>]\n"
                     "       stale_windows_cli load <index>\nRead-only CLI. Ctrl+C cancels. Times are last modified, never last used.\n";
        return 2;
    }
    auto command = utf8(argv[1]); stale::win::Snapshot snapshot; std::string error;
    if (command == "load") {
        if (!stale::win::load_snapshot(utf8(argv[2]), snapshot, &error)) { std::cerr << error << '\n'; return 1; }
        std::cout << "Saved index (may be stale); scanned at Unix time " << snapshot.scanned_at << '\n';
    } else if (command == "scan" || command == "cleanup" || command == "duplicates") snapshot = stale::win::scan(utf8(argv[2]), &control);
    else { std::cerr << "Unknown command\n"; return 2; }
    std::cout << snapshot.root << "\nFiles: " << snapshot.files << "\nErrors: " << snapshot.errors << '\n';
    if (!snapshot.entries.empty()) std::cout << "Logical bytes: " << snapshot.entries[0].logical_bytes << "\nAllocated bytes: " << snapshot.entries[0].allocated_bytes << '\n';
    if (!snapshot.error.empty()) std::cerr << snapshot.error << '\n';
    if (snapshot.cancelled) { std::cerr << "Scan cancelled; results are partial\n"; return 130; }
    if (command == "cleanup" || command == "duplicates") {
        auto candidates = command == "cleanup" ? stale::win::cleanup_candidates(snapshot) : stale::win::find_duplicates(snapshot, &control);
        for (const auto& c : candidates) std::cout << c.bytes << '\t' << c.path << "\n  " << c.reason << '\n';
        if (control.cancel) return 130;
    }
    if (argc == 5 && utf8(argv[3]) == "--save" && !stale::win::save_snapshot(snapshot, utf8(argv[4]), &error)) { std::cerr << error << '\n'; return 1; }
    return snapshot.errors ? 1 : 0;
}
