#include "wallpaper_hook_protocol.h"

#include <MinHook.h>
#include <tlhelp32.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cwchar>

using Microsoft::WRL::ComPtr;
using namespace snow::wallpaper_hook;

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
    const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
    DXGI_FORMAT, UINT);
using D3D9PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*,
    const RECT*, HWND, const RGNDATA*);
using D3D9PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*,
    const RECT*, HWND, const RGNDATA*, DWORD);
using D3D9SwapChainPresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*,
    const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using D3D9ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,
    D3DPRESENT_PARAMETERS*);
using D3D9ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*,
    D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

struct RuntimeSlot {
    IDXGISwapChain* swapChain = nullptr;
    IDirect3DDevice9* d3d9Device = nullptr;
    IDirect3DSwapChain9* d3d9SwapChain = nullptr;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<ID3D11Texture2D> resolveTexture;
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    ComPtr<IDirect3DTexture9> d3d9SharedTexture;
    ComPtr<IDirect3DSurface9> d3d9SharedSurface;
    ComPtr<IDirect3DQuery9> d3d9CompletionQuery;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ULONGLONG lastCopyTick = 0;
    bool d3d9CopyPending = false;
    LONG d3d9PendingRequestSerial = 0;
};

HMODULE g_module = nullptr;
HANDLE g_mapping = nullptr;
SharedState* g_state = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;
D3D9PresentFn g_d3d9Present = nullptr;
D3D9PresentExFn g_d3d9PresentEx = nullptr;
D3D9SwapChainPresentFn g_d3d9SwapChainPresent = nullptr;
D3D9ResetFn g_d3d9Reset = nullptr;
D3D9ResetExFn g_d3d9ResetEx = nullptr;
SRWLOCK g_slotLock = SRWLOCK_INIT;
std::array<RuntimeSlot, kMaxFrameSlots> g_slots;

struct ActiveHook {
    ActiveHook()
    {
        if (g_state)
            InterlockedIncrement(&g_state->active_hooks);
    }
    ~ActiveHook()
    {
        if (g_state)
            InterlockedDecrement(&g_state->active_hooks);
    }
};

void SetFailure(HRESULT hr)
{
    if (!g_state)
        return;
    InterlockedExchange(&g_state->last_hresult, static_cast<LONG>(hr));
    InterlockedExchange(&g_state->status, static_cast<LONG>(Status::failed));
}

bool IsWallpaperEngineClass(const wchar_t* className)
{
    return _wcsicmp(className, L"WPEDesktopDX11Window") == 0 ||
        _wcsicmp(className, L"WPECloneView") == 0 ||
        _wcsicmp(className, L"WPEVideoWallpaper") == 0;
}

bool IsWallpaperEngineVideoClass(const wchar_t* className)
{
    return _wcsicmp(className, L"WPEVideoWallpaper") == 0 ||
        _wcsicmp(className, L"EVRFullscreenVideo") == 0;
}

bool IsDesktopHostClass(const wchar_t* className)
{
    return _wcsicmp(className, L"WorkerW") == 0 ||
        _wcsicmp(className, L"Progman") == 0;
}

bool IsDesktopOutputWindow(HWND window)
{
    if (!window || !IsWindow(window))
        return false;
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (IsWallpaperEngineClass(className))
        return true;
    for (HWND current = GetParent(window); current; current = GetParent(current))
    {
        wchar_t parentClass[64]{};
        GetClassNameW(current, parentClass,
            static_cast<int>(std::size(parentClass)));
        if (IsDesktopHostClass(parentClass))
            return true;
    }
    return false;
}

bool IsValidDesktopRect(const RECT& rect)
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

struct RendererOutputSearch {
    std::array<DWORD, 8> processIds{};
    std::size_t processCount = 0;
    HWND bestWindow = nullptr;
    RECT bestRect{};
    std::uint64_t bestArea = 0;
};

bool SearchContainsProcess(const RendererOutputSearch& search, DWORD processId)
{
    return std::find(search.processIds.begin(),
        search.processIds.begin() + search.processCount, processId) !=
        search.processIds.begin() + search.processCount;
}

BOOL CALLBACK FindRendererOutputWindow(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<RendererOutputSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!SearchContainsProcess(*search, processId) ||
        !IsDesktopOutputWindow(window))
        return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect) || !IsValidDesktopRect(rect))
        return TRUE;
    const auto area = static_cast<std::uint64_t>(rect.right - rect.left) *
        static_cast<std::uint64_t>(rect.bottom - rect.top);
    if (area > search->bestArea)
    {
        search->bestWindow = window;
        search->bestRect = rect;
        search->bestArea = area;
    }
    return TRUE;
}

BOOL CALLBACK FindRendererOutputRoot(HWND window, LPARAM parameter)
{
    FindRendererOutputWindow(window, parameter);
    EnumChildWindows(window, FindRendererOutputWindow, parameter);
    return TRUE;
}

