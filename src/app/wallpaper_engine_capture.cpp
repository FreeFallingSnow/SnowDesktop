/**
 * @file wallpaper_engine_capture.cpp
 * @brief 注入 Wallpaper Engine 的轻量 DXGI Hook，并接收共享 D3D11 纹理。
 */
#include "wallpaper_engine_capture.h"
#include "../wallpaper_hook/wallpaper_hook_protocol.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace snow::wallpaper_hook;

namespace {

constexpr ULONGLONG kProducerHeartbeatTimeoutMs = 2500;
constexpr ULONGLONG kHookInitializationTimeoutMs = 5000;
constexpr ULONGLONG kHookInitializationPollMs = 250;
constexpr ULONGLONG kReconnectMaxDelayMs = 8000;

std::wstring FormatSystemError(const wchar_t* stage, DWORD error)
{
    wchar_t message[192]{};
    swprintf_s(message, L"%s（错误 %lu）", stage, static_cast<unsigned long>(error));
    return message;
}

std::wstring FormatHresult(const wchar_t* stage, HRESULT hr)
{
    wchar_t message[192]{};
    swprintf_s(message, L"%s（0x%08X）", stage, static_cast<unsigned>(hr));
    return message;
}

bool IsWallpaper64Process(DWORD processId, std::wstring& error)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
    {
        error = FormatSystemError(L"无法查询 Wallpaper Engine 进程", GetLastError());
        return false;
    }
    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    if (!queried)
    {
        error = FormatSystemError(L"无法读取 Wallpaper Engine 路径", GetLastError());
        return false;
    }
    std::wstring fileName = std::filesystem::path(path).filename().wstring();
    std::transform(fileName.begin(), fileName.end(), fileName.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (fileName != L"wallpaper64.exe")
    {
        error = L"DXGI Hook 当前仅支持 64 位 Wallpaper Engine";
        return false;
    }
    return true;
}

std::filesystem::path HookDllPath()
{
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath,
        static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath))
        return {};
    return std::filesystem::path(modulePath).parent_path() /
        L"SnowDesktopWallpaperHook.dll";
}

bool InjectHook(DWORD processId, const std::filesystem::path& dllPath, std::wstring& error)
{
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
    if (!process)
    {
        error = FormatSystemError(L"无法打开 Wallpaper Engine 进程", GetLastError());
        return false;
    }

    const std::wstring path = std::filesystem::absolute(dllPath).wstring();
    const SIZE_T pathBytes = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, pathBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
    {
        error = FormatSystemError(L"Hook 路径远程内存分配失败", GetLastError());
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, path.c_str(), pathBytes, &written) ||
        written != pathBytes)
    {
        error = FormatSystemError(L"Hook 路径写入失败", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = loadLibrary
        ? CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr)
        : nullptr;
    if (!thread)
    {
        error = FormatSystemError(L"Wallpaper Engine Hook 注入失败", GetLastError());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    if (waitResult != WAIT_OBJECT_0)
    {
        error = L"Wallpaper Engine Hook 加载超时";
        return false;
    }
    return true;
}

} // namespace

struct WallpaperEngineCaptureSession::Impl {
    struct OpenedSlot {
        LONG generation = -1;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        ComPtr<ID2D1Bitmap1> bitmap;
        LONG64 lastConsumedFrame = 0;
        LONG64 acquiredFrame = 0;
        bool acquired = false;

        void ResetResource()
        {
            bitmap.Reset();
            keyedMutex.Reset();
            texture.Reset();
            generation = -1;
            acquiredFrame = 0;
            acquired = false;
        }
    };

    DWORD rendererPid = 0;
    HANDLE mapping = nullptr;
    SharedState* state = nullptr;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    std::array<OpenedSlot, kMaxFrameSlots> slots;
    std::wstring lastError;
    ULONGLONG connectedTick = 0;
    bool injectionAttempted = false;
    ULONGLONG injectionAttemptTick = 0;
    ULONGLONG nextConnectAttemptTick = 0;
    unsigned reconnectAttempt = 0;
    std::wstring reconnectReason;

    ~Impl()
    {
        CloseMapping();
    }

    void CloseMapping()
    {
        for (auto& slot : slots)
        {
            if (slot.acquired && slot.keyedMutex)
                slot.keyedMutex->ReleaseSync(0);
            slot.ResetResource();
            slot.lastConsumedFrame = 0;
        }
        if (state)
            UnmapViewOfFile(state);
        if (mapping)
            CloseHandle(mapping);
        state = nullptr;
        mapping = nullptr;
        d2dContext.Reset();
        d3dDevice.Reset();
        connectedTick = 0;
    }

