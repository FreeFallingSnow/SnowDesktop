#pragma once
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <string>

#pragma comment(lib, "dbghelp.lib")

// Called exactly once at startup. dbghelp must NOT be re-initialised later.
inline void InstallCrashHandler()
{
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = L'\0';

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES |
                  SYMOPT_UNDNAME | SYMOPT_NO_PROMPTS);
    SymInitializeW(GetCurrentProcess(), exeDir, TRUE);
}

inline LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);

    HANDLE f = CreateFileW((dir + L"crash.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;

    DWORD code = info->ExceptionRecord->ExceptionCode;
    DWORD64 addr = (DWORD64)info->ExceptionRecord->ExceptionAddress;
    wchar_t buf[1024];
    DWORD w;

    auto write = [&](const wchar_t* s) {
        WriteFile(f, s, (DWORD)(wcslen(s) * sizeof(wchar_t)), &w, nullptr);
    };

    wsprintfW(buf, L"\r\n=== CRASH 0x%08X at 0x%016I64X  pid=%lu  tick=%lu ===\r\n",
        code, addr, GetCurrentProcessId(), GetTickCount());
    write(buf);

    CONTEXT* ctx = info->ContextRecord;
#if defined(_M_X64)
    wsprintfW(buf, L"  RIP=%016I64X RSP=%016I64X RBP=%016I64X\r\n",
        ctx->Rip, ctx->Rsp, ctx->Rbp);
#else
    wsprintfW(buf, L"  EIP=%08lX ESP=%08lX EBP=%08lX\r\n",
        ctx->Eip, ctx->Esp, ctx->Ebp);
#endif
    write(buf);

    // --- symbol-resolved call stack ---
    HANDLE hProcess = GetCurrentProcess();

    constexpr DWORD kSymBufSize = sizeof(SYMBOL_INFOW) + 512 * sizeof(wchar_t);
    alignas(SYMBOL_INFOW) char symBuf[kSymBufSize]{};
    SYMBOL_INFOW* sym = reinterpret_cast<SYMBOL_INFOW*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
    sym->MaxNameLen = 512;

    IMAGEHLP_LINEW64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINEW64);

    PVOID frames[64];
    USHORT count = RtlCaptureStackBackTrace(0, 64, frames, nullptr);

    for (int i = 0; i < count; ++i)
    {
        DWORD64 frameAddr = (DWORD64)frames[i];
        DWORD64 displacement = 0;

        if (SymFromAddrW(hProcess, frameAddr, &displacement, sym))
        {
            IMAGEHLP_MODULEW64 modInfo{};
            modInfo.SizeOfStruct = sizeof(modInfo);
            const wchar_t* modName = L"?";
            if (SymGetModuleInfoW64(hProcess, sym->ModBase, &modInfo))
                modName = modInfo.ModuleName;

            DWORD lineDisp = 0;
            if (SymGetLineFromAddrW64(hProcess, frameAddr, &lineDisp, &line))
            {
                wsprintfW(buf, L"  %2d  %s!%s+0x%I64X  (%s:%lu)\r\n",
                    i, modName, sym->Name, displacement,
                    line.FileName, line.LineNumber);
            }
            else
            {
                wsprintfW(buf, L"  %2d  %s!%s+0x%I64X\r\n",
                    i, modName, sym->Name, displacement);
            }
        }
        else
        {
            HMODULE mod = nullptr;
            wchar_t modPath[MAX_PATH]{};
            DWORD64 modBase = 0;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(frameAddr), &mod) &&
                mod)
            {
                MODULEINFO mi{};
                if (GetModuleInformation(hProcess, mod, &mi, sizeof(mi)))
                    modBase = (DWORD64)mi.lpBaseOfDll;
                GetModuleFileNameW(mod, modPath, MAX_PATH);
                wchar_t* name = wcsrchr(modPath, L'\\');
                name = name ? name + 1 : modPath;
                wsprintfW(buf, L"  %2d  %s+0x%I64X\r\n",
                    i, name, frameAddr - modBase);
            }
            else
            {
                wsprintfW(buf, L"  %2d  0x%016I64X\r\n", i, frameAddr);
            }
        }
        write(buf);
    }

    wsprintfW(buf, L"=== END ===\r\n");
    write(buf);
    CloseHandle(f);
    return EXCEPTION_EXECUTE_HANDLER;
}
