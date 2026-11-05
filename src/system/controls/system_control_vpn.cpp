#include "system_control_vpn.h"
#include "system_control_feedback.h"
#include <raserror.h>
#include <winerror.h>
#include <algorithm>
#include <cwchar>
#include <map>
#include <string.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::system_control::windows
{
namespace
{
constexpr std::string_view VpnTopic = "host.vpn";
struct WideLess
{
    bool operator()(const std::wstring& left, const std::wstring& right) const
    { return _wcsicmp(left.c_str(), right.c_str()) < 0; }
};
struct EntryState
{
    bool connected = false, busy = false;
};
struct DialParameters
{
    RASDIALPARAMSW value{};
    ~DialParameters() { SecureZeroMemory(&value, sizeof(value)); }
};
using EntryStates = std::map<std::wstring, EntryState, WideLess>;

std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring Wide(std::string_view value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string Argument(const Request& request, const char* name)
{
    const auto found = request.arguments.find(name);
    return found == request.arguments.end() ? std::string{} : found->second;
}
Result Error(DWORD status, std::string fallback = "unavailable")
{
    switch (status)
    {
    case ERROR_ACCESS_DENIED: return {false, "accessDenied", status};
    case ERROR_AUTHENTICATION_FAILURE:
    case ERROR_VPN_BAD_CERT:
    case ERROR_VPN_BAD_PSK:
    case ERROR_IPSEC_IKE_AUTH_FAIL: return {false, "credentialsRequired", status};
    case ERROR_VPN_TIMEOUT:
    case ERROR_TIMEOUT: return {false, "timeout", status};
    case ERROR_DIAL_ALREADY_IN_PROGRESS:
    case ERROR_BUSY: return {false, "busy", status};
    case ERROR_VPN_REFUSED:
    case ERROR_VPN_DISCONNECT:
    case ERROR_NO_CONNECTION:
    case ERROR_FROM_DEVICE:
    case ERROR_NO_ANSWER:
    case ERROR_NO_DIALTONE:
    case ERROR_PPP_TIMEOUT:
    case ERROR_PPP_NO_RESPONSE:
    case ERROR_VPN_GRE_BLOCKED: return {false, "connectionFailed", status};
    case ERROR_CANCELLED: return {false, "canceled", status};
    default: return {false, std::move(fallback), status};
    }
}
Snapshot Value(JsonValue value)
{
    Snapshot result; result.available = true; result.value = std::move(value); return result;
}
Snapshot Missing(std::string error)
{
    Snapshot result; result.error = std::move(error); return result;
}

// Enumerate active connections once per sample. A RAS connection handle is
// process-local and remains valid while the connection exists; the backend never
// keeps one after the call, so a panel close cannot end an established link.
DWORD EnumerateConnections(const VpnApi& api, EntryStates& result, const Cancellation& cancel)
{
    if (cancel.Stop()) return ERROR_CANCELLED;
    DWORD size = 0, count = 0;
    DWORD status = api.enumConnections(nullptr, &size, &count);
    if (status != ERROR_SUCCESS && status != ERROR_BUFFER_TOO_SMALL) return status;
    if (!count) return ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        std::vector<RASCONNW> connections(count);
        for (auto& connection : connections) connection.dwSize = sizeof(RASCONNW);
        DWORD bytes = static_cast<DWORD>(connections.size() * sizeof(RASCONNW));
        status = api.enumConnections(connections.data(), &bytes, &count);
        if (status == ERROR_SUCCESS)
        {
            for (DWORD index = 0; index < count; ++index)
            {
                if (cancel.Stop()) return ERROR_CANCELLED;
                const auto& connection = connections[index];
                if (!connection.hrasconn || !connection.szEntryName[0]) continue;
                RASCONNSTATUSW state{}; state.dwSize = sizeof(state);
                auto& entry = result[connection.szEntryName];
                if (api.connectStatus(connection.hrasconn, &state) != ERROR_SUCCESS)
                {
                    // An unreadable state is not proof of a completed connection.
                    entry.busy = true;
                    continue;
                }
                if (state.rasconnstate == RASCS_Connected) entry.connected = true;
                else if (state.rasconnstate != RASCS_Disconnected) entry.busy = true;
            }
            return ERROR_SUCCESS;
        }
        if (status != ERROR_BUFFER_TOO_SMALL) return status;
        count = (std::max)(count, static_cast<DWORD>(bytes / sizeof(RASCONNW)));
        if (!count) return ERROR_SUCCESS;
    }
    return ERROR_BUFFER_TOO_SMALL;
}

// Only entries currently stored as VPN connections in the default RAS phone book
// are exposed. Reading the type prevents a stale UI row from dialing a dial-up
// or broadband entry by name.
DWORD EnumerateVpnEntries(const VpnApi& api, std::vector<std::wstring>& result, const Cancellation& cancel)
{
    if (cancel.Stop()) return ERROR_CANCELLED;
    DWORD size = 0, count = 0;
    DWORD status = api.enumEntries(nullptr, nullptr, nullptr, &size, &count);
    if (status != ERROR_SUCCESS && status != ERROR_BUFFER_TOO_SMALL) return status;
    if (!count) return ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 3; ++attempt)
    {
        const DWORD capacity = (std::max)(size, static_cast<DWORD>(sizeof(RASENTRYNAMEW) * count));
        std::vector<BYTE> storage((std::max)(capacity, static_cast<DWORD>(sizeof(RASENTRYNAMEW))));
        auto* names = reinterpret_cast<RASENTRYNAMEW*>(storage.data());
        names[0].dwSize = sizeof(RASENTRYNAMEW);
        size = static_cast<DWORD>(storage.size());
        status = api.enumEntries(nullptr, nullptr, names, &size, &count);
        if (status == ERROR_SUCCESS)
        {
            std::vector<std::wstring> entries;
            for (DWORD index = 0; index < count; ++index)
            {
                if (cancel.Stop()) return ERROR_CANCELLED;
                const std::wstring name = names[index].szEntryName;
                if (name.empty() || name.size() > RAS_MaxEntryName) continue;
                RASENTRYW entry{}; entry.dwSize = sizeof(RASENTRYW);
                DWORD entrySize = sizeof(RASENTRYW), deviceSize = 0;
                status = api.entryProperties(nullptr, name.c_str(), &entry, &entrySize, nullptr, &deviceSize);
                if (status == ERROR_BUFFER_TOO_SMALL && entrySize >= sizeof(RASENTRYW) && entrySize <= 65536)
                {
                    std::vector<BYTE> properties(entrySize);
                    auto* retry = reinterpret_cast<RASENTRYW*>(properties.data());
                    retry->dwSize = sizeof(RASENTRYW);
                    status = api.entryProperties(nullptr, name.c_str(), retry, &entrySize, nullptr, &deviceSize);
                    if (status == ERROR_SUCCESS && retry->dwType == RASET_Vpn) entries.push_back(name);
                }
                else if (status == ERROR_SUCCESS && entry.dwType == RASET_Vpn) entries.push_back(name);
                status = ERROR_SUCCESS;
            }
            result = std::move(entries);
            return ERROR_SUCCESS;
        }
        if (status != ERROR_BUFFER_TOO_SMALL) return status;
        count = (std::max)(count, static_cast<DWORD>(size / sizeof(RASENTRYNAMEW)));
    }
    return ERROR_BUFFER_TOO_SMALL;
}
ControlReadback ReadState(const VpnApi& api, std::wstring_view name, const Cancellation& cancel)
{
    EntryStates states;
    const DWORD status = EnumerateConnections(api, states, cancel);
    if (status) return {std::nullopt, Error(status)};
    const auto found = states.find(std::wstring(name));
    if (found == states.end()) return {0., {}};
    if (found->second.connected) return {1., {}};
    // Connecting or disconnecting is not a final value. ConfirmControlValue
    // retries while no settled value is available and reports a timeout instead
    // of claiming a state that was never observed.
    return {std::nullopt, {}};
}
}

