#include "widget_audio_playback.h"
#include "widget/runtime/widget_runtime_image.h"
#include "widget/packages/widget_audio_format.h"

#include <windows.h>
#include <shcore.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.FileProperties.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cwctype>
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
// Local property/thumbnail operations must not indefinitely block other voices
// or shutdown. Optional metadata failure never prevents audio playback.
template<class Operation> auto AwaitLocal(Operation operation)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started &&
        std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (operation.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
    {
        operation.Cancel();
        throw winrt::hresult_canceled();
    }
    return operation.GetResults();
}
std::string AudioTag(const winrt::hstring& value)
{
    std::wstring text(value.c_str(), (std::min)(std::size_t{1024}, static_cast<std::size_t>(value.size())));
    if (!text.empty() && text.back() >= 0xd800 && text.back() <= 0xdbff) text.pop_back();
    return winrt::to_string(text);
}

class WindowsAudioPlayer final : public WidgetAudioPlayer
{
    struct Terminal { std::atomic<int> value{0}; std::atomic<bool> looping{false}; };
    struct Commands
    {
        std::mutex mutex;
        std::vector<AudioPlaybackCommand> pending;
        void Push(AudioPlaybackCommand command)
        {
            std::lock_guard lock(mutex);
            if (pending.size() < 32) pending.push_back(std::move(command));
        }
    };
    std::vector<HANDLE> pinned_;
    std::filesystem::path path_;
    winrt::Windows::Media::Playback::MediaPlayer player_{nullptr};
    winrt::Windows::Media::Core::MediaSource source_{nullptr};
    winrt::Windows::Storage::Streams::IRandomAccessStream stream_{nullptr};
    std::shared_ptr<Terminal> terminal_ = std::make_shared<Terminal>();
    std::shared_ptr<Commands> commands_ = std::make_shared<Commands>();
    winrt::Windows::Storage::FileProperties::StorageItemThumbnail thumbnail_{nullptr};
    AudioPlaybackState metadata_;
    winrt::event_token nextToken_{}, previousToken_{}, shuffleToken_{}, repeatToken_{}, rateToken_{};
public:
    explicit WindowsAudioPlayer(const std::filesystem::path& path) : path_(path)
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
            const auto extension = path.extension().wstring();
            const auto* format = widget::FindWidgetAudioFormat(std::wstring_view(extension));
            if (!format) throw std::runtime_error("unsupportedFormat");
            source_ = winrt::Windows::Media::Core::MediaSource::CreateFromStream(stream_,
                winrt::hstring(format->contentType));
            player_ = winrt::Windows::Media::Playback::MediaPlayer();
            player_.CommandManager().IsEnabled(false);
            player_.AutoPlay(false);
            player_.MediaEnded([terminal = terminal_](const auto&, const auto&) {
                // Explicit playback items can report each loop boundary. Keep
                // the voice alive, and never overwrite a concurrent failure.
                if (!terminal->looping)
                {
                    int running = 0;
                    terminal->value.compare_exchange_strong(running, 1);
                }
            });
            player_.MediaFailed([terminal = terminal_](const auto&, const auto&) { terminal->value = 2; });
            // Bind once in Configure. Assigning a raw source first creates an
            // implicit playback item; wrapping that same source again for SMTC
            // would fail because the source is already in use.
        }
        catch (...) { Close(); throw; }
    }
    ~WindowsAudioPlayer() override { Close(); }
    void Close() noexcept
    {
        try
        {
            if (player_)
            {
                const auto commands = player_.CommandManager();
                commands.IsEnabled(false);
                if (nextToken_.value) commands.NextReceived(nextToken_);
                if (previousToken_.value) commands.PreviousReceived(previousToken_);
                if (shuffleToken_.value) commands.ShuffleReceived(shuffleToken_);
                if (repeatToken_.value) commands.AutoRepeatModeReceived(repeatToken_);
                if (rateToken_.value) commands.RateReceived(rateToken_);
            }
        }
        catch (...) {}
        try { if (player_) player_.Close(); } catch (...) {}
        try { if (source_) source_.Close(); } catch (...) {}
        try { if (stream_) stream_.Close(); } catch (...) {}
        try { if (thumbnail_) thumbnail_.Close(); } catch (...) {}
        for (HANDLE file : pinned_) CloseHandle(file);
        pinned_.clear();
    }
    void Configure(bool readMetadata, bool systemMediaControls) override
    {
        using namespace winrt::Windows;
        if (readMetadata || systemMediaControls)
        {
            metadata_.title = winrt::to_string(path_.stem().wstring());
            try
            {
                const auto file = AwaitLocal(Storage::StorageFile::GetFileFromPathAsync(path_.wstring()));
                const auto properties = AwaitLocal(file.Properties().GetMusicPropertiesAsync());
                if (!properties.Title().empty()) metadata_.title = AudioTag(properties.Title());
                metadata_.artist = AudioTag(properties.Artist());
                metadata_.album = AudioTag(properties.Album());
                thumbnail_ = AwaitLocal(file.GetThumbnailAsync(Storage::FileProperties::ThumbnailMode::MusicView, 512,
                    Storage::FileProperties::ThumbnailOptions::None));
                if (thumbnail_ && thumbnail_.Type() == Storage::FileProperties::ThumbnailType::Image &&
                    thumbnail_.Size() > 0 && thumbnail_.Size() <= 4 * 1024 * 1024)
                {
                    using namespace Graphics::Imaging;
                    const auto decoder = AwaitLocal(BitmapDecoder::CreateAsync(thumbnail_));
                    const auto sourceWidth = decoder.PixelWidth(), sourceHeight = decoder.PixelHeight();
                    if (sourceWidth && sourceHeight && sourceWidth <= 16384 && sourceHeight <= 16384)
                    {
                        const double scale = (std::min)(1.0, 512.0 / (std::max)(sourceWidth, sourceHeight));
                        BitmapTransform transform;
                        transform.ScaledWidth((std::max)(1u, static_cast<unsigned>(std::lround(sourceWidth * scale))));
                        transform.ScaledHeight((std::max)(1u, static_cast<unsigned>(std::lround(sourceHeight * scale))));
                        const auto pixels = AwaitLocal(decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8,
                            BitmapAlphaMode::Premultiplied, transform, ExifOrientationMode::IgnoreExifOrientation,
                            ColorManagementMode::DoNotColorManage)).DetachPixelData();
                        auto image = std::make_shared<WidgetRuntimeImagePixels>();
                        image->width = transform.ScaledWidth(); image->height = transform.ScaledHeight();
                        image->stride = image->width * 4;
                        image->bgraPremultiplied.assign(pixels.begin(), pixels.end());
                        if (IsValidWidgetRuntimeImage(*image, 512))
                        {
                            metadata_.artworkToken = MakeWidgetRuntimeImageToken("audio", *image);
                            metadata_.artwork = std::move(image);
                        }
                    }
                }
            }
            catch (...) {} // Missing/invalid tags and artwork are optional.
        }
        if (!systemMediaControls)
        {
            player_.Source(source_);
            return;
        }
        const Media::Playback::MediaPlaybackItem item(source_);
        const auto properties = item.GetDisplayProperties();
        properties.Type(Media::MediaPlaybackType::Music);
        properties.MusicProperties().Title(winrt::to_hstring(metadata_.title));
        properties.MusicProperties().Artist(winrt::to_hstring(metadata_.artist));
        properties.MusicProperties().AlbumTitle(winrt::to_hstring(metadata_.album));
        if (metadata_.artwork && thumbnail_)
            properties.Thumbnail(Storage::Streams::RandomAccessStreamReference::CreateFromStream(thumbnail_));
        item.ApplyDisplayProperties(properties);
        player_.Source(item);
        const auto manager = player_.CommandManager();
        using Media::Playback::MediaCommandEnablingRule;
        manager.NextBehavior().EnablingRule(MediaCommandEnablingRule::Always);
        manager.PreviousBehavior().EnablingRule(MediaCommandEnablingRule::Always);
        manager.ShuffleBehavior().EnablingRule(MediaCommandEnablingRule::Always);
        manager.AutoRepeatModeBehavior().EnablingRule(MediaCommandEnablingRule::Always);
        manager.RateBehavior().EnablingRule(MediaCommandEnablingRule::Always);
        nextToken_ = manager.NextReceived([queue = commands_](const auto&, const auto& args) {
            args.Handled(true); queue->Push({"next"});
        });
        previousToken_ = manager.PreviousReceived([queue = commands_](const auto&, const auto& args) {
            args.Handled(true); queue->Push({"previous"});
        });
        shuffleToken_ = manager.ShuffleReceived([queue = commands_](const auto&, const auto& args) {
            args.Handled(true); queue->Push({"setShuffle", args.IsShuffleRequested() ? 1.0 : 0.0});
        });
        repeatToken_ = manager.AutoRepeatModeReceived([queue = commands_](const auto&, const auto& args) {
            args.Handled(true);
            const auto mode = args.AutoRepeatMode();
            queue->Push({"setRepeatMode", 0, mode == Media::MediaPlaybackAutoRepeatMode::Track ? "track" :
                mode == Media::MediaPlaybackAutoRepeatMode::List ? "list" : "none"});
        });
        rateToken_ = manager.RateReceived([queue = commands_](const auto&, const auto& args) {
            args.Handled(true); const auto rate = args.PlaybackRate();
            if (std::isfinite(rate) && rate >= .25 && rate <= 4) queue->Push({"setPlaybackRate", rate});
        });
        manager.IsEnabled(true);
        metadata_.systemMediaControls = true;
    }
    std::vector<AudioPlaybackCommand> DrainCommands() override
    {
        std::lock_guard lock(commands_->mutex);
        std::vector<AudioPlaybackCommand> result;
        result.swap(commands_->pending);
        return result;
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
    void SetLoop(bool value) override { SetPlaybackMode(metadata_.shuffle, value ? "track" : "none"); }
    void SetPlaybackMode(bool shuffle, const std::string& repeatMode) override
    {
        const bool looping = repeatMode == "track";
        const bool previous = terminal_->looping.exchange(looping);
        try { player_.IsLoopingEnabled(looping); }
        catch (...) { terminal_->looping = previous; throw; }
        metadata_.shuffle = shuffle; metadata_.repeatMode = repeatMode;
        if (metadata_.systemMediaControls)
        {
            const auto controls = player_.SystemMediaTransportControls();
            controls.ShuffleEnabled(shuffle);
            using winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
            controls.AutoRepeatMode(repeatMode == "track" ? MediaPlaybackAutoRepeatMode::Track :
                repeatMode == "list" ? MediaPlaybackAutoRepeatMode::List : MediaPlaybackAutoRepeatMode::None);
        }
    }
    void SetPlaybackRate(double rate) override
    {
        player_.PlaybackSession().PlaybackRate(rate);
        if (metadata_.systemMediaControls) player_.SystemMediaTransportControls().PlaybackRate(rate);
    }
    AudioPlaybackState State() override
    {
        AudioPlaybackState value = metadata_;
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
        value.canSeek = session.CanSeek();
        value.playbackRate = session.PlaybackRate();
        return value;
    }
};
}

