#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

struct mpv_handle;
struct mpv_event;
struct mpv_node;

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" void InitializeForwardedExports(HMODULE real_module);
extern "C" __declspec(noreturn) void MissingForwardedExport() {
    TerminateProcess(GetCurrentProcess(), ERROR_PROC_NOT_FOUND);
    __assume(false);
}

enum mpv_format {
    MPV_FORMAT_NONE = 0,
    MPV_FORMAT_STRING = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG = 3,
    MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5,
    MPV_FORMAT_NODE = 6,
    MPV_FORMAT_NODE_ARRAY = 7,
    MPV_FORMAT_NODE_MAP = 8,
    MPV_FORMAT_BYTE_ARRAY = 9,
};

namespace {

constexpr wchar_t kRealDllName[] = L"libmpv_real.dll";
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\bodian-smtc-bridge";
constexpr int64_t kStateSnapshotIntervalMs = 5000;

HMODULE g_real_module = nullptr;
std::once_flag g_load_once;
std::atomic<mpv_handle*> g_active_handle{nullptr};
std::atomic<bool> g_sampler_running{false};
std::atomic<bool> g_process_exiting{false};
std::atomic<int64_t> g_last_snapshot_ms{0};
std::thread g_sampler_thread;

std::mutex g_pipe_mutex;
HANDLE g_pipe = INVALID_HANDLE_VALUE;
std::deque<std::string> g_outbox;
std::atomic<bool> g_pipe_running{false};
std::thread g_pipe_thread;

using mpv_create_fn = mpv_handle* (*)();
using mpv_destroy_fn = void (*)(mpv_handle*);
using mpv_initialize_fn = int (*)(mpv_handle*);
using mpv_command_fn = int (*)(mpv_handle*, const char**);
using mpv_command_async_fn = int (*)(mpv_handle*, uint64_t, const char**);
using mpv_command_string_fn = int (*)(mpv_handle*, const char*);
using mpv_get_property_fn = int (*)(mpv_handle*, const char*, mpv_format, void*);
using mpv_get_property_string_fn = char* (*)(mpv_handle*, const char*);
using mpv_set_property_fn = int (*)(mpv_handle*, const char*, mpv_format, void*);
using mpv_set_property_string_fn = int (*)(mpv_handle*, const char*, const char*);
using mpv_free_fn = void (*)(void*);
using mpv_wait_event_fn = mpv_event* (*)(mpv_handle*, double);
using mpv_wakeup_fn = void (*)(mpv_handle*);

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string JsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char buffer[7]{};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                escaped += buffer;
            } else {
                escaped += static_cast<char>(ch);
            }
            break;
        }
    }
    return escaped;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void LoadRealModule() {
    std::call_once(g_load_once, [] {
        wchar_t module_path[MAX_PATH]{};
        GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), module_path, MAX_PATH);
        wchar_t* slash = wcsrchr(module_path, L'\\');
        if (slash != nullptr) {
            *(slash + 1) = L'\0';
            wcscat_s(module_path, kRealDllName);
            g_real_module = LoadLibraryW(module_path);
        }
        if (g_real_module == nullptr) {
            g_real_module = LoadLibraryW(kRealDllName);
        }
        InitializeForwardedExports(g_real_module);
    });
}

FARPROC ResolveRealProc(const char* name) {
    LoadRealModule();
    if (g_real_module == nullptr) {
        return nullptr;
    }
    return GetProcAddress(g_real_module, name);
}

template <typename T>
T Real(const char* name) {
    return reinterpret_cast<T>(ResolveRealProc(name));
}

void QueueMessage(std::string message) {
    {
        std::lock_guard lock(g_pipe_mutex);
        if (g_outbox.size() > 256) {
            g_outbox.pop_front();
        }
        g_outbox.push_back(std::move(message));
    }
}

std::string ReadStringProperty(mpv_handle* handle, const char* name) {
    auto get_string = Real<mpv_get_property_string_fn>("mpv_get_property_string");
    auto free_value = Real<mpv_free_fn>("mpv_free");
    if (get_string == nullptr || free_value == nullptr || handle == nullptr) {
        return {};
    }

    char* raw = get_string(handle, name);
    if (raw == nullptr) {
        return {};
    }
    std::string value(raw);
    free_value(raw);
    return value;
}

