#include "bridge_core.h"

#include <windows.h>
#include <SystemMediaTransportControlsInterop.h>
#include <urlmon.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace media = winrt::Windows::Media;
namespace foundation = winrt::Windows::Foundation;
namespace imaging = winrt::Windows::Graphics::Imaging;
namespace streams = winrt::Windows::Storage::Streams;

namespace bodian_bridge {
namespace {

constexpr int64_t kTimelineUpdateIntervalMs = 5000;
constexpr int64_t kTimelineDurationToleranceMs = 1000;
constexpr int64_t kTimelinePositionDriftToleranceMs = 3000;

struct SmtcPublishCache {
    bool has_status = false;
    bool status_playing = false;
    bool has_display = false;
    std::string display_key;
    bool has_timeline = false;
    bool timeline_playing = false;
    int64_t timeline_position_ms = 0;
    int64_t timeline_duration_ms = 0;
    int64_t timeline_publish_ms = 0;
    double timeline_speed = 1.0;
    std::string timeline_track_key;
};

struct WindowCandidate {
    HWND window = nullptr;
    LONG64 area = 0;
};

std::mutex g_state_mutex;
PlaybackState g_state;

std::mutex g_callbacks_mutex;
ControlCallbacks g_callbacks;

std::mutex g_smtc_mutex;
media::SystemMediaTransportControls g_smtc{nullptr};
HWND g_smtc_window = nullptr;
SmtcPublishCache g_smtc_publish;

std::atomic<bool> g_started{false};
std::atomic<bool> g_running{false};
std::thread g_bridge_thread;

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

foundation::TimeSpan TimeSpanFromMs(int64_t value) {
    return std::chrono::duration_cast<foundation::TimeSpan>(std::chrono::milliseconds(std::max<int64_t>(0, value)));
}

int64_t AbsDiff(int64_t left, int64_t right) {
    return left >= right ? left - right : right - left;
}

bool StartsWithAsciiIgnoreCase(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t index = 0; index < prefix.size(); ++index) {
        char left = value[index];
        char right = prefix[index];
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

bool EndsWithAsciiIgnoreCase(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    return StartsWithAsciiIgnoreCase(value.substr(value.size() - suffix.size()), suffix);
}

std::string PreferredCoverUrl(std::string_view url) {
    std::string result(url);
    const size_t query_pos = result.find_first_of("?#");
    const size_t path_end = query_pos == std::string::npos ? result.size() : query_pos;
    std::string_view path(result.data(), path_end);
    if (EndsWithAsciiIgnoreCase(path, ".webp")) {
        result.replace(path_end - 5, 5, ".jpg");
    }
    return result;
}

std::optional<std::vector<uint8_t>> DownloadUrl(std::string_view url) {
    winrt::com_ptr<IStream> stream;
    const HRESULT hr = URLOpenBlockingStreamW(nullptr, Utf8ToWide(url).c_str(), stream.put(), 0, nullptr);
    if (FAILED(hr)) {
        return std::nullopt;
    }

    std::vector<uint8_t> bytes;
    std::array<uint8_t, 16 * 1024> buffer{};
    for (;;) {
        ULONG read = 0;
        const HRESULT read_hr = stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read);
        if (FAILED(read_hr)) {
            return std::nullopt;
        }
        if (read == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
    }

    if (bytes.empty()) {
        return std::nullopt;
    }
    return bytes;
}

void WriteBytesToStream(streams::InMemoryRandomAccessStream const& stream, const std::vector<uint8_t>& bytes) {
    streams::DataWriter writer(stream.GetOutputStreamAt(0));
    writer.WriteBytes(winrt::array_view<const uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    writer.StoreAsync().get();
    writer.FlushAsync().get();
    writer.DetachStream();
    stream.Seek(0);
}

streams::RandomAccessStreamReference CoverThumbnailReference(std::string_view album_pic) {
    if (!StartsWithAsciiIgnoreCase(album_pic, "http://") && !StartsWithAsciiIgnoreCase(album_pic, "https://")) {
        return nullptr;
    }

    const std::string cover_url = PreferredCoverUrl(album_pic);
    auto cover_bytes = DownloadUrl(cover_url);
    if (!cover_bytes) {
        return nullptr;
    }

    try {
        streams::InMemoryRandomAccessStream input;
        WriteBytesToStream(input, *cover_bytes);

        const auto decoder = imaging::BitmapDecoder::CreateAsync(input).get();
        const auto bitmap = decoder.GetSoftwareBitmapAsync(
            imaging::BitmapPixelFormat::Bgra8,
            imaging::BitmapAlphaMode::Ignore)
                                .get();

        streams::InMemoryRandomAccessStream output;
        const auto encoder = imaging::BitmapEncoder::CreateAsync(imaging::BitmapEncoder::JpegEncoderId(), output).get();
        encoder.SetSoftwareBitmap(bitmap);
        encoder.FlushAsync().get();
        output.Seek(0);
        return streams::RandomAccessStreamReference::CreateFromStream(output);
    } catch (winrt::hresult_error const& error) {
        OutputDebugStringW((L"[BodianSmtcBridge] 封面转换失败: " + std::wstring(error.message().c_str()) + L"\n").c_str());
    } catch (std::exception const& error) {
        OutputDebugStringW((L"[BodianSmtcBridge] 封面转换失败: " + Utf8ToWide(error.what()) + L"\n").c_str());
    }
    return nullptr;
}

std::optional<std::string> ReadTail(const fs::path& path, size_t max_bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    const std::streamoff start = std::max<std::streamoff>(0, size - static_cast<std::streamoff>(max_bytes));
    file.seekg(start, std::ios::beg);
    std::string content(static_cast<size_t>(size - start), '\0');
    file.read(content.data(), static_cast<std::streamsize>(content.size()));
    return content;
}

fs::path NewestLogFile() {
    wchar_t local_app_data[MAX_PATH]{};
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
    if (size == 0 || size >= MAX_PATH) {
        return {};
    }

    fs::path log_dir = fs::path(local_app_data) / L"cn.wenyu.bodian" / L"bodian_pc" / L"bdlog";
    fs::path newest;
    fs::file_time_type newest_time{};
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(log_dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() != L".log") {
            continue;
        }
        const auto write_time = entry.last_write_time(ec);
        if (!ec && (newest.empty() || write_time > newest_time)) {
            newest = entry.path();
            newest_time = write_time;
        }
    }
    return newest;
}

std::string ExtractBetween(std::string_view text, std::string_view start_marker, std::string_view end_marker) {
    const size_t start = text.find(start_marker);
    if (start == std::string_view::npos) {
        return {};
    }
    const size_t value_start = start + start_marker.size();
    const size_t end = text.find(end_marker, value_start);
    if (end == std::string_view::npos || end <= value_start) {
        return {};
    }
    return std::string(text.substr(value_start, end - value_start));
}

std::optional<PlaybackState> ParseMetadataFromLogs() {
    const fs::path log = NewestLogFile();
    if (log.empty()) {
        return std::nullopt;
    }

    auto content = ReadTail(log, 2 * 1024 * 1024);
    if (!content) {
        return std::nullopt;
    }

    size_t pos = content->rfind("albumPic:");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    size_t line_start = content->rfind('\n', pos);
    line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
    const size_t line_end = content->find('\n', pos);
    const std::string_view line(content->data() + line_start, (line_end == std::string::npos ? content->size() : line_end) - line_start);

    PlaybackState parsed;
    parsed.title = ExtractBetween(line, "name: ", ", albumId:");
    parsed.album = ExtractBetween(line, "album: ", ", albumPic:");
    parsed.album_pic = ExtractBetween(line, "albumPic: ", ", albumPic120:");
    parsed.artist = ExtractBetween(line, "artist: ", ", artistId:");
    const std::string duration = ExtractBetween(line, "duration: ", ",");
    if (!duration.empty()) {
        parsed.duration_ms = std::strtoll(duration.c_str(), nullptr, 10) * 1000;
    }

    if (parsed.title.empty()) {
        parsed.title = ExtractBetween(line, "songName: ", ",");
    }

    return parsed.title.empty() ? std::nullopt : std::make_optional(parsed);
}

PlaybackState CurrentStateSnapshot() {
    std::lock_guard lock(g_state_mutex);
    return g_state;
}

std::string BuildDisplayKey(const PlaybackState& state) {
    std::ostringstream key;
    key << state.title << '\n'
        << state.artist << '\n'
        << state.album << '\n'
        << state.album_pic << '\n'
        << state.filename;
    return key.str();
}

std::string BuildTimelineTrackKey(const PlaybackState& state) {
    if (!state.path.empty()) {
        return state.path;
    }
    if (!state.filename.empty()) {
        return state.filename;
    }
    return state.title;
}

bool ShouldUpdateTimeline(const PlaybackState& state, int64_t now_ms, SmtcPublishCache& cache) {
    const std::string track_key = BuildTimelineTrackKey(state);
    if (!cache.has_timeline || track_key != cache.timeline_track_key || state.playing != cache.timeline_playing) {
        return true;
    }

    if (AbsDiff(state.duration_ms, cache.timeline_duration_ms) > kTimelineDurationToleranceMs) {
        return true;
    }

    const int64_t elapsed_ms = std::max<int64_t>(0, now_ms - cache.timeline_publish_ms);
    const double speed = cache.timeline_speed > 0.0 ? cache.timeline_speed : 1.0;
    const int64_t expected_position_ms = cache.timeline_position_ms +
        (cache.timeline_playing ? static_cast<int64_t>(static_cast<double>(elapsed_ms) * speed) : 0);

    // 正常播放时由 SMTC 按播放状态推进位置；这里仅捕获跳转、切歌等非线性变化。
    if (AbsDiff(state.position_ms, expected_position_ms) > kTimelinePositionDriftToleranceMs) {
        return true;
    }

    return elapsed_ms >= kTimelineUpdateIntervalMs;
}

void RememberTimelinePublish(const PlaybackState& state, int64_t now_ms, SmtcPublishCache& cache) {
    cache.has_timeline = true;
    cache.timeline_playing = state.playing;
    cache.timeline_position_ms = state.position_ms;
    cache.timeline_duration_ms = state.duration_ms;
    cache.timeline_publish_ms = now_ms;
    cache.timeline_speed = state.speed;
    cache.timeline_track_key = BuildTimelineTrackKey(state);
}

void InvokeCallback(std::function<void()> ControlCallbacks::*callback) {
    std::function<void()> action;
    {
        std::lock_guard lock(g_callbacks_mutex);
        action = (g_callbacks.*callback);
    }
    if (action) {
        action();
    }
}

void InvokeSeekCallback(int64_t position_ms) {
    std::function<void(int64_t)> action;
    {
        std::lock_guard lock(g_callbacks_mutex);
        action = g_callbacks.seek_to_ms;
    }
    if (action) {
        action(position_ms);
    }
}

bool IsUsableMainWindow(HWND window, DWORD process_id) {
    DWORD window_process_id = 0;
    GetWindowThreadProcessId(window, &window_process_id);
    if (window_process_id != process_id) {
        return false;
    }
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) {
        return false;
    }
    const LONG_PTR ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((ex_style & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect)) {
        return false;
    }
    return rect.right > rect.left && rect.bottom > rect.top;
}

BOOL CALLBACK EnumMainWindowProc(HWND window, LPARAM param) {
    auto* candidate = reinterpret_cast<WindowCandidate*>(param);
    const DWORD process_id = GetCurrentProcessId();
    if (!IsUsableMainWindow(window, process_id)) {
        return TRUE;
    }

    RECT rect{};
    GetWindowRect(window, &rect);
    const LONG64 area = static_cast<LONG64>(rect.right - rect.left) * static_cast<LONG64>(rect.bottom - rect.top);
    if (area > candidate->area) {
        candidate->window = window;
        candidate->area = area;
    }
    return TRUE;
}

HWND FindMainWindow() {
    WindowCandidate candidate;
    EnumWindows(EnumMainWindowProc, reinterpret_cast<LPARAM>(&candidate));
    return candidate.window;
}

void ResetSmtcLocked() {
    g_smtc = nullptr;
    g_smtc_window = nullptr;
    g_smtc_publish = {};
}

void UpdateSmtc() {
    std::lock_guard smtc_lock(g_smtc_mutex);
    if (!g_smtc) {
        return;
    }

    const PlaybackState state = CurrentStateSnapshot();
    const int64_t now_ms = NowMs();

    bool update_status = false;
    bool update_display = false;
    bool update_timeline = false;
    update_status = !g_smtc_publish.has_status || g_smtc_publish.status_playing != state.playing;
    if (update_status) {
        g_smtc_publish.has_status = true;
        g_smtc_publish.status_playing = state.playing;
    }

    const std::string display_key = BuildDisplayKey(state);
    update_display = !g_smtc_publish.has_display || display_key != g_smtc_publish.display_key;
    if (update_display) {
        g_smtc_publish.has_display = true;
        g_smtc_publish.display_key = display_key;
    }

    update_timeline = ShouldUpdateTimeline(state, now_ms, g_smtc_publish);
    if (update_timeline) {
        RememberTimelinePublish(state, now_ms, g_smtc_publish);
    }

    try {
        if (update_status) {
            g_smtc.PlaybackStatus(state.playing ? media::MediaPlaybackStatus::Playing : media::MediaPlaybackStatus::Paused);
        }

        if (update_display) {
            auto updater = g_smtc.DisplayUpdater();
            updater.Type(media::MediaPlaybackType::Music);
            updater.MusicProperties().Title(winrt::hstring(Utf8ToWide(state.title.empty() ? state.filename : state.title)));
            updater.MusicProperties().Artist(winrt::hstring(Utf8ToWide(state.artist)));
            updater.MusicProperties().AlbumTitle(winrt::hstring(Utf8ToWide(state.album)));
            updater.Thumbnail(CoverThumbnailReference(state.album_pic));
            updater.Update();
        }

        if (update_timeline) {
            media::SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(TimeSpanFromMs(0));
            timeline.MinSeekTime(TimeSpanFromMs(0));
            timeline.Position(TimeSpanFromMs(state.position_ms));
            timeline.EndTime(TimeSpanFromMs(std::max(state.duration_ms, state.position_ms)));
            timeline.MaxSeekTime(TimeSpanFromMs(std::max(state.duration_ms, state.position_ms)));
            g_smtc.UpdateTimelineProperties(timeline);
        }
    } catch (...) {
        ResetSmtcLocked();
    }
}

void InitializeSmtcForWindow(HWND window) {
    auto interop = winrt::get_activation_factory<media::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
    media::SystemMediaTransportControls smtc{nullptr};
    winrt::check_hresult(interop->GetForWindow(
        window,
        winrt::guid_of<media::SystemMediaTransportControls>(),
        winrt::put_abi(smtc)));

    smtc.IsEnabled(true);
    smtc.IsPlayEnabled(true);
    smtc.IsPauseEnabled(true);
    smtc.IsNextEnabled(true);
    smtc.IsPreviousEnabled(true);
    smtc.IsStopEnabled(false);

    smtc.ButtonPressed([](media::SystemMediaTransportControls const&, media::SystemMediaTransportControlsButtonPressedEventArgs const& args) {
        switch (args.Button()) {
        case media::SystemMediaTransportControlsButton::Play:
            InvokeCallback(&ControlCallbacks::play);
            break;
        case media::SystemMediaTransportControlsButton::Pause:
            InvokeCallback(&ControlCallbacks::pause);
            break;
        case media::SystemMediaTransportControlsButton::Next:
            InvokeCallback(&ControlCallbacks::next);
            break;
        case media::SystemMediaTransportControlsButton::Previous:
            InvokeCallback(&ControlCallbacks::previous);
            break;
        default:
            break;
        }
    });

    smtc.PlaybackPositionChangeRequested([](media::SystemMediaTransportControls const&, media::PlaybackPositionChangeRequestedEventArgs const& args) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(args.RequestedPlaybackPosition()).count();
        InvokeSeekCallback(ms);
    });

