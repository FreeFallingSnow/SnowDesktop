#include "export_service.h"
#include "artifact_io.h"
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace snowdesktop::demo
{
namespace
{
std::uint64_t UnixMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::wstring Quote(std::wstring_view value)
{
    std::wstring result = L"\""; std::size_t slashes = 0;
    for (const auto c : value)
    {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; result += c;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}
struct ExportService::Impl
{
    mutable std::mutex mutex;
    ExportState state;
    std::atomic<bool> cancelled{false};
    std::thread worker;
};
ExportService::ExportService() : impl_(std::make_unique<Impl>()) {}
ExportService::~ExportService() { Stop(); }
ExportState ExportService::Current() const { std::lock_guard lock(impl_->mutex); return impl_->state; }
bool ExportService::Start(const Scene& scene, const std::filesystem::path& executable, const std::filesystem::path& root, std::string& error)
{
    error.clear();
    if (Current().status == "running") { error = "demo_busy"; return false; }
    if (!executable.is_absolute() || !std::filesystem::is_regular_file(executable) || !root.is_absolute())
    { error = "demo_invalid_host_or_root"; return false; }
    GUID guid{}; wchar_t buffer[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, buffer, 40)) { error = "demo_id_failed"; return false; }
    if (impl_->worker.joinable()) impl_->worker.join();
    ExportState state; state.id = Utf8(std::wstring_view(buffer + 1, 36)); state.status = "running";
    state.directory = root / state.id; state.frames = scene.FrameCount(); state.startedUnixMs = UnixMs();
    impl_->cancelled.store(false);
    { std::lock_guard lock(impl_->mutex); impl_->state = state; }
    try
    {
        impl_->worker = std::thread([this, state, scene, executable, root]() mutable {
            try
            {
                std::filesystem::create_directories(root);
                if (!std::filesystem::create_directory(state.directory)) throw std::runtime_error("demo_output_already_exists");
                WriteJson(state.directory / L"scene.json", scene.source); state.sourceExeSha256 = Sha256(executable);
                const auto resultFile = state.directory / L"host-result.json";
                std::wstring command = Quote(executable.wstring());
                const std::wstring arguments[]{L"--native-component-preview", L"demo-scene", state.directory.wstring(), L"96",
                    Wide(scene.locale), Wide(scene.appearance), L"", std::to_wstring(scene.width), std::to_wstring(scene.height), L"0", L"0", L"0", resultFile.wstring()};
                for (const auto& argument : arguments) command += L" " + Quote(argument);
                Handle job; job.value = CreateJobObjectW(nullptr, nullptr);
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                    throw std::runtime_error("demo_job_unavailable");
                STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
                PROCESS_INFORMATION process{};
                if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                    nullptr, executable.parent_path().c_str(), &startup, &process)) throw std::runtime_error("demo_worker_start_failed");
                Handle child; child.value = process.hProcess; Handle thread; thread.value = process.hThread;
                if (!AssignProcessToJobObject(job.value, child.value))
                { TerminateProcess(child.value, 1); WaitForSingleObject(child.value, 5000); throw std::runtime_error("demo_worker_ownership_failed"); }
                if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) throw std::runtime_error("demo_worker_resume_failed");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
                for (;;)
                {
                    const auto wait = WaitForSingleObject(child.value, 100);
                    if (wait == WAIT_OBJECT_0) break;
                    if (wait != WAIT_TIMEOUT) throw std::runtime_error("demo_worker_wait_failed");
                    if (impl_->cancelled.load() || std::chrono::steady_clock::now() >= deadline)
                    {
                        TerminateJobObject(job.value, 1); WaitForSingleObject(child.value, 5000);
                        throw std::runtime_error(impl_->cancelled.load() ? "demo_cancelled" : "demo_render_timeout");
                    }
                    try
                    {
                        const auto progress = ReadJson(state.directory / L"progress.json", 4096);
                        state.completedFrames = static_cast<unsigned>(control::Integer(progress, "completedFrames", 0, state.frames));
                    }
                    catch (...) {} // A progress file is optional until the first frame.
                    std::lock_guard lock(impl_->mutex); impl_->state = state;
                }
                DWORD exit = 1; if (!GetExitCodeProcess(child.value, &exit)) throw std::runtime_error("demo_worker_exit_unavailable");
                if (exit)
                {
                    std::string message = "demo_render_failed";
                    try { message += ": " + control::Text(ReadJson(resultFile), "error"); } catch (...) {}
                    throw std::runtime_error(message);
                }
                state.manifest = ReadJson(state.directory / L"scene-result.json");
                if (control::Text(state.manifest, "status") != "completed" ||
                    control::Text(state.manifest, "sourceExeSha256") != state.sourceExeSha256 ||
                    control::Integer(state.manifest, "frames", 1, 1800) != static_cast<int>(state.frames))
                    throw std::runtime_error("demo_result_identity_mismatch");
                for (const auto* file : {L"poster.png", L"video.mp4", L"scene.json"})
                    if (!std::filesystem::is_regular_file(state.directory / file)) throw std::runtime_error("demo_output_missing");
                state.completedFrames = state.frames; state.status = "completed";
            }
            catch (const std::exception& failure) { state.status = "failed"; state.error = failure.what(); }
            catch (...) { state.status = "failed"; state.error = "demo_export_failed"; }
            if (impl_->cancelled.load()) { state.status = "cancelled"; state.error = "demo_cancelled"; }
            state.finishedUnixMs = UnixMs(); std::lock_guard lock(impl_->mutex); impl_->state = std::move(state);
        });
    }
    catch (...) { std::lock_guard lock(impl_->mutex); impl_->state.status = "failed"; impl_->state.error = "demo_worker_thread_failed"; error = impl_->state.error; return false; }
    return true;
}
void ExportService::Cancel() noexcept { impl_->cancelled.store(true); }
void ExportService::Stop() noexcept
{
    Cancel(); if (impl_->worker.joinable()) impl_->worker.join();
}
}
