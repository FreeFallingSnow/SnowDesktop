#pragma once
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>
#include <string_view>

namespace snowdesktop::shell_extensions
{
// Opt-in diagnostics contain no paths, selection names or command arguments.
// Each process has its own file, so helper logging cannot block the UI writer.
inline void MenuTrace(std::string_view phase, std::string_view reason, double milliseconds = 0,
                      unsigned count = 0)
{
    wchar_t base[32768]{};
    const auto length = GetEnvironmentVariableW(L"SNOWDESKTOP_MENU_TRACE", base, 32768);
    if (!length || length >= 32768)
        return;
    const auto file = std::wstring(base) + L"." + std::to_wstring(GetCurrentProcessId()) + L".log";
    // Readers may inspect a live trace. Serialize this process's appenders,
    // while allowing those readers to coexist with a complete record write.
    static std::mutex appendMutex;
    const std::lock_guard lock(appendMutex);
    FILE *output = _wfsopen(file.c_str(), L"ab", _SH_DENYNO);
    if (!output)
        return;
    fprintf(output, "tick=%llu phase=%.*s reason=%.*s ms=%.3f count=%u\n", GetTickCount64(),
            static_cast<int>(phase.size()), phase.data(), static_cast<int>(reason.size()), reason.data(),
            milliseconds, count);
    fclose(output);
}
class MenuTiming
{
    std::string phase_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  public:
    explicit MenuTiming(std::string phase) : phase_(std::move(phase)) {}
    double Elapsed() const
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
    }
    void Record(std::string_view reason, unsigned count = 0) const { MenuTrace(phase_, reason, Elapsed(), count); }
};
}