void CollectProcessAncestors(RendererOutputSearch& search)
{
    search.processIds[search.processCount++] = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    DWORD current = search.processIds[0];
    while (search.processCount < search.processIds.size())
    {
        DWORD parent = 0;
        PROCESSENTRY32W entry{ sizeof(entry) };
        if (Process32FirstW(snapshot, &entry))
        {
            do {
                if (entry.th32ProcessID == current)
                {
                    parent = entry.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        if (!parent || parent == current)
            break;
        current = parent;
        search.processIds[search.processCount++] = current;
    }
    CloseHandle(snapshot);
}

bool TryGetCachedSwapChainOutput(IDXGISwapChain* chain, HWND& outputWindow,
    RECT& desktopRect)
{
    bool found = false;
    AcquireSRWLockShared(&g_slotLock);
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (g_slots[i].swapChain != chain)
            continue;
        outputWindow = reinterpret_cast<HWND>(g_state->slots[i].output_window);
        desktopRect = g_state->slots[i].desktop_rect;
        found = IsValidDesktopRect(desktopRect);
        break;
    }
    ReleaseSRWLockShared(&g_slotLock);
    return found;
}

bool ResolveSwapChainOutput(IDXGISwapChain* chain, HWND describedWindow,
    HWND& outputWindow, RECT& desktopRect)
{
    outputWindow = describedWindow;
    if (IsDesktopOutputWindow(outputWindow) &&
        GetWindowRect(outputWindow, &desktopRect) &&
        IsValidDesktopRect(desktopRect))
        return true;
    if (TryGetCachedSwapChainOutput(chain, outputWindow, desktopRect))
        return true;

    ComPtr<IDXGISwapChain1> chain1;
    HWND chainWindow = nullptr;
    if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&chain1))) && chain1 &&
        SUCCEEDED(chain1->GetHwnd(&chainWindow)) &&
        IsDesktopOutputWindow(chainWindow) &&
        GetWindowRect(chainWindow, &desktopRect) &&
        IsValidDesktopRect(desktopRect))
    {
        outputWindow = chainWindow;
        return true;
    }

    // Chromium/CEF web wallpapers use DirectComposition swap chains whose
    // OutputWindow is null. The GPU child has no HWND of its own, so walk its
    // process ancestry to the webwallpaper owner window.
    RendererOutputSearch ancestry{};
    CollectProcessAncestors(ancestry);
    for (std::size_t depth = 0; depth < ancestry.processCount; ++depth)
    {
        RendererOutputSearch search{};
        search.processIds[search.processCount++] = ancestry.processIds[depth];
        EnumWindows(FindRendererOutputRoot, reinterpret_cast<LPARAM>(&search));
        if (search.bestWindow)
        {
            outputWindow = search.bestWindow;
            desktopRect = search.bestRect;
            return true;
        }
    }

    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC outputDesc{};
    if (SUCCEEDED(chain->GetContainingOutput(&output)) && output &&
        SUCCEEDED(output->GetDesc(&outputDesc)) &&
        IsValidDesktopRect(outputDesc.DesktopCoordinates))
    {
        outputWindow = nullptr;
        desktopRect = outputDesc.DesktopCoordinates;
        return true;
    }
    return false;
}

void ClearPublishedSlot(std::size_t index)
{
    if (!g_state || index >= kMaxFrameSlots)
        return;
    auto& published = g_state->slots[index];
    published.shared_handle = 0;
    published.width = 0;
    published.height = 0;
    published.format = static_cast<std::uint32_t>(DXGI_FORMAT_UNKNOWN);
    InterlockedIncrement(&published.generation);
}

void ReleaseRuntimeSlot(std::size_t index, bool removeSwapChain)
{
    if (index >= kMaxFrameSlots)
        return;
    auto& runtime = g_slots[index];
    runtime.keyedMutex.Reset();
    runtime.sharedTexture.Reset();
    runtime.resolveTexture.Reset();
    runtime.d3d9CompletionQuery.Reset();
    runtime.d3d9SharedSurface.Reset();
    runtime.d3d9SharedTexture.Reset();
    runtime.width = 0;
    runtime.height = 0;
    runtime.format = DXGI_FORMAT_UNKNOWN;
    runtime.lastCopyTick = 0;
    runtime.d3d9CopyPending = false;
    runtime.d3d9PendingRequestSerial = 0;
    ClearPublishedSlot(index);
    if (removeSwapChain)
    {
        runtime.swapChain = nullptr;
        runtime.d3d9Device = nullptr;
        runtime.d3d9SwapChain = nullptr;
        if (g_state)
        {
            auto& published = g_state->slots[index];
            published.swap_chain = 0;
            published.output_window = 0;
            published.window_class[0] = L'\0';
        }
    }
}

