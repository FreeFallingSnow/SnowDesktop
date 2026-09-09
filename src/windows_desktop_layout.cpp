#include "windows_desktop_layout.h"

#include <exdisp.h>
#include <shlobj.h>
#include <shlguid.h>
#include <wrl/client.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace snowdesktop::windows_desktop_layout
{
namespace
{
using Microsoft::WRL::ComPtr;

struct ChildPidlDeleter
{
    // Keep the SDK's pointer qualifiers when owning an enumerated PIDL.
    using pointer = PITEMID_CHILD;
    void operator()(pointer value) const noexcept { ILFree(value); }
};

struct CaptureState
{
    std::mutex mutex;
    std::condition_variable completed;
    bool ready = false;
    Snapshot snapshot;
};

Snapshot ReadView(std::chrono::steady_clock::time_point deadline)
{
    Snapshot result;
    ComPtr<IShellWindows> windows;
    result.status = CoCreateInstance(CLSID_ShellWindows, nullptr,
        CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows));
    if (FAILED(result.status)) return result;
    VARIANT location{}, unused{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    long ignoredWindow = 0;
    ComPtr<IDispatch> dispatch;
    result.status = windows->FindWindowSW(&location, &unused, SWC_DESKTOP,
        &ignoredWindow, SWFO_NEEDDISPATCH, &dispatch);
    if (FAILED(result.status) || !dispatch) return result;
    ComPtr<IServiceProvider> provider;
    result.status = dispatch.As(&provider);
    if (FAILED(result.status)) return result;
    ComPtr<IShellBrowser> browser;
    result.status = provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    if (FAILED(result.status)) return result;
    ComPtr<IShellView> shellView;
    result.status = browser->QueryActiveShellView(&shellView);
    if (FAILED(result.status)) return result;
    ComPtr<IFolderView> view;
    result.status = shellView.As(&view);
    if (FAILED(result.status)) return result;
    result.status = view->GetSpacing(&result.spacing);
    if (!result.Available()) return result;
    HWND window = nullptr;
    result.status = shellView->GetWindow(&window);
    if (FAILED(result.status) || !IsWindow(window))
    {
        result.status = E_FAIL;
        return result;
    }
    ComPtr<IFolderView2> view2;
    if (SUCCEEDED(view.As(&view2)))
    {
        FOLDERVIEWMODE mode{};
        view2->GetViewModeAndIconSize(&mode, &result.iconSize);
    }
    ComPtr<IShellFolder> folder;
    result.status = view->GetFolder(IID_PPV_ARGS(&folder));
    if (FAILED(result.status)) return result;
    ComPtr<IEnumIDList> enumeration;
    result.status = view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&enumeration));
    if (FAILED(result.status) || !enumeration) return result;
    while (std::chrono::steady_clock::now() < deadline && result.items.size() < 10000)
    {
        PITEMID_CHILD child = nullptr;
        if (enumeration->Next(1, &child, nullptr) != S_OK) break;
        std::unique_ptr<ITEMIDLIST, ChildPidlDeleter> owner(child);
        POINT point{};
        if (FAILED(view->GetItemPosition(child, &point)) ||
            !ClientToScreen(window, &point)) continue;
        ComPtr<IShellItem> item;
        if (FAILED(SHCreateItemWithParent(nullptr, folder.Get(), child,
                IID_PPV_ARGS(&item)))) continue;
        PWSTR parsingName = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsingName)))
        {
            const auto freeString = [](wchar_t* value) { CoTaskMemFree(value); };
            std::unique_ptr<wchar_t, decltype(freeString)> name(parsingName, freeString);
            if (parsingName && *parsingName)
                result.items.push_back({parsingName, point});
        }
    }
    return result;
}
}

Snapshot Capture(std::chrono::milliseconds budget)
{
    static std::mutex captureMutex;
    static std::weak_ptr<CaptureState> activeCapture;
    std::shared_ptr<CaptureState> state;
    {
        std::lock_guard lock(captureMutex);
        if (!activeCapture.expired())
        {
            Snapshot busy;
            busy.status = HRESULT_FROM_WIN32(ERROR_BUSY);
            return busy;
        }
        state = std::make_shared<CaptureState>();
        activeCapture = state;
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    try
    {
        std::thread([state, deadline] {
            Snapshot snapshot;
            const auto previousDpi = SetThreadDpiAwarenessContext(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(initialized))
            {
                try { snapshot = ReadView(deadline); }
                catch (...) { snapshot.status = E_FAIL; }
                CoUninitialize();
            }
            else snapshot.status = initialized;
            if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
            {
                std::lock_guard lock(state->mutex);
                state->snapshot = std::move(snapshot);
                state->ready = true;
            }
            state->completed.notify_one();
        }).detach();
    }
    catch (...)
    {
        Snapshot failed;
        failed.status = E_OUTOFMEMORY;
        return failed;
    }
    std::unique_lock lock(state->mutex);
    if (!state->completed.wait_until(lock, deadline, [&] { return state->ready; }))
    {
        Snapshot timeout;
        timeout.status = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        return timeout;
    }
    return std::move(state->snapshot);
}
}
