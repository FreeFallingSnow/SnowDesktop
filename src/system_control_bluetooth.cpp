#include "system_control_windows.h"
#include "system_control_bluetooth_sampling.h"
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksproxy.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <cwctype>
#include <mutex>
#include <set>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace snowdesktop::system_control::windows
{
namespace
{
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Radios;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
// Protocol constants referenced by the pinned YASB Bluetooth service; see its
// MIT notice in third_party/yasb. No device is selected by its display name.
constexpr GUID BluetoothAudio{0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};
constexpr DEVPROPKEY BatteryKey{{0x104ea319, 0x6ee2, 0x4701, {0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5}}, 2};
std::wstring Compact(std::wstring_view text)
{
    std::wstring result;
    for (const wchar_t ch : text) if (ch != L':' && ch != L'-' && ch != L'_') result.push_back(static_cast<wchar_t>(towupper(ch)));
    return result;
}
std::wstring Mac(std::uint64_t address)
{ wchar_t value[16]{}; swprintf_s(value, L"%012llX", static_cast<unsigned long long>(address)); return value; }
struct AudioFilter { std::wstring endpointId, filterId; };
std::vector<AudioFilter> AudioFilters(const std::wstring& mac, const Cancellation& cancel)
{
    std::vector<AudioFilter> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) return result;
    ComPtr<IMMDeviceCollection> endpoints;
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &endpoints))) return result;
    UINT count = 0; endpoints->GetCount(&count);
    for (UINT i = 0; i < count && i < 256 && !cancel.Stop(); ++i)
    {
        ComPtr<IMMDevice> device; ComPtr<IDeviceTopology> topology;
        if (FAILED(endpoints->Item(i, &device)) || FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER,
            nullptr, reinterpret_cast<void**>(topology.GetAddressOf())))) continue;
        UINT connectors = 0; topology->GetConnectorCount(&connectors);
        for (UINT c = 0; c < connectors && c < 64; ++c)
        {
            ComPtr<IConnector> connector; LPWSTR other = nullptr;
            if (FAILED(topology->GetConnector(c, &connector)) || FAILED(connector->GetDeviceIdConnectedTo(&other))) continue;
            const std::wstring filter(other ? other : L""); CoTaskMemFree(other);
            const auto compact = Compact(filter);
            if (compact.find(L"BTH") == std::wstring::npos || compact.find(mac) == std::wstring::npos) continue;
            LPWSTR endpoint = nullptr; if (FAILED(device->GetId(&endpoint))) continue;
            result.push_back({endpoint ? endpoint : L"", filter}); CoTaskMemFree(endpoint);
        }
    }
    return result;
}
std::optional<bool> AudioConnected(const std::vector<AudioFilter>& filters)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)))) return {};
    bool found = false;
    for (const auto& filter : filters)
    {
        ComPtr<IMMDevice> endpoint; DWORD state = 0;
        if (FAILED(enumerator->GetDevice(filter.endpointId.c_str(), &endpoint)) || FAILED(endpoint->GetState(&state))) continue;
        found = true; if (state == DEVICE_STATE_ACTIVE) return true;
    }
    return found ? std::optional(false) : std::nullopt;
}
std::optional<unsigned> Battery(const std::wstring& mac, const Cancellation& cancel)
{
    const auto devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) return {};
    std::optional<unsigned> result;
    for (DWORD index = 0; index < 16384 && !cancel.Stop(); ++index)
    {
        SP_DEVINFO_DATA info{}; info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInfo(devices, index, &info)) break;
        wchar_t id[1024]{};
        if (!SetupDiGetDeviceInstanceIdW(devices, &info, id, 1024, nullptr) || Compact(id).find(mac) == std::wstring::npos) continue;
        DEVPROPTYPE type = 0; BYTE level = 255;
        if (SetupDiGetDevicePropertyW(devices, &info, &BatteryKey, &type, &level, sizeof(level), nullptr, 0) && type == DEVPROP_TYPE_BYTE && level <= 100)
        { result = level; break; }
    }
    SetupDiDestroyDeviceInfoList(devices); return result;
}
class Bluetooth final : public Backend
{
    std::mutex mutex_;
    struct Cached { std::optional<unsigned> battery; bool audio = false; std::chrono::steady_clock::time_point expires; };
    std::map<std::wstring, Cached> cache_;
    Cached Info(std::uint64_t address, const Cancellation& cancel)
    {
        const auto mac = Mac(address);
        { std::lock_guard guard(mutex_); const auto found = cache_.find(mac);
          if (found != cache_.end() && found->second.expires > std::chrono::steady_clock::now()) return found->second; }
        Cached result; result.audio = !AudioFilters(mac, cancel).empty(); result.battery = Battery(mac, cancel);
        result.expires = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        { std::lock_guard guard(mutex_); if (cache_.size() > 256) cache_.clear(); cache_[mac] = result; }
        return result;
    }
    BluetoothCollection ReadRadios(const Cancellation& cancel)
    {
        try
        {
            auto radios = json::Array();
            for (const auto& info : Await(DeviceInformation::FindAllAsync(Radio::GetDeviceSelector()), cancel))
            {
                if (cancel.Stop()) break;
                const auto radio = Await(Radio::FromIdAsync(info.Id()), cancel);
                if (!radio || radio.Kind() != RadioKind::Bluetooth) continue;
                auto item = json::Object(); item.object["id"] = json::Text(winrt::to_string(info.Id()));
                item.object["name"] = json::Text(winrt::to_string(radio.Name()));
                const auto state = radio.State();
                item.object["enabled"] = json::Boolean(state == RadioState::On);
                item.object["available"] = json::Boolean(state == RadioState::On || state == RadioState::Off);
                radios.array.push_back(std::move(item));
            }
            return {std::move(radios), {}};
        }
        catch (const winrt::hresult_error& error)
        { return {json::Array(), cancel.Stop() ? cancel.Failure().error : error.code() == E_ACCESSDENIED ? "accessDenied" : "unavailable"}; }
    }
    BluetoothCollection ReadDevices(const Cancellation& cancel)
    {
        try
        {
            auto devices = json::Array();
            std::set<std::uint64_t> seen;
            for (const bool le : {false, true})
            {
                const auto selector = le ? BluetoothLEDevice::GetDeviceSelectorFromPairingState(true) : BluetoothDevice::GetDeviceSelectorFromPairingState(true);
                const auto list = Await(DeviceInformation::FindAllAsync(selector), cancel);
                for (const auto& info : list)
                {
                    if (cancel.Stop() || devices.array.size() >= 128) break;
                    std::uint64_t address = 0; bool connected = false;
                    if (le)
                    {
                        const auto device = Await(BluetoothLEDevice::FromIdAsync(info.Id()), cancel); if (!device) continue;
                        address = device.BluetoothAddress(); connected = device.ConnectionStatus() == BluetoothConnectionStatus::Connected; device.Close();
                    }
                    else
                    {
                        const auto device = Await(BluetoothDevice::FromIdAsync(info.Id()), cancel); if (!device) continue;
                        address = device.BluetoothAddress(); connected = device.ConnectionStatus() == BluetoothConnectionStatus::Connected; device.Close();
                    }
                    if (!address || !seen.insert(address).second) continue;
                    const auto details = Info(address, cancel);
                    auto item = json::Object(); item.object["id"] = json::Text(winrt::to_string(info.Id()));
                    item.object["name"] = json::Text(winrt::to_string(info.Name())); item.object["address"] = json::Text(Utf8(Mac(address)));
                    item.object["connected"] = json::Boolean(connected); item.object["paired"] = json::Boolean(true);
                    item.object["canConnect"] = json::Boolean(details.audio); item.object["canDisconnect"] = json::Boolean(details.audio);
                    item.object["lowEnergy"] = json::Boolean(le);
                    if (details.battery) item.object["batteryPercent"] = json::Number(*details.battery);
                    devices.array.push_back(std::move(item));
                }
            }
            return {std::move(devices), {}};
        }
        catch (const winrt::hresult_error& error)
        { return {json::Array(), cancel.Stop() ? cancel.Failure().error : error.code() == E_ACCESSDENIED ? "accessDenied" : "unavailable"}; }
    }
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        return {{"bluetooth.devices", SampleBluetooth([&] { return ReadRadios(cancel); },
            [&] { return ReadDevices(cancel); }, cancel)}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        if (cancel.Stop()) return cancel.Failure();
        try
        {
            if (request.name == "bluetooth.setRadio")
            {
                if (Await(Radio::RequestAccessAsync(), cancel) != RadioAccessStatus::Allowed) return Error(ERROR_ACCESS_DENIED);
                const auto radio = Await(Radio::FromIdAsync(Wide(Argument(request, "radioId"))), cancel);
                if (!radio || radio.Kind() != RadioKind::Bluetooth) return Error(ERROR_NOT_FOUND, "deviceGone");
                const auto target = Argument(request, "enabled") == "1" ? RadioState::On : RadioState::Off;
                if (Await(radio.SetStateAsync(target), cancel) != RadioAccessStatus::Allowed) return Error(ERROR_ACCESS_DENIED);
                while (!cancel.Stop())
                {
                    if (radio.State() == target) return {true, {}, 0};
                    if (radio.State() == RadioState::Disabled) return Error(ERROR_NOT_SUPPORTED, "actionUnsupported");
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return cancel.Failure();
            }
            const auto id = Wide(Argument(request, "deviceId"));
            std::uint64_t address = 0;
            // Resolve from the supplied system identity; never infer a device
            // address from a component-provided name or substring.
            const auto classic = Await(BluetoothDevice::FromIdAsync(id), cancel);
            if (classic) { address = classic.BluetoothAddress(); classic.Close(); }
            if (!address) return Error(ERROR_NOT_SUPPORTED, "systemSettingsRequired");
            const auto filters = AudioFilters(Mac(address), cancel);
            if (filters.empty()) return Error(ERROR_NOT_SUPPORTED, "systemSettingsRequired");
            ComPtr<IMMDeviceEnumerator> enumerator;
            HRESULT status = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
            if (FAILED(status)) return Status(status);
            std::set<std::wstring> sent; const bool connect = request.name == "bluetooth.connect";
            for (const auto& filter : filters)
            {
                if (cancel.Stop()) return cancel.Failure();
                if (sent.contains(filter.filterId)) continue;
                ComPtr<IMMDevice> device; ComPtr<IKsControl> ks;
                if (FAILED(enumerator->GetDevice(filter.filterId.c_str(), &device))) continue;
                status = device->Activate(__uuidof(IKsControl), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(ks.GetAddressOf()));
                if (FAILED(status)) continue;
                KSPROPERTY property{}; property.Set = BluetoothAudio; property.Id = connect ? 0u : 1u; property.Flags = KSPROPERTY_TYPE_GET;
                ULONG returned = 0; status = ks->KsProperty(&property, sizeof(property), nullptr, 0, &returned);
                if (SUCCEEDED(status)) sent.insert(filter.filterId);
            }
            if (sent.empty()) return Error(ERROR_NOT_SUPPORTED, "controlRejected");
            while (!cancel.Stop())
            {
                const auto connected = AudioConnected(filters);
                if (!connected) return Error(ERROR_NOT_FOUND, "deviceGone");
                if (*connected == connect) { std::lock_guard guard(mutex_); cache_.erase(Mac(address)); return {true, {}, 0}; }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return cancel.Failure();
        }
        catch (const winrt::hresult_error& error)
        { return cancel.Stop() ? cancel.Failure() : Error(static_cast<DWORD>(error.code().value)); }
    }
    void Release(std::string_view) override { std::lock_guard guard(mutex_); cache_.clear(); }
};
}
std::shared_ptr<Backend> CreateBluetoothBackend() { return std::make_shared<Bluetooth>(); }
}
