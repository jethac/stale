#include "desktop/agent_bridge.h"
#include <cassert>
#include <chrono>
#include <iostream>

void require(bool condition) { if (!condition) throw std::runtime_error("Agent transport assertion failed"); }
HANDLE connect(const std::string& name) {
    auto path = "\\\\.\\pipe\\stale-" + name;
    for (int i = 0; i < 100; ++i) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        Sleep(10);
    }
    throw std::runtime_error("Could not connect to test pipe");
}
std::string request(const std::string& name, const std::string& text) {
    HANDLE h = connect(name); DWORD count = 0; auto line = text + "\n";
    require(WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &count, nullptr) != FALSE);
    std::string result;
    while (result.find('\n') == std::string::npos) {
        char buffer[4096]; require(ReadFile(h, buffer, sizeof(buffer), &count, nullptr) && count);
        result.append(buffer, count);
    }
    WriteFile(h, "ACK\n", 4, &count, nullptr); CloseHandle(h);
    return result;
}
int main() {
    try {
        const auto name = "test-" + std::to_string(GetCurrentProcessId());
        const auto immediate = [](std::function<void()> f) { f(); };
        auto bridge = std::make_unique<AgentBridge>(name, [](const std::string& s) { return s; }, immediate);
        require(request(name, R"({"ok":true,"unicode":"日本語"})").find("日本語") != std::string::npos);
        require(request(name, R"({"second":true})").find("second") != std::string::npos);
        require(request(name, std::string(70000, 'a')).find("64 KiB") != std::string::npos);
        bool rejected = false;
        try { AgentBridge duplicate(name, [](auto) { return "{}"; }, immediate); } catch (...) { rejected = true; }
        require(rejected);
        rejected = false;
        try { AgentBridge invalid("../bad", [](auto) { return "{}"; }, immediate); } catch (...) { rejected = true; }
        require(rejected);
        HANDLE idle = connect(name);
        const auto start = std::chrono::steady_clock::now(); bridge.reset();
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2)); CloseHandle(idle);
        std::function<void()> queued; std::atomic<bool> ready{false}; bool called = false;
        bridge = std::make_unique<AgentBridge>(name, [&](auto) { called = true; return "{}"; }, [&](auto f) { queued = std::move(f); ready = true; });
        HANDLE pending = connect(name); DWORD count; WriteFile(pending, "{}\n", 3, &count, nullptr);
        for (int i = 0; i < 100 && !ready; ++i) Sleep(10);
        require(ready); bridge.reset(); queued(); require(!called); CloseHandle(pending);
        std::cout << "Agent transport tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
