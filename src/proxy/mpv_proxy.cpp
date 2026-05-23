#include <windows.h>

#include "../bridge/bridge_core.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
constexpr int64_t kStateSnapshotIntervalMs = 5000;

HMODULE g_real_module = nullptr;
std::once_flag g_load_once;
std::atomic<mpv_handle*> g_active_handle{nullptr};
std::atomic<bool> g_sampler_running{false};
std::atomic<bool> g_bridge_started{false};
std::atomic<bool> g_process_exiting{false};
std::atomic<int64_t> g_last_snapshot_ms{0};
std::atomic<int64_t> g_ignore_synthetic_media_key_until_ms{0};
std::thread g_sampler_thread;

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

    bodian_bridge::PlaybackState state;
    state.connected = true;
    state.playing = !paused && !idle && !eof;
    state.position_ms = static_cast<int64_t>(position * 1000.0);
    state.duration_ms = static_cast<int64_t>(duration * 1000.0);
    state.speed = speed;
    state.timestamp_ms = NowMs();
    state.title = title;
    state.path = path;
    state.filename = filename;
    bodian_bridge::SubmitPlaybackState(state);
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

void SetPause(bool pause) {
    mpv_handle* handle = g_active_handle.load();
    auto set_property = Real<mpv_set_property_fn>("mpv_set_property");
    if (handle == nullptr || set_property == nullptr) {
        return;
    }

    int value = pause ? 1 : 0;
    set_property(handle, "pause", MPV_FORMAT_FLAG, &value);
    SendStateSnapshot(handle);
}

void SeekToMs(int64_t position_ms) {
    mpv_handle* handle = g_active_handle.load();
    auto command = Real<mpv_command_fn>("mpv_command");
    if (handle == nullptr || command == nullptr) {
        return;
    }

    std::string seconds = std::to_string(static_cast<double>(position_ms) / 1000.0);
    const char* args[] = {"seek", seconds.c_str(), "absolute", "exact", nullptr};
    command(handle, args);
    SendStateSnapshot(handle);
}

void SendSyntheticMediaKey(BYTE virtual_key) {
    const int64_t now = NowMs();
    if (now < g_ignore_synthetic_media_key_until_ms.load()) {
        return;
    }

    // 波点的 Dart 侧媒体键监听只响应真实键盘事件，SMTC 切歌需模拟系统媒体键。
    g_ignore_synthetic_media_key_until_ms.store(now + 500);
    keybd_event(virtual_key, 0, 0, 0);
    keybd_event(virtual_key, 0, KEYEVENTF_KEYUP, 0);
}

void EnsureBridge() {
    bool expected = false;
    if (g_bridge_started.compare_exchange_strong(expected, true)) {
        bodian_bridge::ControlCallbacks callbacks;
        callbacks.play = [] { SetPause(false); };
        callbacks.pause = [] { SetPause(true); };
        callbacks.next = [] { SendSyntheticMediaKey(VK_MEDIA_NEXT_TRACK); };
        callbacks.previous = [] { SendSyntheticMediaKey(VK_MEDIA_PREV_TRACK); };
        callbacks.seek_to_ms = [](int64_t position_ms) { SeekToMs(position_ms); };
        bodian_bridge::StartSmtcBridge(std::move(callbacks));
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
    bool expected = false;
    if (g_sampler_running.compare_exchange_strong(expected, true)) {
        g_sampler_thread = std::thread(SamplerWorker);
        g_sampler_thread.detach();
    }
}

void NotifyEvent(std::string_view name, mpv_handle* handle) {
    (void)name;
    if (handle == nullptr) {
        bodian_bridge::MarkPlaybackDisconnected();
    } else {
        SendStateSnapshot(handle);
    }
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
    }
    return handle;
}

extern "C" __declspec(dllexport) mpv_handle* mpv_create_client(mpv_handle* handle, const char* name) {
    using fn = mpv_handle* (*)(mpv_handle*, const char*);
    auto real = Real<fn>("mpv_create_client");
    mpv_handle* client = real ? real(handle, name) : nullptr;
    if (client != nullptr) {
        g_active_handle.store(client);
    }
    return client;
}

extern "C" __declspec(dllexport) mpv_handle* mpv_create_weak_client(mpv_handle* handle, const char* name) {
    using fn = mpv_handle* (*)(mpv_handle*, const char*);
    auto real = Real<fn>("mpv_create_weak_client");
    mpv_handle* client = real ? real(handle, name) : nullptr;
    if (client != nullptr) {
        g_active_handle.store(client);
    }
    return client;
}

extern "C" __declspec(dllexport) int mpv_initialize(mpv_handle* handle) {
    auto real = Real<mpv_initialize_fn>("mpv_initialize");
    const int result = real ? real(handle) : -1;
    if (result >= 0) {
        g_active_handle.store(handle);
        EnsureBridge();
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
        bodian_bridge::StopSmtcBridge();
    }
    return TRUE;
}
