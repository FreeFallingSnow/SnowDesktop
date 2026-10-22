#pragma once

#include "diagnostic_log_modules.h"

enum class DiagnosticLogLevel
{
    Debug,
    Info,
    Warning,
    Error,
};

void WriteDiagnosticLogEntry(const wchar_t* message,
    DiagnosticLogLevel level = DiagnosticLogLevel::Info);

// Critical evidence bypasses the detailed-log switches. The original sink is
// also used directly for a small set of essential lifecycle/error records.
inline bool ShouldWriteDiagnosticLog(DiagnosticLogModule module,
    DiagnosticLogLevel level = DiagnosticLogLevel::Info) noexcept
{
    return level == DiagnosticLogLevel::Warning || level == DiagnosticLogLevel::Error ||
        snowdesktop::diagnostics::IsLogEnabled(module);
}
inline void WriteModuleDiagnosticLogEntry(DiagnosticLogModule module,
    const wchar_t* message, DiagnosticLogLevel level = DiagnosticLogLevel::Info)
{
    if (ShouldWriteDiagnosticLog(module, level)) WriteDiagnosticLogEntry(message, level);
}
