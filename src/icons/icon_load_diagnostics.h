#pragma once

#include "diagnostics/diagnostic_log.h"

#include <windows.h>
#include <cwchar>
#include <string>
#include <string_view>

namespace snowdesktop::icon_diagnostics
{
struct ShellIconLoadTiming
{
    unsigned phase = 1;
    ULONGLONG queueMs = 0, metadataMs = 0, pidlMs = 0, bitmapMs = 0, totalMs = 0;
    bool hasBitmap = false;
    ULONGLONG traceId = 0;
    std::wstring_view path;
};

// Queueing is expected during a batch. A successful slow load is optional
// detail; an unsuccessful slow load retains its warning even when disabled.
inline void LogShellIconLoadTiming(const ShellIconLoadTiming& timing)
{
    if (timing.totalMs < 250) return;
    const auto level = timing.hasBitmap ? DiagnosticLogLevel::Info : DiagnosticLogLevel::Warning;
    if (!ShouldWriteDiagnosticLog(DiagnosticLogModule::Icons, level)) return;
    wchar_t fields[512]{};
    swprintf_s(fields,
        L"Shell icon slow: phase=%u queueMs=%llu metadataMs=%llu "
        L"pidlMs=%llu bitmapMs=%llu shortcutMs=0 totalMs=%llu bitmap=%d trace=%llu path=",
        timing.phase, timing.queueMs, timing.metadataMs, timing.pidlMs, timing.bitmapMs,
        timing.totalMs, timing.hasBitmap ? 1 : 0, timing.traceId);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Icons,
        (std::wstring(fields) + std::wstring(timing.path)).c_str(), level);
}
} // namespace snowdesktop::icon_diagnostics