int FindOrCreateSlot(IDXGISwapChain* chain, HWND outputWindow, const wchar_t* className)
{
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (g_slots[i].swapChain == chain)
            return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (g_slots[i].swapChain || g_slots[i].d3d9Device)
            continue;
        g_slots[i].swapChain = chain;
        auto& published = g_state->slots[i];
        published.swap_chain = reinterpret_cast<std::uint64_t>(chain);
        published.output_window = reinterpret_cast<std::uint64_t>(outputWindow);
        wcsncpy_s(published.window_class, className, _TRUNCATE);
        const LONG requiredCount = static_cast<LONG>(i + 1);
        if (g_state->slot_count < requiredCount)
            InterlockedExchange(&g_state->slot_count, requiredCount);
        return static_cast<int>(i);
    }
    return -1;
}

int FindOrCreateD3D9Slot(IDirect3DDevice9* device,
    IDirect3DSwapChain9* swapChain, HWND outputWindow, const wchar_t* className)
{
    const ULONGLONG now = GetTickCount64();
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (!g_slots[i].d3d9Device ||
            reinterpret_cast<HWND>(g_state->slots[i].output_window) != outputWindow)
            continue;
        if (g_slots[i].d3d9Device == device &&
            (!swapChain || !g_slots[i].d3d9SwapChain ||
                g_slots[i].d3d9SwapChain == swapChain))
        {
            if (swapChain)
                g_slots[i].d3d9SwapChain = swapChain;
            return static_cast<int>(i);
        }

        // EVR can expose several D3D9 presenters for one WPEVideoWallpaper.
        // Bind the output to the first active producer so a single monitor does
        // not trigger duplicate shared-texture copies. If that producer stops,
        // a later presenter may take over after the cached frame grace period.
        if (g_slots[i].lastCopyTick &&
            now - g_slots[i].lastCopyTick <= 2000)
            return -1;
        ReleaseRuntimeSlot(i, true);
        break;
    }
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (g_slots[i].swapChain || g_slots[i].d3d9Device)
            continue;
        g_slots[i].d3d9Device = device;
        g_slots[i].d3d9SwapChain = swapChain;
        auto& published = g_state->slots[i];
        published.swap_chain = reinterpret_cast<std::uint64_t>(
            swapChain ? static_cast<void*>(swapChain) : static_cast<void*>(device));
        published.output_window = reinterpret_cast<std::uint64_t>(outputWindow);
        wcsncpy_s(published.window_class, className, _TRUNCATE);
        const LONG requiredCount = static_cast<LONG>(i + 1);
        if (g_state->slot_count < requiredCount)
            InterlockedExchange(&g_state->slot_count, requiredCount);
        return static_cast<int>(i);
    }
    return -1;
}

HRESULT CreateSharedTexture(std::size_t index, ID3D11Texture2D* source,
    const D3D11_TEXTURE2D_DESC& sourceDesc)
{
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    if (!device)
        return E_NOINTERFACE;

    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device.As(&dxgiDevice);
    if (FAILED(hr))
        return hr;
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr))
        return hr;
    DXGI_ADAPTER_DESC adapterDesc{};
    hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr))
        return hr;

    D3D11_TEXTURE2D_DESC sharedDesc{};
    sharedDesc.Width = sourceDesc.Width;
    sharedDesc.Height = sourceDesc.Height;
    sharedDesc.MipLevels = 1;
    sharedDesc.ArraySize = 1;
    sharedDesc.Format = sourceDesc.Format;
    sharedDesc.SampleDesc.Count = 1;
    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    sharedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sharedDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    ComPtr<ID3D11Texture2D> sharedTexture;
    hr = device->CreateTexture2D(&sharedDesc, nullptr, &sharedTexture);
    if (FAILED(hr))
        return hr;
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    hr = sharedTexture.As(&keyedMutex);
    if (FAILED(hr))
        return hr;
    ComPtr<IDXGIResource> resource;
    hr = sharedTexture.As(&resource);
    if (FAILED(hr))
        return hr;
    HANDLE sharedHandle = nullptr;
    hr = resource->GetSharedHandle(&sharedHandle);
    if (FAILED(hr))
        return hr;

    ComPtr<ID3D11Texture2D> resolveTexture;
    if (sourceDesc.SampleDesc.Count > 1)
    {
        D3D11_TEXTURE2D_DESC resolveDesc = sharedDesc;
        resolveDesc.BindFlags = 0;
        resolveDesc.MiscFlags = 0;
        hr = device->CreateTexture2D(&resolveDesc, nullptr, &resolveTexture);
        if (FAILED(hr))
            return hr;
    }

    auto& runtime = g_slots[index];
    runtime.sharedTexture = std::move(sharedTexture);
    runtime.resolveTexture = std::move(resolveTexture);
    runtime.keyedMutex = std::move(keyedMutex);
    runtime.width = sourceDesc.Width;
    runtime.height = sourceDesc.Height;
    runtime.format = sourceDesc.Format;

    auto& published = g_state->slots[index];
    published.shared_handle = reinterpret_cast<std::uint64_t>(sharedHandle);
    published.width = sourceDesc.Width;
    published.height = sourceDesc.Height;
    published.format = static_cast<std::uint32_t>(sourceDesc.Format);
    published.adapter_luid_low = adapterDesc.AdapterLuid.LowPart;
    published.adapter_luid_high = adapterDesc.AdapterLuid.HighPart;
    InterlockedIncrement(&published.generation);
    InterlockedExchange(&g_state->status, static_cast<LONG>(Status::sharing));
    return S_OK;
}

