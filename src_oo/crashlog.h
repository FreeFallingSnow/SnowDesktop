#pragma once
#include <windows.h>
#include <psapi.h>
#include <string>

inline LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

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

    // Map crash address to its owning module, not always the main executable.
    HMODULE crashModule = nullptr;
    MODULEINFO mi{};
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress), &crashModule) &&
        crashModule && GetModuleInformation(GetCurrentProcess(), crashModule, &mi, sizeof(mi)))
    {
        DWORD64 base = (DWORD64)mi.lpBaseOfDll;
        DWORD64 offset = addr - base;
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(crashModule, modulePath, MAX_PATH);
        wsprintfW(buf, L"  Module=%s  base=0x%016I64X  offset=%I64u (0x%016I64X)\r\n",
            modulePath, base, offset, offset);
        WriteFile(f, buf, (DWORD)(wcslen(buf) * 2), &w, nullptr);
    }

    // Dump register context
    CONTEXT* ctx = info->ContextRecord;
#if defined(_M_X64)
    wsprintfW(buf, L"  RIP=%016I64X RSP=%016I64X RBP=%016I64X\r\n", ctx->Rip, ctx->Rsp, ctx->Rbp);
#else
    wsprintfW(buf, L"  EIP=%08lX ESP=%08lX EBP=%08lX\r\n", ctx->Eip, ctx->Esp, ctx->Ebp);
#endif
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
