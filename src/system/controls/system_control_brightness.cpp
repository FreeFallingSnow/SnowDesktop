#include "system_control_windows.h"
#include "system_control_feedback.h"
#include "system_control_brightness_identity.h"
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <wbemidl.h>
#include <oleauto.h>
#include <mutex>

namespace snowdesktop::system_control::windows
{
namespace
{
struct Bstr
{
    BSTR value;
    explicit Bstr(std::wstring_view text) : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size()))) {}
    ~Bstr() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
struct Variant
{
    VARIANT value{};
    ~Variant() { VariantClear(&value); }
    std::wstring Text() const { return value.vt == VT_BSTR && value.bstrVal ? std::wstring(value.bstrVal, SysStringLen(value.bstrVal)) : L""; }
    long Number(long fallback = -1) const
    { return value.vt == VT_I4 ? value.lVal : value.vt == VT_UI1 ? value.bVal : value.vt == VT_UI4 ? static_cast<long>(value.ulVal) : fallback; }
    std::wstring ArrayText() const
    {
        if (!(value.vt & VT_ARRAY) || (value.vt & VT_BYREF) || !value.parray || SafeArrayGetDim(value.parray) != 1) return {};
        LONG first = 0, last = -1;
        if (FAILED(SafeArrayGetLBound(value.parray, 1, &first)) || FAILED(SafeArrayGetUBound(value.parray, 1, &last)) ||
            last < first || static_cast<std::int64_t>(last) - first > 255) return {};
        std::wstring result;
        for (std::int64_t offset = 0; offset <= static_cast<std::int64_t>(last) - first; ++offset)
        {
            LONG index = static_cast<LONG>(first + offset);
            unsigned long character = 0;
            HRESULT status = E_INVALIDARG;
            switch (value.vt & VT_TYPEMASK)
            {
            case VT_UI2: { USHORT item = 0; status = SafeArrayGetElement(value.parray, &index, &item); character = item; break; }
            case VT_I4: { LONG item = 0; status = SafeArrayGetElement(value.parray, &index, &item); character = static_cast<unsigned long>(item); break; }
            case VT_UI4: status = SafeArrayGetElement(value.parray, &index, &character); break;
            default: return {};
            }
            if (FAILED(status) || character > 0xffff) return {};
            if (!character) break;
            result.push_back(static_cast<wchar_t>(character));
        }
        return result;
    }
};
struct Monitor
{
    std::string id, name;
    HANDLE physical = nullptr;
    std::wstring identity;
    DWORD minimum = 0, maximum = 0;
};
struct Display
{
    HMONITOR handle = nullptr;
    std::wstring device;
    std::vector<BrightnessDisplayTarget> targets;
    bool operator==(const Display&) const = default;
};
struct PhysicalMonitors
{
    std::vector<PHYSICAL_MONITOR> values;
    ~PhysicalMonitors()
    { for (const auto& monitor : values) if (monitor.hPhysicalMonitor) DestroyPhysicalMonitor(monitor.hPhysicalMonitor); }
};
class Brightness final : public Backend
{
    std::mutex mutex_;
    std::vector<Monitor> monitors_;
    std::vector<Display> topology_;
    std::vector<BrightnessDisplayTarget> targets_;
    std::map<std::wstring, std::wstring> friendlyNames_;
    ComPtr<IWbemServices> wmi_;
    bool topologyKnown_ = false;
    bool friendlyNamesLoaded_ = false;
    void Clear()
    {
        for (auto& monitor : monitors_) if (monitor.physical) DestroyPhysicalMonitor(monitor.physical);
        monitors_.clear(); topology_.clear(); targets_.clear(); friendlyNames_.clear();
        topologyKnown_ = false; friendlyNamesLoaded_ = false; wmi_.Reset();
    }
    bool Wmi()
    {
        if (wmi_) return true;
        ComPtr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)))) return false;
        if (FAILED(locator->ConnectServer(Bstr(L"ROOT\\WMI"), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT,
            nullptr, nullptr, &wmi_))) return false;
        if (FAILED(CoSetProxyBlanket(wmi_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE))) { wmi_.Reset(); return false; }
        return true;
    }
    std::vector<ComPtr<IWbemClassObject>> Query(const wchar_t* query, const Cancellation& cancel)
    {
        std::vector<ComPtr<IWbemClassObject>> result;
        if (cancel.Stop() || !Wmi()) return result;
        ComPtr<IEnumWbemClassObject> enumeration;
        if (FAILED(wmi_->ExecQuery(Bstr(L"WQL"), Bstr(query), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumeration))) return result;
        while (!cancel.Stop() && result.size() < 64)
        {
            ComPtr<IWbemClassObject> value; ULONG count = 0;
            const auto status = enumeration->Next(200, 1, &value, &count);
            if (count && value) result.push_back(std::move(value));
            if (FAILED(status) || status == WBEM_S_FALSE) break;
        }
        return result;
    }
    void LoadFriendlyNames(const Cancellation& cancel)
    {
        // Names are optional metadata. Reuse the sampling query's connection;
        // DDC Execute must never start a WMI connection just to label a target.
        if (friendlyNamesLoaded_ || !wmi_ || cancel.Stop()) return;
        const Cancellation metadata{cancel.canceled, (std::min)(cancel.deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500))};
        for (const auto& object : Query(L"SELECT InstanceName,UserFriendlyName FROM WmiMonitorID WHERE Active = TRUE", metadata))
        {
            Variant instance, friendly;
            object->Get(L"InstanceName", 0, &instance.value, nullptr, nullptr);
            object->Get(L"UserFriendlyName", 0, &friendly.value, nullptr, nullptr);
            const auto identity = BrightnessMonitorIdentity(instance.Text(), true);
            if (!identity.empty()) friendlyNames_[identity] = friendly.ArrayText();
        }
        // EDID names are static until the physical topology changes; do not
        // perform another WMI identity query on every slider readback.
        friendlyNamesLoaded_ = true;
    }
    bool Refresh(const Cancellation& cancel)
    {
        if (cancel.Stop()) return false;
        std::vector<Display> topology;
        if (!EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR handle, HDC, LPRECT, LPARAM pointer) -> BOOL {
            MONITORINFOEXW info{}; info.cbSize = sizeof(info);
            if (GetMonitorInfoW(handle, &info)) reinterpret_cast<decltype(topology)*>(pointer)->push_back({handle, info.szDevice, {}});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&topology))) return false;
        for (auto& display : topology)
        {
            // EDD_GET_DEVICE_INTERFACE_NAME links this GDI view to each actual
            // monitor PnP instance, including distinct identical-model panels.
            for (DWORD index = 0; index < 64 && !cancel.Stop(); ++index)
            {
                DISPLAY_DEVICEW target{}; target.cb = sizeof(target);
                if (!EnumDisplayDevicesW(display.device.c_str(), index, &target, EDD_GET_DEVICE_INTERFACE_NAME)) break;
                display.targets.push_back({BrightnessMonitorIdentity(target.DeviceID), target.DeviceString,
                    (target.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0});
            }
        }
        if (cancel.Stop()) return false;
        if (topologyKnown_ && topology == topology_) return true;
        Clear(); topology_ = topology;
        for (const auto& display : topology) targets_.insert(targets_.end(), display.targets.begin(), display.targets.end());
        // WMI exposes internal panels. DDC handles enumerate physical displays,
        // including unsupported ones. Keep every handle as an executable legacy
        // endpoint even when its presentation is merged with the WMI endpoint.
        for (const auto& display : topology)
        {
            if (cancel.Stop()) break;
            DWORD count = 0;
            if (!GetNumberOfPhysicalMonitorsFromHMONITOR(display.handle, &count) || count == 0 || count > 64) continue;
            PhysicalMonitors physical{std::vector<PHYSICAL_MONITOR>(count)};
            if (!GetPhysicalMonitorsFromHMONITOR(display.handle, count, physical.values.data())) continue;
            const auto identity = BrightnessDdcIdentity(display.targets, count);
            for (DWORD index = 0; index < count; ++index)
            {
                Monitor monitor;
                monitor.id = "ddc:" + Utf8(display.device) + ":" + std::to_string(index);
                monitor.identity = identity;
                monitor.name = Utf8(BrightnessMonitorName(L"", physical.values[index].szPhysicalMonitorDescription, identity, display.device));
                monitor.physical = physical.values[index].hPhysicalMonitor;
                monitors_.push_back(std::move(monitor));
                physical.values[index].hPhysicalMonitor = nullptr;
            }
        }
        topologyKnown_ = !cancel.Stop();
        return topologyKnown_;
    }