struct VideoWindowSearch {
    DWORD processId = 0;
    HWND best = nullptr;
    std::uint64_t bestArea = 0;
};

BOOL CALLBACK FindVideoWindow(HWND window, LPARAM parameter)
{
    auto* search = reinterpret_cast<VideoWindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != search->processId)
        return TRUE;
    wchar_t className[64]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (_wcsicmp(className, L"WPEVideoWallpaper") != 0)
        return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect))
        return TRUE;
    const auto width = static_cast<std::uint64_t>(std::max<LONG>(0,
        rect.right - rect.left));
    const auto height = static_cast<std::uint64_t>(std::max<LONG>(0,
        rect.bottom - rect.top));
    const std::uint64_t area = width * height;
    if (area > search->bestArea)
    {
        search->best = window;
        search->bestArea = area;
    }
    return TRUE;
}

BOOL CALLBACK FindVideoWindowRoot(HWND window, LPARAM parameter)
{
    FindVideoWindow(window, parameter);
    EnumChildWindows(window, FindVideoWindow, parameter);
    return TRUE;
}

HWND ResolveD3D9OutputWindow(IDirect3DDevice9* device,
    IDirect3DSwapChain9* presentedSwapChain, HWND overrideWindow)
{
    if (IsDesktopOutputWindow(overrideWindow))
        return overrideWindow;

    // EVR may call Present/PresentEx without hDestWindowOverride. The swap-chain
    // presentation parameters retain the per-output device window and are more
    // precise than the device focus window when one process renders two monitors.
    ComPtr<IDirect3DSwapChain9> primarySwapChain;
    IDirect3DSwapChain9* swapChain = presentedSwapChain;
    if (!swapChain && SUCCEEDED(device->GetSwapChain(0, &primarySwapChain)))
        swapChain = primarySwapChain.Get();
    D3DPRESENT_PARAMETERS presentParameters{};
    if (swapChain && SUCCEEDED(swapChain->GetPresentParameters(&presentParameters)) &&
        IsDesktopOutputWindow(presentParameters.hDeviceWindow))
        return presentParameters.hDeviceWindow;

    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(device->GetCreationParameters(&creation)) &&
        IsDesktopOutputWindow(creation.hFocusWindow))
        return creation.hFocusWindow;

    VideoWindowSearch search{};
    search.processId = GetCurrentProcessId();
    EnumWindows(FindVideoWindowRoot, reinterpret_cast<LPARAM>(&search));
    if (search.best)
        return search.best;
    if (overrideWindow)
        return overrideWindow;
    return SUCCEEDED(device->GetCreationParameters(&creation))
        ? creation.hFocusWindow
        : nullptr;
}

HRESULT CreateD3D9SharedTexture(std::size_t index, IDirect3DDevice9* device,
    const D3DSURFACE_DESC& sourceDesc)
{
    ComPtr<IDirect3DDevice9Ex> deviceEx;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&deviceEx));
    if (FAILED(hr) || !deviceEx)
        return E_NOINTERFACE;

    HANDLE sharedHandle = nullptr;
    ComPtr<IDirect3DTexture9> texture;
    hr = device->CreateTexture(sourceDesc.Width, sourceDesc.Height, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        &texture, &sharedHandle);
    if (FAILED(hr) || !texture || !sharedHandle)
        return FAILED(hr) ? hr : E_FAIL;

    ComPtr<IDirect3DSurface9> surface;
    hr = texture->GetSurfaceLevel(0, &surface);
    if (FAILED(hr) || !surface)
        return FAILED(hr) ? hr : E_FAIL;
    ComPtr<IDirect3DQuery9> completionQuery;
    hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &completionQuery);
    if (FAILED(hr) || !completionQuery)
        return FAILED(hr) ? hr : E_FAIL;

    LUID adapterLuid{};
    D3DDEVICE_CREATION_PARAMETERS creation{};
    ComPtr<IDirect3D9> d3d9;
    ComPtr<IDirect3D9Ex> d3d9Ex;
    if (SUCCEEDED(device->GetCreationParameters(&creation)) &&
        SUCCEEDED(device->GetDirect3D(&d3d9)) && d3d9 &&
        SUCCEEDED(d3d9.As(&d3d9Ex)) && d3d9Ex)
        d3d9Ex->GetAdapterLUID(creation.AdapterOrdinal, &adapterLuid);

    auto& runtime = g_slots[index];
    runtime.d3d9SharedTexture = std::move(texture);
    runtime.d3d9SharedSurface = std::move(surface);
    runtime.d3d9CompletionQuery = std::move(completionQuery);
    runtime.width = sourceDesc.Width;
    runtime.height = sourceDesc.Height;
    runtime.format = DXGI_FORMAT_B8G8R8A8_UNORM;

    auto& published = g_state->slots[index];
    published.shared_handle = reinterpret_cast<std::uint64_t>(sharedHandle);
    published.width = sourceDesc.Width;
    published.height = sourceDesc.Height;
    published.format = static_cast<std::uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM);
    published.adapter_luid_low = adapterLuid.LowPart;
    published.adapter_luid_high = adapterLuid.HighPart;
    InterlockedIncrement(&published.generation);
    InterlockedExchange(&g_state->status, static_cast<LONG>(Status::sharing));
    return S_OK;
}

