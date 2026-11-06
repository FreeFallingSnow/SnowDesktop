#include "steam_widget_multiplayer.h"

#include "steam_child_environment.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace snowdesktop::steam_widget_multiplayer
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { Reset(); }
    void Reset() noexcept
    {
        if (value) CloseHandle(std::exchange(value, nullptr));
    }
};

bool StartChild(const std::filesystem::path& path, Handle& process,
    Handle& write, Handle& read)
{
    Handle childRead, childWrite;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    if (!CreatePipe(&childRead.value, &write.value, &security, 64 * 1024) ||
        !CreatePipe(&read.value, &childWrite.value, &security, 64 * 1024) ||
        !SetHandleInformation(write.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0)) return false;

    auto environment = BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty()) return false;
    std::wstring command = L"\"" + path.wstring() + L"\" widget-multiplayer " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(childRead.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(childWrite.value));
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return false;
    HANDLE inherited[]{childRead.value, childWrite.value};
    PROCESS_INFORMATION started{};
    const bool ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) &&
        CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
            environment.data(), path.parent_path().c_str(), &startup.StartupInfo, &started);
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (ok)
    {
        process.value = started.hProcess;
        CloseHandle(started.hThread);
    }
    return ok;
}
}

struct Service::Impl
{
    Impl(std::filesystem::path executable, std::function<void()> callback)
        : wake(std::move(callback)), worker([this, executable = std::move(executable)](std::stop_token stop) {
            Run(executable, stop);
        })
    {}

    bool Emit(const Frame& event)
    {
        bool shouldWake = false;
        {
            const std::lock_guard lock(mutex);
            if (events.size() >= 64) return false;
            shouldWake = events.empty();
            events.push_back(event);
        }
        if (shouldWake && wake) wake();
        return true;
    }

    void Run(const std::filesystem::path& executable, std::stop_token stop)
    {
        Handle process, write, read;
        if (StartChild(executable, process, write, read))
        {
            Pump(process.value, write.value, read.value, stop);
            write.Reset();
            read.Reset();
            if (WaitForSingleObject(process.value, 250) == WAIT_TIMEOUT)
                (void)TerminateProcess(process.value, ERROR_CANCELLED);
        }
        {
            const std::lock_guard lock(mutex);
            closed = true;
            commands.clear();
            Frame terminal;
            terminal.kind = Kind::Disconnected;
            events.push_back(terminal);
        }
        if (!stop.stop_requested() && wake) wake();
    }

    void Pump(HANDLE process, HANDLE write, HANDLE read, std::stop_token stop)
    {
        bool ready = false;
        std::optional<std::uint64_t> awaiting;
        auto deadline = Clock::now() + 5s;
        while (!stop.stop_requested())
        {
            Frame frame;
            const auto result = ReadFrame(read, frame);
            if (result == ReadResult::Closed) break;
            if (result == ReadResult::Received)
            {
                if (!ready)
                {
                    if (frame.kind != Kind::Ready) break;
                    ready = true;
                }
                if (frame.kind == Kind::Result)
                {
                    if (!awaiting || *awaiting != frame.request) break;
                    awaiting.reset();
                }
                if (!Emit(frame)) break;
                continue;
            }
            if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT ||
                ((!ready || awaiting) && Clock::now() >= deadline)) break;
            std::unique_lock lock(mutex);
            if (ready && !awaiting && !commands.empty())
            {
                frame = commands.front();
                commands.pop_front();
                lock.unlock();
                // At most one frame is in flight. It fits in the empty pipe
                // even if Steam has stalled while processing callbacks.
                if (!WriteFrame(write, frame)) break;
                awaiting = frame.request;
                deadline = Clock::now() + 22s;
                continue;
            }
            signal.wait_for(lock, stop, 25ms,
                [&] { return ready && !awaiting && !commands.empty(); });
        }
    }

    std::mutex mutex;
    std::condition_variable_any signal;
    std::deque<Frame> commands;
    std::vector<Frame> events;
    bool closed = false;
    std::function<void()> wake;
    std::jthread worker;
};

Service::Service(std::filesystem::path executable, std::function<void()> wake)
    : impl_(std::make_unique<Impl>(std::move(executable), std::move(wake)))
{}

Service::~Service() = default;

bool Service::Submit(Frame frame)
{
    const std::lock_guard lock(impl_->mutex);
    if (impl_->closed || impl_->commands.size() >= 8) return false;
    impl_->commands.push_back(frame);
    impl_->signal.notify_one();
    return true;
}

std::vector<Frame> Service::Drain()
{
    const std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->events, {});
}
}