public:
    ~Brightness() override { Clear(); }
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        std::lock_guard guard(mutex_);
        if (!Refresh(cancel)) return {{"system.display.brightness", Missing(cancel.Stop() ? cancel.Failure().error : "unavailable")}};
        auto value = json::Object(); std::vector<BrightnessMonitorSample> endpoints;
        // Refresh WMI values on demand; do not retain apartment-bound objects.
        const auto internal = Query(L"SELECT * FROM WmiMonitorBrightness WHERE Active = TRUE", cancel);
        LoadFriendlyNames(cancel);
        for (const auto& object : internal)
        {
            Variant instance, current;
            object->Get(L"InstanceName", 0, &instance.value, nullptr, nullptr);
            object->Get(L"CurrentBrightness", 0, &current.value, nullptr, nullptr);
            const auto name = instance.Text(); const auto level = current.Number(); if (name.empty()) continue;
            const auto identity = BrightnessMonitorIdentity(name, true);
            if (BrightnessMonitorActive(identity, targets_) == false) continue;
            const auto friendly = friendlyNames_.find(identity);
            auto item = json::Object(); item.object["id"] = json::Text("wmi:" + Utf8(name));
            item.object["name"] = json::Text(Utf8(BrightnessMonitorName(
                friendly == friendlyNames_.end() ? L"" : friendly->second, L"", identity)));
            item.object["kind"] = json::Text("internal");
            item.object["available"] = json::Boolean(level >= 0 && level <= 100);
            if (level >= 0 && level <= 100) item.object["brightness"] = json::Number(level);
            endpoints.push_back({identity, std::move(item)});
        }
        for (auto& monitor : monitors_)
        {
            if (cancel.Stop()) break;
            DWORD current = 0;
            const bool available = GetMonitorBrightness(monitor.physical, &monitor.minimum, &current, &monitor.maximum) &&
                monitor.maximum > monitor.minimum && current >= monitor.minimum && current <= monitor.maximum;
            const auto friendly = friendlyNames_.find(monitor.identity);
            auto item = json::Object(); item.object["id"] = json::Text(monitor.id);
            item.object["name"] = json::Text(Utf8(BrightnessMonitorName(
                friendly == friendlyNames_.end() ? L"" : friendly->second, Wide(monitor.name), monitor.identity)));
            item.object["kind"] = json::Text("ddc"); item.object["available"] = json::Boolean(available);
            if (available) item.object["brightness"] = json::Number(100.0 * (current - monitor.minimum) / (monitor.maximum - monitor.minimum));
            else item.object["error"] = json::Text("actionUnsupported");
            endpoints.push_back({monitor.identity, std::move(item)});
        }
        value.object["monitors"] = MergeBrightnessMonitors(endpoints);
        return {{"system.display.brightness", Value(std::move(value))}};
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        std::lock_guard guard(mutex_); if (cancel.Stop()) return cancel.Failure();
        if (!Refresh(cancel)) return cancel.Stop() ? cancel.Failure() : Error(ERROR_NOT_READY);
        const auto id = Argument(request, "monitorId"); const double target = Numeric(request, "brightness");
        if (id.starts_with("wmi:"))
        {
            const auto instanceName = Wide(id.substr(4));
            const auto methods = Query(L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active = TRUE", cancel);
            for (const auto& object : methods)
            {
                Variant instance, path;
                object->Get(L"InstanceName", 0, &instance.value, nullptr, nullptr);
                if (instance.Text() != instanceName) continue;
                object->Get(L"__PATH", 0, &path.value, nullptr, nullptr);
                ComPtr<IWbemClassObject> definition, inputClass, input;
                HRESULT status = wmi_->GetObject(Bstr(L"WmiMonitorBrightnessMethods"), 0, nullptr, &definition, nullptr);
                if (SUCCEEDED(status)) status = definition->GetMethod(L"WmiSetBrightness", 0, &inputClass, nullptr);
                if (SUCCEEDED(status)) status = inputClass->SpawnInstance(0, &input);
                if (FAILED(status)) return Status(status);
                Variant level, timeout; level.value.vt = VT_UI1; level.value.bVal = static_cast<BYTE>(std::clamp(target, 0.0, 100.0));
                timeout.value.vt = VT_I4; timeout.value.lVal = 0;
                status = input->Put(L"Brightness", 0, &level.value, CIM_UINT8);
                if (SUCCEEDED(status)) status = input->Put(L"Timeout", 0, &timeout.value, CIM_UINT32);
                if (FAILED(status)) return Status(status);
                ComPtr<IWbemCallResult> call;
                status = wmi_->ExecMethod(Bstr(path.Text()), Bstr(L"WmiSetBrightness"), WBEM_FLAG_RETURN_IMMEDIATELY,
                    nullptr, input.Get(), nullptr, &call);
                if (FAILED(status)) return Status(status);
                ComPtr<IWbemClassObject> output;
                while (!cancel.Stop())
                {
                    status = call->GetResultObject(200, &output);
                    if (status != WBEM_S_TIMEDOUT) break;
                }
                if (cancel.Stop()) return cancel.Failure();
                if (FAILED(status) || !output) return Status(FAILED(status) ? status : E_FAIL);
                Variant returned; output->Get(L"ReturnValue", 0, &returned.value, nullptr, nullptr);
                if (returned.Number() != 0) return Error(static_cast<DWORD>(returned.Number()), "controlRejected");
                const Cancellation settling{cancel.canceled, (std::min)(cancel.deadline,
                    std::chrono::steady_clock::now() + std::chrono::seconds(3))};
                return ConfirmBrightness(level.value.bVal, 2, settling, [&]() -> BrightnessReadback {
                    for (const auto& actual : Query(L"SELECT * FROM WmiMonitorBrightness WHERE Active = TRUE", settling))
                    {
                        Variant actualId, actualLevel; actual->Get(L"InstanceName", 0, &actualId.value, nullptr, nullptr);
                        actual->Get(L"CurrentBrightness", 0, &actualLevel.value, nullptr, nullptr);
                        if (actualId.Text() != instanceName) continue;
                        const auto value = actualLevel.Number();
                        if (value < 0 || value > 100) return {{}, Error(ERROR_INVALID_DATA)};
                        return {static_cast<double>(value), {}};
                    }
                    return {{}, Error(ERROR_NOT_FOUND, "deviceGone")};
                }, BrightnessReadbackPause);
            }
            return Error(ERROR_NOT_FOUND, "deviceGone");
        }
        return WithBrightnessEndpoint(monitors_, id, [&](Monitor& monitor) -> Result {
            DWORD current = 0;
            if (!GetMonitorBrightness(monitor.physical, &monitor.minimum, &current, &monitor.maximum) || monitor.maximum <= monitor.minimum)
                return Error(GetLastError(), "actionUnsupported");
            if (cancel.Stop()) return cancel.Failure();
            const auto level = monitor.minimum + static_cast<DWORD>((monitor.maximum - monitor.minimum) * target / 100.0 + 0.5);
            if (!SetMonitorBrightness(monitor.physical, level)) return Error(GetLastError());
            const Cancellation settling{cancel.canceled, (std::min)(cancel.deadline,
                std::chrono::steady_clock::now() + std::chrono::seconds(3))};
            return ConfirmBrightness(level, 1, settling, [&]() -> BrightnessReadback {
                if (!GetMonitorBrightness(monitor.physical, &monitor.minimum, &current, &monitor.maximum))
                    return {{}, Error(GetLastError())};
                return {static_cast<double>(current), {}};
            }, BrightnessReadbackPause);
        });
    }
    void Release(std::string_view) override { std::lock_guard guard(mutex_); Clear(); }
};
}
std::shared_ptr<Backend> CreateBrightnessBackend() { return std::make_shared<Brightness>(); }
}
