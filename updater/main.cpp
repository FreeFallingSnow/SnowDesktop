// SparkDesktopUpdater - 便携版自更新辅助程序。
//
// 主程序下载并校验更新 zip 后，以隐藏窗口启动本程序并退出；
// 本程序等待主进程结束后，解压 zip 覆盖程序文件（保留 data 等
// 额外目录），然后重新启动 SparkDesktop.exe。
//
// 用法:
//   SparkDesktopUpdater.exe --zip <update.zip> --dir <installDir>
//       --launch SparkDesktop.exe --pid <mainPid>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>

namespace {

std::wstring GetSystemTool(const wchar_t* name)
{
    wchar_t systemDir[MAX_PATH]{};
    if (GetSystemDirectoryW(systemDir, MAX_PATH))
    {
        const std::wstring candidate =
            std::wstring(systemDir) + L"\\" + name;
        if (GetFileAttributesW(candidate.c_str()) !=
            INVALID_FILE_ATTRIBUTES)
        {
            return candidate;
        }
    }
    return name;
}

bool RunTool(const std::wstring& tool, const std::wstring& arguments,
    bool hidden)
{
    std::wstring commandLine =
        L"\"" + tool + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = hidden ? SW_HIDE : SW_SHOWNORMAL;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tool.c_str(), commandLine.data(), nullptr,
        nullptr, FALSE, hidden ? CREATE_NO_WINDOW : 0, nullptr,
        nullptr, &startup, &process))
    {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void DeleteDirectoryTree(const std::wstring& path)
{
    // SHFileOperationW 需要双 null 终止的路径。
    std::wstring buffer = path;
    buffer.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = buffer.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    SHFileOperationW(&op);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;

    std::wstring zipPath;
    std::wstring installDir;
    std::wstring launchName;
    DWORD mainPid = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--zip") == 0 && i + 1 < argc)
            zipPath = argv[++i];
        else if (_wcsicmp(argv[i], L"--dir") == 0 && i + 1 < argc)
            installDir = argv[++i];
        else if (_wcsicmp(argv[i], L"--launch") == 0 && i + 1 < argc)
            launchName = argv[++i];
        else if (_wcsicmp(argv[i], L"--pid") == 0 && i + 1 < argc)
            mainPid = static_cast<DWORD>(_wtoi(argv[++i]));
    }
    LocalFree(argv);

    if (zipPath.empty() || installDir.empty() || launchName.empty())
        return 2;

    // 1. 等待主进程退出（最长 60 秒；超时则尽力继续）。
    if (mainPid != 0)
    {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, mainPid);
        if (process)
        {
            WaitForSingleObject(process, 60000);
            CloseHandle(process);
        }
    }

    // 2. 解压 zip 到临时目录。
    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring workRoot = std::wstring(tempPath) +
        L"SparkDesktopUpdate-" +
        std::to_wstring(GetCurrentProcessId());
    const std::wstring extractDir = workRoot + L"\\extract";
    DeleteDirectoryTree(workRoot);
    if (!CreateDirectoryW(extractDir.c_str(), nullptr))
        return 3;

    const std::wstring tar = GetSystemTool(L"tar.exe");
    if (!RunTool(tar,
            L"-xf \"" + zipPath + L"\" -C \"" + extractDir + L"\"",
            true))
    {
        DeleteDirectoryTree(workRoot);
        return 4;
    }

    // 3. 将解压内容复制到安装目录（覆盖同名文件，保留 data 等额外目录）。
    const std::wstring xcopy = GetSystemTool(L"xcopy.exe");
    RunTool(xcopy,
        L"/e /i /y \"" + extractDir + L"\" \"" + installDir + L"\"",
        true);

    // 4. 清理临时文件。
    DeleteDirectoryTree(workRoot);
    DeleteFileW(zipPath.c_str());

    // 5. 重启主程序。
    const std::wstring launcher =
        installDir + L"\\" + launchName;
    RunTool(launcher, L"", false);
    return 0;
}