bool ReadFlagProperty(mpv_handle* handle, const char* name, bool fallback = false) {
    auto get_property = Real<mpv_get_property_fn>("mpv_get_property");
    if (get_property == nullptr || handle == nullptr) {
        return fallback;
    }

    int value = fallback ? 1 : 0;
    if (get_property(handle, name, MPV_FORMAT_FLAG, &value) < 0) {
        return fallback;
    }
    return value != 0;
}

double ReadDoubleProperty(mpv_handle* handle, const char* name, double fallback = 0.0) {
    auto get_property = Real<mpv_get_property_fn>("mpv_get_property");
    if (get_property == nullptr || handle == nullptr) {
        return fallback;
    }

    double value = fallback;
    if (get_property(handle, name, MPV_FORMAT_DOUBLE, &value) < 0) {
        return fallback;
    }
    return value;
}

void SendStateSnapshot(mpv_handle* handle) {
    if (handle == nullptr) {
        return;
    }
    g_last_snapshot_ms.store(NowMs());

    const bool paused = ReadFlagProperty(handle, "pause", true);
    const bool idle = ReadFlagProperty(handle, "idle-active", false);
    const bool eof = ReadFlagProperty(handle, "eof-reached", false);
    const double position = ReadDoubleProperty(handle, "time-pos", 0.0);
    const double duration = ReadDoubleProperty(handle, "duration", 0.0);
    const double speed = ReadDoubleProperty(handle, "speed", 1.0);
    const std::string title = ReadStringProperty(handle, "media-title");
    const std::string path = ReadStringProperty(handle, "path");
    const std::string filename = ReadStringProperty(handle, "filename");

    std::ostringstream json;
    json << "{\"type\":\"state\""
         << ",\"playing\":" << ((!paused && !idle && !eof) ? "true" : "false")
         << ",\"positionMs\":" << static_cast<int64_t>(position * 1000.0)
         << ",\"durationMs\":" << static_cast<int64_t>(duration * 1000.0)
         << ",\"speed\":" << speed
         << ",\"title\":\"" << JsonEscape(title) << "\""
         << ",\"path\":\"" << JsonEscape(path) << "\""
         << ",\"filename\":\"" << JsonEscape(filename) << "\""
         << ",\"timestampMs\":" << NowMs()
         << "}\n";
    QueueMessage(json.str());
}

void SendStateSnapshotThrottled(mpv_handle* handle) {
    if (handle == nullptr) {
        return;
    }

    const int64_t now = NowMs();
    int64_t last = g_last_snapshot_ms.load();
    while (now - last >= kStateSnapshotIntervalMs) {
        if (g_last_snapshot_ms.compare_exchange_weak(last, now)) {
            SendStateSnapshot(handle);
            return;
        }
    }
}

void ApplyBridgeCommand(std::string_view line) {
    mpv_handle* handle = g_active_handle.load();
    auto command = Real<mpv_command_fn>("mpv_command");
    auto set_property = Real<mpv_set_property_fn>("mpv_set_property");
    if (handle == nullptr) {
        return;
    }

    if (line.find("\"command\":\"play\"") != std::string_view::npos && set_property != nullptr) {
        int value = 0;
        set_property(handle, "pause", MPV_FORMAT_FLAG, &value);
    } else if (line.find("\"command\":\"pause\"") != std::string_view::npos && set_property != nullptr) {
        int value = 1;
        set_property(handle, "pause", MPV_FORMAT_FLAG, &value);
    } else if (line.find("\"command\":\"toggle\"") != std::string_view::npos && command != nullptr) {
        const char* args[] = {"cycle", "pause", nullptr};
        command(handle, args);
    } else if (line.find("\"command\":\"next\"") != std::string_view::npos && command != nullptr) {
        const char* args[] = {"playlist-next", "force", nullptr};
        command(handle, args);
    } else if (line.find("\"command\":\"previous\"") != std::string_view::npos && command != nullptr) {
        const char* args[] = {"playlist-prev", "force", nullptr};
        command(handle, args);
    } else {
        const std::string marker = "\"command\":\"seekToMs\"";
        const size_t command_pos = line.find(marker);
        const size_t value_pos = line.find("\"positionMs\":");
        if (command_pos != std::string_view::npos && value_pos != std::string_view::npos && command != nullptr) {
            const char* start = line.data() + value_pos + 13;
            char* end = nullptr;
            const long long position_ms = std::strtoll(start, &end, 10);
            std::string seconds = std::to_string(static_cast<double>(position_ms) / 1000.0);
            const char* args[] = {"seek", seconds.c_str(), "absolute", "exact", nullptr};
            command(handle, args);
        }
    }
}

