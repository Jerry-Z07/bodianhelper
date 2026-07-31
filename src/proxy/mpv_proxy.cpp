#include <windows.h>

#include "../bridge/bridge_core.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
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
// 定期采样间隔：1s。兼顾进度刷新及时性与对 mpv 的干扰最小化。
// 所有 mpv 属性读取统一由 SamplerWorker 执行，避免在波点主线程（mpv_wait_event/
// mpv_command/mpv_set_property 的调用线程）上同步读取属性，从而不延迟波点对 mpv 事件的处理。
// 见 https://mpv.io/manual/master/#client-api ：client API 线程安全，但同一 handle 上
// 的属性读取仍会与 mpv 内部核心竞争锁，快速切歌时累积会拖慢事件处理，间接触发 wasapi 初始化失败。
constexpr int64_t kStateSnapshotIntervalMs = 1000;

HMODULE g_real_module = nullptr;
std::once_flag g_load_once;
std::atomic<mpv_handle*> g_active_handle{nullptr};
std::atomic<bool> g_sampler_running{false};
std::atomic<bool> g_bridge_started{false};
std::atomic<bool> g_process_exiting{false};
// 上次采样时间戳，SamplerWorker 据此判断是否到定期采样时刻。
std::atomic<int64_t> g_last_snapshot_ms{0};
// 脏标志：mpv API 拦截路径（command/set_property/wait_event）检测到状态可能变化时置位，
// SamplerWorker 下一个 tick 立即采样。替代原先在调用线程同步采样的做法。
std::atomic<bool> g_snapshot_pending{false};
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

// 仅用于记录 NODE 类型取值（按需读取常见子类型，足够覆盖音频相关属性）。
struct mpv_node {
    int format;
    union {
        char* string;
        int64_t int64;
        double double_;
        int flag;
    } u;
};

// ---- 选项/属性记录（排查波点是否对音频做处理）----
// 受环境变量 BODIAN_MPV_TRACE=1 控制，默认关闭，不影响正常 SMTC 功能。
// 命中时把波点下发的 mpv 选项/属性/命令写入 %TEMP%\bodian_mpv_trace.log，
// 同时输出到 OutputDebugString（可用 DebugView / 调试器查看）。
constexpr wchar_t kTraceEnv[] = L"BODIAN_MPV_TRACE";
constexpr wchar_t kTraceFile[] = L"bodian_mpv_trace.log";

std::mutex g_trace_mutex;
std::ofstream g_trace_file;
bool g_trace_open = false;
bool g_trace_checked = false;
bool g_trace_enabled = false;

bool TraceEnabled() {
    if (g_trace_checked) {
        return g_trace_enabled;
    }
    wchar_t buf[8]{};
    const DWORD len = GetEnvironmentVariableW(kTraceEnv, buf, static_cast<DWORD>(std::size(buf)));
    g_trace_enabled = (len > 0 && (buf[0] == L'1' || buf[0] == L'Y' || buf[0] == L'y'));
    g_trace_checked = true;
    return g_trace_enabled;
}

void OpenTrace() {
    if (g_trace_open) {
        return;
    }
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + kTraceFile;
    g_trace_file.open(path, std::ios::out | std::ios::app);
    g_trace_open = true;
}

void TraceLine(const std::string& line) {
    if (!TraceEnabled()) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    OpenTrace();
    if (g_trace_file.is_open()) {
        g_trace_file << line << "\n";
        g_trace_file.flush();
    }
    OutputDebugStringA(line.c_str());
    OutputDebugStringA("\n");
}

std::string TraceTs() {
    const auto now = std::chrono::system_clock::now();
    const auto t = std::chrono::system_clock::to_time_t(now);
    const int ms = static_cast<int>(
        (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000).count());
    struct tm tm_buf {};
    localtime_s(&tm_buf, &t);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm_buf.tm_hour, tm_buf.tm_min,
                  tm_buf.tm_sec, ms);
    return buf;
}

