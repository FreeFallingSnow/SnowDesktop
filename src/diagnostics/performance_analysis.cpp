#include "performance_analysis.h"
#include "common/json_value.h"
#include <windows.h>
#include <array>
#include <cwchar>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace snowdesktop::performance
{
namespace
{
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::vector<wchar_t> ShellEnvironment()
{
    // Native launch does not get PowerShell 7's module-path compatibility filter.
    // Let system Windows PowerShell construct its own module search path.
    auto* inherited = GetEnvironmentStringsW();
    if (!inherited) throw std::runtime_error("analysisEnvironmentUnavailable");
    struct EnvironmentStrings { wchar_t* value; ~EnvironmentStrings() { FreeEnvironmentStringsW(value); } } cleanup{inherited};
    std::vector<wchar_t> environment;
    for (const auto* entry = inherited; *entry; entry += std::wcslen(entry) + 1)
        if (_wcsnicmp(entry, L"PSModulePath=", 13) != 0)
            environment.insert(environment.end(), entry, entry + std::wcslen(entry) + 1);
    if (environment.empty()) environment.push_back(0);
    environment.push_back(0);
    return environment;
}
std::wstring Argument(std::wstring_view value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto ch : value)
    {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(slashes * (ch == L'"' ? 2 : 1), L'\\'); slashes = 0;
        if (ch == L'"') result.push_back(L'\\');
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\'); result.push_back(L'"'); return result;
}
}
AnalysisResult AnalyzeCapture(const std::filesystem::path& manifest, const std::atomic<bool>* cancel)
{
    AnalysisResult result;
    try
    {
        if (cancel && cancel->load()) { result.error = "analysisCanceled"; return result; }
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) { result.error = "analysisExecutablePathUnavailable"; return result; }
        auto script = std::filesystem::path(path.data()).parent_path() / L"performance/scripts/profile_analyze.ps1";
#ifdef SNOWDESKTOP_PERFORMANCE_SOURCE_ROOT
        if (!std::filesystem::exists(script)) script = std::filesystem::path(SNOWDESKTOP_PERFORMANCE_SOURCE_ROOT) / L"scripts/profile_analyze.ps1";
#endif
        if (!std::filesystem::is_regular_file(script)) { result.error = "analysisToolsMissing"; return result; }
        const auto systemLength = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
        if (!systemLength || systemLength >= path.size()) { result.error = "analysisShellUnavailable"; return result; }
        const auto shell = std::filesystem::path(path.data()) / L"WindowsPowerShell/v1.0/powershell.exe";
        const auto log = manifest.parent_path() / (L"analysis-command-" + std::to_wstring(GetTickCount64()) + L".log");
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) { result.error = "analysisLogUnavailable"; return result; }
        auto environment = ShellEnvironment();
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.value;
        startup.StartupInfo.hStdInput = input.value;
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> attributes(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) { result.error = "analysisLaunchFailed"; return result; }
        const auto attributeCleanup = [&] { DeleteProcThreadAttributeList(startup.lpAttributeList); };
        HANDLE handles[]{output.value, input.value};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr))
        { attributeCleanup(); result.error = "analysisLaunchFailed"; return result; }
        Handle job{CreateJobObjectW(nullptr, nullptr)};
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        { attributeCleanup(); result.error = "analysisLaunchFailed"; return result; }
        auto command = Argument(shell.wstring()) + L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
            Argument(script.wstring()) + L" -ManifestPath " + Argument(manifest.wstring());
        PROCESS_INFORMATION process{};
        const auto launched = CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), nullptr, &startup.StartupInfo, &process);
        attributeCleanup();
        if (!launched) { result.error = "analysisLaunchFailed"; return result; }
        Handle child{process.hProcess}, thread{process.hThread};
        if (!AssignProcessToJobObject(job.value, child.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1))
        { TerminateProcess(child.value, 1); WaitForSingleObject(child.value, 5000); result.error = "analysisLaunchFailed"; return result; }
        const auto deadline = GetTickCount64() + 180000;
        while (WaitForSingleObject(child.value, 100) != WAIT_OBJECT_0)
        {
            if ((cancel && cancel->load()) || GetTickCount64() >= deadline)
            {
                TerminateJobObject(job.value, 124); WaitForSingleObject(child.value, 5000);
                result.error = cancel && cancel->load() ? "analysisCanceled" : "analysisTimeout"; return result;
            }
        }
        DWORD exit = 1; GetExitCodeProcess(child.value, &exit);
        CloseHandle(output.value); output.value = nullptr;
        if (exit || std::filesystem::file_size(log) > 65536) { result.error = "analysisFailed"; return result; }
        std::ifstream stream(log, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), {});
        JsonValue value; std::string error;
        if (!ParseJson(text, value, &error)) { result.error = "analysisResultInvalid"; return result; }
        const auto* report = value.Find("report");
        if (!report || !report->IsString()) { result.error = "analysisResultInvalid"; return result; }
        const std::u8string utf8Path(report->string.begin(), report->string.end());
        result.report = std::filesystem::path(utf8Path).lexically_normal();
        if (const auto* quality = value.Find("quality"); quality && quality->IsObject())
        {
            const auto* valid = quality->Find("valid"); const auto* coverage = quality->Find("scopeCoverage");
            if (valid && valid->IsBoolean()) result.quality = valid->boolean ?
                (coverage && coverage->IsString() && coverage->string == "partial" ? "partial" : "valid") : "invalid";
        }
        if (result.report.filename() != L"perfetto_analysis_report.md" ||
            result.report.parent_path().parent_path() != manifest.parent_path() || !std::filesystem::is_regular_file(result.report))
        { result.report.clear(); result.error = "analysisResultInvalid"; }
    }
    catch (...) { result.error = "analysisFailed"; }
    return result;
}
}