void PipeWorker() {
    std::string read_buffer;
    while (!g_process_exiting.load()) {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        {
            std::lock_guard lock(g_pipe_mutex);
            pipe = g_pipe;
        }

        if (pipe == INVALID_HANDLE_VALUE) {
            HANDLE new_pipe = CreateFileW(
                kPipeName,
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);

            if (new_pipe == INVALID_HANDLE_VALUE) {
                Sleep(1000);
                continue;
            }

            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(new_pipe, &mode, nullptr, nullptr);
            std::lock_guard lock(g_pipe_mutex);
            g_pipe = new_pipe;
            pipe = new_pipe;
        }

        std::string next_message;
        {
            std::lock_guard lock(g_pipe_mutex);
            if (!g_outbox.empty()) {
                next_message = std::move(g_outbox.front());
                g_outbox.pop_front();
            }
        }

        if (!next_message.empty()) {
            DWORD written = 0;
            if (!WriteFile(pipe, next_message.data(), static_cast<DWORD>(next_message.size()), &written, nullptr)) {
                std::lock_guard lock(g_pipe_mutex);
                CloseHandle(g_pipe);
                g_pipe = INVALID_HANDLE_VALUE;
                continue;
            }
        }

        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (ReadFile(pipe, buffer.data(), available, &read, nullptr)) {
                read_buffer.append(buffer.data(), read);
                size_t newline = std::string::npos;
                while ((newline = read_buffer.find('\n')) != std::string::npos) {
                    std::string line = read_buffer.substr(0, newline);
                    read_buffer.erase(0, newline + 1);
                    ApplyBridgeCommand(line);
                }
            }
        }

        Sleep(next_message.empty() ? 50 : 5);
    }
}

void EnsurePipeWorker() {
    bool expected = false;
    if (g_pipe_running.compare_exchange_strong(expected, true)) {
        g_pipe_thread = std::thread(PipeWorker);
        g_pipe_thread.detach();
    }
}

void SamplerWorker() {
    while (!g_process_exiting.load()) {
        mpv_handle* handle = g_active_handle.load();
        if (handle != nullptr) {
            SendStateSnapshot(handle);
        }
        Sleep(static_cast<DWORD>(kStateSnapshotIntervalMs));
    }
}

void EnsureSampler() {
    EnsurePipeWorker();
    bool expected = false;
    if (g_sampler_running.compare_exchange_strong(expected, true)) {
        g_sampler_thread = std::thread(SamplerWorker);
        g_sampler_thread.detach();
    }
}

void NotifyEvent(std::string_view name, mpv_handle* handle) {
    std::ostringstream json;
    json << "{\"type\":\"event\",\"name\":\"" << JsonEscape(name) << "\",\"timestampMs\":" << NowMs() << "}\n";
    QueueMessage(json.str());
    SendStateSnapshot(handle);
}

} // namespace

extern "C" __declspec(dllexport) mpv_handle* mpv_create() {
    auto real = Real<mpv_create_fn>("mpv_create");
    if (real == nullptr) {
        return nullptr;
    }
    mpv_handle* handle = real();
    if (handle != nullptr) {
        g_active_handle.store(handle);
        EnsureSampler();
        NotifyEvent("mpv_create", handle);
    }
    return handle;
}

extern "C" __declspec(dllexport) mpv_handle* mpv_create_client(mpv_handle* handle, const char* name) {
    using fn = mpv_handle* (*)(mpv_handle*, const char*);
    auto real = Real<fn>("mpv_create_client");
    mpv_handle* client = real ? real(handle, name) : nullptr;
    if (client != nullptr) {
        g_active_handle.store(client);
        EnsureSampler();
    }
    return client;
}

extern "C" __declspec(dllexport) mpv_handle* mpv_create_weak_client(mpv_handle* handle, const char* name) {
    using fn = mpv_handle* (*)(mpv_handle*, const char*);
    auto real = Real<fn>("mpv_create_weak_client");
    mpv_handle* client = real ? real(handle, name) : nullptr;
    if (client != nullptr) {
        g_active_handle.store(client);
        EnsureSampler();
    }
    return client;
}