class Vpn final : public Backend
{
public:
    explicit Vpn(VpnApi api) : api_(api) {}
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return {{std::string(VpnTopic), Missing(cancel.Failure().error)}};
        std::vector<std::wstring> names;
        DWORD status = EnumerateVpnEntries(api_, names, cancel);
        if (status) return {{std::string(VpnTopic), Missing(Error(status).error)}};
        EntryStates states;
        status = EnumerateConnections(api_, states, cancel);
        if (status) return {{std::string(VpnTopic), Missing(Error(status).error)}};
        auto value = json::Object();
        auto entries = json::Array();
        bool connected = false, busy = false;
        for (const auto& name : names)
        {
            const auto found = states.find(name);
            const bool entryConnected = found != states.end() && found->second.connected;
            const bool entryBusy = found != states.end() && found->second.busy;
            const auto id = Utf8(name);
            auto item = json::Object();
            item.object["id"] = json::Text(id);
            item.object["name"] = json::Text(id);
            item.object["state"] = json::Text(entryConnected ? "connected" : entryBusy ? "connecting" : "disconnected");
            item.object["connected"] = json::Boolean(entryConnected);
            item.object["busy"] = json::Boolean(entryBusy);
            item.object["canConnect"] = json::Boolean(!entryConnected && !entryBusy);
            item.object["canDisconnect"] = json::Boolean(entryConnected);
            entries.array.push_back(std::move(item));
            connected = connected || entryConnected;
            busy = busy || entryBusy;
        }
        value.object["entries"] = std::move(entries);
        value.object["enabled"] = json::Boolean(connected);
        value.object["busy"] = json::Boolean(busy);
        return {{std::string(VpnTopic), Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        const auto name = Wide(Argument(request, "entryName"));
        if (name.empty() || name.size() > RAS_MaxEntryName || request.arguments.size() != 1)
            return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
        if (cancel.Stop()) return cancel.Failure();
        std::vector<std::wstring> names;
        DWORD status = EnumerateVpnEntries(api_, names, cancel);
        if (status) return Error(status);
        if (cancel.Stop()) return cancel.Failure();
        if (std::none_of(names.begin(), names.end(), [&](const auto& value) { return _wcsicmp(value.c_str(), name.c_str()) == 0; }))
            return {false, "stateMismatch", ERROR_FILE_NOT_FOUND};
        if (request.name == "host.vpn.connect") return Connect(name, cancel);
        return Disconnect(name, cancel);
    }
    void Release(std::string_view) override {}
private:
    Result Connect(const std::wstring& name, const Cancellation& cancel)
    {
        EntryStates states;
        DWORD status = EnumerateConnections(api_, states, cancel);
        if (status) return Error(status);
        const auto found = states.find(name);
        if (found != states.end() && found->second.connected) return {true, {}, 0};
        if (found != states.end() && found->second.busy) return Error(ERROR_BUSY, "busy");
        if (cancel.Stop()) return cancel.Failure();
        DialParameters saved;
        auto& parameters = saved.value;
        parameters.dwSize = sizeof(parameters);
        wcsncpy_s(parameters.szEntryName, RAS_MaxEntryName + 1, name.c_str(), _TRUNCATE);
        // RasDial does not load the saved user name/password on its own. Keep
        // the opaque password handle returned by Windows intact for RasDial.
        BOOL hasPassword = FALSE;
        status = api_.entryDialParams(nullptr, &parameters, &hasPassword);
        if (status) return Error(status);
        if (cancel.Stop()) return cancel.Failure();
        RASDIALEXTENSIONS extensions{}; extensions.dwSize = sizeof(extensions);
        // Keep the control center responsible for feedback; do not open the
        // separate connected/reconnect UI. Credentials remain owned by Windows.
        extensions.dwfOptions = RDEOPT_DisableConnectedUI | RDEOPT_DisableReconnectUI;
        HRASCONN connection = nullptr;
        status = api_.dial(&extensions, nullptr, &parameters, 0, nullptr, &connection);
        if (status)
        {
            // RAS can allocate a connection even when dialing fails. Release it
            // without replacing the original error with a cleanup result.
            if (connection) api_.hangUp(connection);
            return Error(status);
        }
        if (cancel.Stop()) return cancel.Failure();
        return ConfirmControlValue(1., 0, cancel, [&] { return ReadState(api_, name, cancel); }, ControlReadbackPause);
    }
    Result Disconnect(const std::wstring& name, const Cancellation& cancel)
    {
        // RasHangUp needs the process-local handle, so enumerate once and hang up
        // every connection whose phone-book entry matches this name.
        DWORD bytes = 0, count = 0;
        DWORD status = api_.enumConnections(nullptr, &bytes, &count);
        if (status != ERROR_SUCCESS && status != ERROR_BUFFER_TOO_SMALL) return Error(status);
        if (!count) return {true, {}, 0};
        std::vector<RASCONNW> connections(count);
        for (auto& connection : connections) connection.dwSize = sizeof(RASCONNW);
        bytes = static_cast<DWORD>(connections.size() * sizeof(RASCONNW));
        status = api_.enumConnections(connections.data(), &bytes, &count);
        if (status != ERROR_SUCCESS) return Error(status);
        bool hungUp = false;
        for (DWORD index = 0; index < count; ++index)
        {
            if (cancel.Stop()) return cancel.Failure();
            if (_wcsicmp(connections[index].szEntryName, name.c_str()) != 0) continue;
            const DWORD result = api_.hangUp(connections[index].hrasconn);
            // A handle that disappeared between enumeration and hang-up means the
            // link is already gone; do not report it as a new failure.
            if (result && result != ERROR_INVALID_HANDLE && result != ERROR_NOT_CONNECTED) return Error(result);
            hungUp = true;
        }
        if (!hungUp) return {true, {}, 0};
        return ConfirmControlValue(0., 0, cancel, [&] { return ReadState(api_, name, cancel); }, ControlReadbackPause);
    }
    VpnApi api_;
};

std::shared_ptr<Backend> CreateVpnBackend(VpnApi api) { return std::make_shared<Vpn>(api); }
}
