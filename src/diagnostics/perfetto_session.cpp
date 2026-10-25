#include "perfetto_session.h"
#include "perfetto_categories.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string_view>

namespace snowdesktop::performance
{
namespace
{
std::once_flag initializeOnce;
std::atomic<bool> initialized{ false };
std::atomic<bool> sessionOwned{ false };
constexpr auto acknowledgementTimeout = std::chrono::seconds(15);
constexpr std::array<std::string_view, 14> categoryNames{
    "snow.resource", "snow.lua.callback", "snow.lua.detail", "snow.task",
    "snow.task.detail", "snow.frame", "snow.input", "snow.dock", "snow.popup",
    "snow.layout", "snow.render", "snow.icon", "snow.cache.detail", "snow.profiler" };

bool SafeOutputDirectory(const std::filesystem::path& output)
{
    if (!output.is_absolute() || output != output.lexically_normal() ||
        output.extension() != L".pftrace") return false;
    auto directory = output.parent_path();
    while (!directory.empty())
    {
        const auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return true;
}

struct Signals
{
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false, stopped = false, readComplete = false;
    bool writeFailed = false;
    std::string sdkError;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::uint64_t bytes = 0, maximumBytes = 0;

    ~Signals() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};
}

struct PerfettoSession::Impl
{
    std::unique_ptr<perfetto::TracingSession> session;
    std::shared_ptr<Signals> signals;
    std::filesystem::path output, partial;
    bool ownsSession = false;