std::string FormatMpvValue(mpv_format format, const void* data) {
    if (data == nullptr) {
        return "(null)";
    }
    switch (format) {
        case MPV_FORMAT_STRING:
        case MPV_FORMAT_OSD_STRING:
            return std::string("\"") + static_cast<const char*>(data) + "\"";
        case MPV_FORMAT_FLAG:
            return (*reinterpret_cast<const int*>(data) != 0) ? "true" : "false";
        case MPV_FORMAT_INT64:
            return std::to_string(*reinterpret_cast<const int64_t*>(data));
        case MPV_FORMAT_DOUBLE:
            return std::to_string(*reinterpret_cast<const double*>(data));
        case MPV_FORMAT_NODE: {
            const auto* n = reinterpret_cast<const mpv_node*>(data);
            switch (n->format) {
                case MPV_FORMAT_STRING:
                case MPV_FORMAT_OSD_STRING:
                    return std::string("NODE(str)=\"") + (n->u.string ? n->u.string : "") + "\"";
                case MPV_FORMAT_FLAG:
                    return std::string("NODE(flag)=") + (n->u.flag ? "true" : "false");
                case MPV_FORMAT_INT64:
                    return "NODE(int64)=" + std::to_string(n->u.int64);
                case MPV_FORMAT_DOUBLE:
                    return "NODE(double)=" + std::to_string(n->u.double_);
                default:
                    return "NODE(format=" + std::to_string(n->format) + ")";
            }
        }
        case MPV_FORMAT_NODE_ARRAY:
            return "NODE_ARRAY";
        case MPV_FORMAT_NODE_MAP:
            return "NODE_MAP";
        case MPV_FORMAT_BYTE_ARRAY:
            return "BYTE_ARRAY";
        default:
            return "format=" + std::to_string(format);
    }
}

std::string FormatCommandArgs(const char** args) {
    std::string s;
    if (args != nullptr) {
        for (int i = 0; args[i] != nullptr; ++i) {
            if (i) {
                s += " ";
            }
            s += args[i];
        }
    }
    return s;
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

// 请求一次异步采样：仅置脏标志，SamplerWorker 会在下一个 tick（最多 1s）内执行。
// 用于 mpv API 拦截路径，避免在波点调用线程上同步读取 mpv 属性。
void RequestSnapshot() {
    g_snapshot_pending.store(true);
}

void SetPause(bool pause) {
    mpv_handle* handle = g_active_handle.load();
    auto set_property = Real<mpv_set_property_fn>("mpv_set_property");
    if (handle == nullptr || set_property == nullptr) {
        return;
    }

    int value = pause ? 1 : 0;
    set_property(handle, "pause", MPV_FORMAT_FLAG, &value);
    // SMTC 控制回调（Play/Pause）需要立即反映状态，但仍在 SamplerWorker 线程异步采样，
    // 避免在回调线程上读取 mpv 属性。延迟最多 1s 可接受（SMTC 按钮反馈不需要毫秒级）。
    RequestSnapshot();
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
    RequestSnapshot();
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
            const int64_t now = NowMs();
            // 满足以下任一条件则采样：脏标志被置位（mpv API 拦截路径检测到变化）、
            // 或到了定期采样时刻（兜底保活）。
            const bool pending = g_snapshot_pending.exchange(false);
            const bool due = (now - g_last_snapshot_ms.load()) >= kStateSnapshotIntervalMs;
            if (pending || due) {
                SendStateSnapshot(handle);
            }
        }
        Sleep(200);  // 短轮询间隔，保证脏标志触发后最多 200ms 采样
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
        // 仅请求异步采样，不在调用线程（波点主线程）上同步读取 mpv 属性。
        // 原先在此同步调用 SendStateSnapshot 会读取 9 个属性，快速切歌时大量
        // mpv_command/mpv_set_property 调用累积，延迟波点对 mpv 事件的处理，
        // 间接触发 wasapi 音频设备初始化失败（no sound）。
        RequestSnapshot();
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
    TraceLine(TraceTs() + " mpv_command args=\"" + FormatCommandArgs(args) + "\"");
    if (args != nullptr && args[0] != nullptr) {
        NotifyEvent(args[0], handle);
    }
    return result;
}

