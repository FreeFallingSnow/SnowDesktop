#include "system_control_windows.h"
#include <wlanapi.h>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace snowdesktop::system_control::windows
{
namespace
{
struct WlanMemory
{
    void* value = nullptr;
    ~WlanMemory() { if (value) WlanFreeMemory(value); }
    template<class T> T* As() const { return static_cast<T*>(value); }
    template<class T> T** Out() { return reinterpret_cast<T**>(&value); }
};
struct Wlan
{
    HANDLE handle = nullptr;
    DWORD status;
    Wlan() { DWORD version = 0; status = WlanOpenHandle(2, nullptr, &version, &handle); }
    // Closing unregisters notifications and waits for callbacks to return.
    ~Wlan() { if (handle) WlanCloseHandle(handle, nullptr); }
};
std::string Hex(const DOT11_SSID& ssid)
{
    constexpr char digits[] = "0123456789ABCDEF"; std::string result;
    for (ULONG i = 0; i < (std::min)(ssid.uSSIDLength, 32ul); ++i)
    { result.push_back(digits[ssid.ucSSID[i] >> 4]); result.push_back(digits[ssid.ucSSID[i] & 15]); }
    return result;
}
std::string SsidName(const DOT11_SSID& ssid)
{
    const std::string value(reinterpret_cast<const char*>(ssid.ucSSID), (std::min)(ssid.uSSIDLength, 32ul));
    // SSIDs are bytes, not necessarily UTF-8. The hex identity remains exact.
    if (value.find('\0') == std::string::npos && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) > 0)
        return value;
    return Hex(ssid);
}
std::string NetworkId(const WLAN_AVAILABLE_NETWORK& network)
{ return Hex(network.dot11Ssid) + ":" + std::to_string(network.dot11DefaultAuthAlgorithm) + ":" + std::to_string(network.dot11DefaultCipherAlgorithm); }
std::string Security(const WLAN_AVAILABLE_NETWORK& network)
{
    if (!network.bSecurityEnabled) return "open";
    if (network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_RSNA_PSK && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_CCMP) return "wpa2";
    if (network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_WPA3_SAE) return "wpa3";
    return "system";
}
DWORD Query(HANDLE handle, const GUID& id, WLAN_INTF_OPCODE opcode, WlanMemory& memory)
{ DWORD size = 0; return WlanQueryInterface(handle, &id, opcode, nullptr, &size, &memory.value, nullptr); }
struct Notifications
{
    GUID interfaceId{};
    std::mutex mutex;
    std::condition_variable changed;
    bool scanning = false, complete = false;
    DWORD reason = 0;
    std::wstring profile;
    static void WINAPI Receive(PWLAN_NOTIFICATION_DATA data, PVOID context)
    {
        auto& self = *static_cast<Notifications*>(context);
        if (!data || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM || data->InterfaceGuid != self.interfaceId) return;
        std::lock_guard guard(self.mutex);
        if (self.scanning)
        {
            if (data->NotificationCode == wlan_notification_acm_scan_complete) self.complete = true;
            else if (data->NotificationCode == wlan_notification_acm_scan_fail && data->pData && data->dwDataSize >= sizeof(DWORD))
            { std::memcpy(&self.reason, data->pData, sizeof(DWORD)); self.complete = true; }
        }
        else if ((data->NotificationCode == wlan_notification_acm_connection_complete || data->NotificationCode == wlan_notification_acm_connection_attempt_fail) &&
            data->pData && data->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA))
        {
            const auto* value = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData);
            if (std::wstring_view(value->strProfileName, wcsnlen_s(value->strProfileName, WLAN_MAX_NAME_LENGTH)) == self.profile)
            { self.reason = value->wlanReasonCode; self.complete = true; }
        }
        self.changed.notify_all();
    }
    Result Wait(const Cancellation& cancel)
    {
        std::unique_lock guard(mutex);
        while (!complete && !cancel.Stop()) changed.wait_for(guard, std::chrono::milliseconds(100));
        if (cancel.Stop()) return cancel.Failure();
        return reason == 0 ? Result{true, {}, 0} : Error(reason, scanning ? "scanFailed" : "connectionFailed");
    }
};
void AppendXml(std::wstring& output, std::wstring_view value)
{
    for (const wchar_t character : value)
    {
        switch (character)
        {
        case L'&': output += L"&amp;"; break;
        case L'<': output += L"&lt;"; break;
        case L'>': output += L"&gt;"; break;
        case L'\"': output += L"&quot;"; break;
        case L'\'': output += L"&apos;"; break;
        default: output += character; break;
        }
    }
}
struct ProfileXml
{
    std::wstring text;
    ~ProfileXml() { if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t)); }
};
class Wifi final : public Backend
{
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        Wlan wlan; if (wlan.status) return {{"network.wifi", Missing(wlan.status == ERROR_ACCESS_DENIED ? "accessDenied" : "unavailable")}};
        WlanMemory memory; const DWORD status = WlanEnumInterfaces(wlan.handle, nullptr, memory.Out<WLAN_INTERFACE_INFO_LIST>());
        if (status) return {{"network.wifi", Missing(status == ERROR_ACCESS_DENIED ? "accessDenied" : "unavailable")}};
        const auto* list = memory.As<WLAN_INTERFACE_INFO_LIST>(); auto value = json::Object(); auto interfaces = json::Array();
        for (DWORD i = 0; i < list->dwNumberOfItems && i < 64 && !cancel.Stop(); ++i)
        {
            const auto& entry = list->InterfaceInfo[i]; auto item = json::Object();
            item.object["id"] = json::Text(Guid(entry.InterfaceGuid)); item.object["name"] = json::Text(Utf8(entry.strInterfaceDescription));
            item.object["connected"] = json::Boolean(entry.isState == wlan_interface_state_connected);
            WlanMemory radio;
            DWORD queryStatus = Query(wlan.handle, entry.InterfaceGuid, wlan_intf_opcode_radio_state, radio);
            if (!queryStatus)
            {
                const auto* state = radio.As<WLAN_RADIO_STATE>(); bool software = false, hardware = false;
                for (DWORD p = 0; p < state->dwNumberOfPhys && p < WLAN_MAX_PHY_INDEX; ++p)
                { software |= state->PhyRadioState[p].dot11SoftwareRadioState == dot11_radio_state_on; hardware |= state->PhyRadioState[p].dot11HardwareRadioState == dot11_radio_state_on; }
                item.object["enabled"] = json::Boolean(software); item.object["hardwareEnabled"] = json::Boolean(hardware);
            }
            WlanMemory networks; auto entries = json::Array();
            // Read cached results only. Active scans are explicit tasks.
            queryStatus = WlanGetAvailableNetworkList(wlan.handle, &entry.InterfaceGuid,
                WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, nullptr, networks.Out<WLAN_AVAILABLE_NETWORK_LIST>());
            item.object["available"] = json::Boolean(queryStatus == ERROR_SUCCESS);
            if (queryStatus) item.object["error"] = json::Text(queryStatus == ERROR_ACCESS_DENIED ? "accessDenied" : "unavailable");
            else
            {
                const auto* available = networks.As<WLAN_AVAILABLE_NETWORK_LIST>();
                for (DWORD n = 0; n < available->dwNumberOfItems && n < 512; ++n)
                {
                    const auto& network = available->Network[n]; auto entryValue = json::Object();
                    entryValue.object["id"] = json::Text(NetworkId(network)); entryValue.object["ssid"] = json::Text(SsidName(network.dot11Ssid));
                    entryValue.object["signal"] = json::Number(network.wlanSignalQuality); entryValue.object["security"] = json::Text(Security(network));
                    entryValue.object["connected"] = json::Boolean((network.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0);
                    entryValue.object["connectable"] = json::Boolean(network.bNetworkConnectable != FALSE);
                    if (network.strProfileName[0]) entryValue.object["profileName"] = json::Text(Utf8(network.strProfileName));
                    entries.array.push_back(std::move(entryValue));
                }
            }
            item.object["networks"] = std::move(entries);
            WlanMemory profiles; auto saved = json::Array();
            if (WlanGetProfileList(wlan.handle, &entry.InterfaceGuid, nullptr, profiles.Out<WLAN_PROFILE_INFO_LIST>()) == ERROR_SUCCESS)
            {
                const auto* profileList = profiles.As<WLAN_PROFILE_INFO_LIST>();
                for (DWORD n = 0; n < profileList->dwNumberOfItems && n < 512; ++n)
                {
                    auto profile = json::Object(); profile.object["name"] = json::Text(Utf8(profileList->ProfileInfo[n].strProfileName));
                    profile.object["managed"] = json::Boolean((profileList->ProfileInfo[n].dwFlags & WLAN_PROFILE_GROUP_POLICY) != 0);
                    saved.array.push_back(std::move(profile));
                }
            }
            item.object["profiles"] = std::move(saved); interfaces.array.push_back(std::move(item));
        }
        value.object["interfaces"] = std::move(interfaces); return {{"network.wifi", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return cancel.Failure();
        // Notification state outlives the handle, including in-flight callbacks.
        Notifications notifications; Wlan wlan; if (wlan.status) return Error(wlan.status);
        GUID id{}; if (!ParseGuid(Argument(request, "interfaceId"), id)) return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
        notifications.interfaceId = id;
        WlanMemory interfaces; DWORD status = WlanEnumInterfaces(wlan.handle, nullptr, interfaces.Out<WLAN_INTERFACE_INFO_LIST>());
        if (status) return Error(status);
        bool present = false; const auto* list = interfaces.As<WLAN_INTERFACE_INFO_LIST>();
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) present |= list->InterfaceInfo[i].InterfaceGuid == id;
        if (!present) return Error(ERROR_NOT_FOUND, "deviceGone");
        if (request.name == "network.wifi.setRadio")
        {
            WlanMemory radio; status = Query(wlan.handle, id, wlan_intf_opcode_radio_state, radio); if (status) return Error(status);
            const auto desired = Argument(request, "enabled") == "1" ? dot11_radio_state_on : dot11_radio_state_off;
            const auto* state = radio.As<WLAN_RADIO_STATE>();
            if (!state->dwNumberOfPhys) return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
            for (DWORD p = 0; p < state->dwNumberOfPhys && p < WLAN_MAX_PHY_INDEX; ++p)
            {
                if (cancel.Stop()) return cancel.Failure();
                auto phy = state->PhyRadioState[p]; phy.dot11SoftwareRadioState = desired;
                status = WlanSetInterface(wlan.handle, &id, wlan_intf_opcode_radio_state, sizeof(phy), &phy, nullptr);
                if (status) return Error(status);
            }
            WlanMemory actual; status = Query(wlan.handle, id, wlan_intf_opcode_radio_state, actual); if (status) return Error(status);
            const auto* readback = actual.As<WLAN_RADIO_STATE>();
            for (DWORD p = 0; p < readback->dwNumberOfPhys && p < WLAN_MAX_PHY_INDEX; ++p)
                if (readback->PhyRadioState[p].dot11SoftwareRadioState != desired) return Error(ERROR_INVALID_STATE, "stateMismatch");
            return {true, {}, 0};
        }
        if (request.name == "network.wifi.forget")
        {
            if (!request.hostConfirmed) return Error(ERROR_CANCELLED, "confirmationRequired");
            const auto name = Wide(Argument(request, "profileName"));
            status = WlanDeleteProfile(wlan.handle, &id, name.c_str(), nullptr); if (status) return Error(status);
            WlanMemory profiles; status = WlanGetProfileList(wlan.handle, &id, nullptr, profiles.Out<WLAN_PROFILE_INFO_LIST>());
            if (status) return Error(status);
            const auto* saved = profiles.As<WLAN_PROFILE_INFO_LIST>();
            for (DWORD i = 0; i < saved->dwNumberOfItems; ++i) if (name == saved->ProfileInfo[i].strProfileName) return Error(ERROR_INVALID_STATE, "stateMismatch");
            return {true, {}, 0};
        }
        if (request.name == "network.wifi.disconnect")
        {
            status = WlanDisconnect(wlan.handle, &id, nullptr); if (status) return Error(status);
            while (!cancel.Stop())
            {
                WlanMemory state; status = Query(wlan.handle, id, wlan_intf_opcode_interface_state, state);
                if (status) return Error(status, "deviceGone");
                if (*state.As<WLAN_INTERFACE_STATE>() == wlan_interface_state_disconnected) return {true, {}, 0};
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return cancel.Failure();
        }
        notifications.scanning = request.name == "network.wifi.scan";
        status = WlanRegisterNotification(wlan.handle, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, Notifications::Receive, &notifications, nullptr, nullptr);
        if (status) return Error(status);
        if (notifications.scanning)
        {
            status = WlanScan(wlan.handle, &id, nullptr, nullptr, nullptr); if (status) return Error(status);
            return notifications.Wait(cancel);
        }
        if (request.name != "network.wifi.connect") return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
        std::wstring profile = Wide(Argument(request, "profileName")); bool created = false;
        if (profile.empty())
        {
            DOT11_SSID ssid{}; auto security = Argument(request, "security");
            const auto target = Argument(request, "networkId");
            if (!target.empty())
            {
                WlanMemory networks; status = WlanGetAvailableNetworkList(wlan.handle, &id,
                    WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, nullptr, networks.Out<WLAN_AVAILABLE_NETWORK_LIST>());
                if (status) return Error(status);
                const auto* available = networks.As<WLAN_AVAILABLE_NETWORK_LIST>(); bool found = false;
                for (DWORD n = 0; n < available->dwNumberOfItems; ++n)
                    if (NetworkId(available->Network[n]) == target) { ssid = available->Network[n].dot11Ssid; security = Security(available->Network[n]); found = true; break; }
                if (!found) return Error(ERROR_NOT_FOUND, "networkGone");
            }
            else
            {
                const auto name = Argument(request, "ssid"); if (name.empty() || name.size() > 32) return Error(ERROR_INVALID_PARAMETER, "invalidArguments");
                ssid.uSSIDLength = static_cast<ULONG>(name.size()); std::memcpy(ssid.ucSSID, name.data(), name.size());
            }
            if (security != "open" && security != "wpa2" && security != "wpa3") return Error(ERROR_NOT_SUPPORTED, "systemSettingsRequired");
            const auto password = request.password.View();
            if (security != "open" && (password.size() < 8 || password.size() > 63 ||
                std::any_of(password.begin(), password.end(), [](wchar_t ch) { return ch < 32 || ch > 126; })))
                return Error(ERROR_INVALID_PASSWORD, "passwordRequired");
            GUID profileId{}; if (FAILED(CoCreateGuid(&profileId))) return Error(ERROR_GEN_FAILURE);
            profile = L"SnowDesktop-" + Wide(Guid(profileId));
            ProfileXml xml;
            xml.text.reserve(4096);
            xml.text = L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>";
            AppendXml(xml.text, profile); xml.text += L"</name><SSIDConfig><SSID><hex>" + Wide(Hex(ssid)) + L"</hex></SSID><nonBroadcast>";
            xml.text += Argument(request, "hidden") == "1" ? L"true" : L"false";
            xml.text += L"</nonBroadcast></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security><authEncryption><authentication>";
            xml.text += security == "open" ? L"open" : security == "wpa3" ? L"WPA3SAE" : L"WPA2PSK";
            xml.text += L"</authentication><encryption>"; xml.text += security == "open" ? L"none" : L"AES";
            xml.text += L"</encryption><useOneX>false</useOneX></authEncryption>";
            if (security != "open")
            {
                xml.text += L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>";
                AppendXml(xml.text, password); xml.text += L"</keyMaterial></sharedKey>";
            }
            xml.text += L"</security></MSM></WLANProfile>";
            DWORD reason = 0;
            status = WlanSetProfile(wlan.handle, &id, WLAN_PROFILE_USER, xml.text.c_str(), nullptr, FALSE, nullptr, &reason);
            if (status) return Error(status, "profileRejected"); created = true;
        }
        { std::lock_guard guard(notifications.mutex); notifications.profile = profile; }
        WLAN_CONNECTION_PARAMETERS parameters{}; parameters.wlanConnectionMode = wlan_connection_mode_profile;
        parameters.strProfile = profile.c_str(); parameters.dot11BssType = dot11_BSS_type_infrastructure;
        if (Argument(request, "hidden") == "1") parameters.dwFlags = WLAN_CONNECTION_HIDDEN_NETWORK;
        status = WlanConnect(wlan.handle, &id, &parameters, nullptr);
        Result result = status ? Error(status, "connectionFailed") : notifications.Wait(cancel);
        if (result.ok)
        {
            WlanMemory actual; status = Query(wlan.handle, id, wlan_intf_opcode_current_connection, actual);
            if (status) result = Error(status);
            else
            {
                const auto* connection = actual.As<WLAN_CONNECTION_ATTRIBUTES>();
                if (connection->isState != wlan_interface_state_connected || profile != connection->strProfileName)
                    result = Error(ERROR_INVALID_STATE, "stateMismatch");
            }
        }
        if (!result.ok && created) WlanDeleteProfile(wlan.handle, &id, profile.c_str(), nullptr);
        return result;
    }
    void Release(std::string_view) override {}
};
}
std::shared_ptr<Backend> CreateWifiBackend() { return std::make_shared<Wifi>(); }
}
