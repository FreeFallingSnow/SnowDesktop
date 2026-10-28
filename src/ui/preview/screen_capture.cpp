#include "screen_capture.h"
#include "preview_png_writer.h"
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace snowdesktop::screen_capture
{
namespace
{
using namespace winrt::Windows::Graphics;
std::int64_t UnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
BOOL CALLBACK VisitMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM parameter)
{
    MONITORINFOEXW information{}; information.cbSize = sizeof(information);
    if (GetMonitorInfoW(monitor, &information))
        reinterpret_cast<std::vector<Monitor>*>(parameter)->push_back(
            {information.szDevice, information.rcMonitor, (information.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}
struct Apartment
{
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};
struct CaptureLifetime
{
    Capture::Direct3D11CaptureFramePool pool{nullptr};
    Capture::GraphicsCaptureSession session{nullptr};
    ~CaptureLifetime()
    {
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
    }
};
struct FrameLifetime
{
    Capture::Direct3D11CaptureFrame frame{nullptr};
    ~FrameLifetime() { try { if (frame) frame.Close(); } catch (...) {} }
};
struct Pixels
{
    int width = 0, height = 0;
    std::int64_t time100ns = 0;
    std::vector<std::uint32_t> data;
};
Pixels ReadFrame(const Monitor& monitor, bool systemCursor, const std::atomic<bool>& cancelled)
{
    if (!Capture::GraphicsCaptureSession::IsSupported()) throw std::runtime_error("capture_unsupported");
    const POINT center{monitor.bounds.left + (monitor.bounds.right - monitor.bounds.left) / 2,
        monitor.bounds.top + (monitor.bounds.bottom - monitor.bounds.top) / 2};
    const auto handle = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
    MONITORINFOEXW current{}; current.cbSize = sizeof(current);
    if (!handle || !GetMonitorInfoW(handle, &current) || _wcsicmp(current.szDevice, monitor.id.c_str()) ||
        !EqualRect(&current.rcMonitor, &monitor.bounds)) throw std::runtime_error("display_changed");
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    auto created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    if (FAILED(created)) created = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, device.put(), nullptr, context.put());
    winrt::check_hresult(created);
    const auto dxgi = device.as<IDXGIDevice>();
    winrt::com_ptr<IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    const auto graphicsDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    const auto interop = winrt::get_activation_factory<Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    Capture::GraphicsCaptureItem item{nullptr};
    winrt::check_hresult(interop->CreateForMonitor(handle, winrt::guid_of<Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    const auto size = item.Size();
    if (size.Width <= 0 || size.Height <= 0 || size.Width > 16384 || size.Height > 16384 ||
        static_cast<std::uint64_t>(size.Width) * size.Height > 64ULL * 1024 * 1024)
        throw std::runtime_error("capture_too_large");
    CaptureLifetime capture;
    capture.pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(graphicsDevice,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    capture.session = capture.pool.CreateCaptureSession(item);
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
        capture.session.IsCursorCaptureEnabled(systemCursor);
    else if (!systemCursor) throw std::runtime_error("cursor_exclusion_unavailable");
    capture.session.StartCapture();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cancelled.load()) throw std::runtime_error("capture_cancelled");
        FrameLifetime next{capture.pool.TryGetNextFrame()};
        if (!next.frame) { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
        const auto content = next.frame.ContentSize();
        if (content.Width != size.Width || content.Height != size.Height) throw std::runtime_error("display_changed");
        const auto surface = next.frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(surface->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        D3D11_TEXTURE2D_DESC description{}; texture->GetDesc(&description);
        if (description.Width < static_cast<UINT>(size.Width) || description.Height < static_cast<UINT>(size.Height) ||
            description.Format != DXGI_FORMAT_B8G8R8A8_UNORM || description.SampleDesc.Count != 1)
            throw std::runtime_error("unsupported_capture_surface");
        description.Usage = D3D11_USAGE_STAGING; description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ; description.MiscFlags = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
        context->CopyResource(staging.get(), texture.get());
        Pixels pixels; pixels.width = size.Width; pixels.height = size.Height;
        pixels.time100ns = next.frame.SystemRelativeTime().count();
        pixels.data.resize(static_cast<std::size_t>(size.Width) * size.Height);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (int y = 0; y < size.Height; ++y)
            std::memcpy(pixels.data.data() + static_cast<std::size_t>(y) * size.Width,
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                static_cast<std::size_t>(size.Width) * 4);
        context->Unmap(staging.get(), 0);
        for (auto& pixel : pixels.data) pixel |= 0xff000000U;
        return pixels;
    }
    throw std::runtime_error("capture_frame_timeout");
}
}

std::vector<Monitor> Monitors()
{
    std::vector<Monitor> result;
    if (!EnumDisplayMonitors(nullptr, nullptr, VisitMonitor, reinterpret_cast<LPARAM>(&result)))
        throw std::runtime_error("monitor_enumeration_failed");
    return result;
}
struct Service::Impl
{
    mutable std::mutex mutex;
    State state;
    std::atomic<bool> cancelled{false};
    std::thread worker;
};
Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() { Stop(); }
State Service::Current() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
bool Service::Screenshot(std::wstring monitorId, std::filesystem::path root, bool systemCursor, std::string& error)
{
    error.clear();
    if (Current().status == "running") { error = "capture_busy"; return false; }
    Monitor selected;
    for (const auto& monitor : Monitors())
        if ((monitorId == L"primary" && monitor.primary) || !_wcsicmp(monitor.id.c_str(), monitorId.c_str()))
        { selected = monitor; break; }
    if (selected.id.empty()) { error = "monitor_not_found"; return false; }
    if (root.empty() || !root.is_absolute()) { error = "invalid_capture_root"; return false; }
    GUID guid{}; wchar_t identifier[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, identifier, 40)) { error = "capture_id_failed"; return false; }
    const std::wstring wideId(identifier + 1, 36);
    const std::string id(wideId.begin(), wideId.end());
    if (impl_->worker.joinable()) impl_->worker.join();
    State state; state.id = id; state.status = "running"; state.monitor = selected;
    state.systemCursor = systemCursor; state.startedUnixMs = UnixMs();
    impl_->cancelled.store(false);
    { std::lock_guard lock(impl_->mutex); impl_->state = state; }
    try
    {
        impl_->worker = std::thread([this, root = std::move(root), state]() mutable {
            try
            {
                Apartment apartment;
                std::error_code ec;
                std::filesystem::create_directories(root, ec);
                const auto directory = root / state.id;
                if (ec || !std::filesystem::create_directory(directory, ec) || ec)
                    throw std::runtime_error("capture_directory_failed");
                const auto pixels = ReadFrame(state.monitor, state.systemCursor, impl_->cancelled);
                if (impl_->cancelled.load()) throw std::runtime_error("capture_cancelled");
                std::string pngError;
                const auto output = directory / L"capture.png";
                if (!preview_png::Save(output, pixels.width, pixels.height, pixels.data, pngError))
                    throw std::runtime_error("capture_png_failed: " + pngError);
                state.width = pixels.width; state.height = pixels.height; state.path = output;
                state.frameTime100ns = pixels.time100ns; state.status = "completed";
            }
            catch (const winrt::hresult_error& failure) { state.status = "failed"; state.error = winrt::to_string(failure.message()); }
            catch (const std::exception& failure) { state.status = "failed"; state.error = failure.what(); }
            catch (...) { state.status = "failed"; state.error = "capture_failure"; }
            if (impl_->cancelled.load()) { state.status = "cancelled"; state.error = "capture_cancelled"; }
            state.finishedUnixMs = UnixMs();
            std::lock_guard lock(impl_->mutex); impl_->state = std::move(state);
        });
    }
    catch (...) { std::lock_guard lock(impl_->mutex); impl_->state.status = "failed"; impl_->state.error = "capture_worker_failed"; error = impl_->state.error; return false; }
    return true;
}
void Service::Cancel() { impl_->cancelled.store(true); }
void Service::Stop() noexcept
{
    impl_->cancelled.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
}
}