    void Release()
    {
        session.reset();
        signals.reset();
        if (ownsSession)
        {
            ownsSession = false;
            sessionOwned.store(false, std::memory_order_release);
        }
    }
};

PerfettoSession::PerfettoSession() : impl_(std::make_unique<Impl>()) {}
PerfettoSession::~PerfettoSession()
{
    if (Active()) { std::string ignored; Stop(ignored); }
}
bool PerfettoSession::Active() const noexcept { return impl_->ownsSession; }
bool PerfettoSession::Initialized() noexcept
{ return initialized.load(std::memory_order_acquire); }

bool PerfettoSession::Start(const PerfettoSessionOptions& options, std::string& error)
{
    error.clear();
    if (Active()) { error = "captureBusy"; return false; }
    if (options.bufferKiB < 64 || options.bufferKiB > 65536 ||
        options.maximumFileBytes < 1024 || options.maximumFileBytes > 256ULL * 1024 * 1024 ||
        options.categories.empty() || options.categories.size() > categoryNames.size() ||
        !SafeOutputDirectory(options.output) ||
        std::any_of(options.categories.begin(), options.categories.end(), [](const auto& category) {
            return std::find(categoryNames.begin(), categoryNames.end(), category) == categoryNames.end();
        }))
    { error = "invalidCaptureOptions"; return false; }
    bool expected = false;
    if (!sessionOwned.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    { error = "captureBusy"; return false; }
    impl_->ownsSession = true;
    try
    {
        impl_->output = options.output;
        impl_->partial = std::filesystem::path(options.output.wstring() + L".partial");
        if (GetFileAttributesW(impl_->output.c_str()) != INVALID_FILE_ATTRIBUTES)
        { error = "outputAlreadyExists"; impl_->Release(); return false; }
        auto signals = std::make_shared<Signals>();
        impl_->signals = signals;
        signals->maximumBytes = options.maximumFileBytes;
        signals->file = CreateFileW(impl_->partial.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (signals->file == INVALID_HANDLE_VALUE)
        { error = "cannotReserveOutput"; impl_->Release(); return false; }

        std::call_once(initializeOnce, [] {
            perfetto::TracingInitArgs args;
            args.backends = perfetto::kInProcessBackend;
            perfetto::Tracing::Initialize(args);
            native_trace::TrackEvent::Register();
            initialized.store(true, std::memory_order_release);
        });

        perfetto::TraceConfig config;
        auto* buffer = config.add_buffers();
        buffer->set_size_kb(options.bufferKiB);
        buffer->set_fill_policy(perfetto::TraceConfig::BufferConfig::DISCARD);
        config.set_data_source_stop_timeout_ms(5000);
        perfetto::protos::gen::TrackEventConfig events;
        // C++ categories default to enabled. Always disable the wildcard first.
        events.add_disabled_categories("*");
        for (const auto& category : options.categories) events.add_enabled_categories(category);
        events.set_enable_thread_time_sampling(options.threadCpu);
        auto* source = config.add_data_sources()->mutable_config();
        source->set_name("track_event");
        source->set_track_event_config_raw(events.SerializeAsString());

        impl_->session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
        impl_->session->SetOnStartCallback([signals] {
            std::lock_guard lock(signals->mutex);
            signals->started = true;
            signals->changed.notify_all();
        });
        impl_->session->SetOnStopCallback([signals] {
            std::lock_guard lock(signals->mutex);
            signals->stopped = true;
            signals->changed.notify_all();
        });
        impl_->session->SetOnErrorCallback([signals](perfetto::TracingError failure) {
            std::lock_guard lock(signals->mutex);
            signals->sdkError = failure.message;
            if (signals->sdkError.empty()) signals->sdkError = "sdkSessionError";
            signals->changed.notify_all();
        });
        impl_->session->Setup(config);
        impl_->session->Start();
        std::unique_lock lock(signals->mutex);
        if (!signals->changed.wait_for(lock, acknowledgementTimeout,
                [&] { return signals->started || !signals->sdkError.empty(); }))
            error = "sdkStartTimeout";
        else if (!signals->sdkError.empty()) error = signals->sdkError;
        lock.unlock();
        if (!error.empty()) { impl_->Release(); return false; }
        return true;
    }
    catch (...)
    { error = "captureInitializationFailed"; impl_->Release(); return false; }
}

bool PerfettoSession::Stop(std::string& error)
{
    error.clear();
    if (!Active()) { error = "noActiveCapture"; return false; }
    try
    {
        const auto signals = impl_->signals;
        impl_->session->Stop();
        std::unique_lock lock(signals->mutex);
        if (!signals->changed.wait_for(lock, acknowledgementTimeout,
                [&] { return signals->stopped || !signals->sdkError.empty(); }))
            error = "sdkStopTimeout";
        else if (!signals->sdkError.empty()) error = signals->sdkError;
        lock.unlock();
        if (error.empty())
        {
            impl_->session->ReadTrace([signals](perfetto::TracingSession::ReadTraceCallbackArgs chunk) {
                std::lock_guard writeLock(signals->mutex);
                if (chunk.size > signals->maximumBytes - signals->bytes)
                    signals->writeFailed = true;
                if (!signals->writeFailed && chunk.size)
                {
                    DWORD written = 0;
                    if (chunk.size > MAXDWORD || !WriteFile(signals->file, chunk.data,
                            static_cast<DWORD>(chunk.size), &written, nullptr) || written != chunk.size)
                        signals->writeFailed = true;
                    else signals->bytes += written;
                }
                if (!chunk.has_more)
                {
                    signals->readComplete = true;
                    signals->changed.notify_all();
                }
            });
            lock.lock();
            if (!signals->changed.wait_for(lock, acknowledgementTimeout,
                    [&] { return signals->readComplete || !signals->sdkError.empty(); }))
            {
                signals->writeFailed = true;
                error = "traceReadTimeout";
            }
            else if (!signals->sdkError.empty()) error = signals->sdkError;
            else if (signals->writeFailed || !signals->bytes) error = "traceWriteFailedOrLimitExceeded";
            else if (!FlushFileBuffers(signals->file)) error = "traceFlushFailed";
            if (signals->readComplete)
            {
                CloseHandle(signals->file);
                signals->file = INVALID_HANDLE_VALUE;
            }
            lock.unlock();
        }
        if (error.empty() && !MoveFileExW(impl_->partial.c_str(), impl_->output.c_str(), MOVEFILE_WRITE_THROUGH))
            error = "tracePublishFailed";
        impl_->Release();
        return error.empty();
    }
    catch (...)
    { error = "captureFinalizationFailed"; impl_->Release(); return false; }
}
}
