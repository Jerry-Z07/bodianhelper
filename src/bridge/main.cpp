#include <windows.h>
#include <SystemMediaTransportControlsInterop.h>
#include <urlmon.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace media = winrt::Windows::Media;
namespace foundation = winrt::Windows::Foundation;
namespace storage = winrt::Windows::Storage;
namespace streams = winrt::Windows::Storage::Streams;

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\bodian-smtc-bridge";
constexpr wchar_t kWindowClassName[] = L"BodianSmtcBridgeHiddenWindow";
constexpr wchar_t kCacheDirName[] = L"BodianSmtcBridge";
constexpr wchar_t kCoverCacheDirName[] = L"album-art";
constexpr int kCoverCacheMaxFiles = 50;
constexpr auto kCoverCacheMaxAge = std::chrono::hours(24 * 3);
constexpr int64_t kTimelineUpdateIntervalMs = 5000;
constexpr int64_t kTimelineDurationToleranceMs = 1000;
constexpr int64_t kTimelinePositionDriftToleranceMs = 3000;

struct PlaybackState {
    bool connected = false;
    bool playing = false;
    int64_t position_ms = 0;
    int64_t duration_ms = 0;
    double speed = 1.0;
    int64_t timestamp_ms = 0;
    std::string title;
    std::string artist;
    std::string album;
    std::string album_pic;
    std::string path;
    std::string filename;
};

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

std::mutex g_state_mutex;
PlaybackState g_state;

std::mutex g_pipe_mutex;
HANDLE g_pipe = INVALID_HANDLE_VALUE;

std::atomic<bool> g_running{true};
media::SystemMediaTransportControls g_smtc{nullptr};
std::mutex g_smtc_publish_mutex;
SmtcPublishCache g_smtc_publish;

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
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

std::wstring CoverExtension(std::string_view url) {
    const size_t query_pos = url.find_first_of("?#");
    const size_t path_end = query_pos == std::string_view::npos ? url.size() : query_pos;
    const std::string_view path(url.data(), path_end);
    const size_t dot = path.rfind('.');
    if (dot != std::string_view::npos) {
        const std::string_view extension = path.substr(dot);
        if (EndsWithAsciiIgnoreCase(extension, ".png")) {
            return L".png";
        }
        if (EndsWithAsciiIgnoreCase(extension, ".jpeg")) {
            return L".jpg";
        }
    }
    return L".jpg";
}

uint64_t Fnv1a64(std::string_view value) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring Hex64(uint64_t value) {
    std::wostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return stream.str();
}

std::optional<fs::path> LocalAppDataPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0 || size >= std::size(buffer)) {
        return std::nullopt;
    }
    return fs::path(buffer);
}

void PruneCoverCache(const fs::path& cache_dir) {
    struct CacheFile {
        fs::path path;
        fs::file_time_type write_time;
    };

    std::vector<CacheFile> files;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }

        const auto path = entry.path();
        if (path.extension() != L".jpg" && path.extension() != L".png") {
            continue;
        }

        const auto write_time = entry.last_write_time(ec);
        if (ec) {
            continue;
        }

        // 缓存仅用于降低 SMTC 封面更新失败率，过旧文件可以直接淘汰。
        if (now - write_time > kCoverCacheMaxAge) {
            fs::remove(path, ec);
            continue;
        }
        files.push_back({path, write_time});
    }

    if (files.size() <= kCoverCacheMaxFiles) {
        return;
    }

    std::sort(files.begin(), files.end(), [](const CacheFile& left, const CacheFile& right) {
        return left.write_time > right.write_time;
    });
    for (size_t index = kCoverCacheMaxFiles; index < files.size(); ++index) {
        fs::remove(files[index].path, ec);
    }
}

std::optional<fs::path> CachedCoverPath(std::string_view album_pic) {
    if (!StartsWithAsciiIgnoreCase(album_pic, "http://") && !StartsWithAsciiIgnoreCase(album_pic, "https://")) {
        return std::nullopt;
    }

    const std::string cover_url = PreferredCoverUrl(album_pic);
    auto local_app_data = LocalAppDataPath();
    if (!local_app_data) {
        return std::nullopt;
    }

    std::error_code ec;
    const fs::path cache_dir = *local_app_data / kCacheDirName / kCoverCacheDirName;
    fs::create_directories(cache_dir, ec);
    if (ec) {
        return std::nullopt;
    }
    static std::atomic<int64_t> last_prune_ms{0};
    const int64_t now_ms = NowMs();
    int64_t last_prune = last_prune_ms.load();
    if (now_ms - last_prune > std::chrono::duration_cast<std::chrono::milliseconds>(1h).count() &&
        last_prune_ms.compare_exchange_strong(last_prune, now_ms)) {
        PruneCoverCache(cache_dir);
    }

    const fs::path cover_path = cache_dir / (Hex64(Fnv1a64(cover_url)) + CoverExtension(cover_url));
    if (fs::exists(cover_path, ec) && fs::file_size(cover_path, ec) > 0) {
        fs::last_write_time(cover_path, fs::file_time_type::clock::now(), ec);
        return cover_path;
    }

    const fs::path temp_path = cover_path.wstring() + L".tmp";
    DeleteFileW(temp_path.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, Utf8ToWide(cover_url).c_str(), temp_path.c_str(), 0, nullptr);
    if (FAILED(hr) || !fs::exists(temp_path, ec) || fs::file_size(temp_path, ec) == 0) {
        DeleteFileW(temp_path.c_str());
        return std::nullopt;
    }

    MoveFileExW(temp_path.c_str(), cover_path.c_str(), MOVEFILE_REPLACE_EXISTING);
    DeleteFileW(temp_path.c_str());
    if (fs::exists(cover_path, ec) && fs::file_size(cover_path, ec) > 0) {
        return cover_path;
    }
    return std::nullopt;
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
            if (ch >= 0x20) {
                escaped += static_cast<char>(ch);
            }
            break;
        }
    }
    return escaped;
}

