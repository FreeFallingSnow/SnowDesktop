#include "screen_capture.h"
#include "preview_png_writer.h"
#include "video_writer.h"
#include "platform/control/control_protocol.h"
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
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
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
    std::uint64_t sequence = 0;
    std::vector<std::uint32_t> data;
};
std::int64_t QpcTime()
{
    LARGE_INTEGER counter{}, frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
        throw std::runtime_error("capture_clock_unavailable");
    return counter.QuadPart / frequency.QuadPart * 10000000LL +
        counter.QuadPart % frequency.QuadPart * 10000000LL / frequency.QuadPart;
}
class Reader final
{
    Monitor monitor_;
    HMONITOR handle_ = nullptr;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11Texture2D> staging;
    D3D11_TEXTURE2D_DESC stagingDescription{};
    SizeInt32 size{};
    Capture::GraphicsCaptureItem item{nullptr};
    winrt::event_token closedToken{};
    std::shared_ptr<std::atomic<bool>> closed = std::make_shared<std::atomic<bool>>(false);
    CaptureLifetime capture;
    std::uint64_t sequence_ = 0;
public:
    Reader(const Monitor& monitor, bool systemCursor) : monitor_(monitor)
    {
    if (!Capture::GraphicsCaptureSession::IsSupported()) throw std::runtime_error("capture_unsupported");
    const POINT center{monitor.bounds.left + (monitor.bounds.right - monitor.bounds.left) / 2,
        monitor.bounds.top + (monitor.bounds.bottom - monitor.bounds.top) / 2};
    handle_ = MonitorFromPoint(center, MONITOR_DEFAULTTONULL);
    Validate();
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
    winrt::check_hresult(interop->CreateForMonitor(handle_, winrt::guid_of<Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
    closedToken = item.Closed([flag = closed](const auto&, const auto&) { flag->store(true); });
    size = item.Size();
    if (size.Width <= 0 || size.Height <= 0 || size.Width > 16384 || size.Height > 16384 ||
        static_cast<std::uint64_t>(size.Width) * size.Height > 64ULL * 1024 * 1024)
        throw std::runtime_error("capture_too_large");
    if (size.Width != monitor.bounds.right - monitor.bounds.left || size.Height != monitor.bounds.bottom - monitor.bounds.top)
        throw std::runtime_error("display_changed");
    capture.pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(graphicsDevice,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
    capture.session = capture.pool.CreateCaptureSession(item);
    if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
        capture.session.IsCursorCaptureEnabled(systemCursor);
    else if (!systemCursor) throw std::runtime_error("cursor_exclusion_unavailable");
    capture.session.StartCapture();
    }
    ~Reader()
    {
        try { if (item) item.Closed(closedToken); } catch (...) {}
    }
    void Validate() const
    {
        if (closed->load()) throw std::runtime_error("capture_source_closed");
        MONITORINFOEXW current{}; current.cbSize = sizeof(current);
        if (!handle_ || !GetMonitorInfoW(handle_, &current) || _wcsicmp(current.szDevice, monitor_.id.c_str()) ||
            !EqualRect(&current.rcMonitor, &monitor_.bounds)) throw std::runtime_error("display_changed");
    }
    std::uint64_t CapturedFrames() const noexcept { return sequence_; }
    bool Latest(Pixels& pixels)
    {
        if (closed->load()) throw std::runtime_error("capture_source_closed");
        // Return every checked-out surface to the bounded pool before another
        // poll. Read back only the newest one; older acquired surfaces count as
        // discarded rather than pretending they were encoded.
        FrameLifetime next;
        for (unsigned drain = 0; drain < 8; ++drain)
        {
            auto frame = capture.pool.TryGetNextFrame();
            if (!frame) break;
            if (next.frame) next.frame.Close();
            next.frame = std::move(frame); ++sequence_;
        }
        if (!next.frame) return false;
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
        if (!staging)
        {
            winrt::check_hresult(device->CreateTexture2D(&description, nullptr, staging.put()));
            stagingDescription = description;
        }
        else if (description.Width != stagingDescription.Width || description.Height != stagingDescription.Height ||
            description.MipLevels != stagingDescription.MipLevels || description.ArraySize != stagingDescription.ArraySize)
            throw std::runtime_error("display_changed");
        context->CopyResource(staging.get(), texture.get());
        pixels.width = size.Width; pixels.height = size.Height;
        pixels.time100ns = next.frame.SystemRelativeTime().count();
        pixels.sequence = sequence_;
        pixels.data.resize(static_cast<std::size_t>(size.Width) * size.Height);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (int y = 0; y < size.Height; ++y)
            std::memcpy(pixels.data.data() + static_cast<std::size_t>(y) * size.Width,
                static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
                static_cast<std::size_t>(size.Width) * 4);
        context->Unmap(staging.get(), 0);
        for (auto& pixel : pixels.data) pixel |= 0xff000000U;
        return true;
    }
    Pixels First(const std::atomic<bool>& cancelled, std::int64_t minimumTime100ns = 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (cancelled.load()) throw std::runtime_error("capture_cancelled");
            Pixels pixels;
            if (Latest(pixels))
            {
                Validate();
                // The pool can still contain images captured while a caller
                // prepared its consumer. Returning them would back-date the
                // recording clock, even though this checkout happened now.
                // Latest counts every acquired image, including these rejects.
                if (pixels.time100ns >= minimumTime100ns) return pixels;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        throw std::runtime_error("capture_frame_timeout");
    }
};
void WriteManifest(State& state)
{
    using namespace control;
    std::error_code partialError;
    const bool hasPartial = state.kind == "recording" && state.path.empty() &&
        std::filesystem::is_regular_file(state.directory / L"capture.partial.mp4", partialError);
    const auto value = Object({{"schemaVersion", Number(1)}, {"source", String(state.source)},
        {"kind", String(state.kind)}, {"status", String(state.status)}, {"error", String(state.error)},
        {"monitorId", String(winrt::to_string(winrt::hstring{state.monitor.id}))},
        {"monitorBounds", Object({{"left", Number(state.monitor.bounds.left)}, {"top", Number(state.monitor.bounds.top)},
            {"right", Number(state.monitor.bounds.right)}, {"bottom", Number(state.monitor.bounds.bottom)}})},
        {"width", Number(state.width)}, {"height", Number(state.height)}, {"fps", Number(state.fps)},
        {"requestedDurationMs", Number(state.durationMs)}, {"frames", Number(static_cast<double>(state.frames))},
        {"capturedFrames", Number(static_cast<double>(state.capturedFrames))},
        {"duplicatedFrames", Number(static_cast<double>(state.duplicatedFrames))},
        {"discardedAcquiredFrames", Number(static_cast<double>(state.discardedFrames))},
        {"includeSystemCursor", Boolean(state.systemCursor)}, {"startedUnixMs", Number(static_cast<double>(state.startedUnixMs))},
        {"captureStartedUnixMs", Number(static_cast<double>(state.captureStartedUnixMs))},
        {"finishedUnixMs", Number(static_cast<double>(state.finishedUnixMs))},
        {"elapsed100ns", String(std::to_string(state.elapsed100ns))},
        {"encodedDuration100ns", String(std::to_string(state.encodedDuration100ns))},
        {"firstFrameTime100ns", String(std::to_string(state.firstFrameTime100ns))},
        {"lastFrameTime100ns", String(std::to_string(state.lastFrameTime100ns))},
        {"frameMap", String(state.frameMapPath.empty() ? "" : "frames.csv")},
        {"partialFile", String(hasPartial ? "capture.partial.mp4" : "")},
        {"file", String(state.path.empty() ? "" : winrt::to_string(winrt::hstring{state.path.filename().wstring()}))},
        {"processId", Number(GetCurrentProcessId())}});
    const auto path = state.directory / L"capture.json";
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || !(stream << Serialize(value) << '\n') || !stream.flush()) throw std::runtime_error("capture_manifest_failed");
    state.manifestPath = path;
}
void RecordFrames(State& state, const std::atomic<bool>& cancelled, const std::function<void()>& publish)
{
    const auto expected = RecordingFrameCount(state.durationMs, state.fps);
    const int monitorWidth = state.monitor.bounds.right - state.monitor.bounds.left;
    const int monitorHeight = state.monitor.bounds.bottom - state.monitor.bounds.top;
    if (monitorWidth < 2 || monitorHeight < 2 || monitorWidth > 3840 || monitorHeight > 2160 || (monitorWidth & 1) || (monitorHeight & 1))
        throw std::runtime_error("unsupported_recording_geometry");
    const auto partial = state.directory / L"capture.partial.mp4";
    // Initializing the encoder can take much longer than one video frame.
    // Do not start WGC until that work finishes: a static source may produce
    // only its initial image, which must not age out while the encoder starts.
    video_writer::Mp4 writer(partial, static_cast<unsigned>(monitorWidth), static_cast<unsigned>(monitorHeight), state.fps);
    const auto encoderReadyTime = QpcTime();
    if (cancelled.load()) throw std::runtime_error("capture_cancelled");
    Reader reader(state.monitor, state.systemCursor);
    Pixels held;
    try { held = reader.First(cancelled, encoderReadyTime); }
    catch (...)
    {
        // A timeout/cancellation can occur after stale pool frames were
        // acquired and rejected. Preserve that loss even with no video frames.
        state.capturedFrames = state.discardedFrames = reader.CapturedFrames();
        throw;
    }
    state.width = held.width; state.height = held.height;
    state.capturedFrames = state.discardedFrames = reader.CapturedFrames();
    if (state.width < 2 || state.height < 2 || state.width > 3840 || state.height > 2160 || (state.width & 1) || (state.height & 1))
        throw std::runtime_error("unsupported_recording_geometry");
    state.firstFrameTime100ns = state.lastFrameTime100ns = held.time100ns;
    const auto origin = held.time100ns;
    const auto firstAge = QpcTime() - origin;
    if (firstAge < 0 || firstAge > 50000000LL) throw std::runtime_error("capture_clock_mismatch");
    state.captureStartedUnixMs = UnixMs() - firstAge / 10000;
    state.frameMapPath = state.directory / L"frames.csv";
    std::ofstream mapping(state.frameMapPath, std::ios::binary | std::ios::trunc);
    if (!mapping || !(mapping << "frame,outputTime100ns,sourceSequence,sourceTime100ns,sourceRelative100ns,writeStartedElapsed100ns,repeated\n"))
        throw std::runtime_error("capture_frame_map_failed");
    std::optional<Pixels> pending;
    std::uint64_t lastUsed = 0, usedSources = 0;
    auto lastPublished = QpcTime(), lastValidated = lastPublished;
    const auto duration = static_cast<std::int64_t>(state.durationMs) * 10000;
    while (state.frames < expected || QpcTime() - origin < duration)
    {
        if (cancelled.load()) throw std::runtime_error("capture_cancelled");
        auto now = QpcTime(); state.elapsed100ns = now - origin;
        if (now - lastValidated >= 2000000)
        { reader.Validate(); lastValidated = now; }
        if (state.elapsed100ns > duration + 50000000LL) throw std::runtime_error("capture_encoder_too_slow");
        Pixels fresh;
        if (reader.Latest(fresh))
        {
            if (fresh.time100ns < state.lastFrameTime100ns) throw std::runtime_error("capture_nonmonotonic_time");
            state.lastFrameTime100ns = fresh.time100ns;
            pending = std::move(fresh);
        }
        state.capturedFrames = reader.CapturedFrames();
        const auto target = video_writer::FrameTime(state.frames, state.fps);
        if (state.frames < expected && state.elapsed100ns >= target)
        {
            if (state.elapsed100ns - target > 5000000LL)
                throw std::runtime_error("capture_encoder_too_slow");
            // Never back-date a newly captured image into an earlier output
            // slot. If encoding/readback missed a deadline, repeat the last
            // eligible image and make that loss visible in the frame map.
            if (pending && pending->time100ns - origin <= target)
            { held = std::move(*pending); pending.reset(); }
            const bool repeated = held.sequence == lastUsed;
            writer.Write(held.data);
            if (!(mapping << state.frames << ',' << target << ',' << held.sequence << ',' << held.time100ns << ','
                << held.time100ns - origin << ',' << state.elapsed100ns << ',' << (repeated ? 1 : 0) << '\n'))
                throw std::runtime_error("capture_frame_map_failed");
            if (repeated) ++state.duplicatedFrames;
            else { ++usedSources; lastUsed = held.sequence; }
            state.frames = writer.FrameCount();
            state.frameTime100ns = held.time100ns;
            state.encodedDuration100ns = video_writer::FrameTime(state.frames, state.fps);
        }
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        state.discardedFrames = state.capturedFrames - usedSources;
        if (now - lastPublished >= 1000000)
        { publish(); lastPublished = now; }
    }
    reader.Validate();
    if (cancelled.load()) throw std::runtime_error("capture_cancelled");
    if (!mapping.flush()) throw std::runtime_error("capture_frame_map_failed");
    writer.Finish();
    if (cancelled.load()) throw std::runtime_error("capture_cancelled");
    const auto output = state.directory / L"capture.mp4";
    std::error_code error;
    std::filesystem::rename(partial, output, error);
    if (error) throw std::runtime_error("capture_video_finalize_failed");
    state.path = output;
    state.elapsed100ns = QpcTime() - origin;
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
    std::mutex startMutex;
    State state;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    bool Start(std::wstring monitorId, std::filesystem::path root, bool systemCursor,
        unsigned durationMs, unsigned fps, std::string& error);
};
std::uint64_t RecordingFrameCount(unsigned durationMs, unsigned fps)
{
    if (durationMs < 100 || durationMs > 120000 || fps < 1 || fps > 60)
        throw std::invalid_argument("invalid_recording_options");
    return (static_cast<std::uint64_t>(durationMs) * fps + 999) / 1000;
}
Service::Service() : impl_(std::make_unique<Impl>()) {}
Service::~Service() { Stop(); }
State Service::Current() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
bool Service::Screenshot(std::wstring monitorId, std::filesystem::path root, bool systemCursor, std::string& error)
{ return impl_->Start(std::move(monitorId), std::move(root), systemCursor, 0, 0, error); }
bool Service::Record(std::wstring monitorId, std::filesystem::path root, bool systemCursor,
    unsigned durationMs, unsigned fps, std::string& error)
{
    try { (void)RecordingFrameCount(durationMs, fps); }
    catch (const std::invalid_argument&) { error = "invalid_recording_options"; return false; }
    return impl_->Start(std::move(monitorId), std::move(root), systemCursor, durationMs, fps, error);
}
bool Service::Impl::Start(std::wstring monitorId, std::filesystem::path root, bool systemCursor,
    unsigned durationMs, unsigned fps, std::string& error)
{
    std::lock_guard startLock(startMutex);
    error.clear();
    { std::lock_guard lock(mutex); if (state.status == "running") { error = "capture_busy"; return false; } }
    if (root.empty() || !root.is_absolute()) { error = "invalid_capture_root"; return false; }
    Monitor selected;
    for (const auto& monitor : Monitors())
        if ((monitorId == L"primary" && monitor.primary) || !_wcsicmp(monitor.id.c_str(), monitorId.c_str()))
        { selected = monitor; break; }
    if (selected.id.empty()) { error = "monitor_not_found"; return false; }
    if (durationMs && (selected.bounds.right - selected.bounds.left > 3840 || selected.bounds.bottom - selected.bounds.top > 2160 ||
        ((selected.bounds.right - selected.bounds.left) & 1) || ((selected.bounds.bottom - selected.bounds.top) & 1)))
    { error = "unsupported_recording_geometry"; return false; }
    GUID guid{}; wchar_t identifier[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, identifier, 40)) { error = "capture_id_failed"; return false; }
    const std::wstring wideId(identifier + 1, 36);
    const auto id = winrt::to_string(winrt::hstring{wideId});
    if (worker.joinable()) worker.join();
    State task; task.id = id; task.status = "running"; task.monitor = selected;
    task.systemCursor = systemCursor; task.startedUnixMs = UnixMs();
    task.durationMs = durationMs; task.fps = fps;
    task.kind = durationMs ? "recording" : "screenshot";
    cancelled.store(false);
    { std::lock_guard lock(mutex); state = task; }
    try
    {
        worker = std::thread([this, root = std::move(root), task = std::move(task)]() mutable {
            const auto publish = [&] { std::lock_guard lock(mutex); state = task; };
            try
            {
                Apartment apartment;
                std::error_code ec;
                std::filesystem::create_directories(root, ec);
                const auto directory = root / task.id;
                if (ec || !std::filesystem::create_directory(directory, ec) || ec)
                    throw std::runtime_error("capture_directory_failed");
                task.directory = directory;
                if (task.kind == "recording") RecordFrames(task, cancelled, publish);
                else
                {
                    Reader reader(task.monitor, task.systemCursor);
                    const auto pixels = reader.First(cancelled);
                    if (cancelled.load()) throw std::runtime_error("capture_cancelled");
                    std::string pngError;
                    const auto output = directory / L"capture.png";
                    if (!preview_png::Save(output, pixels.width, pixels.height, pixels.data, pngError))
                        throw std::runtime_error("capture_png_failed: " + pngError);
                    task.width = pixels.width; task.height = pixels.height; task.path = output;
                    task.frameTime100ns = task.firstFrameTime100ns = task.lastFrameTime100ns = pixels.time100ns;
                    task.frames = 1; task.capturedFrames = reader.CapturedFrames();
                }
                task.status = "completed";
            }
            catch (const winrt::hresult_error& failure) { task.status = "failed"; task.error = winrt::to_string(failure.message()); }
            catch (const std::exception& failure) { task.status = "failed"; task.error = failure.what(); }
            catch (...) { task.status = "failed"; task.error = "capture_failure"; }
            if (cancelled.load()) { task.status = "cancelled"; task.error = "capture_cancelled"; }
            task.finishedUnixMs = UnixMs();
            if (!task.directory.empty())
                try { WriteManifest(task); }
                catch (...) { if (task.status == "completed") { task.status = "failed"; task.error = "capture_manifest_failed"; } }
            publish();
        });
    }
    catch (...)
    {
        std::lock_guard lock(mutex);
        state.status = "failed"; state.error = "capture_worker_failed"; state.finishedUnixMs = UnixMs();
        error = state.error; return false;
    }
    return true;
}
void Service::Cancel() { impl_->cancelled.store(true); }
void Service::Stop() noexcept
{
    std::lock_guard startLock(impl_->startMutex);
    impl_->cancelled.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
}
}
