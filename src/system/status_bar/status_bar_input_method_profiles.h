#pragma once
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>

namespace snowdesktop::status_bar_input_method::detail
{
// Owned by one sampler thread, inside its COM apartment. Retain interfaces,
// never the active profile: different input programs can share the same HKL.
class ProfileReader
{
public:
    explicit ProfileReader(decltype(&CoCreateInstance) create = &CoCreateInstance) noexcept
        : create_(create) {}
    ProfileReader(const ProfileReader&) = delete;
    ProfileReader& operator=(const ProfileReader&) = delete;

    HRESULT Active(TF_INPUTPROCESSORPROFILE& active) noexcept
    {
        active = {};
        if (!manager_)
        {
            HRESULT result = create_(CLSID_TF_InputProcessorProfiles, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&profiles_));
            if (SUCCEEDED(result)) result = profiles_.As(&manager_);
            if (FAILED(result)) { Reset(); return result; }
        }
        const HRESULT result = manager_->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
        if (result != S_OK) active = {};
        // S_FALSE is a valid "no active profile", not a broken interface.
        // Failed providers are discarded so the next sample can reconnect.
        if (FAILED(result)) Reset();
        return result;
    }

    HRESULT Description(const TF_INPUTPROCESSORPROFILE& active, BSTR* description) const noexcept
    {
        if (!description) return E_POINTER;
        *description = nullptr;
        return profiles_ ? profiles_->GetLanguageProfileDescription(active.clsid,
            active.langid, active.guidProfile, description) : E_UNEXPECTED;
    }

private:
    void Reset() noexcept { manager_.Reset(); profiles_.Reset(); }
    decltype(&CoCreateInstance) create_;
    Microsoft::WRL::ComPtr<ITfInputProcessorProfiles> profiles_;
    Microsoft::WRL::ComPtr<ITfInputProcessorProfileMgr> manager_;
};
}
