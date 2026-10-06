#include "status_bar_input_method.h"
#include "ui/menu/modern_menu.h"
#include "status_bar_input_method_native.h"
#include "diagnostics/diagnostic_log.h"
#include <wrl/client.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace snowdesktop::status_bar_input_method
{
namespace
{
// Windows' private InputSwitch ABI, restricted to its common Win10/Win11
// prefix through ClickImeModeItem. Later slots/structures changed in Win11 and
// MUST NOT be called here; opaque profile data is declared but never queried.
// ABI reference: https://github.com/valinet/ExplorerPatcher/blob/master/ExplorerPatcher/InputSwitch.h
// No hotkey registration, callbacks, patches or forced global profile changes.
struct __declspec(uuid("b9bc2a50-43c3-41aa-a082-5db14e184bae")) InputSwitch : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Init(int clientType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE ShowInputSwitch(const RECT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProfileCount(UINT*, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentProfile(void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterHotkeys() = 0;
    virtual HRESULT STDMETHODCALLTYPE ClickImeModeItem(int clickType, POINT, const RECT*) = 0;
};
constexpr CLSID kInputSwitch{0xb9bc2a50, 0x43c3, 0x41aa, {0xa0, 0x86, 0x5d, 0xb1, 0x4e, 0x18, 0x4b, 0xae}};
// IMM's scalar query commands, omitted from current public imm.h. Keep them
// local to this compatibility boundary; unsuccessful queries remain unknown.
constexpr WPARAM kGetConversionMode = 0x0001, kGetOpenStatus = 0x0005;

struct Apartment
{
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    Apartment() = default;
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

struct NativeState
{
    // Released after the interface, even if the bar is destroyed from a nested
    // native menu/COM message loop or Run() releases the app's OLE reference.
    Apartment apartment;
    Microsoft::WRL::ComPtr<InputSwitch> picker;
};

Snapshot Target()
{
    Snapshot result;
    result.foreground = GetForegroundWindow();
    if (!result.foreground) return {};
    result.thread = GetWindowThreadProcessId(result.foreground, nullptr);
    if (!result.thread) return {};
    GUITHREADINFO info{sizeof(info)};
    if (GetGUIThreadInfo(result.thread, &info))
    {
        result.menuActive = (info.flags & GUI_INMENUMODE) != 0;
        // XAML islands and attached GUI queues may focus a child on a different
        // thread. Read that input thread, not just the top-level frame's HKL.
        if (info.hwndFocus)
        {
            result.focus = info.hwndFocus;
            result.thread = GetWindowThreadProcessId(result.focus, nullptr);
            if (!result.thread) return {};
        }
    }
    result.layout = GetKeyboardLayout(result.thread);
    if (!result.layout) return {};
    return result;
}
Snapshot Read()
{
    Snapshot result = Target();
    if (!result.layout || result.menuActive) return {};
    const LANGID language = LOWORD(reinterpret_cast<ULONG_PTR>(result.layout));
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{}, name[256]{}, abbreviation[16]{};
    if (LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0))
    {
        GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, static_cast<int>(std::size(name)));
        GetLocaleInfoEx(locale, LOCALE_SISO639LANGNAME, abbreviation, static_cast<int>(std::size(abbreviation)));
        CharUpperBuffW(abbreviation, static_cast<DWORD>(wcslen(abbreviation)));
    }
    result.description = name;
    wchar_t imeName[256]{};
    if (ImmGetDescriptionW(result.layout, imeName, static_cast<UINT>(std::size(imeName))) && imeName[0])
        result.description = imeName;
    std::optional<bool> open;
    std::optional<DWORD> conversion;
    const auto primary = PRIMARYLANGID(language);
    if (primary == LANG_CHINESE || primary == LANG_JAPANESE || primary == LANG_KOREAN)
    {
        if (result.focus)
        {
            const HWND ime = ImmGetDefaultIMEWnd(result.focus);
            DWORD_PTR opened = 0, mode = 0;
            constexpr UINT flags = SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT;
            // A foreign IME may be hung or disallow messages (UIPI). Each query
            // has a hard budget on this worker; failure cannot replace a good sample.
            if (ime && SendMessageTimeoutW(ime, WM_IME_CONTROL, kGetOpenStatus, 0, flags, 20, &opened) &&
                SendMessageTimeoutW(ime, WM_IME_CONTROL, kGetConversionMode, 0, flags, 20, &mode))
            { open = opened != 0; conversion = static_cast<DWORD>(mode); }
        }
    }
    result.label = detail::Label(language, abbreviation, open, conversion);
    // Do not publish a sample collected across a foreground/layout transition.
    const auto current = Target();
    if (!detail::Matches(result, current.foreground, current.thread, current.layout, current.focus)) return {};
    return result;
}
}

namespace
{
std::wstring LanguageName(LANGID language)
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{}, name[256]{};
    if (LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0))
        GetLocaleInfoEx(locale, LOCALE_SLOCALIZEDDISPLAYNAME, name, static_cast<int>(std::size(name)));
    return name;
}
std::wstring KeyboardName(HKL layout)
{
    wchar_t name[256]{};
    if (ImmGetDescriptionW(layout, name, static_cast<UINT>(std::size(name)))) return name;
    // Ordinary layouts use their high word as KLID. Unknown/substituted
    // layouts fall back to their localized language; never activate one just
    // to call GetKeyboardLayoutName on our own thread.
    const auto device = HIWORD(reinterpret_cast<ULONG_PTR>(layout));
    wchar_t keyName[16]{};
    swprintf_s(keyName, L"%08X", static_cast<unsigned>(device));
    const std::wstring root = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, (root + keyName).c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    DWORD size = sizeof(name);
    if (RegLoadMUIStringW(key, L"Layout Display Name", name, sizeof(name), nullptr, 0, nullptr) != ERROR_SUCCESS)
        RegGetValueW(key, nullptr, L"Layout Text", RRF_RT_REG_SZ, nullptr, name, &size);
    RegCloseKey(key);
    return name;
}
}
Selection CaptureSelection()
{
    Selection result;
    result.target = Target();
    Apartment apartment;
    if (FAILED(apartment.result)) return result;
    Microsoft::WRL::ComPtr<ITfInputProcessorProfiles> profiles;
    if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&profiles)))) return result;
    Microsoft::WRL::ComPtr<ITfInputProcessorProfileMgr> manager;
    if (FAILED(profiles.As(&manager))) return result;
    TF_INPUTPROCESSORPROFILE active{};
    const bool known = SUCCEEDED(manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active));
    Microsoft::WRL::ComPtr<IEnumTfInputProcessorProfiles> entries;
    if (FAILED(manager->EnumProfiles(0, &entries))) return result;
    TF_INPUTPROCESSORPROFILE profile{};
    ULONG fetched = 0;
    for (unsigned index = 0; index < 1024 && result.choices.size() < 256 &&
        entries->Next(1, &profile, &fetched) == S_OK && fetched == 1; ++index)
    {
        if (!(profile.dwFlags & TF_IPP_FLAG_ENABLED) ||
            (profile.dwFlags & TF_IPP_FLAG_SUBSTITUTEDBYINPUTPROCESSOR) ||
            (profile.dwProfileType != TF_PROFILETYPE_KEYBOARDLAYOUT &&
                (profile.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR || profile.catid != GUID_TFCAT_TIP_KEYBOARD))) continue;
        Choice choice;
        choice.profile = profile;
        choice.language = LanguageName(profile.langid);
        if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
        {
            BSTR description = nullptr;
            if (SUCCEEDED(profiles->GetLanguageProfileDescription(profile.clsid, profile.langid, profile.guidProfile, &description)) && description)
                choice.name.assign(description, SysStringLen(description));
            SysFreeString(description);
        }
        else choice.name = KeyboardName(profile.hkl);
        if (choice.name.empty()) choice.name = choice.language;
        if (choice.name.empty()) continue;
        choice.selected = known ? SameProfile(profile, active) :
            profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT && profile.hkl == result.target.layout;
        if (std::none_of(result.choices.begin(), result.choices.end(), [&](const auto& item) { return SameProfile(item.profile, profile); }))
            result.choices.push_back(std::move(choice));
    }
    return result;
}
bool RestoreTarget(const Snapshot& target)
{
    if (!target.foreground || !target.thread || !IsWindow(target.foreground)) return false;
    const HWND input = target.focus ? target.focus : target.foreground;
    if (!IsWindow(input) || GetWindowThreadProcessId(input, nullptr) != target.thread) return false;
    if (GetForegroundWindow() != target.foreground && !SetForegroundWindow(target.foreground)) return false;
    const auto current = Target();
    return current.foreground == target.foreground && current.thread == target.thread && current.focus == target.focus;
}
HRESULT Select(const Selection& selection, const Choice& choice)
{
    if (std::none_of(selection.choices.begin(), selection.choices.end(), [&](const auto& item) {
        return SameProfile(item.profile, choice.profile);
    })) return E_INVALIDARG;
    if (!RestoreTarget(selection.target)) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    Apartment apartment;
    if (FAILED(apartment.result)) return apartment.result;
    Microsoft::WRL::ComPtr<ITfInputProcessorProfileMgr> manager;
    const HRESULT created = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
    if (FAILED(created)) return created;
    TF_INPUTPROCESSORPROFILE live{};
    const auto& profile = choice.profile;
    const HRESULT found = manager->GetProfile(profile.dwProfileType, profile.langid, profile.clsid,
        profile.guidProfile, profile.hkl, &live);
    if (FAILED(found)) return found;
    if (!(live.dwFlags & TF_IPP_FLAG_ENABLED)) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (profile.dwProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT)
    {
        // Windows posts this request to the original focused input window.
        // The recipient may reject it; queuing is not acceptance/readback.
        const HWND input = selection.target.focus ? selection.target.focus : selection.target.foreground;
        if (PostMessageW(input, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(profile.hkl))) return S_OK;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HKL languageLayout = profile.hklSubstitute;
    if (LOWORD(reinterpret_cast<ULONG_PTR>(selection.target.layout)) != profile.langid && !languageLayout)
    {
        const int count = GetKeyboardLayoutList(0, nullptr);
        std::vector<HKL> layouts(static_cast<std::size_t>((std::max)(0, count)));
        const int received = count > 0 ? GetKeyboardLayoutList(count, layouts.data()) : 0;
        for (int index = 0; index < received; ++index)
            if (LOWORD(reinterpret_cast<ULONG_PTR>(layouts[static_cast<std::size_t>(index)])) == profile.langid)
            { languageLayout = layouts[static_cast<std::size_t>(index)]; break; }
        if (!languageLayout) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    }
    // A TIP can share its language/HKL with other programs. The documented
    // session activation reaches the restored external application; changing
    // only our own thread would select the panel's IME instead. This is an
    // explicit user selection, not a registry/default-profile change.
    const HRESULT activated = manager->ActivateProfile(profile.dwProfileType, profile.langid, profile.clsid,
        profile.guidProfile, profile.hkl, TF_IPPMF_FORSESSION | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE);
    if (activated != S_OK) return activated;
    // DONTCARECURRENTINPUTLANGUAGE defers a cross-language TIP until its input
    // locale is selected. Post that selection to the restored typing window.
    if (LOWORD(reinterpret_cast<ULONG_PTR>(selection.target.layout)) != profile.langid)
    {
        const HWND input = selection.target.focus ? selection.target.focus : selection.target.foreground;
        if (!PostMessageW(input, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(languageLayout)))
            return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

struct Service::Impl
{
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    Snapshot snapshot;
    detail::DisplayCache display;
    std::shared_ptr<NativeState> native;
    std::wstring nativeModeName = native_menu::ModeButtonName();
    struct ContextRequest { ULONGLONG deadline; Snapshot target; };
    std::optional<ContextRequest> contextRequest;
    // Declared last so shutdown joins before its state/mutex are destroyed.
    std::jthread worker{[this](std::stop_token stop) {
        const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop.stop_requested())
        {
            std::optional<ContextRequest> request;
            {
                std::lock_guard lock(mutex);
                request = std::exchange(contextRequest, {});
            }
            if (request && !stop.stop_requested())
            {
                HRESULT hr = apartment;
                if (SUCCEEDED(hr))
                {
                    try { hr = native_menu::ShowContextMenu(nativeModeName, request->deadline, stop,
                        request->target.foreground, request->target.thread, request->target.layout); }
                    catch (...) { hr = E_UNEXPECTED; }
                }
                wchar_t message[128]{};
                swprintf_s(message, L"StatusBar input method native context provider hr=0x%08lX",
                    static_cast<unsigned long>(hr));
                WriteDiagnosticLogEntry(message);
            }
            Snapshot next;
            try { next = Read(); } catch (...) { /* Unavailable, never stale. */ }
            std::unique_lock lock(mutex);
            snapshot = std::move(next);
            wake.wait_for(lock, stop, std::chrono::milliseconds(200), [this] { return contextRequest.has_value(); });
        }
        if (SUCCEEDED(apartment)) CoUninitialize();
    }};
};
Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() = default;
Snapshot Service::Current() const
{
    const auto current = Target();
    std::lock_guard lock(impl_->mutex);
    return impl_->display.Get(impl_->snapshot, current,
        current.menuActive || modern_menu::ActiveRootWindow() != nullptr);
}
HRESULT Service::Show(RECT anchor, bool context)
{
    if (IsRectEmpty(&anchor)) return E_INVALIDARG;
    if (context && !impl_->nativeModeName.empty() && native_menu::HasModernTaskbar())
    {
        // New Windows shell IMEs require the native button's XAML anchor. The
        // legacy coordinate call queues S_OK but does not open their menu.
        // Keep Win10/classic-shell and mode-toggle behavior on the common ABI.
        std::lock_guard lock(impl_->mutex);
        impl_->contextRequest = Impl::ContextRequest{GetTickCount64() + 1500, Target()};
        impl_->wake.notify_one();
        return S_OK; // Queued, not a claim that the popup has been displayed.
    }
    if (!impl_->native) impl_->native = std::make_shared<NativeState>();
    // Retain native state for the whole call. No Service/Impl access may follow
    // a foreign COM call: a nested message can disable or rebuild the bar.
    const auto native = impl_->native;
    if (FAILED(native->apartment.result)) return native->apartment.result;
    if (!native->picker)
    {
        auto hr = CoCreateInstance(kInputSwitch, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(native->picker.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = native->picker->Init(0); // Desktop client, common to Windows 10/11.
        if (FAILED(hr)) { native->picker.Reset(); return hr; }
    }
    const auto picker = native->picker;
    taskbar_hook::native::MenuAccess access;
    if (context)
    {
        const HRESULT prepared = access.Begin(FindWindowW(L"Shell_TrayWnd", nullptr));
        if (FAILED(prepared)) return prepared;
    }
    // The bar is no-activate. Windows therefore receives the original typing
    // target, including its per-window input preference, and owns dismissal.
    const POINT point{anchor.left + (anchor.right - anchor.left) / 2,
        anchor.top + (anchor.bottom - anchor.top) / 2};
    // The mode indicator differs from the language-list button: left toggles
    // conversion mode, right opens the active IME's own menu on both OS versions.
    auto hr = picker->ClickImeModeItem(context ? 1 : 0, point, &anchor);
    // A plain keyboard layout may have no IME mode item. Keep its language
    // picker available without synthesizing a key or changing input mode.
    if (hr == S_FALSE || hr == E_NOTIMPL)
        hr = picker->ShowInputSwitch(&anchor);
    if (FAILED(hr) && native->picker.Get() == picker.Get())
        native->picker.Reset(); // Allow recovery after Shell restart.
    if (SUCCEEDED(hr)) access.HandOff();
    return hr;
}
}
