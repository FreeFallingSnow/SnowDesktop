#include "diagnostic_log.h"
#include "data/data_paths.h"

#include <windows.h>
#include <cwchar>
#include <mutex>
#include <string>

void WriteDiagnosticLogEntry(
    const wchar_t* message, DiagnosticLogLevel level)
try
{
    if (!message) return;
    static std::mutex logMutex;
    std::scoped_lock lock(logMutex);

    constexpr LONGLONG kMaximumLogBytes = 1024 * 1024;
    std::wstring filename = GetDataFilePath(L"SnowDesktop.log");
    const auto fallback = [&]() {
        wchar_t temporary[MAX_PATH + 1]{};
        const DWORD count = GetTempPathW(MAX_PATH, temporary);
        if (!count || count > MAX_PATH) return INVALID_HANDLE_VALUE;
        filename = std::wstring(temporary) + L"SnowDesktop-" +
            std::to_wstring(GetCurrentProcessId()) + L".log";
        return CreateFileW(filename.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    };

    HANDLE file = CreateFileW(filename.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) file = fallback();
    if (file == INVALID_HANDLE_VALUE) { OutputDebugStringW(message); return; }

    const std::wstring previousFilename = filename + L".1";
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) &&
        size.QuadPart >= kMaximumLogBytes)
    {
        CloseHandle(file);
        if (MoveFileExW(filename.c_str(), previousFilename.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            file = CreateFileW(filename.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        else
        {
            file = CreateFileW(filename.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (file == INVALID_HANDLE_VALUE) file = fallback();
        if (file == INVALID_HANDLE_VALUE) { OutputDebugStringW(message); return; }
    }

    const wchar_t* levelName = L"INFO";
    switch (level)
    {
    case DiagnosticLogLevel::Debug: levelName = L"DEBUG"; break;
    case DiagnosticLogLevel::Warning: levelName = L"WARN"; break;
    case DiagnosticLogLevel::Error: levelName = L"ERROR"; break;
    case DiagnosticLogLevel::Info:
    default: break;
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t prefix[96]{};
    swprintf_s(prefix,
        L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%ls] ",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds, levelName);
    std::wstring line = prefix;
    line += message;
    line += L"\r\n";

    LARGE_INTEGER end{};
    end.QuadPart = 0;
    SetFilePointerEx(file, end, nullptr, FILE_END);
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(line.size() * sizeof(wchar_t));
    const bool saved = WriteFile(file, line.data(), bytes, &written, nullptr) && written == bytes;
    CloseHandle(file);
    if (!saved)
    {
        file = fallback();
        if (file != INVALID_HANDLE_VALUE)
        {
            SetFilePointerEx(file, end, nullptr, FILE_END);
            if (!WriteFile(file, line.data(), bytes, &written, nullptr) || written != bytes)
                OutputDebugStringW(line.c_str());
            CloseHandle(file);
        }
        else OutputDebugStringW(line.c_str());
    }
}
catch (...)
{
    // Diagnostics must never prevent the error UI or the original operation.
    if (message) OutputDebugStringW(message);
}
