#include "status_bar_input_method.h"
#include "modern_menu.h"
#include "status_bar_input_method_native.h"
#include "diagnostic_log.h"
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
            // has a hard budget on this worker; failure clears old mode data.
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
    return impl_->display.Get(impl_->snapshot, current, GetTickCount64(),
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
    return hr;
}
}
