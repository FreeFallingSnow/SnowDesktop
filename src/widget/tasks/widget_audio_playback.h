#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct AudioPlaybackState
{
    std::string status = "loading";
    std::string error;
    std::int64_t positionMs = 0;
    std::int64_t durationMs = 0;
    double volume = 1;
    bool loop = false;
};

// Backend instances are created, accessed and destroyed only on the audio worker.
class WidgetAudioPlayer
{
public:
    virtual ~WidgetAudioPlayer() = default;
    virtual void Play() = 0;
    virtual void Pause() = 0;
    virtual void Seek(std::int64_t positionMs) = 0;
    virtual void SetVolume(double volume) = 0;
    virtual void SetLoop(bool loop) = 0;
    virtual AudioPlaybackState State() = 0;
};

struct AudioPlaybackRequest
{
    std::uint64_t taskId = 0;
    std::uint64_t owner = 0;
    std::string action;
    std::string playbackId;
    std::filesystem::path path;
    // Nonempty only for user-selected files, for revocation tracking.
    std::string sourceHandle;
    std::int64_t positionMs = 0;
    double volume = 1;
    bool loop = false;
};

struct AudioPlaybackMessage
{
    std::uint64_t owner = 0;
    std::uint64_t taskId = 0; // nonzero means task completion, otherwise state event
    std::string playbackId;
    bool ok = true;
    AudioPlaybackState state;
};

class WidgetAudioPlayback
{
public:
    using Factory = std::function<std::unique_ptr<WidgetAudioPlayer>(const std::filesystem::path&)>;
    static constexpr std::size_t MaximumPerOwner = 8;
    static constexpr std::size_t MaximumPlayers = 32;
    explicit WidgetAudioPlayback(Factory factory = {});
    ~WidgetAudioPlayback();
    WidgetAudioPlayback(const WidgetAudioPlayback&) = delete;
    WidgetAudioPlayback& operator=(const WidgetAudioPlayback&) = delete;
    static bool SupportsAction(std::string_view action) noexcept;
    static bool Validate(const AudioPlaybackRequest& request) noexcept;
    static bool SupportedFile(const std::filesystem::path& path);
    bool Submit(AudioPlaybackRequest request);
    void Cancel(std::uint64_t taskId);
    void ForgetOwner(std::uint64_t owner);
    void RevokeFile(std::uint64_t owner, std::string handle);
    void SetWakeCallback(std::function<void()> callback);
    std::vector<AudioPlaybackMessage> Drain();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
