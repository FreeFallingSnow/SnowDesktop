#pragma once
#include "system/status_bar/status_bar_input_method_profiles.h"
#include <string>

namespace input_profile_cases
{
// Replace COM activation only. The production reader still queries both real
// interfaces, handles HRESULTs and owns their actual COM reference lifetime.
struct Provider
{
    unsigned creates = 0, queries = 0, destroyed = 0;
    HRESULT createResult = S_OK, managerResult = S_OK, activeResult = S_OK;
    TF_INPUTPROCESSORPROFILE current{};
    std::wstring description = L"first";
    DWORD thread = GetCurrentThreadId();
    bool validApartmentOnRelease = true, correctThread = true;
};
inline thread_local Provider* provider = nullptr;

class Profiles final : public ITfInputProcessorProfiles, public ITfInputProcessorProfileMgr
{
public:
    explicit Profiles(Provider& state) : state_(state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override
    {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ITfInputProcessorProfiles))
            *output = static_cast<ITfInputProcessorProfiles*>(this);
        else if (iid == __uuidof(ITfInputProcessorProfileMgr))
        {
            if (FAILED(state_.managerResult)) return state_.managerResult;
            *output = static_cast<ITfInputProcessorProfileMgr*>(this);
        }
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = --references_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetActiveProfile(REFGUID category, TF_INPUTPROCESSORPROFILE* active) override
    {
        ++state_.queries;
        if (category != GUID_TFCAT_TIP_KEYBOARD || !active) return E_INVALIDARG;
        *active = state_.current; // Also write on failure: caller must clear it.
        return state_.activeResult;
    }
    HRESULT STDMETHODCALLTYPE GetLanguageProfileDescription(REFCLSID, LANGID, REFGUID, BSTR* text) override
    {
        *text = SysAllocString(state_.description.c_str());
        return *text ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Register(REFCLSID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Unregister(REFCLSID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE AddLanguageProfile(REFCLSID, LANGID, REFGUID, const WCHAR*, ULONG, const WCHAR*, ULONG, ULONG) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RemoveLanguageProfile(REFCLSID, LANGID, REFGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumInputProcessorInfo(IEnumGUID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultLanguageProfile(LANGID, REFGUID, CLSID*, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDefaultLanguageProfile(LANGID, REFCLSID, REFGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ActivateLanguageProfile(REFCLSID, LANGID, REFGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetActiveLanguageProfile(REFCLSID, LANGID*, GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentLanguage(LANGID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ChangeCurrentLanguage(LANGID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetLanguageList(LANGID**, ULONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumLanguageProfiles(LANGID, IEnumTfLanguageProfiles**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnableLanguageProfile(REFCLSID, LANGID, REFGUID, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsEnabledLanguageProfile(REFCLSID, LANGID, REFGUID, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnableLanguageProfileByDefault(REFCLSID, LANGID, REFGUID, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SubstituteKeyboardLayout(REFCLSID, LANGID, REFGUID, HKL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ActivateProfile(DWORD, LANGID, REFCLSID, REFGUID, HKL, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DeactivateProfile(DWORD, LANGID, REFCLSID, REFGUID, HKL, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetProfile(DWORD, LANGID, REFCLSID, REFGUID, HKL, TF_INPUTPROCESSORPROFILE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumProfiles(LANGID, IEnumTfInputProcessorProfiles**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReleaseInputProcessor(REFCLSID, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterProfile(REFCLSID, LANGID, REFGUID, const WCHAR*, ULONG, const WCHAR*, ULONG, ULONG, HKL, DWORD, BOOL, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterProfile(REFCLSID, LANGID, REFGUID, DWORD) override { return E_NOTIMPL; }

private:
    ~Profiles()
    {
        ++state_.destroyed;
        ULONG_PTR context = 0;
        state_.validApartmentOnRelease &= SUCCEEDED(CoGetContextToken(&context));
        state_.correctThread &= state_.thread == GetCurrentThreadId();
    }
    Provider& state_;
    ULONG references_ = 1;
};

inline HRESULT WINAPI Create(REFCLSID clsid, IUnknown* outer, DWORD context, REFIID iid, void** output)
{
    *output = nullptr;
    ++provider->creates;
    if (clsid != CLSID_TF_InputProcessorProfiles || outer || context != CLSCTX_INPROC_SERVER) return E_INVALIDARG;
    if (FAILED(provider->createResult)) return provider->createResult;
    auto* object = new Profiles(*provider);
    const HRESULT result = object->QueryInterface(iid, output);
    object->Release();
    return result;
}

template<class Check>
void Run(Check&& check)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // The enclosing native fixture can already own STA. Either apartment is
    // sufficient here; production confines this reader to its MTA worker.
    check(SUCCEEDED(apartment) || apartment == RPC_E_CHANGED_MODE, "profile reader fixture has a COM apartment");
    Provider state;
    provider = &state;
    state.current.langid = 0x0804;
    state.current.hkl = reinterpret_cast<HKL>(0x0804);
    state.current.guidProfile.Data1 = 1;
    {
        snowdesktop::status_bar_input_method::detail::ProfileReader reader(Create);
        TF_INPUTPROCESSORPROFILE active{};
        check(reader.Active(active) == S_OK && active.guidProfile.Data1 == 1, "initial profile is queried");
        state.current.guidProfile.Data1 = 2; state.description = L"second";
        check(reader.Active(active) == S_OK && active.guidProfile.Data1 == 2 && active.hkl == state.current.hkl,
            "same-HKL input program changes are read through the retained interface");
        BSTR description = nullptr;
        check(reader.Description(active, &description) == S_OK && description && std::wstring(description) == L"second",
            "the current input program description is not cached with the interface");
        SysFreeString(description);
        state.activeResult = S_FALSE;
        check(reader.Active(active) == S_FALSE && active.langid == 0 && active.guidProfile == GUID{},
            "no active profile cannot expose the previous profile");
        state.activeResult = S_OK;
        check(reader.Active(active) == S_OK && state.creates == 1 && state.queries == 4,
            "no-profile and successful samples retain one interface while querying every time");
        state.activeResult = E_FAIL;
        check(reader.Active(active) == E_FAIL && active.langid == 0 && state.destroyed == 1,
            "provider failure clears output and releases both interfaces");
        state.activeResult = S_OK;
        check(reader.Active(active) == S_OK && state.creates == 2,
            "the sample after provider failure reconnects and reads current state");
    }
    check(state.destroyed == 2 && state.validApartmentOnRelease && state.correctThread,
        "reader shutdown releases all COM references on the owning initialized thread");
    {
        state.createResult = REGDB_E_CLASSNOTREG;
        snowdesktop::status_bar_input_method::detail::ProfileReader reader(Create);
        TF_INPUTPROCESSORPROFILE active = state.current;
        check(reader.Active(active) == REGDB_E_CLASSNOTREG && active.langid == 0,
            "unavailable TSF activation is reported without stale output");
        state.createResult = S_OK; state.managerResult = E_NOINTERFACE;
        const auto destroyed = state.destroyed;
        check(reader.Active(active) == E_NOINTERFACE && state.destroyed == destroyed + 1,
            "failed manager acquisition releases the partially acquired profiles interface");
        state.managerResult = S_OK;
        check(reader.Active(active) == S_OK && active.guidProfile.Data1 == 2,
            "activation and interface acquisition failures are recoverable on the next sample");
    }
    provider = nullptr;
    if (SUCCEEDED(apartment)) CoUninitialize();
}
}
