#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

static_assert(sizeof(void*) == 4, "SnowDesktopWallpaperInjector32 must be built for x86");

namespace {

std::wstring AbsolutePath(const wchar_t* path)
{
    if (!path || !*path)
        return {};
    const DWORD required = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!required)
        return {};
    std::vector<wchar_t> buffer(required + 1);
    const DWORD length = GetFullPathNameW(path, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (!length || length >= buffer.size())
        return {};
    return std::wstring(buffer.data(), length);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3)
        return 2;

    wchar_t* end = nullptr;
    const unsigned long parsedPid = std::wcstoul(argv[1], &end, 10);
    if (!parsedPid || !end || *end != L'\0')
        return 3;

    const std::wstring dllPath = AbsolutePath(argv[2]);
    if (dllPath.empty() || GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 4;

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, static_cast<DWORD>(parsedPid));
    if (!process)
        return 5;

    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(process, nullptr, pathBytes,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
    {
        CloseHandle(process);
        return 6;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), pathBytes, &written) ||
        written != pathBytes)
    {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 7;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 ? GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
    HANDLE thread = loadLibrary
        ? CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr)
        : nullptr;
    if (!thread)
    {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return 8;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 10000);
    DWORD remoteModule = 0;
    const bool loaded = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &remoteModule) && remoteModule != 0;

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return loaded ? 0 : 9;
}