    void SignalHookShutdown()
    {
        if (!state || state->magic != kMagic || state->version != kVersion)
            return;
        InterlockedExchange(&state->capture_enabled, 0);
        InterlockedExchange64(&state->consumer_heartbeat, 0);
        InterlockedExchange(&state->shutdown_requested, 1);
    }

    void ScheduleReconnect(const std::wstring& reason)
    {
        SignalHookShutdown();
        CloseMapping();
        injectionAttempted = false;
        injectionAttemptTick = 0;
        reconnectReason = reason.empty() ? L"Wallpaper Engine Hook 连接中断" : reason;
        const unsigned shift = std::min<unsigned>(reconnectAttempt, 3);
        const ULONGLONG delay = std::min<ULONGLONG>(
            1000ull << shift, kReconnectMaxDelayMs);
        ++reconnectAttempt;
        nextConnectAttemptTick = GetTickCount64() + delay;
        lastError = reconnectReason + L"，等待自动重连";
    }

    bool IsReconnectDelayed()
    {
        const ULONGLONG now = GetTickCount64();
        if (!nextConnectAttemptTick || now >= nextConnectAttemptTick)
        {
            nextConnectAttemptTick = 0;
            return false;
        }
        const ULONGLONG remaining = nextConnectAttemptTick - now;
        lastError = reconnectReason + L"，等待自动重连（" +
            std::to_wstring(remaining) + L" ms）";
        return true;
    }

    void MarkConnected()
    {
        reconnectAttempt = 0;
        nextConnectAttemptTick = 0;
        reconnectReason.clear();
    }

    bool OpenMapping()
    {
        if (state)
        {
            if (state->magic == kMagic && state->version == kVersion &&
                state->process_id == rendererPid)
                return true;
            lastError = L"Wallpaper Engine Hook 共享协议已失效";
            CloseMapping();
        }
        wchar_t mappingName[128]{};
        MakeMappingName(rendererPid, mappingName);
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
        if (!mapping)
            return false;
        state = static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
            0, 0, sizeof(SharedState)));
        if (!state || state->magic != kMagic || state->version != kVersion ||
            state->process_id != rendererPid)
        {
            lastError = L"Wallpaper Engine Hook 共享协议不匹配";
            CloseMapping();
            return false;
        }
        connectedTick = GetTickCount64();
        return true;
    }

    bool OpenSlot(std::size_t index)
    {
        auto& opened = slots[index];
        auto& published = state->slots[index];
        const LONG generation = published.generation;
        if (generation == opened.generation && opened.texture && opened.keyedMutex)
            return true;
        opened.ResetResource();

        const std::uint64_t sharedHandle = published.shared_handle;
        if (!sharedHandle)
            return false;
        HRESULT hr = d3dDevice->OpenSharedResource(reinterpret_cast<HANDLE>(sharedHandle),
            IID_PPV_ARGS(&opened.texture));
        if (FAILED(hr) || !opened.texture)
        {
            lastError = FormatHresult(L"打开 Wallpaper Engine 共享纹理失败", hr);
            opened.ResetResource();
            return false;
        }
        hr = opened.texture.As(&opened.keyedMutex);
        if (FAILED(hr) || !opened.keyedMutex)
        {
            lastError = FormatHresult(L"打开 Wallpaper Engine 同步锁失败", hr);
            opened.ResetResource();
            return false;
        }
        opened.generation = generation;
        return true;
    }

    bool EnsureBitmap(std::size_t index)
    {
        auto& opened = slots[index];
        if (opened.bitmap)
            return true;
        ComPtr<IDXGISurface> surface;
        HRESULT hr = opened.texture.As(&surface);
        if (FAILED(hr) || !surface)
        {
            lastError = FormatHresult(L"共享纹理转换为 DXGI 表面失败", hr);
            return false;
        }
        const auto format = static_cast<DXGI_FORMAT>(state->slots[index].format);
        const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(format, D2D1_ALPHA_MODE_IGNORE));
        hr = d2dContext->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
            &opened.bitmap);
        if (FAILED(hr) || !opened.bitmap)
        {
            lastError = FormatHresult(L"共享纹理转换为 D2D 位图失败", hr);
            return false;
        }
        return true;
    }
};