void CaptureD3D9Device(IDirect3DDevice9* device,
    IDirect3DSwapChain9* presentedSwapChain, HWND overrideWindow)
{
    if (!device || !g_state || g_state->shutdown_requested ||
        !g_state->capture_enabled)
        return;
    const ULONGLONG now = GetTickCount64();
    const auto heartbeat = static_cast<ULONGLONG>(g_state->consumer_heartbeat);
    if (heartbeat == 0 || (now >= heartbeat && now - heartbeat > 5000))
        return;

    HWND outputWindow = ResolveD3D9OutputWindow(device, presentedSwapChain,
        overrideWindow);
    wchar_t className[64]{};
    if (!outputWindow ||
        !GetClassNameW(outputWindow, className,
            static_cast<int>(std::size(className))) ||
        (!IsDesktopOutputWindow(outputWindow) &&
            !IsWallpaperEngineVideoClass(className)))
        return;
    InterlockedIncrement64(&g_state->matched_present_calls);

    ComPtr<IDirect3DSurface9> source;
    const HRESULT backBufferHr = presentedSwapChain
        ? presentedSwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &source)
        : device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &source);
    if (FAILED(backBufferHr) ||
        !source)
        return;
    D3DSURFACE_DESC sourceDesc{};
    if (FAILED(source->GetDesc(&sourceDesc)) ||
        sourceDesc.Width < 64 || sourceDesc.Height < 64)
        return;

    AcquireSRWLockExclusive(&g_slotLock);
    const int slotIndex = FindOrCreateD3D9Slot(device, presentedSwapChain,
        outputWindow, className);
    if (slotIndex < 0)
    {
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }
    const auto index = static_cast<std::size_t>(slotIndex);
    auto& runtime = g_slots[index];
    auto& published = g_state->slots[index];
    published.output_window = reinterpret_cast<std::uint64_t>(outputWindow);
    wcsncpy_s(published.window_class, className, _TRUNCATE);
    GetWindowRect(outputWindow, &published.desktop_rect);

    if (runtime.d3d9CopyPending)
    {
        const HRESULT queryHr = runtime.d3d9CompletionQuery
            ? runtime.d3d9CompletionQuery->GetData(nullptr, 0, 0)
            : E_FAIL;
        if (queryHr == S_FALSE)
        {
            ReleaseSRWLockExclusive(&g_slotLock);
            return;
        }
        if (FAILED(queryHr))
        {
            ReleaseRuntimeSlot(index, false);
            ReleaseSRWLockExclusive(&g_slotLock);
            return;
        }
        runtime.d3d9CopyPending = false;
        InterlockedExchange(&published.completed_request_serial,
            runtime.d3d9PendingRequestSerial);
        InterlockedIncrement64(&published.frame_number);
    }

    const LONG requestSerial = g_state->request_serial;
    const LONG observedInterval = g_state->requested_interval_ms;
    const DWORD interval = static_cast<DWORD>(std::max<LONG>(0, observedInterval));
    const bool explicitlyRequested =
        published.completed_request_serial != requestSerial;
    const bool intervalDue = interval != 0 &&
        (runtime.lastCopyTick == 0 || now - runtime.lastCopyTick >= interval);
    if ((!explicitlyRequested && !intervalDue) ||
        published.frame_number > published.consumed_frame_number)
    {
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }

    if (!runtime.d3d9SharedTexture ||
        runtime.width != sourceDesc.Width || runtime.height != sourceDesc.Height)
    {
        ReleaseRuntimeSlot(index, false);
        const HRESULT createHr = CreateD3D9SharedTexture(index, device, sourceDesc);
        if (FAILED(createHr))
        {
            SetFailure(createHr);
            ReleaseSRWLockExclusive(&g_slotLock);
            return;
        }
    }

    const HRESULT copyHr = device->StretchRect(source.Get(), nullptr,
        runtime.d3d9SharedSurface.Get(), nullptr, D3DTEXF_NONE);
    const HRESULT queryHr = SUCCEEDED(copyHr)
        ? runtime.d3d9CompletionQuery->Issue(D3DISSUE_END)
        : copyHr;
    if (FAILED(queryHr))
    {
        SetFailure(queryHr);
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }
    runtime.d3d9CopyPending = true;
    runtime.d3d9PendingRequestSerial = requestSerial;
    runtime.lastCopyTick = now;
    ReleaseSRWLockExclusive(&g_slotLock);
}