    {
        std::lock_guard lock(g_smtc_mutex);
        g_smtc = smtc;
        g_smtc_window = window;
        g_smtc_publish = {};
    }
    UpdateSmtc();
}

void MergeMetadata(const PlaybackState& metadata) {
    bool changed = false;
    {
        std::lock_guard lock(g_state_mutex);
        if (!metadata.title.empty() && metadata.title != g_state.title) {
            changed = true;
        }
        if (!metadata.artist.empty() && metadata.artist != g_state.artist) {
            changed = true;
        }
        if (!metadata.album.empty() && metadata.album != g_state.album) {
            changed = true;
        }
        if (!metadata.album_pic.empty() && metadata.album_pic != g_state.album_pic) {
            changed = true;
        }
        if (metadata.duration_ms > 0 && metadata.duration_ms != g_state.duration_ms) {
            changed = true;
        }
        if (!metadata.title.empty()) {
            g_state.title = metadata.title;
        }
        if (!metadata.artist.empty()) {
            g_state.artist = metadata.artist;
        }
        if (!metadata.album.empty()) {
            g_state.album = metadata.album;
        }
        if (!metadata.album_pic.empty()) {
            g_state.album_pic = metadata.album_pic;
        }
        if (metadata.duration_ms > 0) {
            g_state.duration_ms = metadata.duration_ms;
        }
    }
    if (changed) {
        UpdateSmtc();
    }
}

void EnsureSmtcBoundToMainWindow() {
    HWND current_window = nullptr;
    {
        std::lock_guard lock(g_smtc_mutex);
        current_window = g_smtc_window;
        if (current_window != nullptr && (!IsWindow(current_window) || !IsWindowVisible(current_window))) {
            ResetSmtcLocked();
            current_window = nullptr;
        }
    }

    if (current_window != nullptr) {
        return;
    }

    HWND main_window = FindMainWindow();
    if (main_window == nullptr) {
        return;
    }

    try {
        InitializeSmtcForWindow(main_window);
    } catch (...) {
        std::lock_guard lock(g_smtc_mutex);
        ResetSmtcLocked();
    }
}

void BridgeWorker() {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        int64_t last_metadata_ms = 0;
        while (g_running.load()) {
            EnsureSmtcBoundToMainWindow();

            const int64_t now_ms = NowMs();
            if (now_ms - last_metadata_ms >= std::chrono::duration_cast<std::chrono::milliseconds>(2s).count()) {
                last_metadata_ms = now_ms;
                if (auto metadata = ParseMetadataFromLogs()) {
                    MergeMetadata(*metadata);
                }
            }

            std::this_thread::sleep_for(500ms);
        }
    } catch (...) {
        g_running.store(false);
    }

