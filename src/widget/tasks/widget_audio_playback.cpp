#include "widget_audio_playback.h"

#include <windows.h>
#include <shcore.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace snowdesktop::widget_runtime
{
namespace
{
class WindowsAudioPlayer final : public WidgetAudioPlayer
{
    struct Terminal { std::atomic<int> value{0}; };
    std::vector<HANDLE> pinned_;
    winrt::Windows::Media::Playback::MediaPlayer player_{nullptr};
    winrt::Windows::Media::Core::MediaSource source_{nullptr};
    winrt::Windows::Storage::Streams::IRandomAccessStream stream_{nullptr};
    std::shared_ptr<Terminal> terminal_ = std::make_shared<Terminal>();
public:
    explicit WindowsAudioPlayer(const std::filesystem::path& path)
    {
        try
        {
            if (!path.is_absolute() || !WidgetAudioPlayback::SupportedFile(path))
                throw std::runtime_error("unsupportedFormat");
            // Pin ancestors and leaf for the lifetime of playback. Never let a
            // junction/path replacement redirect a previously authorized source.
            auto current = path.root_path();
            for (const auto& part : path.relative_path())
            {
                current /= part;
                const bool leaf = current == path;
                HANDLE file = CreateFileW(current.c_str(), leaf ? GENERIC_READ : FILE_READ_ATTRIBUTES,
                    leaf ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("audioOpenFailed");
                pinned_.push_back(file);
                BY_HANDLE_FILE_INFORMATION info{};
                if (!GetFileInformationByHandle(file, &info)) throw std::runtime_error("audioOpenFailed");
                if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    throw std::runtime_error("reparsePointDenied");
                if (leaf && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    throw std::runtime_error("notFile");
            }
            // This parameter is a WinRT FileAccessMode, not a CreateFile access
            // mask. A nonzero value requests write access and conflicts with the
            // read-only pinned leaf above.
            winrt::check_hresult(CreateRandomAccessStreamOnFile(path.c_str(),
                static_cast<DWORD>(winrt::Windows::Storage::FileAccessMode::Read),
                winrt::guid_of<winrt::Windows::Storage::Streams::IRandomAccessStream>(), winrt::put_abi(stream_)));
            auto extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            source_ = winrt::Windows::Media::Core::MediaSource::CreateFromStream(stream_,
                extension == L".mp3" ? L"audio/mpeg" : L"audio/wav");
            player_ = winrt::Windows::Media::Playback::MediaPlayer();
            player_.CommandManager().IsEnabled(false);
            player_.AutoPlay(false);
            player_.MediaEnded([terminal = terminal_](const auto&, const auto&) { terminal->value = 1; });
            player_.MediaFailed([terminal = terminal_](const auto&, const auto&) { terminal->value = 2; });
            player_.Source(source_);
        }
        catch (...) { Close(); throw; }
    }
    ~WindowsAudioPlayer() override { Close(); }
    void Close() noexcept
    {
        try { if (player_) player_.Close(); } catch (...) {}
        try { if (source_) source_.Close(); } catch (...) {}
        try { if (stream_) stream_.Close(); } catch (...) {}
        for (HANDLE file : pinned_) CloseHandle(file);
        pinned_.clear();
    }
    void Play() override
    {
        if (terminal_->value == 2) throw std::runtime_error("audioDecodeFailed");
        player_.Play();
    }
    void Pause() override { player_.Pause(); }
    void Seek(std::int64_t ms) override
    {
        const auto session = player_.PlaybackSession();
        const auto duration = session.NaturalDuration().count() / 10000;
        if (!session.CanSeek() || ms > duration) throw std::runtime_error("seekOutOfRange");
        session.Position(winrt::Windows::Foundation::TimeSpan(ms * 10000));
    }
    void SetVolume(double value) override { player_.Volume(value); }
    void SetLoop(bool value) override { player_.IsLoopingEnabled(value); }
    AudioPlaybackState State() override
    {
        AudioPlaybackState value;
        const auto session = player_.PlaybackSession();
        using winrt::Windows::Media::Playback::MediaPlaybackState;
        const auto state = session.PlaybackState();
        value.status = terminal_->value == 1 ? "ended" : terminal_->value == 2 ? "error" :
            state == MediaPlaybackState::Playing ? "playing" :
            state == MediaPlaybackState::Paused ? "paused" : "loading";
        if (terminal_->value == 2) value.error = "audioDecodeFailed";
        value.positionMs = (std::max)(std::int64_t{0}, session.Position().count() / 10000);
        value.durationMs = (std::max)(std::int64_t{0}, session.NaturalDuration().count() / 10000);
        value.volume = player_.Volume();
        value.loop = player_.IsLoopingEnabled();
        return value;
    }
};
}

struct WidgetAudioPlayback::Impl
{
    struct Entry
    {
        AudioPlaybackRequest request;
        std::unique_ptr<WidgetAudioPlayer> player;
        AudioPlaybackState previous;
    };
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<AudioPlaybackRequest> pending;
    std::vector<AudioPlaybackMessage> messages;
    std::unordered_set<std::uint64_t> retired, canceled;
    std::vector<std::pair<std::uint64_t, std::string>> revoked;
    std::function<void()> wake;
    Factory factory;
    bool stopping = false;
    bool ownersChanged = false;
    std::thread worker;
    explicit Impl(Factory create) : factory(std::move(create)), worker([this] { Run(); }) {}
    ~Impl()
    {
        { std::lock_guard lock(mutex); stopping = true; }
        condition.notify_one();
        worker.join();
    }
    void Publish(AudioPlaybackMessage message)
    {
        std::function<void()> callback;
        {
            std::lock_guard lock(mutex);
            if (retired.contains(message.owner)) return;
            // Coalesce progress without losing terminal events or task results.
            if (!message.taskId && message.state.status != "ended" && message.state.status != "error" &&
                message.state.status != "stopped")
                std::erase_if(messages, [&](const auto& old) {
                    return !old.taskId && old.playbackId == message.playbackId &&
                        old.state.status != "ended" && old.state.status != "error" && old.state.status != "stopped";
                });
            messages.push_back(std::move(message));
            callback = wake;
        }
        if (callback) callback();
    }
    void Run()
    {
        bool initialized = false;
        try { winrt::init_apartment(winrt::apartment_type::multi_threaded); initialized = true; } catch (...) {}
        std::unordered_map<std::string, Entry> players;
        for (;;)
        {
            std::deque<AudioPlaybackRequest> work;
            std::unordered_set<std::uint64_t> dead, cancel;
            std::vector<std::pair<std::uint64_t, std::string>> files;
            {
                std::unique_lock lock(mutex);
                if (pending.empty() && !ownersChanged && canceled.empty() && revoked.empty())
                {
                    if (players.empty()) condition.wait(lock, [&] { return stopping || !pending.empty() || ownersChanged || !canceled.empty() || !revoked.empty(); });
                    else condition.wait_for(lock, std::chrono::milliseconds(250));
                }
                if (stopping) break;
                work.swap(pending);
                dead = retired;
                ownersChanged = false;
                cancel.swap(canceled);
                files.swap(revoked);
            }
            std::erase_if(players, [&](const auto& pair) {
                const auto& request = pair.second.request;
                const bool remove = dead.contains(request.owner) || cancel.contains(request.taskId) ||
                    std::find(files.begin(), files.end(), std::pair(request.owner, request.sourceHandle)) != files.end();
                if (remove && !dead.contains(request.owner))
                {
                    auto state = pair.second.previous; state.status = "stopped";
                    Publish({request.owner, 0, pair.first, true, state});
                }
                return remove;
            });
            for (auto& request : work)
            {
                if (dead.contains(request.owner)) continue;
                AudioPlaybackMessage message{request.owner, request.taskId, request.playbackId};
                try
                {
                    if (cancel.contains(request.taskId)) throw std::runtime_error("canceled");
                    if (!request.sourceHandle.empty() && std::find(files.begin(), files.end(),
                        std::pair(request.owner, request.sourceHandle)) != files.end()) throw std::runtime_error("permissionRevoked");
                    if (request.action == "audio.play")
                    {
                        if (players.size() >= MaximumPlayers || static_cast<std::size_t>(std::count_if(players.begin(), players.end(),
                            [&](const auto& pair) { return pair.second.request.owner == request.owner; })) >= MaximumPerOwner)
                            throw std::runtime_error("quotaExceeded");
                        auto player = factory ? factory(request.path) : std::make_unique<WindowsAudioPlayer>(request.path);
                        player->SetVolume(request.volume); player->SetLoop(request.loop); player->Play();
                        message.playbackId = "audio-" + std::to_string(request.taskId);
                        players.emplace(message.playbackId, Entry{request, std::move(player), {}});
                    }
                    else
                    {
                        auto item = players.find(request.playbackId);
                        if (item == players.end() || item->second.request.owner != request.owner)
                            throw std::runtime_error("invalidPlayback");
                        auto& player = item->second.player;
                        if (request.action == "audio.stop")
                        {
                            auto state = item->second.previous; state.status = "stopped";
                            Publish({request.owner, 0, request.playbackId, true, state});
                            players.erase(item);
                        }
                        else if (request.action == "audio.pause") player->Pause();
                        else if (request.action == "audio.resume") player->Play();
                        else if (request.action == "audio.seek") player->Seek(request.positionMs);
                        else if (request.action == "audio.setVolume") player->SetVolume(request.volume);
                        else if (request.action == "audio.setLoop") player->SetLoop(request.loop);
                    }
                }
                catch (const std::runtime_error& error) { message.ok = false; message.state.error = error.what(); }
                catch (...) { message.ok = false; message.state.error = "audioPlaybackFailed"; }
                Publish(std::move(message));
            }
            for (auto item = players.begin(); item != players.end();)
            {
                auto& entry = item->second;
                AudioPlaybackState state;
                try { state = entry.player->State(); }
                catch (...) { state.status = "error"; state.error = "audioPlaybackFailed"; }
                const bool terminal = state.status == "ended" || state.status == "error";
                if (terminal || state.status != entry.previous.status || state.positionMs != entry.previous.positionMs ||
                    state.durationMs != entry.previous.durationMs || state.volume != entry.previous.volume || state.loop != entry.previous.loop)
                    Publish({entry.request.owner, 0, item->first, !terminal || state.error.empty(), state});
                entry.previous = state;
                if (terminal) item = players.erase(item); else ++item;
            }
            // Retired owners never submit again; retain their IDs to reject races.
        }
        players.clear();
        if (initialized) winrt::uninit_apartment();
    }
};

WidgetAudioPlayback::WidgetAudioPlayback(Factory factory) : impl_(std::make_unique<Impl>(std::move(factory))) {}
WidgetAudioPlayback::~WidgetAudioPlayback() = default;
bool WidgetAudioPlayback::SupportsAction(std::string_view name) noexcept
{
    return name == "audio.play" || name == "audio.pause" || name == "audio.resume" || name == "audio.stop" ||
        name == "audio.seek" || name == "audio.setVolume" || name == "audio.setLoop";
}
bool WidgetAudioPlayback::SupportedFile(const std::filesystem::path& path)
{
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".mp3" || extension == L".wav";
}
bool WidgetAudioPlayback::Validate(const AudioPlaybackRequest& request) noexcept
{
    return request.taskId && request.owner && SupportsAction(request.action) &&
        std::isfinite(request.volume) && request.volume >= 0 && request.volume <= 1 &&
        request.positionMs >= 0 && request.positionMs <= INT64_MAX / 10000 &&
        (request.action == "audio.play" ? !request.path.empty() :
            request.playbackId.starts_with("audio-") && request.playbackId.size() <= 64);
}
bool WidgetAudioPlayback::Submit(AudioPlaybackRequest request)
{
    if (!Validate(request)) return false;
    { std::lock_guard lock(impl_->mutex);
      if (impl_->stopping || impl_->retired.contains(request.owner) || impl_->pending.size() >= 128) return false;
      impl_->pending.push_back(std::move(request)); }
    impl_->condition.notify_one(); return true;
}
void WidgetAudioPlayback::Cancel(std::uint64_t id)
{
    { std::lock_guard lock(impl_->mutex); impl_->canceled.insert(id); }
    impl_->condition.notify_one();
}
void WidgetAudioPlayback::ForgetOwner(std::uint64_t owner)
{
    { std::lock_guard lock(impl_->mutex); impl_->retired.insert(owner); impl_->ownersChanged = true;
      std::erase_if(impl_->messages, [owner](const auto& item) { return item.owner == owner; }); }
    impl_->condition.notify_one();
}
void WidgetAudioPlayback::RevokeFile(std::uint64_t owner, std::string handle)
{
    { std::lock_guard lock(impl_->mutex); impl_->revoked.emplace_back(owner, std::move(handle)); }
    impl_->condition.notify_one();
}
void WidgetAudioPlayback::SetWakeCallback(std::function<void()> callback)
{
    std::lock_guard lock(impl_->mutex); impl_->wake = std::move(callback);
}
std::vector<AudioPlaybackMessage> WidgetAudioPlayback::Drain()
{
    std::lock_guard lock(impl_->mutex);
    std::vector<AudioPlaybackMessage> result; result.swap(impl_->messages); return result;
}
}