extern "C" __declspec(dllexport) int mpv_initialize(mpv_handle* handle) {
    auto real = Real<mpv_initialize_fn>("mpv_initialize");
    const int result = real ? real(handle) : -1;
    if (result >= 0) {
        g_active_handle.store(handle);
        EnsureSampler();
        NotifyEvent("mpv_initialize", handle);
    }
    return result;
}

extern "C" __declspec(dllexport) void mpv_destroy(mpv_handle* handle) {
    if (g_active_handle.load() == handle) {
        g_active_handle.store(nullptr);
    }
    NotifyEvent("mpv_destroy", nullptr);
    auto real = Real<mpv_destroy_fn>("mpv_destroy");
    if (real) {
        real(handle);
    }
}

extern "C" __declspec(dllexport) void mpv_terminate_destroy(mpv_handle* handle) {
    if (g_active_handle.load() == handle) {
        g_active_handle.store(nullptr);
    }
    NotifyEvent("mpv_terminate_destroy", nullptr);
    auto real = Real<mpv_destroy_fn>("mpv_terminate_destroy");
    if (real) {
        real(handle);
    }
}

extern "C" __declspec(dllexport) void mpv_free(void* data) {
    auto real = Real<mpv_free_fn>("mpv_free");
    if (real) {
        real(data);
    }
}

extern "C" __declspec(dllexport) int mpv_command(mpv_handle* handle, const char** args) {
    auto real = Real<mpv_command_fn>("mpv_command");
    const int result = real ? real(handle, args) : -1;
    if (args != nullptr && args[0] != nullptr) {
        NotifyEvent(args[0], handle);
    }
    return result;
}

extern "C" __declspec(dllexport) int mpv_command_async(mpv_handle* handle, uint64_t reply_userdata, const char** args) {
    auto real = Real<mpv_command_async_fn>("mpv_command_async");
    const int result = real ? real(handle, reply_userdata, args) : -1;
    if (args != nullptr && args[0] != nullptr) {
        NotifyEvent(args[0], handle);
    }
    return result;
}

extern "C" __declspec(dllexport) int mpv_command_string(mpv_handle* handle, const char* args) {
    auto real = Real<mpv_command_string_fn>("mpv_command_string");
    const int result = real ? real(handle, args) : -1;
    NotifyEvent(args ? args : "mpv_command_string", handle);
    return result;
}

extern "C" __declspec(dllexport) int mpv_get_property(mpv_handle* handle, const char* name, mpv_format format, void* data) {
    auto real = Real<mpv_get_property_fn>("mpv_get_property");
    return real ? real(handle, name, format, data) : -1;
}

extern "C" __declspec(dllexport) char* mpv_get_property_string(mpv_handle* handle, const char* name) {
    auto real = Real<mpv_get_property_string_fn>("mpv_get_property_string");
    return real ? real(handle, name) : nullptr;
}

extern "C" __declspec(dllexport) int mpv_set_property(mpv_handle* handle, const char* name, mpv_format format, void* data) {
    auto real = Real<mpv_set_property_fn>("mpv_set_property");
    const int result = real ? real(handle, name, format, data) : -1;
    NotifyEvent(name ? name : "mpv_set_property", handle);
    return result;
}

extern "C" __declspec(dllexport) int mpv_set_property_string(mpv_handle* handle, const char* name, const char* data) {
    auto real = Real<mpv_set_property_string_fn>("mpv_set_property_string");
    const int result = real ? real(handle, name, data) : -1;
    NotifyEvent(name ? name : "mpv_set_property_string", handle);
    return result;
}

extern "C" __declspec(dllexport) mpv_event* mpv_wait_event(mpv_handle* handle, double timeout) {
    auto real = Real<mpv_wait_event_fn>("mpv_wait_event");
    mpv_event* event = real ? real(handle, timeout) : nullptr;
    if (event != nullptr) {
        SendStateSnapshotThrottled(handle);
    }
    return event;
}

extern "C" __declspec(dllexport) void mpv_wakeup(mpv_handle* handle) {
    auto real = Real<mpv_wakeup_fn>("mpv_wakeup");
    if (real) {
        real(handle);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        LoadRealModule();
    } else if (reason == DLL_PROCESS_DETACH) {
        g_process_exiting.store(true);
        std::lock_guard lock(g_pipe_mutex);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }
    }
    return TRUE;
}
