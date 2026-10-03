#include "system_control_windows.h"
#include "system_control_feedback.h"
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
    operators::NetworkOperatorTetheringManager manager_{nullptr};
    operators::NetworkOperatorTetheringManager Manager()
    {
        // Keep the active manager when the upstream disappears, so Stop still
        // works. Prefer an active hotspot over a newly preferred connection.
        if (manager_)
        {
            try { const auto state = manager_.TetheringOperationalState();
                if (state == operators::TetheringOperationalState::On || state == operators::TetheringOperationalState::InTransition) return manager_; }
            catch (const winrt::hresult_error&) { /* A disconnected upstream must not pin an invalid manager. */ }
            manager_ = nullptr;
        }
        const auto upstream = connectivity::NetworkInformation::GetInternetConnectionProfile();
        operators::NetworkOperatorTetheringManager fallback{nullptr};
        for (const auto& profile : connectivity::NetworkInformation::GetConnectionProfiles())
        {
            try
            {
                if (operators::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile) != operators::TetheringCapability::Enabled) continue;
                auto candidate = operators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
                const auto state = candidate.TetheringOperationalState();
                if (state == operators::TetheringOperationalState::On || state == operators::TetheringOperationalState::InTransition)
                { manager_ = candidate; return manager_; }
                if (!fallback) fallback = candidate;
            }
            catch (const winrt::hresult_error&) { /* Continue past an inaccessible or disappearing profile. */ }
        }
        if (upstream && operators::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(upstream) == operators::TetheringCapability::Enabled)
            manager_ = operators::NetworkOperatorTetheringManager::CreateFromConnectionProfile(upstream);
        else manager_ = fallback;
        return manager_;
    }
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        try
        {
            if (cancel.Stop()) return {{"host.hotspot", Missing(cancel.Failure().error)}};
            const auto manager = Manager();
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
        catch (const winrt::hresult_error& error) { return {{"host.hotspot", Missing(Error(static_cast<DWORD>(error.code().value)).error)}}; }
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        try
        {
            if (cancel.Stop()) return cancel.Failure();
            const auto manager = Manager(); if (!manager) return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
            const bool enabled = Argument(request, "enabled") == "1";
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
        catch (const winrt::hresult_error& error) { return Error(static_cast<DWORD>(error.code().value)); }
    }
    void Release(std::string_view) override { manager_ = nullptr; }
};
class Awake final : public Backend
{
    HANDLE request_ = INVALID_HANDLE_VALUE;
public:
    ~Awake() override { if (request_ != INVALID_HANDLE_VALUE) CloseHandle(request_); }
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation&) override
    {
        auto value = json::Object(); value.object["enabled"] = json::Boolean(request_ != INVALID_HANDLE_VALUE);
        return {{"host.awake", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return cancel.Failure();
        if (Argument(request, "enabled") == "0")
        { if (request_ != INVALID_HANDLE_VALUE) { CloseHandle(request_); request_ = INVALID_HANDLE_VALUE; } return {true, {}, 0}; }
        if (request_ != INVALID_HANDLE_VALUE) return {true, {}, 0};
        auto reason = Wide(Argument(request, "reason")); if (reason.empty()) reason = L"SnowDesktop";
        REASON_CONTEXT context{}; context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING; context.Reason.SimpleReasonString = reason.data();
        const HANDLE handle = PowerCreateRequest(&context);
        if (handle == INVALID_HANDLE_VALUE) return Error(GetLastError());
        if (!PowerSetRequest(handle, PowerRequestSystemRequired) || !PowerSetRequest(handle, PowerRequestDisplayRequired))
        { const DWORD status = GetLastError(); CloseHandle(handle); return Error(status); }
        request_ = handle; return {true, {}, 0};
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
