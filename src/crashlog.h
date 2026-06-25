/**
 * @file crashlog.h
 * @brief 崩溃日志处理
 * @details 使用 dbghelp.dll 捕获未处理异常并生成包含调用栈的崩溃日志文件 crash.log
 */

#pragma once
#include <windows.h>
#include <shobjidl.h>
#include <dbghelp.h>
#include <psapi.h>
#include <algorithm>
#include <string>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

/**
 * @brief 安装崩溃处理程序
 * @details 初始化 Symbol 加载器，必须在程序启动时精确调用一次，不可重复初始化
 */
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

/**
 * @brief 写入 Windows minidump 文件
 * @details 在崩溃时生成 .dmp 文件，配合 PDB 可在 Visual Studio / WinDbg 中
 *          精确还原崩溃现场（调用栈、局部变量、寄存器、线程状态）。
 *          文件命名为 SnowDesktop_<pid>_<tick>.dmp，写入可执行文件所在目录下的
 *          crashdumps 子目录（首次崩溃时自动创建）。自动清理旧 dump，仅保留最近
 *          5 个，避免磁盘无限增长。
 * @param info 异常指针，传给 MiniDumpWriteDump 用于记录异常上下文
 * @param exeDir 可执行文件所在目录
 */
inline void WriteMiniDump(EXCEPTION_POINTERS* info, const std::wstring& exeDir)
{
    CreateDirectoryW(exeDir.c_str(), nullptr);

    wchar_t name[96];
    wsprintfW(name, L"SnowDesktop_%lu_%lu.dmp",
        GetCurrentProcessId(), GetTickCount());
    std::wstring path = exeDir + L"\\" + name;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;

    MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithDataSegs |
        MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData);

    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
        f, dumpType, &mei, nullptr, nullptr);
    CloseHandle(f);

    if (!ok)
    {
        DeleteFileW(path.c_str());
        return;
    }

    // --- 清理旧 dump，仅保留最近 5 个 ---
    std::vector<std::pair<std::wstring, FILETIME>> dumps;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((exeDir + L"\\SnowDesktop_*.dmp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            dumps.emplace_back(fd.cFileName, fd.ftLastWriteTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    constexpr size_t kMaxDumps = 5;
    if (dumps.size() <= kMaxDumps) return;

    std::sort(dumps.begin(), dumps.end(),
        [](const auto& a, const auto& b) {
            return CompareFileTime(&a.second, &b.second) < 0;
        });
    for (size_t i = 0; i + kMaxDumps < dumps.size(); ++i)
        DeleteFileW((exeDir + L"\\" + dumps[i].first).c_str());
}

/**
 * @brief 顶层未处理异常处理函数
 * @details 通过 SetUnhandledExceptionFilter 注册，异常发生时生成包含调用栈的崩溃日志，
 *          日志写入可执行文件所在目录的 crash.log 文件
 * @param info 异常指针，包含异常记录与线程上下文
 * @return 始终返回 EXCEPTION_EXECUTE_HANDLER，终止进程
 */
inline LONG WINAPI CrashHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

    // --- 打开崩溃日志文件 crash.log（追加模式） ---
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    std::wstring dumpDir = dir + L"\\crashdumps";
    CreateDirectoryW(dumpDir.c_str(), nullptr);

    HANDLE f = CreateFileW((dumpDir + L"\\crash.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;

    // --- 写入崩溃头部信息：异常代码、地址、进程 ID、运行时长 ---
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

    // --- 写入寄存器上下文（RIP/EIP、RSP/ESP、RBP/EBP） ---
    CONTEXT* ctx = info->ContextRecord;
#if defined(_M_X64)
    wsprintfW(buf, L"  RIP=%016I64X RSP=%016I64X RBP=%016I64X\r\n",
        ctx->Rip, ctx->Rsp, ctx->Rbp);
#else
    wsprintfW(buf, L"  EIP=%08lX ESP=%08lX EBP=%08lX\r\n",
        ctx->Eip, ctx->Esp, ctx->Ebp);
#endif
    write(buf);

    // --- 符号解析与调用栈遍历 ---
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

        // --- 通过 SymFromAddrW 解析符号名称 ---
        if (SymFromAddrW(hProcess, frameAddr, &displacement, sym))
        {
            IMAGEHLP_MODULEW64 modInfo{};
            modInfo.SizeOfStruct = sizeof(modInfo);
            const wchar_t* modName = L"?";
            if (SymGetModuleInfoW64(hProcess, sym->ModBase, &modInfo))
                modName = modInfo.ModuleName;

            // --- 尝试解析源码文件名与行号 ---
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
        // --- 符号解析失败时，回退显示模块基址偏移 ---
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

    // --- 写入结束标记并关闭文件 ---
    wsprintfW(buf, L"=== END ===\r\n");
    write(buf);
    CloseHandle(f);

    // --- 写入 minidump，便于后续用 PDB 精确还原崩溃现场 ---
    WriteMiniDump(info, dumpDir);

    return EXCEPTION_EXECUTE_HANDLER;
}

/**
 * @brief 手动触发一次访问违规崩溃，用于测试崩溃日志与 minidump 写入流程
 * @details 通过对空指针解引用写入的方式产生 0xC0000005 访问违规，
 *          会进入已注册的 UnhandledFilter / CrashHandler，从而生成
 *          crash.log 条目与 crashdumps\*.dmp 文件。
 *          仅供调试页"崩溃测试"按钮调用，正式发布构建也应保留以便
 *          在用户现场验证崩溃捕获是否正常。
 */
inline void TriggerCrashForTesting()
{
    volatile int* nullPtr = nullptr;
    *nullPtr = 0xDEAD;
}

/**
 * @brief 安全调用 IContextMenu::InvokeCommand，使用 SEH 捕获第三方 Shell 扩展
 *        可能抛出的访问违规等异常，防止导致 SnowDesktop 崩溃。
 * @param ctxMenu  目标 IContextMenu 接口指针
 * @param pici     指向 CMINVOKECOMMANDINFO 或 CMINVOKECOMMANDINFOEX 的指针
 * @return 调用成功返回 TRUE，异常或失败返回 FALSE
 */
inline BOOL SafeInvokeCommand(IContextMenu* ctxMenu, LPCMINVOKECOMMANDINFO pici)
{
    __try
    {
        return SUCCEEDED(ctxMenu->InvokeCommand(pici));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
}
