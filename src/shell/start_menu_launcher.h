#pragma once

#include <windows.h>
#include <servprov.h>
#include <wrl/client.h>
#include <optional>

namespace snowdesktop::start_menu
{
inline constexpr wchar_t kHelperCommand[] = L"--start-menu-helper";
inline constexpr CLSID kImmersiveShell = {0xc2f03a33, 0x21f5, 0x47fa,
    {0xb4, 0xbb, 0x15, 0x63, 0x62, 0xa2, 0xf2, 0x39}};
inline constexpr GUID kLauncherService = {0x6f86e01c, 0xc649, 0x4d61,
    {0xbe, 0x23, 0xf1, 0x32, 0x2d, 0xde, 0xca, 0x9d}};

// Private Shell ABI, verified against RetroBar's Win10RS1/Win81 declarations:
// https://github.com/dremin/RetroBar/blob/69f36278150cb39bc37d3237e3178926734c6804/RetroBar/Utilities/StartMenuMonitor.cs
// Only the one-way Dismiss and visibility methods are called. These objects
// stay in a supervised helper; unsupported interfaces cause an open fallback.
MIDL_INTERFACE("d8d60399-a0f1-f987-5551-321fd1b49864") LauncherCurrent : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE ReservedShow(int, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE Dismiss(int method) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReservedDismiss2(int) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReservedDismissSync() = 0;
    virtual HRESULT STDMETHODCALLTYPE IsVisible(BOOL*) = 0;
};
MIDL_INTERFACE("93f91f5a-a4ca-4205-9beb-ce4d17c708f9") LauncherLegacy : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE ReservedShow(int, int) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reserved2() = 0;
    virtual HRESULT STDMETHODCALLTYPE Reserved3() = 0;
    virtual HRESULT STDMETHODCALLTYPE Reserved4() = 0;
    virtual HRESULT STDMETHODCALLTYPE Reserved5() = 0;
    virtual HRESULT STDMETHODCALLTYPE Dismiss(int method) = 0;
    virtual HRESULT STDMETHODCALLTYPE Reserved7() = 0;
    virtual HRESULT STDMETHODCALLTYPE IsVisible(BOOL*) = 0;
};

class Launcher final
{
public:
    HRESULT Bind(IServiceProvider* provider)
    {
        current_.Reset(); legacy_.Reset();
        if (!provider) return E_POINTER;
        HRESULT result = provider->QueryService(kLauncherService, IID_PPV_ARGS(&current_));
        if (result == E_NOINTERFACE)
            result = provider->QueryService(kLauncherService, IID_PPV_ARGS(&legacy_));
        return result;
    }
    HRESULT Visible(bool& value)
    {
        BOOL visible = FALSE;
        const HRESULT result = current_ ? current_->IsVisible(&visible) :
            legacy_ ? legacy_->IsVisible(&visible) : E_NOINTERFACE;
        value = SUCCEEDED(result) && visible;
        return result;
    }
    HRESULT Dismiss()
    {
        // ILDM_ESCAPEKEY. Unlike a Win key, this never opens a closed menu
        // and does not depend on which process currently owns foreground.
        return current_ ? current_->Dismiss(4) : legacy_ ? legacy_->Dismiss(4) : E_NOINTERFACE;
    }
private:
    Microsoft::WRL::ComPtr<LauncherCurrent> current_;
    Microsoft::WRL::ComPtr<LauncherLegacy> legacy_;
};

std::optional<int> TryRunLauncherHelper();
}