std::string JsonString(std::string_view json, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\":\"";
    size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += pattern.size();

    std::string value;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (escaped) {
            switch (ch) {
            case 'n':
                value += '\n';
                break;
            case 'r':
                value += '\r';
                break;
            case 't':
                value += '\t';
                break;
            default:
                value += ch;
                break;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            break;
        } else {
            value += ch;
        }
    }
    return value;
}

bool JsonBool(std::string_view json, std::string_view key, bool fallback = false) {
    const std::string pattern = "\"" + std::string(key) + "\":";
    const size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return fallback;
    }
    const size_t value_pos = pos + pattern.size();
    if (json.substr(value_pos, 4) == "true") {
        return true;
    }
    if (json.substr(value_pos, 5) == "false") {
        return false;
    }
    return fallback;
}

int64_t JsonInt64(std::string_view json, std::string_view key, int64_t fallback = 0) {
    const std::string pattern = "\"" + std::string(key) + "\":";
    const size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return fallback;
    }
    const char* start = json.data() + pos + pattern.size();
    char* end = nullptr;
    const long long value = std::strtoll(start, &end, 10);
    return end == start ? fallback : value;
}

double JsonDouble(std::string_view json, std::string_view key, double fallback = 0.0) {
    const std::string pattern = "\"" + std::string(key) + "\":";
    const size_t pos = json.find(pattern);
    if (pos == std::string_view::npos) {
        return fallback;
    }
    const char* start = json.data() + pos + pattern.size();
    char* end = nullptr;
    const double value = std::strtod(start, &end);
    return end == start ? fallback : value;
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

void SendProxyCommand(std::string command, int64_t position_ms = -1) {
    std::ostringstream json;
    json << "{\"command\":\"" << JsonEscape(command) << "\"";
    if (position_ms >= 0) {
        json << ",\"positionMs\":" << position_ms;
    }
    json << "}\n";
    const std::string message = json.str();

    std::lock_guard lock(g_pipe_mutex);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(g_pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
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

void UpdateSmtc() {
    if (!g_smtc) {
        return;
    }

    const PlaybackState state = CurrentStateSnapshot();
    const int64_t now_ms = NowMs();

    bool update_status = false;
    bool update_display = false;
    bool update_timeline = false;
    {
        std::lock_guard lock(g_smtc_publish_mutex);
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
    }

    if (update_status) {
        g_smtc.PlaybackStatus(state.playing ? media::MediaPlaybackStatus::Playing : media::MediaPlaybackStatus::Paused);
    }

    if (update_display) {
        auto updater = g_smtc.DisplayUpdater();
        updater.Type(media::MediaPlaybackType::Music);
        updater.MusicProperties().Title(winrt::hstring(Utf8ToWide(state.title.empty() ? state.filename : state.title)));
        updater.MusicProperties().Artist(winrt::hstring(Utf8ToWide(state.artist)));
        updater.MusicProperties().AlbumTitle(winrt::hstring(Utf8ToWide(state.album)));
        if (auto cover_path = CachedCoverPath(state.album_pic)) {
            try {
                auto cover_file = storage::StorageFile::GetFileFromPathAsync(winrt::hstring(cover_path->wstring())).get();
                updater.Thumbnail(streams::RandomAccessStreamReference::CreateFromFile(cover_file));
            } catch (...) {
                updater.Thumbnail(nullptr);
            }
        } else {
            updater.Thumbnail(nullptr);
        }
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
}

void ApplyStateLine(std::string_view line) {
    if (line.find("\"type\":\"state\"") == std::string_view::npos) {
        return;
    }

    {
        std::lock_guard lock(g_state_mutex);
        g_state.connected = true;
        g_state.playing = JsonBool(line, "playing", g_state.playing);
        g_state.position_ms = JsonInt64(line, "positionMs", g_state.position_ms);
        const int64_t duration_ms = JsonInt64(line, "durationMs", g_state.duration_ms);
        if (duration_ms > 0) {
            g_state.duration_ms = duration_ms;
        }
        g_state.speed = JsonDouble(line, "speed", g_state.speed);
        g_state.timestamp_ms = JsonInt64(line, "timestampMs", NowMs());
        g_state.path = JsonString(line, "path");
        g_state.filename = JsonString(line, "filename");
        const std::string title = JsonString(line, "title");
        if (!title.empty() && g_state.title.empty()) {
            g_state.title = title;
        }
    }
    UpdateSmtc();
}

void PipeServerWorker() {
    while (g_running.load()) {
        HANDLE pipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            64 * 1024,
            64 * 1024,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(1s);
            continue;
        }

        const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }

        {
            std::lock_guard lock(g_pipe_mutex);
            g_pipe = pipe;
        }

        std::cout << "代理已连接" << std::endl;
        std::string buffer;
        char chunk[4096]{};
        while (g_running.load()) {
            DWORD read = 0;
            if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
                break;
            }
            buffer.append(chunk, read);
            size_t newline = std::string::npos;
            while ((newline = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, newline);
                buffer.erase(0, newline + 1);
                ApplyStateLine(line);
            }
        }

        {
            std::lock_guard lock(g_pipe_mutex);
            if (g_pipe == pipe) {
                g_pipe = INVALID_HANDLE_VALUE;
            }
        }
        CloseHandle(pipe);
        {
            std::lock_guard lock(g_state_mutex);
            g_state.connected = false;
        }
        UpdateSmtc();
        std::cout << "代理已断开" << std::endl;
    }
}

void MetadataWorker() {
    std::string last_title;
    while (g_running.load()) {
        if (auto metadata = ParseMetadataFromLogs()) {
            bool changed = false;
            {
                std::lock_guard lock(g_state_mutex);
                if (!metadata->title.empty() && metadata->title != g_state.title) {
                    changed = true;
                }
                if (!metadata->title.empty()) {
                    g_state.title = metadata->title;
                }
                if (!metadata->artist.empty()) {
                    g_state.artist = metadata->artist;
                }
                if (!metadata->album.empty()) {
                    g_state.album = metadata->album;
                }
                if (!metadata->album_pic.empty()) {
                    g_state.album_pic = metadata->album_pic;
                }
                if (metadata->duration_ms > 0) {
                    g_state.duration_ms = metadata->duration_ms;
                }
            }
            if (changed) {
                UpdateSmtc();
            }
        }
        std::this_thread::sleep_for(2s);
    }
}

LRESULT CALLBACK HiddenWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) {
        g_running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateHiddenWindow(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = HiddenWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    RegisterClassW(&wc);
    return CreateWindowExW(
        0,
        kWindowClassName,
        L"Bodian SMTC Bridge",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        300,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr);
}

void InitializeSmtc(HWND window) {
    auto interop = winrt::get_activation_factory<media::SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
    winrt::check_hresult(interop->GetForWindow(
        window,
        winrt::guid_of<media::SystemMediaTransportControls>(),
        winrt::put_abi(g_smtc)));

    g_smtc.IsEnabled(true);
    g_smtc.IsPlayEnabled(true);
    g_smtc.IsPauseEnabled(true);
    g_smtc.IsNextEnabled(true);
    g_smtc.IsPreviousEnabled(true);
    g_smtc.IsStopEnabled(false);

    g_smtc.ButtonPressed([](media::SystemMediaTransportControls const&, media::SystemMediaTransportControlsButtonPressedEventArgs const& args) {
        switch (args.Button()) {
        case media::SystemMediaTransportControlsButton::Play:
            SendProxyCommand("play");
            break;
        case media::SystemMediaTransportControlsButton::Pause:
            SendProxyCommand("pause");
            break;
        case media::SystemMediaTransportControlsButton::Next:
            SendProxyCommand("next");
            break;
        case media::SystemMediaTransportControlsButton::Previous:
            SendProxyCommand("previous");
            break;
        default:
            break;
        }
    });

    g_smtc.PlaybackPositionChangeRequested([](media::SystemMediaTransportControls const&, media::PlaybackPositionChangeRequestedEventArgs const& args) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(args.RequestedPlaybackPosition()).count();
        SendProxyCommand("seekToMs", ms);
    });

    UpdateSmtc();
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    HWND window = CreateHiddenWindow(instance);
    if (window == nullptr) {
        MessageBoxW(nullptr, L"创建隐藏窗口失败", L"Bodian SMTC Bridge", MB_ICONERROR);
        return 1;
    }

    try {
        InitializeSmtc(window);
    } catch (winrt::hresult_error const& error) {
        std::wcerr << L"初始化 SMTC 失败: " << error.message().c_str() << std::endl;
        return 2;
    }

    std::thread pipe_thread(PipeServerWorker);
    std::thread metadata_thread(MetadataWorker);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_running.store(false);
    {
        std::lock_guard lock(g_pipe_mutex);
        if (g_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(g_pipe);
            g_pipe = INVALID_HANDLE_VALUE;
        }
    }

    if (pipe_thread.joinable()) {
        pipe_thread.join();
    }
    if (metadata_thread.joinable()) {
        metadata_thread.join();
    }

    return 0;
}