void CaptureSwapChain(IDXGISwapChain* chain)
{
    if (!g_state || g_state->shutdown_requested || !g_state->capture_enabled)
        return;

    const ULONGLONG now = GetTickCount64();
    const auto heartbeat = static_cast<ULONGLONG>(g_state->consumer_heartbeat);
    if (heartbeat == 0 || (now >= heartbeat && now - heartbeat > 5000))
        return;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(chain->GetDesc(&swapDesc)))
        return;
    HWND outputWindow = nullptr;
    RECT desktopRect{};
    if (!ResolveSwapChainOutput(chain, swapDesc.OutputWindow,
            outputWindow, desktopRect))
        return;
    InterlockedIncrement64(&g_state->matched_present_calls);
    wchar_t className[64]{};
    if (outputWindow)
        GetClassNameW(outputWindow, className,
            static_cast<int>(std::size(className)));
    if (!className[0])
        wcsncpy_s(className, L"WPECompositionSurface", _TRUNCATE);

    ComPtr<ID3D11Texture2D> source;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&source))) || !source)
        return;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Width < 640 || sourceDesc.Height < 360)
        return;

    AcquireSRWLockExclusive(&g_slotLock);
    const int slotIndex = FindOrCreateSlot(chain, outputWindow, className);
    if (slotIndex < 0)
    {
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }
    const auto index = static_cast<std::size_t>(slotIndex);
    auto& runtime = g_slots[index];
    auto& published = g_state->slots[index];
    published.output_window = reinterpret_cast<std::uint64_t>(outputWindow);
    published.desktop_rect = desktopRect;
    wcsncpy_s(published.window_class, className, _TRUNCATE);

    const LONG requestSerial = g_state->request_serial;
    const LONG observedInterval = g_state->requested_interval_ms;
    const DWORD interval = static_cast<DWORD>(std::max<LONG>(0, observedInterval));
    const bool explicitlyRequested = published.completed_request_serial != requestSerial;
    const bool intervalDue = interval != 0 &&
        (runtime.lastCopyTick == 0 || now - runtime.lastCopyTick >= interval);
    if (!explicitlyRequested && !intervalDue)
    {
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }
    if (published.frame_number > published.consumed_frame_number)
    {
        // 接收端尚未消费上一帧时不再碰 keyed mutex；Present 线程只读两个
        // 共享计数器后立即返回，不进行重试、自旋或 GPU 复制。
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }

    if (!runtime.sharedTexture || runtime.width != sourceDesc.Width ||
        runtime.height != sourceDesc.Height || runtime.format != sourceDesc.Format)
    {
        ReleaseRuntimeSlot(index, false);
        const HRESULT createHr = CreateSharedTexture(index, source.Get(), sourceDesc);
        if (FAILED(createHr))
        {
            SetFailure(createHr);
            ReleaseSRWLockExclusive(&g_slotLock);
            return;
        }
    }

    const HRESULT acquireHr = runtime.keyedMutex->AcquireSync(0, 0);
    if (acquireHr == WAIT_TIMEOUT)
    {
        InterlockedIncrement64(&published.skipped_frames);
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }
    if (FAILED(acquireHr))
    {
        SetFailure(acquireHr);
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }

    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (sourceDesc.SampleDesc.Count > 1)
    {
        context->ResolveSubresource(runtime.resolveTexture.Get(), 0, source.Get(), 0,
            sourceDesc.Format);
        context->CopyResource(runtime.sharedTexture.Get(), runtime.resolveTexture.Get());
    }
    else
    {
        context->CopyResource(runtime.sharedTexture.Get(), source.Get());
    }
    // ReleaseSync establishes the cross-device producer/consumer ordering.
    // An explicit Flush here forces a command-buffer submission inside the
    // wallpaper's Present path and carries significant per-frame overhead.
    runtime.keyedMutex->ReleaseSync(1);
    runtime.lastCopyTick = now;
    InterlockedExchange(&published.completed_request_serial, requestSerial);
    InterlockedIncrement64(&published.frame_number);
    ReleaseSRWLockExclusive(&g_slotLock);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
{
    ActiveHook active;
    if (g_state)
        InterlockedIncrement64(&g_state->present_calls);
    CaptureSwapChain(chain);
    return g_present(chain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* chain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    ActiveHook active;
    if (g_state)
        InterlockedIncrement64(&g_state->present_calls);
    CaptureSwapChain(chain);
    return g_present1(chain, syncInterval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* chain, UINT bufferCount,
    UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    ActiveHook active;
    AcquireSRWLockExclusive(&g_slotLock);
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (g_slots[i].swapChain == chain)
        {
            ReleaseRuntimeSlot(i, true);
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_slotLock);
    return g_resizeBuffers(chain, bufferCount, width, height, format, flags);
}

HRESULT STDMETHODCALLTYPE HookD3D9Present(IDirect3DDevice9* device,
    const RECT* sourceRect, const RECT* destinationRect, HWND destinationWindow,
    const RGNDATA* dirtyRegion)
{
    ActiveHook active;
    if (g_state)
        InterlockedIncrement64(&g_state->present_calls);
    CaptureD3D9Device(device, nullptr, destinationWindow);
    return g_d3d9Present(device, sourceRect, destinationRect,
        destinationWindow, dirtyRegion);
}

HRESULT STDMETHODCALLTYPE HookD3D9PresentEx(IDirect3DDevice9Ex* device,
    const RECT* sourceRect, const RECT* destinationRect, HWND destinationWindow,
    const RGNDATA* dirtyRegion, DWORD flags)
{
    ActiveHook active;
    if (g_state)
        InterlockedIncrement64(&g_state->present_calls);
    CaptureD3D9Device(device, nullptr, destinationWindow);
    return g_d3d9PresentEx(device, sourceRect, destinationRect,
        destinationWindow, dirtyRegion, flags);
}

HRESULT STDMETHODCALLTYPE HookD3D9SwapChainPresent(IDirect3DSwapChain9* swapChain,
    const RECT* sourceRect, const RECT* destinationRect, HWND destinationWindow,
    const RGNDATA* dirtyRegion, DWORD flags)
{
    ActiveHook active;
    if (g_state)
        InterlockedIncrement64(&g_state->present_calls);
    ComPtr<IDirect3DDevice9> device;
    if (swapChain && SUCCEEDED(swapChain->GetDevice(&device)) && device)
        CaptureD3D9Device(device.Get(), swapChain, destinationWindow);
    return g_d3d9SwapChainPresent(swapChain, sourceRect, destinationRect,
        destinationWindow, dirtyRegion, flags);
}

void ReleaseD3D9DeviceSlots(IDirect3DDevice9* device)
{
    AcquireSRWLockExclusive(&g_slotLock);
    for (std::size_t i = 0; i < g_slots.size(); ++i)
    {
        if (g_slots[i].d3d9Device == device)
            ReleaseRuntimeSlot(i, true);
    }
    ReleaseSRWLockExclusive(&g_slotLock);
}

HRESULT STDMETHODCALLTYPE HookD3D9Reset(IDirect3DDevice9* device,
    D3DPRESENT_PARAMETERS* parameters)
{
    ActiveHook active;
    ReleaseD3D9DeviceSlots(device);
    return g_d3d9Reset(device, parameters);
}

HRESULT STDMETHODCALLTYPE HookD3D9ResetEx(IDirect3DDevice9Ex* device,
    D3DPRESENT_PARAMETERS* parameters, D3DDISPLAYMODEEX* fullscreenMode)
{
    ActiveHook active;
    ReleaseD3D9DeviceSlots(device);
    return g_d3d9ResetEx(device, parameters, fullscreenMode);
}

LRESULT CALLBACK DummyWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

HRESULT DiscoverHookTargets(void** present, void** present1, void** resizeBuffers)
{
    constexpr wchar_t className[] = L"SnowDesktopWallpaperHookDummy";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DummyWindowProc;
    windowClass.hInstance = g_module;
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, g_module, nullptr);
    if (!window)
        return HRESULT_FROM_WIN32(GetLastError());

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swapChain,
        &device, &featureLevel, &context);
    if (SUCCEEDED(hr))
    {
        void** vtable = *reinterpret_cast<void***>(swapChain.Get());
        *present = vtable[8];
        *resizeBuffers = vtable[13];
        ComPtr<IDXGISwapChain1> swapChain1;
        if (SUCCEEDED(swapChain.As(&swapChain1)))
        {
            void** vtable1 = *reinterpret_cast<void***>(swapChain1.Get());
            *present1 = vtable1[22];
        }
    }

    swapChain.Reset();
    context.Reset();
    device.Reset();
    DestroyWindow(window);
    UnregisterClassW(className, g_module);
    return hr;
}

