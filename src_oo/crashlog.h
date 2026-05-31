#pragma once
#include <windows.h>
#include <psapi.h>
#include <string>

inline LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    HANDLE f = CreateFileW((dir + L"crash.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = info->ExceptionRecord->ExceptionCode;
    DWORD64 addr = (DWORD64)info->ExceptionRecord->ExceptionAddress;
    wchar_t buf[512];
    wsprintfW(buf, L"\r\n=== CRASH 0x%08X at 0x%016I64X  pid=%lu  tick=%lu ===\r\n",
        code, addr, GetCurrentProcessId(), GetTickCount());
    DWORD w;
    WriteFile(f, buf, (DWORD)(wcslen(buf) * 2), &w, nullptr);

    // Map crash address to module+offset
    HMODULE exe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    if (exe && GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi)))
    {
        DWORD64 base = (DWORD64)mi.lpBaseOfDll;
        DWORD64 offset = addr - base;
        wsprintfW(buf, L"  Module base=0x%016I64X  offset=%I64u (0x%IX)\r\n", base, offset, (DWORD)offset);
        WriteFile(f, buf, (DWORD)(wcslen(buf) * 2), &w, nullptr);
    }

    // Dump register context
    CONTEXT* ctx = info->ContextRecord;
    wsprintfW(buf, L"  RIP=%016I64X RSP=%016I64X RBP=%016I64X\r\n", ctx->Rip, ctx->Rsp, ctx->Rbp);
    WriteFile(f, buf, (DWORD)(wcslen(buf) * 2), &w, nullptr);

    // Mini stack: simple RtlCaptureStackBackTrace
    PVOID frames[32];
    USHORT count = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
    for (int i = 0; i < count; ++i)
    {
        wsprintfW(buf, L"  %2d  0x%016I64X\r\n", i, (DWORD64)frames[i]);
        WriteFile(f, buf, (DWORD)(wcslen(buf) * 2), &w, nullptr);
    }

    wsprintfW(buf, L"=== END ===\r\n");
    WriteFile(f, buf, (DWORD)(wcslen(buf) * 2), &w, nullptr);
    CloseHandle(f);
    return EXCEPTION_EXECUTE_HANDLER;
}

inline void InstallCrashHandler() {}
