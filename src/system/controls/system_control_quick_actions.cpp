#include "system_control_windows.h"
#include "system_control_feedback.h"
#include "system_awake_session.h"
#include "system_control_hotspot_cache.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

namespace snowdesktop::system_control::windows
{
namespace
{
constexpr std::pair<const char*, UINT32> Topologies[]{
    {"internal", SDC_TOPOLOGY_INTERNAL}, {"clone", SDC_TOPOLOGY_CLONE},
    {"extend", SDC_TOPOLOGY_EXTEND}, {"external", SDC_TOPOLOGY_EXTERNAL}};
std::pair<DWORD, std::string> ProjectionState()
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        UINT32 paths = 0, modes = 0;
        LONG status = GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &paths, &modes);
        if (status) return {static_cast<DWORD>(status), {}};
        if (!paths || paths > 1024 || modes > 4096) return {ERROR_NOT_SUPPORTED, {}};
        std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
        std::vector<DISPLAYCONFIG_MODE_INFO> mode((std::max)(modes, UINT32{1}));
        DISPLAYCONFIG_TOPOLOGY_ID topology{};
        status = QueryDisplayConfig(QDC_DATABASE_CURRENT, &paths, path.data(), &modes, mode.data(), &topology);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status) return {static_cast<DWORD>(status), {}};
        for (const auto& [name, flag] : Topologies)
            if (static_cast<UINT32>(topology) == flag) return {ERROR_SUCCESS, name};
        return {ERROR_NOT_SUPPORTED, {}};
    }
    return {ERROR_INSUFFICIENT_BUFFER, {}};
}
class Projection final : public Backend
{
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
    {
        const auto [status, name] = ProjectionState();
        if (status) return {{"host.projection", Missing(Error(status).error)}};
        auto value = json::Object(); value.object["mode"] = json::Text(name);
        auto modes = json::Array();
        for (const auto& [id, flag] : Topologies)
        {
            auto item = json::Object(); item.object["id"] = json::Text(id);
            item.object["available"] = json::Boolean(SetDisplayConfig(0, nullptr, 0, nullptr, SDC_VALIDATE | flag) == ERROR_SUCCESS);
            modes.array.push_back(std::move(item));
        }
        value.object["modes"] = std::move(modes);
        return {{"host.projection", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        const auto wanted = Argument(request, "mode");
        for (const auto& [id, flag] : Topologies) if (wanted == id)
        {
            if (cancel.Stop()) return cancel.Failure();
            LONG status = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_VALIDATE | flag);
            if (status) return Error(static_cast<DWORD>(status), "actionUnsupported");
            status = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | flag);
            if (status) return Error(static_cast<DWORD>(status));
            return ConfirmControlValue(1, 0, cancel, [&]() -> ControlReadback {
                const auto [readStatus, mode] = ProjectionState();
                return readStatus ? ControlReadback{std::nullopt, Error(readStatus)} : ControlReadback{mode == wanted ? 1. : 0., {}};
            }, ControlReadbackPause);
        }
        return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
    }
    void Release(std::string_view) override {}
};

