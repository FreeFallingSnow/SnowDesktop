#include "system_control_windows.h"
#include "widget_media_contract.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <limits>
#include <unordered_map>

namespace snowdesktop::system_control::windows
{
namespace
{
using namespace winrt::Windows::Media::Control;
using Session = GlobalSystemMediaTransportControlsSession;
Session Resolve(const GlobalSystemMediaTransportControlsSessionManager& manager, const std::string& id)
{
    const auto current = manager.GetCurrentSession(); if (id.empty()) return current;
    std::unordered_map<std::wstring, std::size_t> occurrences; std::size_t exposed = 0;
    const auto match = [&](const Session& session) -> Session {
        if (!session || exposed >= widget_runtime::MaximumExposedMediaSessions) return nullptr;
        ++exposed; const std::wstring source(session.SourceAppUserModelId());
        return widget_runtime::OpaqueMediaSessionId(source, occurrences[source]++) == id ? session : Session{nullptr};
    };
    if (const auto found = match(current)) return found;
    for (const auto& session : manager.GetSessions())
    { if (session == current) continue; if (const auto found = match(session)) return found; }
    return nullptr;
}
class Media final : public Backend
{
public:
    // Media snapshots and artwork already share one provider sample. This
    // backend owns controls only and uses the same opaque session identity.
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override { return {}; }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return cancel.Failure();
        try
        {
            const auto manager = Await(GlobalSystemMediaTransportControlsSessionManager::RequestAsync(), cancel);
            const auto session = Resolve(manager, Argument(request, "sessionId"));
            if (!session) return Error(ERROR_NOT_FOUND, "notAvailable");
            const auto controls = session.GetPlaybackInfo().Controls();
            winrt::Windows::Foundation::IAsyncOperation<bool> operation{nullptr};
            if (request.name == "media.play" && controls.IsPlayEnabled()) operation = session.TryPlayAsync();
            else if (request.name == "media.pause" && controls.IsPauseEnabled()) operation = session.TryPauseAsync();
            else if (request.name == "media.toggle" && controls.IsPlayPauseToggleEnabled()) operation = session.TryTogglePlayPauseAsync();
            else if (request.name == "media.stop" && controls.IsStopEnabled()) operation = session.TryStopAsync();
            else if (request.name == "media.next" && controls.IsNextEnabled()) operation = session.TrySkipNextAsync();
            else if (request.name == "media.previous" && controls.IsPreviousEnabled()) operation = session.TrySkipPreviousAsync();
            else if (request.name == "media.seek" && controls.IsPlaybackPositionEnabled())
            {
                const double position = Numeric(request, "positionMs");
                if (position > static_cast<double>((std::numeric_limits<std::int64_t>::max)() / 10000)) return Error(ERROR_INVALID_PARAMETER, "seekOutOfRange");
                const auto timeline = session.GetTimelineProperties();
                const auto delta = static_cast<std::int64_t>(position) * 10000;
                if (timeline.StartTime().count() > (std::numeric_limits<std::int64_t>::max)() - delta) return Error(ERROR_INVALID_PARAMETER, "seekOutOfRange");
                const auto target = timeline.StartTime().count() + delta;
                if (target < timeline.MinSeekTime().count() || target > timeline.MaxSeekTime().count()) return Error(ERROR_INVALID_PARAMETER, "seekOutOfRange");
                operation = session.TryChangePlaybackPositionAsync(target);
            }
            else if (request.name == "media.setRate" && controls.IsPlaybackRateEnabled()) operation = session.TryChangePlaybackRateAsync(Numeric(request, "rate"));
            else if (request.name == "media.setShuffle" && controls.IsShuffleEnabled()) operation = session.TryChangeShuffleActiveAsync(Argument(request, "enabled") == "1");
            else if (request.name == "media.setRepeat" && controls.IsRepeatEnabled())
            {
                using Mode = winrt::Windows::Media::MediaPlaybackAutoRepeatMode;
                const auto mode = Argument(request, "mode");
                if (mode != "none" && mode != "track" && mode != "list") return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
                operation = session.TryChangeAutoRepeatModeAsync(mode == "track" ? Mode::Track : mode == "list" ? Mode::List : Mode::None);
            }
            if (!operation) return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
            return Await(operation, cancel) ? Result{true, {}, 0} : Error(ERROR_REQUEST_ABORTED, "controlRejected");
        }
        catch (const winrt::hresult_error& error)
        { return cancel.Stop() ? cancel.Failure() : Error(static_cast<DWORD>(error.code().value)); }
    }
    void Release(std::string_view) override {}
};
}
std::shared_ptr<Backend> CreateMediaBackend() { return std::make_shared<Media>(); }
}