HRESULT DiscoverD3D9HookTargets(void** present, void** presentEx,
    void** swapChainPresent, void** reset, void** resetEx)
{
    constexpr wchar_t className[] = L"SnowDesktopWallpaperHookD3D9Dummy";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DummyWindowProc;
    windowClass.hInstance = g_module;
    windowClass.lpszClassName = className;
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, className, L"", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, g_module, nullptr);
    if (!window)
        return HRESULT_FROM_WIN32(GetLastError());

    D3DPRESENT_PARAMETERS parameters{};
    parameters.BackBufferWidth = 64;
    parameters.BackBufferHeight = 64;
    parameters.BackBufferFormat = D3DFMT_A8R8G8B8;
    parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    ComPtr<IDirect3D9Ex> d3d9;
    ComPtr<IDirect3DDevice9Ex> device;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9);
    if (SUCCEEDED(hr) && d3d9)
    {
        hr = d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &parameters, nullptr, &device);
        if (FAILED(hr))
        {
            hr = d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                &parameters, nullptr, &device);
        }
    }
    if (SUCCEEDED(hr) && device)
    {
        void** vtable = *reinterpret_cast<void***>(device.Get());
        *reset = vtable[16];
        *present = vtable[17];
        *presentEx = vtable[121];
        *resetEx = vtable[132];
        ComPtr<IDirect3DSwapChain9> swapChain;
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain)
        {
            void** swapChainVtable = *reinterpret_cast<void***>(swapChain.Get());
            *swapChainPresent = swapChainVtable[3];
        }
    }

    device.Reset();
    d3d9.Reset();
    DestroyWindow(window);
    UnregisterClassW(className, g_module);
    return hr;
}

