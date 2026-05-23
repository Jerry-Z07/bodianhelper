#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bodian_bridge {

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

struct ControlCallbacks {
    std::function<void()> play;
    std::function<void()> pause;
    std::function<void()> next;
    std::function<void()> previous;
    std::function<void(int64_t position_ms)> seek_to_ms;
};

void StartSmtcBridge(ControlCallbacks callbacks);
void StopSmtcBridge();
void SubmitPlaybackState(const PlaybackState& state);
void MarkPlaybackDisconnected();

} // namespace bodian_bridge