    {
        std::lock_guard lock(g_smtc_mutex);
        ResetSmtcLocked();
    }
}

} // namespace

void StartSmtcBridge(ControlCallbacks callbacks) {
    {
        std::lock_guard lock(g_callbacks_mutex);
        g_callbacks = std::move(callbacks);
    }

    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        return;
    }

    g_running.store(true);
    g_bridge_thread = std::thread(BridgeWorker);
    g_bridge_thread.detach();
}

void StopSmtcBridge() {
    g_running.store(false);
}

void SubmitPlaybackState(const PlaybackState& state) {
    {
        std::lock_guard lock(g_state_mutex);
        const bool track_changed =
            (!state.path.empty() && state.path != g_state.path) ||
            (!state.filename.empty() && state.filename != g_state.filename);
        g_state.connected = state.connected;
        g_state.playing = state.playing;
        g_state.position_ms = state.position_ms;
        if (state.duration_ms > 0) {
            g_state.duration_ms = state.duration_ms;
        }
        g_state.speed = state.speed;
        g_state.timestamp_ms = state.timestamp_ms;
        g_state.path = state.path;
        g_state.filename = state.filename;
        if (track_changed) {
            g_state.artist.clear();
            g_state.album.clear();
            g_state.album_pic.clear();
            g_state.title = state.title;
        } else if (!state.title.empty() && g_state.title.empty()) {
            g_state.title = state.title;
        }
    }
    UpdateSmtc();
}

void MarkPlaybackDisconnected() {
    {
        std::lock_guard lock(g_state_mutex);
        g_state.connected = false;
        g_state.playing = false;
    }
    UpdateSmtc();
}

} // namespace bodian_bridge