// Windows' system airplane switch has no public desktop SDK setter. Keep the
// optional RM service ABI isolated, query support and read back every change.
// ABI reference: https://github.com/GrieferAtWork/wintouchg/blob/master/main.c
// Never infer airplane mode from individual Wi-Fi/Bluetooth radio states.
MIDL_INTERFACE("db3afbfb-08e6-46c6-aa70-bf9a34c30ab7") SystemRadioManager : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE IsRMSupported(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetUIRadioInstances(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSystemRadioState(BOOL*, int*, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSystemRadioState(BOOL) = 0;
};
constexpr GUID RadioManagement{0x581333f6, 0x28db, 0x41be, {0xbc, 0x7a, 0xff, 0x20, 0x1f, 0x12, 0xf3, 0xf6}};
HRESULT AirplaneManager(ComPtr<SystemRadioManager>& manager)
{
    HRESULT status = CoCreateInstance(RadioManagement, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(status)) return status;
    DWORD supported = 0; status = manager->IsRMSupported(&supported);
    return FAILED(status) ? status : supported ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}
ControlReadback AirplaneState(SystemRadioManager* manager)
{
    BOOL radioEnabled = FALSE; int hardware = 0; DWORD reason = 0;
    const HRESULT status = manager->GetSystemRadioState(&radioEnabled, &hardware, &reason);
    return FAILED(status) ? ControlReadback{std::nullopt, Status(status)} : ControlReadback{radioEnabled ? 0. : 1., {}};
}
class Airplane final : public Backend
{
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
    {
        ComPtr<SystemRadioManager> manager; const HRESULT status = AirplaneManager(manager);
        if (FAILED(status)) return {{"host.airplane", Missing(Status(status).error)}};
        const auto state = AirplaneState(manager.Get());
        if (!state.value) return {{"host.airplane", Missing(state.failure.error)}};
        auto value = json::Object(); value.object["enabled"] = json::Boolean(*state.value == 1);
        return {{"host.airplane", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return cancel.Failure();
        ComPtr<SystemRadioManager> manager; HRESULT status = AirplaneManager(manager);
        if (FAILED(status)) return Status(status);
        const bool enabled = Argument(request, "enabled") == "1";
        status = manager->SetSystemRadioState(enabled ? FALSE : TRUE);
        if (FAILED(status)) return Status(status);
        return ConfirmControlValue(enabled ? 1. : 0., 0, cancel, [&] { return AirplaneState(manager.Get()); }, ControlReadbackPause);
    }
    void Release(std::string_view) override {}
};

namespace operators = winrt::Windows::Networking::NetworkOperators;
namespace connectivity = winrt::Windows::Networking::Connectivity;
class Hotspot final : public Backend
{
    using ManagerHandle = operators::NetworkOperatorTetheringManager;
    HotspotManagerCache<ManagerHandle> cache_;
    std::shared_ptr<std::atomic<std::uint64_t>> generation_;
    winrt::event_token networkChanged_{};
    bool notificationsAttempted_ = false, notificationsAvailable_ = false;

    void EnsureNotifications()
    {
        if (notificationsAttempted_) return;
        generation_ = std::make_shared<std::atomic<std::uint64_t>>(0);
        notificationsAttempted_ = true;
        try
        {
            std::weak_ptr<std::atomic<std::uint64_t>> weak = generation_;
            networkChanged_ = connectivity::NetworkInformation::NetworkStatusChanged(
                [weak](const winrt::Windows::Foundation::IInspectable&) {
                    if (const auto generation = weak.lock())
                        generation->fetch_add(1, std::memory_order_release);
                });
            notificationsAvailable_ = true;
        }
        catch (const winrt::hresult_error&) { /* Bounded rediscovery remains available without notifications. */ }
    }

    ManagerHandle Discover(const Cancellation& cancel)
    {
        if (cancel.Stop()) throw winrt::hresult_canceled();
        const auto upstream = connectivity::NetworkInformation::GetInternetConnectionProfile();
        ManagerHandle fallback{nullptr}, preferred{nullptr};
        for (const auto& profile : connectivity::NetworkInformation::GetConnectionProfiles())
        {
            if (cancel.Stop()) throw winrt::hresult_canceled();
            try
            {
                if (operators::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile) != operators::TetheringCapability::Enabled) continue;
                auto candidate = operators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
                const auto state = candidate.TetheringOperationalState();
                if (state == operators::TetheringOperationalState::On || state == operators::TetheringOperationalState::InTransition)
                    return candidate;
                // Reuse the candidate when the preferred profile is already
                // in the inventory instead of creating its manager twice.
                if (upstream && profile == upstream) preferred = candidate;
                if (!fallback) fallback = candidate;
            }
            catch (const winrt::hresult_error&) { /* Continue past an inaccessible or disappearing profile. */ }
        }
        if (cancel.Stop()) throw winrt::hresult_canceled();
        if (preferred) return preferred;
        if (upstream && operators::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(upstream) == operators::TetheringCapability::Enabled)
            return operators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(upstream);
        return fallback;
    }

    ManagerHandle Manager(const Cancellation& cancel, bool refreshBeforeEnable = false)
    {
        EnsureNotifications();
        return cache_.Get(std::chrono::steady_clock::now(), generation_->load(std::memory_order_acquire),
            notificationsAvailable_, refreshBeforeEnable,
            [](const ManagerHandle& manager) {
                try
                {
                    switch (manager.TetheringOperationalState())
                    {
                    case operators::TetheringOperationalState::Off: return HotspotManagerState::Off;
                    case operators::TetheringOperationalState::On: return HotspotManagerState::On;
                    case operators::TetheringOperationalState::InTransition: return HotspotManagerState::InTransition;
                    default: break;
                    }
                }
                catch (const winrt::hresult_error&) { /* Rediscover an invalidated OS object. */ }
                return HotspotManagerState::Unknown;
            }, [this, &cancel] { return Discover(cancel); });
    }
public:
    ~Hotspot() override { Release({}); }
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        try
        {
            if (cancel.Stop()) return {{"host.hotspot", Missing(cancel.Failure().error)}};
            const auto manager = Manager(cancel);
            if (cancel.Stop()) { cache_.Reset(); return {{"host.hotspot", Missing(cancel.Failure().error)}}; }
            if (!manager) return {{"host.hotspot", Missing("actionUnsupported")}};
            const auto state = manager.TetheringOperationalState();
            auto value = json::Object();
            value.object["enabled"] = json::Boolean(state == operators::TetheringOperationalState::On);
            value.object["busy"] = json::Boolean(state == operators::TetheringOperationalState::InTransition);
            value.object["canToggle"] = json::Boolean(state == operators::TetheringOperationalState::On || state == operators::TetheringOperationalState::Off);
            value.object["ssid"] = json::Text(Utf8(manager.GetCurrentAccessPointConfiguration().Ssid()));
            value.object["clients"] = json::Number(manager.ClientCount());
            // Passwords are deliberately never sampled, logged or serialized.
            return {{"host.hotspot", Value(std::move(value))}};
        }
        catch (const winrt::hresult_error& error)
        { cache_.Reset(); return {{"host.hotspot", Missing(cancel.Stop() ? cancel.Failure().error : Error(static_cast<DWORD>(error.code().value)).error)}}; }
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        try
        {
            if (cancel.Stop()) return cancel.Failure();
            const bool enabled = Argument(request, "enabled") == "1";
            const auto manager = Manager(cancel, enabled);
            if (cancel.Stop()) { cache_.Reset(); return cancel.Failure(); }
            if (!manager) return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
            const auto before = manager.TetheringOperationalState();
            if (before == operators::TetheringOperationalState::InTransition || before == operators::TetheringOperationalState::Unknown)
                return Error(ERROR_BUSY, "unavailable");
            if ((before == operators::TetheringOperationalState::On) == enabled) return {true, {}, 0};
            const auto result = Await(enabled ? manager.StartTetheringAsync() : manager.StopTetheringAsync(), cancel);
            if (result.Status() != operators::TetheringOperationStatus::Success) return Error(ERROR_GEN_FAILURE, "stateMismatch");
            return ConfirmControlValue(enabled ? 1. : 0., 0, cancel, [&]() -> ControlReadback {
                const auto state = manager.TetheringOperationalState();
                if (state == operators::TetheringOperationalState::Unknown) return {std::nullopt, Error(ERROR_NOT_SUPPORTED)};
                if (state == operators::TetheringOperationalState::InTransition) return {std::nullopt, {}};
                return {state == operators::TetheringOperationalState::On ? 1. : 0., {}};
            }, ControlReadbackPause);
        }
        catch (const winrt::hresult_error& error)
        { cache_.Reset(); return cancel.Stop() ? cancel.Failure() : Error(static_cast<DWORD>(error.code().value)); }
    }
    void Release(std::string_view) override
    {
        cache_.Reset();
        // Late callbacks keep only a weak generation, never this backend.
        generation_.reset();
        if (notificationsAvailable_)
            try { connectivity::NetworkInformation::NetworkStatusChanged(networkChanged_); }
            catch (const winrt::hresult_error&) { /* Service teardown can race notification removal. */ }
        networkChanged_ = {};
        notificationsAvailable_ = notificationsAttempted_ = false;
    }
};
class Awake final : public Backend
{
    HANDLE request_ = INVALID_HANDLE_VALUE;
    AwakeSession session_{
        [this](bool keepScreenOn,const std::string& text)->Result {
            auto reason=Wide(text);if(reason.empty())reason=L"SnowDesktop";
            REASON_CONTEXT context{};context.Version=POWER_REQUEST_CONTEXT_VERSION;
            context.Flags=POWER_REQUEST_CONTEXT_SIMPLE_STRING;context.Reason.SimpleReasonString=reason.data();
            const HANDLE handle=PowerCreateRequest(&context);if(handle==INVALID_HANDLE_VALUE)return Error(GetLastError());
            if(!PowerSetRequest(handle,PowerRequestSystemRequired)||(keepScreenOn&&!PowerSetRequest(handle,PowerRequestDisplayRequired)))
            {const DWORD status=GetLastError();CloseHandle(handle);return Error(status);}
            if(request_!=INVALID_HANDLE_VALUE)CloseHandle(request_);
            request_=handle;return {true,{},0};
        },
        [this]{if(request_!=INVALID_HANDLE_VALUE){CloseHandle(request_);request_=INVALID_HANDLE_VALUE;}}};
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
    {
        const auto state=session_.Read();auto value=json::Object();
        value.object["enabled"]=json::Boolean(state.mode!=AwakeMode::Plan);
        value.object["mode"]=json::Text(state.mode==AwakeMode::Timed?"timed":state.mode==AwakeMode::Indefinite?"indefinite":"plan");
        value.object["keepScreenOn"]=json::Boolean(state.keepScreenOn);
        value.object["durationSeconds"]=json::Number(static_cast<double>(state.duration.count()));
        value.object["remainingSeconds"]=json::Number(static_cast<double>(state.remaining.count()));
        return {{"host.awake", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return cancel.Failure();
        if(request.name=="host.awake.setScreen")return session_.SetScreen(Argument(request,"enabled")=="1");
        const auto mode=Argument(request,"mode");
        const auto duration=std::chrono::seconds{static_cast<std::int64_t>(Numeric(request,"durationSeconds"))};
        return session_.Set(mode=="timed"?AwakeMode::Timed:mode=="indefinite"?AwakeMode::Indefinite:AwakeMode::Plan,
            duration,Argument(request,"keepScreenOn")=="1",Argument(request,"reason"));
    }
    // Closing the panel must not undo an explicitly enabled session switch.
    // The request ends when switched off or when the application exits.
    void Release(std::string_view) override {}
};
}
std::shared_ptr<Backend> CreateProjectionBackend() { return std::make_shared<Projection>(); }
std::shared_ptr<Backend> CreateHotspotBackend() { return std::make_shared<Hotspot>(); }
std::shared_ptr<Backend> CreateAirplaneBackend() { return std::make_shared<Airplane>(); }
std::shared_ptr<Backend> CreateAwakeBackend() { return std::make_shared<Awake>(); }
}