WallpaperEngineCaptureSession::WallpaperEngineCaptureSession()
    : impl_(std::make_unique<Impl>())
{
}

WallpaperEngineCaptureSession::~WallpaperEngineCaptureSession()
{
    Stop();
}

bool WallpaperEngineCaptureSession::EnsureStarted(DWORD rendererPid,
    ID3D11Device* d3dDevice, ID2D1DeviceContext* d2dContext, DWORD updateIntervalMs)
{
    if (!rendererPid || !d3dDevice || !d2dContext)
    {
        impl_->lastError = L"Wallpaper Engine 捕获参数无效";
        return false;
    }
    if (impl_->rendererPid != rendererPid)
    {
        Stop();
        impl_->rendererPid = rendererPid;
    }
    impl_->d3dDevice = d3dDevice;
    impl_->d2dContext = d2dContext;

    if (impl_->IsReconnectDelayed())
        return false;

    if (!impl_->OpenMapping())
    {
        const ULONGLONG now = GetTickCount64();
        if (!impl_->injectionAttempted)
        {
            if (!IsWallpaper64Process(rendererPid, impl_->lastError))
                return false;
            const std::filesystem::path hookPath = HookDllPath();
            if (hookPath.empty() || !std::filesystem::is_regular_file(hookPath))
            {
                impl_->lastError = L"SnowDesktopWallpaperHook.dll 不存在";
                return false;
            }
            impl_->injectionAttempted = true;
            impl_->injectionAttemptTick = now;
            if (!InjectHook(rendererPid, hookPath, impl_->lastError))
            {
                impl_->ScheduleReconnect(impl_->lastError);
                return false;
            }
            for (int attempt = 0; attempt < 50 && !impl_->OpenMapping(); ++attempt)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!impl_->state)
        {
            if (impl_->injectionAttemptTick &&
                now - impl_->injectionAttemptTick >= kHookInitializationTimeoutMs)
                impl_->ScheduleReconnect(L"Wallpaper Engine Hook 初始化超时");
            else
            {
                impl_->nextConnectAttemptTick = now + kHookInitializationPollMs;
                impl_->reconnectReason = L"等待 Wallpaper Engine Hook 初始化";
                impl_->lastError = impl_->reconnectReason;
            }
            return false;
        }
    }

    const LONG status = impl_->state->status;
    if (status == static_cast<LONG>(Status::failed))
    {
        const std::wstring error = FormatHresult(L"Wallpaper Engine Hook 运行失败",
            static_cast<HRESULT>(impl_->state->last_hresult));
        impl_->ScheduleReconnect(error);
        return false;
    }
    if (status == static_cast<LONG>(Status::stopping) ||
        impl_->state->shutdown_requested)
    {
        impl_->ScheduleReconnect(L"Wallpaper Engine Hook 已停止");
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    const auto producerHeartbeat = static_cast<ULONGLONG>(
        impl_->state->producer_heartbeat);
    if ((!producerHeartbeat ||
            (now >= producerHeartbeat &&
                now - producerHeartbeat > kProducerHeartbeatTimeoutMs)) &&
        impl_->connectedTick && now - impl_->connectedTick > kProducerHeartbeatTimeoutMs)
    {
        impl_->ScheduleReconnect(L"Wallpaper Engine Hook 心跳中断");
        return false;
    }

    InterlockedExchange(&impl_->state->capture_enabled, 1);
    InterlockedExchange(&impl_->state->requested_interval_ms,
        static_cast<LONG>(updateIntervalMs));
    InterlockedExchange64(&impl_->state->consumer_heartbeat,
        static_cast<LONG64>(GetTickCount64()));
    if (status >= static_cast<LONG>(Status::hooked))
        impl_->MarkConnected();
    impl_->lastError.clear();
    return true;
}

void WallpaperEngineCaptureSession::RequestFrame()
{
    if (!impl_->state)
        return;
    const LONG currentRequest = impl_->state->request_serial;
    bool requestOutstanding = false;
    const LONG observedCount = impl_->state->slot_count;
    const LONG count = std::clamp<LONG>(observedCount, 0,
        static_cast<LONG>(kMaxFrameSlots));
    for (LONG index = 0; index < count; ++index)
    {
        const auto& published = impl_->state->slots[static_cast<std::size_t>(index)];
        if (published.swap_chain &&
            published.completed_request_serial != currentRequest)
        {
            requestOutstanding = true;
            break;
        }
    }
    if (!requestOutstanding)
        InterlockedIncrement(&impl_->state->request_serial);
}

WallpaperEngineFrameState WallpaperEngineCaptureSession::TryAcquireLatestFrames(
    std::vector<WallpaperEngineFrame>& frames)
{
    frames.clear();
    if (!impl_->state || !impl_->d3dDevice || !impl_->d2dContext)
    {
        impl_->lastError = L"Wallpaper Engine Hook 尚未连接";
        return WallpaperEngineFrameState::pending;
    }
    if (impl_->state->status == static_cast<LONG>(Status::failed))
    {
        impl_->lastError = FormatHresult(L"Wallpaper Engine Hook 运行失败",
            static_cast<HRESULT>(impl_->state->last_hresult));
        return WallpaperEngineFrameState::error;
    }

    const LONG observedCount = impl_->state->slot_count;
    const LONG count = std::clamp<LONG>(observedCount, 0,
        static_cast<LONG>(kMaxFrameSlots));
    bool sawPublishedSlot = false;
    for (LONG rawIndex = 0; rawIndex < count; ++rawIndex)
    {
        const auto index = static_cast<std::size_t>(rawIndex);
        auto& published = impl_->state->slots[index];
        auto& opened = impl_->slots[index];
        if (!published.swap_chain || !published.shared_handle)
            continue;
        sawPublishedSlot = true;
        const LONG64 frameNumber = published.frame_number;
        if (frameNumber <= opened.lastConsumedFrame)
            continue;
        if (!impl_->OpenSlot(index))
            continue;
        const HRESULT acquireHr = opened.keyedMutex->AcquireSync(1, 0);
        if (acquireHr == WAIT_TIMEOUT)
            continue;
        if (FAILED(acquireHr))
        {
            impl_->lastError = FormatHresult(L"获取 Wallpaper Engine 共享帧失败",
                acquireHr);
            ReleaseFrames(false);
            return WallpaperEngineFrameState::error;
        }
        opened.acquired = true;
        opened.acquiredFrame = frameNumber;
        if (!impl_->EnsureBitmap(index))
        {
            ReleaseFrames(false);
            return WallpaperEngineFrameState::error;
        }
        WallpaperEngineFrame frame{};
        frame.desktopRect = published.desktop_rect;
        frame.outputWindow = reinterpret_cast<HWND>(published.output_window);
        frame.bitmap = opened.bitmap.Get();
        frame.slotIndex = index;
        frame.frameNumber = static_cast<unsigned long long>(frameNumber);
        frames.push_back(frame);
    }

    if (!frames.empty())
    {
        impl_->lastError.clear();
        return WallpaperEngineFrameState::ready;
    }
    if (!sawPublishedSlot && impl_->connectedTick &&
        GetTickCount64() - impl_->connectedTick > 3000)
        impl_->lastError = L"等待 Wallpaper Engine DXGI 交换链";
    return WallpaperEngineFrameState::pending;
}

void WallpaperEngineCaptureSession::ReleaseFrames(bool consumed)
{
    for (std::size_t index = 0; index < impl_->slots.size(); ++index)
    {
        auto& slot = impl_->slots[index];
        if (!slot.acquired)
            continue;
        if (consumed)
            slot.lastConsumedFrame = slot.acquiredFrame;
        if (slot.keyedMutex)
            slot.keyedMutex->ReleaseSync(0);
        if (impl_->state)
            InterlockedExchange64(&impl_->state->slots[index].consumed_frame_number,
                slot.acquiredFrame);
        slot.acquired = false;
        slot.acquiredFrame = 0;
    }
}

void WallpaperEngineCaptureSession::RequestReconnect(const std::wstring& reason)
{
    impl_->ScheduleReconnect(reason);
}

void WallpaperEngineCaptureSession::Stop()
{
    impl_->SignalHookShutdown();
    impl_->CloseMapping();
    impl_->rendererPid = 0;
    impl_->injectionAttempted = false;
    impl_->injectionAttemptTick = 0;
    impl_->nextConnectAttemptTick = 0;
    impl_->reconnectAttempt = 0;
    impl_->reconnectReason.clear();
    impl_->lastError.clear();
}

bool WallpaperEngineCaptureSession::IsActiveFor(DWORD rendererPid) const
{
    return impl_->rendererPid == rendererPid && impl_->state &&
        impl_->state->status >= static_cast<LONG>(Status::hooked);
}

const std::wstring& WallpaperEngineCaptureSession::LastError() const
{
    return impl_->lastError;
}
