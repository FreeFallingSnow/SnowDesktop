#pragma once

enum class DiagnosticLogLevel
{
    Debug,
    Info,
    Warning,
    Error,
};

void WriteDiagnosticLogEntry(const wchar_t* message,
    DiagnosticLogLevel level = DiagnosticLogLevel::Info);
