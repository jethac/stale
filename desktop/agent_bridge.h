#pragma once
#include <windows.h>
#include <sddl.h>
#include <slint.h>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Opt-in, same-user, local-only transport. Commands are dispatched to the GUI
// thread, so agent actions use the same controller and state as visible controls.
class AgentBridge {
public:
    using Handler = std::function<std::string(const std::string&)>;
    using Dispatch = std::function<void(std::function<void()>)>;
private:
    struct Handle {
        HANDLE value = INVALID_HANDLE_VALUE;
        explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };
    struct Shared {
        std::atomic<bool> active{true};
        Handler handler;
    };
    Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Handle pipe_;
    std::shared_ptr<Shared> shared_;
    Dispatch dispatch_;
    std::thread thread_;

    bool complete(OVERLAPPED& operation, DWORD& count, DWORD timeout = 10000) {
        HANDLE events[] = {stop_.value, operation.hEvent};
        auto result = WaitForMultipleObjects(2, events, FALSE, timeout);
        if (result != WAIT_OBJECT_0 + 1) {
            CancelIoEx(pipe_.value, &operation);
            GetOverlappedResult(pipe_.value, &operation, &count, TRUE);
            return false;
        }
        return GetOverlappedResult(pipe_.value, &operation, &count, FALSE) != FALSE;
    }
    void serve() {
        while (WaitForSingleObject(stop_.value, 0) != WAIT_OBJECT_0) {
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED operation{}; operation.hEvent = event.value;
            DWORD count = 0;
            bool connected = ConnectNamedPipe(pipe_.value, &operation) != FALSE;
            if (!connected) {
                auto error = GetLastError();
                connected = error == ERROR_PIPE_CONNECTED || (error == ERROR_IO_PENDING && complete(operation, count, INFINITE));
            }
            if (!connected) break;
            std::string request;
            bool valid = true;
            while (request.find('\n') == std::string::npos && request.size() <= 65536) {
                char buffer[4096]; ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
                count = 0;
                bool read = ReadFile(pipe_.value, buffer, sizeof(buffer), &count, &operation) != FALSE;
                if (!read && GetLastError() == ERROR_IO_PENDING) read = complete(operation, count);
                if (!read || !count) { valid = false; break; }
                request.append(buffer, count);
            }
            std::string response;
            if (valid && request.size() <= 65536) {
                request.resize(request.find('\n'));
                struct Reply {
                    Handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
                    std::atomic<bool> expired{false};
                    std::string text;
                };
                auto reply = std::make_shared<Reply>();
                auto state = shared_;
                dispatch_([state, reply, request = std::move(request)] {
                    if (!state->active || reply->expired) return;
                    try { reply->text = state->handler(request); }
                    catch (...) { reply->text = R"({"ok":false,"error":"Command failed"})"; }
                    SetEvent(reply->ready.value);
                });
                HANDLE events[] = {stop_.value, reply->ready.value};
                auto result = WaitForMultipleObjects(2, events, FALSE, 15000);
                if (result == WAIT_OBJECT_0 + 1) response = std::move(reply->text);
                else { reply->expired = true; response = R"({"ok":false,"error":"GUI unavailable; query state before retrying"})"; }
            } else response = R"({"ok":false,"error":"Request must be one JSON line, at most 64 KiB"})";
            if (response.size() > 1024 * 1024) response = R"({"ok":false,"error":"Response too large; use pagination"})";
            response += '\n';
            ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
            count = 0;
            bool written = WriteFile(pipe_.value, response.data(), static_cast<DWORD>(response.size()), &count, &operation) != FALSE;
            if (!written && GetLastError() == ERROR_IO_PENDING) written = complete(operation, count);
            // An acknowledgment keeps DisconnectNamedPipe from discarding unread
            // response bytes. Unlike FlushFileBuffers, this has a bounded wait.
            if (written) {
                char acknowledgment[16]; ResetEvent(event.value); operation = {}; operation.hEvent = event.value;
                if (!ReadFile(pipe_.value, acknowledgment, sizeof(acknowledgment), &count, &operation) && GetLastError() == ERROR_IO_PENDING)
                    complete(operation, count, 2000);
            }
            DisconnectNamedPipe(pipe_.value);
        }
    }
public:
    explicit AgentBridge(const std::string& name, Handler handler,
                         Dispatch dispatch = [](std::function<void()> task) { slint::invoke_from_event_loop(std::move(task)); })
        : shared_(std::make_shared<Shared>()), dispatch_(std::move(dispatch)) {
        if (name.empty() || name.size() > 80 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            throw std::runtime_error("Agent pipe name must contain 1-80 letters, digits, hyphens or underscores");
        Handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) throw std::runtime_error("Cannot secure agent pipe");
        DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
        std::vector<char> data(size);
        if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size)) throw std::runtime_error("Cannot identify agent pipe user");
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) throw std::runtime_error("Cannot identify agent pipe user");
        std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) throw std::runtime_error("Cannot secure agent pipe");
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        std::wstring path = L"\\\\.\\pipe\\stale-" + std::wstring(name.begin(), name.end());
        pipe_.value = CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1024 * 1024, 65536, 0, &security);
        LocalFree(descriptor);
        if (pipe_.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Agent pipe unavailable; choose a unique name");
        shared_->handler = std::move(handler);
        thread_ = std::thread([this] { serve(); });
    }
    ~AgentBridge() {
        shared_->active = false;
        SetEvent(stop_.value);
        if (thread_.joinable()) thread_.join();
    }
    AgentBridge(const AgentBridge&) = delete;
    AgentBridge& operator=(const AgentBridge&) = delete;
};
