#pragma once

#include <windows.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <utility>

namespace snowdesktop::dock_thumbnail
{
// Optional Windows 10/11 DWM ABI, illustrated by ADeltaX's shared-visual demo:
// https://gist.github.com/ADeltaX/aea6aac248604d0cb7d423a61b06e247
// Resolve only on the known platform family. Failure leaves the documented
// thumbnail/snapshot path available; no private API is a hard dependency.
struct Api
{
    using Create = HRESULT(WINAPI*)(HWND, HWND, DWORD,
        DWM_THUMBNAIL_PROPERTIES*, void*, void**, HTHUMBNAIL*);
    using QuerySize = HRESULT(WINAPI*)(HWND, BOOL, SIZE*);
    Create create = nullptr;
    QuerySize querySize = nullptr;

    static Api Load() noexcept
    {
        using Version = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto version = reinterpret_cast<Version>(GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW info{sizeof(info)};
        if (!version || version(&info) < 0 || info.dwMajorVersion != 10 ||
            info.dwBuildNumber < 17763) return {};
        const HMODULE module = GetModuleHandleW(L"dwmapi.dll");
        if (!module) return {};
        Api api;
        api.create = reinterpret_cast<Create>(GetProcAddress(module, MAKEINTRESOURCEA(147)));
        api.querySize = reinterpret_cast<QuerySize>(GetProcAddress(module, MAKEINTRESOURCEA(162)));
        return api;
    }
};

inline const Api& WindowImageApi() noexcept
{
    static const Api api = Api::Load();
    return api;
}

inline HRESULT SourceSize(HWND source, SIZE& size) noexcept
{
    size = {};
    if (!source || !IsWindow(source)) return E_INVALIDARG;
    const auto query = WindowImageApi().querySize;
    const HRESULT hr = query ? query(source, FALSE, &size) : E_NOTIMPL;
    return SUCCEEDED(hr) && (size.cx <= 1 || size.cy <= 1) ? E_UNEXPECTED : hr;
}

class SharedVisual final
{
public:
    SharedVisual() = default;
    ~SharedVisual() { Reset(); }
    SharedVisual(const SharedVisual&) = delete;
    SharedVisual& operator=(const SharedVisual&) = delete;
    SharedVisual(SharedVisual&& other) noexcept
        : visual(std::move(other.visual)), thumbnail_(std::exchange(other.thumbnail_, nullptr)) {}
    SharedVisual& operator=(SharedVisual&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            visual = std::move(other.visual);
            thumbnail_ = std::exchange(other.thumbnail_, nullptr);
        }
        return *this;
    }

    HRESULT Create(HWND destination, HWND source, IUnknown* device, SIZE size) noexcept
    {
        Reset();
        if (!destination || !source || !IsWindow(destination) || !IsWindow(source))
            return E_INVALIDARG;
        const auto create = WindowImageApi().create;
        if (!create || !device || size.cx <= 1 || size.cy <= 1) return E_NOTIMPL;
        constexpr DWORD enable3D = 0x04000000;
        DWM_THUMBNAIL_PROPERTIES properties{};
        properties.dwFlags = DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE |
            DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | enable3D;
        properties.fVisible = TRUE;
        properties.fSourceClientAreaOnly = FALSE;
        properties.opacity = 255;
        properties.rcSource = properties.rcDestination = {0, 0, size.cx, size.cy};
        void* raw = nullptr;
        HRESULT hr = create(destination, source, 2, &properties, device, &raw, &thumbnail_);
        Microsoft::WRL::ComPtr<IUnknown> returned;
        returned.Attach(static_cast<IUnknown*>(raw));
        if (SUCCEEDED(hr)) hr = returned ? returned.As(&visual) : E_UNEXPECTED;
        if (FAILED(hr)) Reset();
        return hr;
    }

    void Reset() noexcept
    {
        visual.Reset();
        if (thumbnail_) DwmUnregisterThumbnail(std::exchange(thumbnail_, nullptr));
    }

    Microsoft::WRL::ComPtr<IDCompositionVisual3> visual;

private:
    HTHUMBNAIL thumbnail_ = nullptr;
};
}