extern "C" __declspec(dllexport) int mpv_command_async(mpv_handle* handle, uint64_t reply_userdata, const char** args) {
    auto real = Real<mpv_command_async_fn>("mpv_command_async");
    const int result = real ? real(handle, reply_userdata, args) : -1;
    TraceLine(TraceTs() + " mpv_command_async args=\"" + FormatCommandArgs(args) + "\"");
    if (args != nullptr && args[0] != nullptr) {
        NotifyEvent(args[0], handle);
    }
    return result;
}

extern "C" __declspec(dllexport) int mpv_command_string(mpv_handle* handle, const char* args) {
    auto real = Real<mpv_command_string_fn>("mpv_command_string");
    const int result = real ? real(handle, args) : -1;
    TraceLine(TraceTs() + " mpv_command_string args=\"" + (args ? args : "?") + "\"");
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
    TraceLine(TraceTs() + " mpv_set_property name=\"" + (name ? name : "?") +
              "\" format=" + std::to_string(format) +
              " value=" + FormatMpvValue(format, data));
    NotifyEvent(name ? name : "mpv_set_property", handle);
    return result;
}

extern "C" __declspec(dllexport) int mpv_set_property_string(mpv_handle* handle, const char* name, const char* data) {
    auto real = Real<mpv_set_property_string_fn>("mpv_set_property_string");
    const int result = real ? real(handle, name, data) : -1;
    TraceLine(TraceTs() + " mpv_set_property_string name=\"" + (name ? name : "?") +
              "\" value=\"" + (data ? data : "?") + "\"");
    NotifyEvent(name ? name : "mpv_set_property_string", handle);
    return result;
}

extern "C" __declspec(dllexport) int mpv_set_option(mpv_handle* handle, const char* name,
                                                    mpv_format format, void* data) {
    using fn = int (*)(mpv_handle*, const char*, mpv_format, void*);
    auto real = Real<fn>("mpv_set_option");
    const int result = real ? real(handle, name, format, data) : -1;
    TraceLine(TraceTs() + " mpv_set_option name=\"" + (name ? name : "?") +
              "\" format=" + std::to_string(format) +
              " value=" + FormatMpvValue(format, data));
    return result;
}

extern "C" __declspec(dllexport) int mpv_set_option_string(mpv_handle* handle, const char* name,
                                                          const char* data) {
    using fn = int (*)(mpv_handle*, const char*, const char*);
    auto real = Real<fn>("mpv_set_option_string");
    const int result = real ? real(handle, name, data) : -1;
    TraceLine(TraceTs() + " mpv_set_option_string name=\"" + (name ? name : "?") +
              "\" value=\"" + (data ? data : "?") + "\"");
    return result;
}

extern "C" __declspec(dllexport) int mpv_set_property_async(mpv_handle* handle,
                                                           uint64_t reply_userdata,
                                                           const char* name, mpv_format format,
                                                           void* data) {
    using fn = int (*)(mpv_handle*, uint64_t, const char*, mpv_format, void*);
    auto real = Real<fn>("mpv_set_property_async");
    const int result = real ? real(handle, reply_userdata, name, format, data) : -1;
    TraceLine(TraceTs() + " mpv_set_property_async name=\"" + (name ? name : "?") +
              "\" format=" + std::to_string(format) +
              " value=" + FormatMpvValue(format, data));
    return result;
}

extern "C" __declspec(dllexport) mpv_event* mpv_wait_event(mpv_handle* handle, double timeout) {
    auto real = Real<mpv_wait_event_fn>("mpv_wait_event");
    mpv_event* event = real ? real(handle, timeout) : nullptr;
    if (event != nullptr) {
        // 仅请求异步采样，不在波点主线程上同步读取属性。
        // 波点主线程需要尽快处理完 mpv 事件并返回等待下一个事件，同步属性读取会
        // 延迟事件循环，快速切歌时影响音频设备初始化时序。
        RequestSnapshot();
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
