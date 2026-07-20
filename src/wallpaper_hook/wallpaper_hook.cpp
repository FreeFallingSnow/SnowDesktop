#include "wallpaper_hook_protocol.h"

#include <MinHook.h>
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

struct RuntimeSlot {
    IDXGISwapChain* swapChain = nullptr;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<ID3D11Texture2D> resolveTexture;
    ComPtr<IDXGIKeyedMutex> keyedMutex;
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ULONGLONG lastCopyTick = 0;
};

HMODULE g_module = nullptr;
HANDLE g_mapping = nullptr;
SharedState* g_state = nullptr;
PresentFn g_present = nullptr;
Present1Fn g_present1 = nullptr;
ResizeBuffersFn g_resizeBuffers = nullptr;
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
        _wcsicmp(className, L"WPECloneView") == 0;
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
    runtime.width = 0;
    runtime.height = 0;
    runtime.format = DXGI_FORMAT_UNKNOWN;
    runtime.lastCopyTick = 0;
    ClearPublishedSlot(index);
    if (removeSwapChain)
    {
        runtime.swapChain = nullptr;
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
        if (g_slots[i].swapChain)
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

void CaptureSwapChain(IDXGISwapChain* chain)
{
    if (!g_state || g_state->shutdown_requested || !g_state->capture_enabled)
        return;

    const ULONGLONG now = GetTickCount64();
    const auto heartbeat = static_cast<ULONGLONG>(g_state->consumer_heartbeat);
    if (heartbeat == 0 || (now >= heartbeat && now - heartbeat > 5000))
        return;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(chain->GetDesc(&swapDesc)) || !swapDesc.OutputWindow)
        return;
    wchar_t className[64]{};
    if (!GetClassNameW(swapDesc.OutputWindow, className,
            static_cast<int>(std::size(className))) || !IsWallpaperEngineClass(className))
        return;

    ComPtr<ID3D11Texture2D> source;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&source))) || !source)
        return;
    D3D11_TEXTURE2D_DESC sourceDesc{};
    source->GetDesc(&sourceDesc);
    if (sourceDesc.Width < 640 || sourceDesc.Height < 360)
        return;

    AcquireSRWLockExclusive(&g_slotLock);
    const int slotIndex = FindOrCreateSlot(chain, swapDesc.OutputWindow, className);
    if (slotIndex < 0)
    {
        ReleaseSRWLockExclusive(&g_slotLock);
        return;
    }
    const auto index = static_cast<std::size_t>(slotIndex);
    auto& runtime = g_slots[index];
    auto& published = g_state->slots[index];
    GetWindowRect(swapDesc.OutputWindow, &published.desktop_rect);

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
    context->Flush();
    runtime.keyedMutex->ReleaseSync(1);
    runtime.lastCopyTick = now;
    InterlockedExchange(&published.completed_request_serial, requestSerial);
    InterlockedIncrement64(&published.frame_number);
    ReleaseSRWLockExclusive(&g_slotLock);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* chain, UINT syncInterval, UINT flags)
{
    ActiveHook active;
    CaptureSwapChain(chain);
    return g_present(chain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* chain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters)
{
    ActiveHook active;
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
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        SetFailure(E_FAIL);
        return 6;
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
