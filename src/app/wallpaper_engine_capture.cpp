/**
 * @file wallpaper_engine_capture.cpp
 * @brief 注入 Wallpaper Engine 的轻量 GPU Hook，并接收共享纹理。
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
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace snow::wallpaper_hook;

namespace {

constexpr ULONGLONG kProducerHeartbeatTimeoutMs = 2500;
constexpr ULONGLONG kHookInitializationTimeoutMs = 5000;
constexpr ULONGLONG kHookInitializationPollMs = 250;
constexpr ULONGLONG kReconnectMaxDelayMs = 8000;

enum class WallpaperProcessArchitecture {
    x86,
    x64,
};

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

bool QueryWallpaperProcessArchitecture(DWORD processId,
    WallpaperProcessArchitecture& architecture, std::wstring& error)
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
    if (!queried)
    {
        const DWORD queryError = GetLastError();
        CloseHandle(process);
        error = FormatSystemError(L"无法读取 Wallpaper Engine 路径", queryError);
        return false;
    }
    std::wstring fileName = std::filesystem::path(path).filename().wstring();
    std::transform(fileName.begin(), fileName.end(), fileName.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });

    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(
        GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    bool architectureKnown = false;
    if (isWow64Process2)
    {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine))
        {
            const DWORD architectureError = GetLastError();
            CloseHandle(process);
            error = FormatSystemError(L"无法识别 Wallpaper Engine 架构",
                architectureError);
            return false;
        }
        if (processMachine == IMAGE_FILE_MACHINE_I386)
        {
            architecture = WallpaperProcessArchitecture::x86;
            architectureKnown = true;
        }
        else if (processMachine == IMAGE_FILE_MACHINE_AMD64 ||
            (processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
                nativeMachine == IMAGE_FILE_MACHINE_AMD64))
        {
            architecture = WallpaperProcessArchitecture::x64;
            architectureKnown = true;
        }
    }
    else
    {
        BOOL isWow64 = FALSE;
        if (!IsWow64Process(process, &isWow64))
        {
            const DWORD architectureError = GetLastError();
            CloseHandle(process);
            error = FormatSystemError(L"无法识别 Wallpaper Engine 架构",
                architectureError);
            return false;
        }
        architecture = isWow64
            ? WallpaperProcessArchitecture::x86
            : WallpaperProcessArchitecture::x64;
        architectureKnown = true;
    }
    CloseHandle(process);

    if (!architectureKnown)
    {
        error = L"GPU Hook 不支持当前 Wallpaper Engine 进程架构";
        return false;
    }
    const bool known32BitRenderer =
        fileName == L"wallpaper32.exe" ||
        fileName == L"webwallpaper32.exe" ||
        fileName == L"edgewallpaper32.exe" ||
        fileName == L"msedgewebview2.exe";
    const bool known64BitRenderer =
        fileName == L"wallpaper64.exe" ||
        fileName == L"webwallpaper64.exe" ||
        fileName == L"edgewallpaper64.exe" ||
        fileName == L"msedgewebview2.exe";
    const bool expectedExecutable =
        (architecture == WallpaperProcessArchitecture::x86 && known32BitRenderer) ||
        (architecture == WallpaperProcessArchitecture::x64 && known64BitRenderer);
    if (!expectedExecutable)
    {
        error = L"目标进程不是受支持的 Wallpaper Engine 渲染器";
        return false;
    }
    return true;
}

std::filesystem::path ApplicationDirectory()
{
    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath,
        static_cast<DWORD>(std::size(modulePath)));
    if (length == 0 || length >= std::size(modulePath))
        return {};
    return std::filesystem::path(modulePath).parent_path();
}

std::filesystem::path HookDllPath(WallpaperProcessArchitecture architecture)
{
    const auto directory = ApplicationDirectory();
    if (directory.empty())
        return {};
    return directory / (architecture == WallpaperProcessArchitecture::x86
        ? L"SnowDesktopWallpaperHook32.dll"
        : L"SnowDesktopWallpaperHook.dll");
}

std::filesystem::path Injector32Path()
{
    const auto directory = ApplicationDirectory();
    return directory.empty()
        ? std::filesystem::path{}
        : directory / L"SnowDesktopWallpaperInjector32.exe";
}

bool InjectHookDirect(DWORD processId, const std::filesystem::path& dllPath,
    std::wstring& error)
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

    const DWORD waitResult = WaitForSingleObject(thread, 10000);
    DWORD remoteModule = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &remoteModule) && remoteModule != 0;
    CloseHandle(thread);
    if (waitResult == WAIT_OBJECT_0)
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    if (waitResult != WAIT_OBJECT_0)
    {
        error = L"Wallpaper Engine Hook 加载超时";
        return false;
    }
    if (!loaded)
    {
        error = L"Wallpaper Engine Hook DLL 加载失败";
        return false;
    }
    return true;
}

bool InjectHook32(DWORD processId, const std::filesystem::path& dllPath,
    std::wstring& error)
{
    const std::filesystem::path injectorPath = Injector32Path();
    if (injectorPath.empty() || !std::filesystem::is_regular_file(injectorPath))
    {
        error = L"SnowDesktopWallpaperInjector32.exe 不存在";
        return false;
    }

    std::wstring commandLine = L"\"" + injectorPath.wstring() + L"\" " +
        std::to_wstring(processId) + L" \"" +
        std::filesystem::absolute(dllPath).wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(injectorPath.c_str(), mutableCommand.data(),
        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        injectorPath.parent_path().c_str(), &startup, &processInfo);
    if (!created)
    {
        error = FormatSystemError(L"无法启动 32 位 Hook 注入器", GetLastError());
        return false;
    }

    CloseHandle(processInfo.hThread);
    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, 15000);
    DWORD exitCode = STILL_ACTIVE;
    const bool completed = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);
    if (!completed)
    {
        error = L"32 位 Wallpaper Engine Hook 注入超时";
        return false;
    }
    if (exitCode != 0)
    {
        wchar_t message[192]{};
        swprintf_s(message, L"32 位 Wallpaper Engine Hook 注入失败（代码 %lu）",
            static_cast<unsigned long>(exitCode));
        error = message;
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
        ComPtr<ID3D11Query> completionQuery;
        ComPtr<ID2D1Bitmap1> bitmap;
        LONG64 lastConsumedFrame = 0;
        LONG64 acquiredFrame = 0;
        LONG64 pendingConsumedFrame = 0;
        bool acquired = false;
        bool completionPending = false;

        void ResetResource()
        {
            bitmap.Reset();
            completionQuery.Reset();
            keyedMutex.Reset();
            texture.Reset();
            generation = -1;
            acquiredFrame = 0;
            pendingConsumedFrame = 0;
            acquired = false;
            completionPending = false;
        }
    };

    DWORD rendererPid = 0;
    HANDLE mapping = nullptr;
    SharedState* state = nullptr;
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
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
        d3dContext.Reset();
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
        if (generation == opened.generation && opened.texture &&
            (opened.keyedMutex || opened.completionQuery))
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
            opened.keyedMutex.Reset();
            D3D11_QUERY_DESC queryDesc{};
            queryDesc.Query = D3D11_QUERY_EVENT;
            hr = d3dDevice->CreateQuery(&queryDesc, &opened.completionQuery);
            if (FAILED(hr) || !opened.completionQuery)
            {
                lastError = FormatHresult(L"创建视频共享纹理同步查询失败", hr);
                opened.ResetResource();
                return false;
            }
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
        D3D11_TEXTURE2D_DESC textureDesc{};
        opened.texture->GetDesc(&textureDesc);
        const auto format = textureDesc.Format;
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

    HRESULT CompletePendingRead(std::size_t index)
    {
        auto& opened = slots[index];
        if (!opened.completionPending)
            return S_OK;
        if (!d3dContext || !opened.completionQuery)
            return E_NOINTERFACE;
        BOOL complete = FALSE;
        const HRESULT hr = d3dContext->GetData(opened.completionQuery.Get(),
            &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE || (SUCCEEDED(hr) && !complete))
            return S_FALSE;
        if (FAILED(hr))
        {
            lastError = FormatHresult(L"等待视频共享纹理读取完成失败", hr);
            return hr;
        }
        if (state)
            InterlockedExchange64(&state->slots[index].consumed_frame_number,
                opened.pendingConsumedFrame);
        opened.lastConsumedFrame = opened.pendingConsumedFrame;
        opened.pendingConsumedFrame = 0;
        opened.completionPending = false;
        return S_OK;
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
    if (!impl_->d3dContext)
        impl_->d3dDevice->GetImmediateContext(&impl_->d3dContext);
    impl_->d2dContext = d2dContext;

    if (impl_->IsReconnectDelayed())
        return false;

    if (!impl_->OpenMapping())
    {
        const ULONGLONG now = GetTickCount64();
        if (!impl_->injectionAttempted)
        {
            WallpaperProcessArchitecture architecture{};
            if (!QueryWallpaperProcessArchitecture(rendererPid, architecture,
                    impl_->lastError))
                return false;
            const std::filesystem::path hookPath = HookDllPath(architecture);
            if (hookPath.empty() || !std::filesystem::is_regular_file(hookPath))
            {
                impl_->lastError = architecture == WallpaperProcessArchitecture::x86
                    ? L"SnowDesktopWallpaperHook32.dll 不存在"
                    : L"SnowDesktopWallpaperHook.dll 不存在";
                return false;
            }
            impl_->injectionAttempted = true;
            impl_->injectionAttemptTick = now;
            const bool injected = architecture == WallpaperProcessArchitecture::x86
                ? InjectHook32(rendererPid, hookPath, impl_->lastError)
                : InjectHookDirect(rendererPid, hookPath, impl_->lastError);
            if (!injected)
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
        const HRESULT completionHr = impl_->CompletePendingRead(index);
        if (completionHr == S_FALSE)
            continue;
        if (FAILED(completionHr))
        {
            ReleaseFrames(false);
            return WallpaperEngineFrameState::error;
        }
        const LONG64 frameNumber = published.frame_number;
        if (frameNumber <= opened.lastConsumedFrame)
            continue;
        if (!impl_->OpenSlot(index))
            continue;
        if (opened.keyedMutex)
        {
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
        }
        opened.acquired = true;
        opened.acquiredFrame = frameNumber;
        if (!impl_->EnsureBitmap(index))
        {
            ReleaseFrames(false);
            return WallpaperEngineFrameState::error;
        }
        WallpaperEngineFrame frame{};
        frame.processId = impl_->rendererPid;
        frame.desktopRect = published.desktop_rect;
        frame.outputWindow = reinterpret_cast<HWND>(published.output_window);
        frame.bitmap = opened.bitmap.Get();
        frame.d3d9Video = !opened.keyedMutex;
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
        impl_->lastError = L"等待 Wallpaper Engine GPU 交换链";
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
        {
            slot.keyedMutex->ReleaseSync(0);
            if (impl_->state)
                InterlockedExchange64(&impl_->state->slots[index].consumed_frame_number,
                    slot.acquiredFrame);
        }
        else if (consumed && slot.completionQuery && impl_->d3dContext)
        {
            // D3D9Ex 与 D3D11 共享表面没有 keyed mutex。让 D2D 提交读取，
            // 再用 D3D11 EVENT query 异步确认 GPU 已经不再访问该表面；生产端
            // 在 consumed_frame_number 前进前不会覆盖它。
            if (impl_->d2dContext)
                impl_->d2dContext->Flush();
            impl_->d3dContext->End(slot.completionQuery.Get());
            impl_->d3dContext->Flush();
            slot.pendingConsumedFrame = slot.acquiredFrame;
            slot.completionPending = true;
        }
        else if (impl_->state)
        {
            InterlockedExchange64(&impl_->state->slots[index].consumed_frame_number,
                slot.acquiredFrame);
            slot.lastConsumedFrame = slot.acquiredFrame;
        }
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
