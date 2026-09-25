#include "system_control_windows.h"
#include "audio_endpoint_identity.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <mutex>

namespace snowdesktop::system_control::windows
{
namespace
{
// Private Windows ABI used by the system's endpoint selector. Only the final
// method is called; preserve the preceding vtable slots. Absence is unsupported.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) EndpointPolicy : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0;
};
constexpr GUID PolicyClass{0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
std::wstring RawId(IMMDevice* endpoint)
{
    LPWSTR value = nullptr;
    if (!endpoint || FAILED(endpoint->GetId(&value))) return {};
    const std::wstring result(value ? value : L""); CoTaskMemFree(value); return result;
}
std::string Name(IMMDevice* endpoint)
{
    ComPtr<IPropertyStore> properties;
    if (FAILED(endpoint->OpenPropertyStore(STGM_READ, &properties))) return {};
    PROPVARIANT value{};
    std::string result;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        result = Utf8(value.pwszVal);
    PropVariantClear(&value); return result;
}
ComPtr<IMMDeviceEnumerator> Enumerator()
{
    ComPtr<IMMDeviceEnumerator> result;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&result));
    return result;
}
class Audio final : public Backend
{
    std::mutex mutex_;
public:
    std::map<std::string, Snapshot> Sample(std::string_view, const Cancellation& cancel) override
    {
        std::lock_guard guard(mutex_);
        std::map<std::string, Snapshot> result;
        for (const auto* topic : {"audio.devices", "audio.output.default", "audio.output.volume", "audio.input.volume"})
            result[topic] = Missing();
        auto enumerator = Enumerator();
        if (!enumerator || cancel.Stop()) return result;
        auto devices = json::Object(); auto items = json::Array();
        std::wstring defaults[2];
        for (const auto flow : {eRender, eCapture})
        {
            ComPtr<IMMDevice> endpoint;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &endpoint))) continue;
            defaults[flow == eRender ? 0 : 1] = RawId(endpoint.Get());
            const auto id = OpaqueAudioEndpointId(defaults[flow == eRender ? 0 : 1]);
            if (flow == eRender)
            {
                auto current = json::Object(); current.object["id"] = json::Text(id);
                current.object["name"] = json::Text(Name(endpoint.Get())); current.object["state"] = json::Text("active");
                result["audio.output.default"] = Value(std::move(current));
            }
            ComPtr<IAudioEndpointVolume> control;
            if (FAILED(endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                reinterpret_cast<void**>(control.GetAddressOf())))) continue;
            float volume = 0; BOOL muted = FALSE;
            if (FAILED(control->GetMasterVolumeLevelScalar(&volume)) || FAILED(control->GetMute(&muted))) continue;
            auto state = json::Object(); state.object["endpointId"] = json::Text(id);
            state.object["volume"] = json::Number(std::clamp(static_cast<double>(volume), 0.0, 1.0));
            state.object["muted"] = json::Boolean(muted != FALSE);
            result[flow == eRender ? "audio.output.volume" : "audio.input.volume"] = Value(std::move(state));
        }
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &collection))) return result;
        UINT count = 0; collection->GetCount(&count);
        for (UINT index = 0; index < count && index < 256 && !cancel.Stop(); ++index)
        {
            ComPtr<IMMDevice> endpoint; ComPtr<IMMEndpoint> flowInterface;
            if (FAILED(collection->Item(index, &endpoint)) || FAILED(endpoint.As(&flowInterface))) continue;
            EDataFlow flow = eAll; DWORD state = 0;
            if (FAILED(flowInterface->GetDataFlow(&flow)) || FAILED(endpoint->GetState(&state))) continue;
            const auto raw = RawId(endpoint.Get()); if (raw.empty()) continue;
            auto item = json::Object(); item.object["id"] = json::Text(OpaqueAudioEndpointId(raw));
            item.object["name"] = json::Text(Name(endpoint.Get()));
            item.object["direction"] = json::Text(flow == eRender ? "output" : "input");
            item.object["isDefault"] = json::Boolean(raw == defaults[flow == eRender ? 0 : 1]);
            item.object["available"] = json::Boolean(state == DEVICE_STATE_ACTIVE);
            item.object["state"] = json::Text(state == DEVICE_STATE_ACTIVE ? "active" : state == DEVICE_STATE_DISABLED ? "disabled" :
                state == DEVICE_STATE_UNPLUGGED ? "unplugged" : "notPresent");
            items.array.push_back(std::move(item));
        }
        devices.object["devices"] = std::move(items); result["audio.devices"] = Value(std::move(devices)); return result;
    }
    Result Execute(const Request& request, const Cancellation& cancel) override
    {
        std::lock_guard guard(mutex_);
        if (cancel.Stop()) return cancel.Failure();
        auto enumerator = Enumerator(); if (!enumerator) return Error(ERROR_NOT_SUPPORTED);
        const auto flow = request.name.starts_with("audio.input.") ? eCapture : eRender;
        ComPtr<IMMDevice> endpoint;
        if (request.name.ends_with(".selectDevice"))
        {
            ComPtr<IMMDeviceCollection> collection;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) return Error(ERROR_NOT_FOUND);
            UINT count = 0; collection->GetCount(&count);
            std::wstring chosen;
            for (UINT index = 0; index < count && !cancel.Stop(); ++index)
            {
                if (FAILED(collection->Item(index, &endpoint))) continue;
                const auto raw = RawId(endpoint.Get());
                if (OpaqueAudioEndpointId(raw) == Argument(request, "endpointId")) { chosen = raw; break; }
            }
            if (cancel.Stop()) return cancel.Failure();
            if (chosen.empty()) return Error(ERROR_NOT_FOUND, "deviceGone");
            ComPtr<EndpointPolicy> policy;
            const auto created = CoCreateInstance(PolicyClass, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy));
            if (FAILED(created)) return Error(static_cast<DWORD>(created), "actionUnsupported");
            // Match the multimedia endpoint used by existing API v2 volume tasks.
            for (const auto role : {eConsole, eMultimedia, eCommunications})
            { const auto status = policy->SetDefaultEndpoint(chosen.c_str(), role); if (FAILED(status)) return Status(status); }
            if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &endpoint))) return Error(ERROR_NOT_FOUND, "deviceGone");
            return RawId(endpoint.Get()) == chosen ? Result{true, {}, 0} : Error(ERROR_INVALID_STATE, "stateMismatch");
        }
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &endpoint))) return Error(ERROR_NOT_FOUND, "deviceGone");
        ComPtr<IAudioEndpointVolume> control;
        auto status = endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void**>(control.GetAddressOf()));
        if (FAILED(status)) return Status(status);
        if (request.name.ends_with(".setVolume"))
        {
            const float target = static_cast<float>(std::clamp(Numeric(request, "volume"), 0.0, 1.0));
            status = control->SetMasterVolumeLevelScalar(target, nullptr);
            if (FAILED(status)) return Status(status);
            float actual = 0; status = control->GetMasterVolumeLevelScalar(&actual);
            if (FAILED(status)) return Status(status);
            return std::abs(actual - target) <= 0.02f ? Result{true, {}, 0} : Error(ERROR_INVALID_STATE, "stateMismatch");
        }
        const BOOL target = Argument(request, "muted") == "1";
        status = control->SetMute(target, nullptr); if (FAILED(status)) return Status(status);
        BOOL actual = FALSE; status = control->GetMute(&actual); if (FAILED(status)) return Status(status);
        return actual == target ? Result{true, {}, 0} : Error(ERROR_INVALID_STATE, "stateMismatch");
    }
    void Release(std::string_view) override {}
};
}
std::shared_ptr<Backend> CreateAudioBackend() { return std::make_shared<Audio>(); }
}