DWORD WINAPI WorkerThread(void*)
{
    wchar_t mappingName[128]{};
    MakeMappingName(GetCurrentProcessId(), mappingName);
    g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(SharedState)), mappingName);
    if (!g_mapping)
        return 1;
    g_state = static_cast<SharedState*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS,
        0, 0, sizeof(SharedState)));
    if (!g_state)
        return 2;
    ZeroMemory(g_state, sizeof(*g_state));
    g_state->magic = kMagic;
    g_state->version = kVersion;
    g_state->process_id = GetCurrentProcessId();
    g_state->status = static_cast<LONG>(Status::starting);
    InterlockedExchange64(&g_state->producer_heartbeat,
        static_cast<LONG64>(GetTickCount64()));

    void* present = nullptr;
    void* present1 = nullptr;
    void* resizeBuffers = nullptr;
    const HRESULT discoverHr = DiscoverHookTargets(&present, &present1, &resizeBuffers);
    if (FAILED(discoverHr) || !present || !resizeBuffers)
    {
        SetFailure(FAILED(discoverHr) ? discoverHr : E_NOINTERFACE);
        return 3;
    }

    void* d3d9Present = nullptr;
    void* d3d9PresentEx = nullptr;
    void* d3d9SwapChainPresent = nullptr;
    void* d3d9Reset = nullptr;
    void* d3d9ResetEx = nullptr;
    const HRESULT d3d9DiscoverHr = DiscoverD3D9HookTargets(&d3d9Present,
        &d3d9PresentEx, &d3d9SwapChainPresent, &d3d9Reset, &d3d9ResetEx);

    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(present, &HookPresent, reinterpret_cast<void**>(&g_present)) != MH_OK ||
        MH_CreateHook(resizeBuffers, &HookResizeBuffers,
            reinterpret_cast<void**>(&g_resizeBuffers)) != MH_OK)
    {
        SetFailure(E_FAIL);
        return 4;
    }
    if (present1)
    {
        const MH_STATUS createStatus = MH_CreateHook(present1, &HookPresent1,
            reinterpret_cast<void**>(&g_present1));
        if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        {
            SetFailure(E_FAIL);
            return 5;
        }
    }
    if (SUCCEEDED(d3d9DiscoverHr) && d3d9Present && d3d9Reset)
    {
        if (MH_CreateHook(d3d9Present, &HookD3D9Present,
                reinterpret_cast<void**>(&g_d3d9Present)) != MH_OK ||
            MH_CreateHook(d3d9Reset, &HookD3D9Reset,
                reinterpret_cast<void**>(&g_d3d9Reset)) != MH_OK)
        {
            SetFailure(E_FAIL);
            return 6;
        }
        if (d3d9SwapChainPresent &&
            MH_CreateHook(d3d9SwapChainPresent, &HookD3D9SwapChainPresent,
                reinterpret_cast<void**>(&g_d3d9SwapChainPresent)) != MH_OK)
        {
            SetFailure(E_FAIL);
            return 7;
        }
        if (d3d9PresentEx &&
            MH_CreateHook(d3d9PresentEx, &HookD3D9PresentEx,
                reinterpret_cast<void**>(&g_d3d9PresentEx)) != MH_OK)
        {
            SetFailure(E_FAIL);
            return 8;
        }
        if (d3d9ResetEx &&
            MH_CreateHook(d3d9ResetEx, &HookD3D9ResetEx,
                reinterpret_cast<void**>(&g_d3d9ResetEx)) != MH_OK)
        {
            SetFailure(E_FAIL);
            return 9;
        }
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        SetFailure(E_FAIL);
        return 10;
    }
    InterlockedExchange(&g_state->status, static_cast<LONG>(Status::hooked));

    while (!g_state->shutdown_requested)
    {
        InterlockedExchange64(&g_state->producer_heartbeat,
            static_cast<LONG64>(GetTickCount64()));
        Sleep(100);
    }
    InterlockedExchange(&g_state->status, static_cast<LONG>(Status::stopping));
    InterlockedExchange(&g_state->capture_enabled, 0);
    MH_DisableHook(MH_ALL_HOOKS);
    for (int attempt = 0; attempt < 100 && g_state->active_hooks != 0; ++attempt)
        Sleep(10);
    MH_Uninitialize();

    AcquireSRWLockExclusive(&g_slotLock);
    for (std::size_t i = 0; i < g_slots.size(); ++i)
        ReleaseRuntimeSlot(i, true);
    ReleaseSRWLockExclusive(&g_slotLock);
    UnmapViewOfFile(g_state);
    g_state = nullptr;
    CloseHandle(g_mapping);
    g_mapping = nullptr;
    FreeLibraryAndExitThread(g_module, 0);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