struct WidgetAudioPlayback::Impl
{
    struct WorkSignal
    {
        HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WorkSignal() { if (!handle) throw std::runtime_error("audioWorkerUnavailable"); }
        ~WorkSignal() { CloseHandle(handle); }
        void Notify() const noexcept { SetEvent(handle); }
    };
    struct Entry
    {
        AudioPlaybackRequest request;
        std::unique_ptr<WidgetAudioPlayer> player;
        AudioPlaybackState previous;
    };
    std::mutex mutex;
    WorkSignal workSignal;
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
        workSignal.Notify();
        worker.join();
    }
    void Publish(AudioPlaybackMessage message)
    {
        std::function<void()> callback;
        {
            std::lock_guard lock(mutex);
            if (retired.contains(message.owner)) return;
            if (!message.command.command.empty() &&
                std::count_if(messages.begin(), messages.end(), [&](const auto& old) {
                    return old.playbackId == message.playbackId && !old.command.command.empty();
                }) >= 32) return;
            // Coalesce progress without losing terminal events or task results.
            if (!message.taskId && message.command.command.empty() && message.state.status != "ended" && message.state.status != "error" &&
                message.state.status != "stopped")
                std::erase_if(messages, [&](const auto& old) {
                    return !old.taskId && old.command.command.empty() && old.playbackId == message.playbackId &&
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
            // MediaPlayer owns an internal SMTC window on this thread. Dispatch
            // its notifications outside our lock, including queued next/prev
            // commands and session cleanup, while bounding each polling pass.
            MSG nativeMessage{};
            bool dispatched = false;
            for (unsigned count = 0; count < 64 &&
                PeekMessageW(&nativeMessage, nullptr, 0, 0, PM_REMOVE); ++count)
            {
                dispatched = true;
                TranslateMessage(&nativeMessage);
                DispatchMessageW(&nativeMessage);
            }
            std::deque<AudioPlaybackRequest> work;
            std::unordered_set<std::uint64_t> dead, cancel;
            std::vector<std::pair<std::uint64_t, std::string>> files;
            bool shouldWait = false;
            {
                std::lock_guard lock(mutex);
                shouldWait = !stopping && !dispatched && pending.empty() &&
                    !ownersChanged && canceled.empty() && revoked.empty();
            }
            if (shouldWait)
            {
                const DWORD result = MsgWaitForMultipleObjectsEx(1, &workSignal.handle,
                    players.empty() ? INFINITE : 250, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (result == WAIT_OBJECT_0 + 1) continue;
                if (result == WAIT_FAILED) WaitForSingleObject(workSignal.handle, 250);
            }
            {
                std::lock_guard lock(mutex);
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
                        player->Configure(request.readMetadata, request.systemMediaControls);
                        player->SetVolume(request.volume);
                        player->SetPlaybackMode(request.shuffle, request.loop ? "track" : request.repeatMode);
                        player->SetPlaybackRate(request.playbackRate);
                        player->Play();
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
                        else if (request.action == "audio.setPlaybackRate") player->SetPlaybackRate(request.playbackRate);
                        else if (request.action == "audio.setPlaybackMode") player->SetPlaybackMode(request.shuffle, request.repeatMode);
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
                try
                {
                    state = entry.player->State();
                    for (auto& command : entry.player->DrainCommands())
                        if (entry.request.systemMediaControls)
                            Publish({entry.request.owner, 0, item->first, true, {}, std::move(command)});
                }
                catch (...) { state.status = "error"; state.error = "audioPlaybackFailed"; }
                const bool terminal = state.status == "ended" || state.status == "error";
                if (terminal || state.status != entry.previous.status || state.positionMs != entry.previous.positionMs ||
                    state.durationMs != entry.previous.durationMs || state.volume != entry.previous.volume || state.loop != entry.previous.loop ||
                    state.playbackRate != entry.previous.playbackRate || state.shuffle != entry.previous.shuffle ||
                    state.repeatMode != entry.previous.repeatMode || state.canSeek != entry.previous.canSeek ||
                    state.title != entry.previous.title || state.artwork != entry.previous.artwork)
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
        name == "audio.seek" || name == "audio.setVolume" || name == "audio.setLoop" ||
        name == "audio.setPlaybackRate" || name == "audio.setPlaybackMode";
}
bool WidgetAudioPlayback::SupportedFile(const std::filesystem::path& path)
{
    const auto extension = path.extension().wstring();
    return widget::FindWidgetAudioFormat(std::wstring_view(extension)) != nullptr;
}
bool WidgetAudioPlayback::Validate(const AudioPlaybackRequest& request) noexcept
{
    return request.taskId && request.owner && SupportsAction(request.action) &&
        std::isfinite(request.volume) && request.volume >= 0 && request.volume <= 1 &&
        std::isfinite(request.playbackRate) && request.playbackRate >= .25 && request.playbackRate <= 4 &&
        (request.repeatMode == "none" || request.repeatMode == "track" || request.repeatMode == "list") &&
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
    impl_->workSignal.Notify(); return true;
}
void WidgetAudioPlayback::Cancel(std::uint64_t id)
{
    { std::lock_guard lock(impl_->mutex); impl_->canceled.insert(id); }
    impl_->workSignal.Notify();
}
void WidgetAudioPlayback::ForgetOwner(std::uint64_t owner)
{
    { std::lock_guard lock(impl_->mutex); impl_->retired.insert(owner); impl_->ownersChanged = true;
      std::erase_if(impl_->messages, [owner](const auto& item) { return item.owner == owner; }); }
    impl_->workSignal.Notify();
}
void WidgetAudioPlayback::RevokeFile(std::uint64_t owner, std::string handle)
{
    { std::lock_guard lock(impl_->mutex); impl_->revoked.emplace_back(owner, std::move(handle)); }
    impl_->workSignal.Notify();
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
