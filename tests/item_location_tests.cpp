#include "item_location.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <string>

namespace
{

using Microsoft::WRL::ComPtr;

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

bool SamePath(const std::wstring& left, const std::wstring& right)
{
    auto normalize = [](const std::wstring& path) {
        wchar_t fullPath[32768]{};
        const DWORD fullLength = GetFullPathNameW(path.c_str(),
            static_cast<DWORD>(_countof(fullPath)), fullPath, nullptr);
        const wchar_t* source =
            fullLength > 0 && fullLength < _countof(fullPath)
            ? fullPath
            : path.c_str();

        wchar_t longPath[32768]{};
        const DWORD longLength = GetLongPathNameW(source, longPath,
            static_cast<DWORD>(_countof(longPath)));
        return std::wstring(
            longLength > 0 && longLength < _countof(longPath)
            ? longPath
            : source);
    };
    const std::wstring normalizedLeft = normalize(left);
    const std::wstring normalizedRight = normalize(right);
    return _wcsicmp(
        normalizedLeft.c_str(),
        normalizedRight.c_str()) == 0;
}

std::wstring JoinPath(const std::wstring& directory, const wchar_t* name)
{
    if (!directory.empty() &&
        (directory.back() == L'\\' ||
            directory.back() == L'/'))
        return directory + name;
    return directory + L"\\" + name;
}

bool CreateShortcut(const std::wstring& shortcutPath,
    const std::wstring& targetPath)
{
    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(shellLink.GetAddressOf()))) ||
        FAILED(shellLink->SetPath(targetPath.c_str())))
        return false;

    ComPtr<IPersistFile> persistFile;
    return SUCCEEDED(shellLink.As(&persistFile)) &&
        SUCCEEDED(persistFile->Save(shortcutPath.c_str(), TRUE));
}

} // namespace

int wmain()
{
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Expect(SUCCEEDED(comResult), "COM initializes for shell-link test");

    wchar_t temporaryRoot[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, temporaryRoot) > 0,
        "temporary directory path is available");

    const std::wstring directory = JoinPath(temporaryRoot,
        (L"SnowDesktopItemLocation-" + std::to_wstring(GetCurrentProcessId())).c_str());
    CreateDirectoryW(directory.c_str(), nullptr);

    const std::wstring targetPath = JoinPath(directory, L"target.txt");
    const std::wstring shortcutPath = JoinPath(directory, L"target.lnk");
    const std::wstring targetDirectory =
        JoinPath(directory, L"folder-target");
    const std::wstring folderShortcutPath =
        JoinPath(directory, L"folder-target.lnk");
    const std::wstring missingFolderShortcutPath =
        JoinPath(directory, L"missing-folder.lnk");
    const std::wstring missingDirectory =
        JoinPath(directory, L"missing-folder");
    CreateDirectoryW(targetDirectory.c_str(), nullptr);
    FILE* targetFile = nullptr;
    _wfopen_s(&targetFile, targetPath.c_str(), L"wb");
    Expect(targetFile != nullptr, "target file is created");
    if (targetFile)
    {
        std::fputs("target", targetFile);
        std::fclose(targetFile);
    }

    Expect(CreateShortcut(shortcutPath, targetPath),
        "test shortcut is created");
    Expect(CreateShortcut(
        folderShortcutPath, targetDirectory),
        "folder shortcut is created");
    Expect(CreateShortcut(
        missingFolderShortcutPath, missingDirectory),
        "unavailable folder shortcut is created");
    Expect(SamePath(
        snowdesktop::item_location::ResolveRevealPath(targetPath),
        targetPath),
        "ordinary file resolves to itself");
    const std::wstring resolvedShortcut =
        snowdesktop::item_location::ResolveRevealPath(shortcutPath);
    if (!SamePath(resolvedShortcut, targetPath))
    {
        std::wcerr << L"Resolved shortcut: " << resolvedShortcut
                   << L"\nExpected target: " << targetPath << L'\n';
    }
    Expect(SamePath(resolvedShortcut, targetPath),
        "shortcut resolves to its target");
    Expect(snowdesktop::item_location::CanReveal(shortcutPath),
        "shortcut with existing target can be revealed");

    const auto missingShortcutProbeStart =
        std::chrono::steady_clock::now();
    Expect(snowdesktop::item_location::CanReveal(
            missingFolderShortcutPath),
        "shortcut with unavailable target can reveal the shortcut itself");
    const auto missingShortcutProbeElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            missingShortcutProbeStart);
    Expect(missingShortcutProbeElapsed < std::chrono::seconds(1),
        "shortcut reveal availability avoids Shell target resolution timeout");

    const std::wstring missingPath = JoinPath(directory, L"missing.txt");
    Expect(!snowdesktop::item_location::CanReveal(missingPath),
        "missing item cannot be revealed");

    using snowdesktop::item_location::FolderTargetKind;
    const auto directFolder =
        snowdesktop::item_location::ResolveFolderTarget(
            targetDirectory);
    Expect(directFolder.available &&
        directFolder.kind == FolderTargetKind::Directory &&
        SamePath(directFolder.path, targetDirectory),
        "real directory resolves as an available directory");

    const auto shortcutFolder =
        snowdesktop::item_location::ResolveFolderTarget(
            folderShortcutPath);
    Expect(shortcutFolder.available &&
        shortcutFolder.kind == FolderTargetKind::Shortcut &&
        SamePath(shortcutFolder.path, targetDirectory),
        "folder shortcut resolves to its file-system directory");

    const auto fileShortcut =
        snowdesktop::item_location::ResolveFolderTarget(
            shortcutPath);
    Expect(fileShortcut.kind == FolderTargetKind::None,
        "file shortcut is not classified as a folder target");

    const auto unavailableShortcutStart =
        std::chrono::steady_clock::now();
    const auto unavailableShortcut =
        snowdesktop::item_location::ResolveFolderTarget(
            missingFolderShortcutPath);
    const auto unavailableShortcutElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() -
            unavailableShortcutStart);
    Expect(!unavailableShortcut.available &&
        unavailableShortcut.kind ==
            FolderTargetKind::Shortcut &&
        SamePath(
            unavailableShortcut.path,
            missingDirectory),
        "unavailable folder shortcut retains a disabled target");
    Expect(unavailableShortcutElapsed < std::chrono::seconds(1),
        "unavailable shortcut target resolution skips Shell search timeout");

    const auto unavailableFolder =
        snowdesktop::item_location::ResolveFolderTarget(
            missingDirectory);
    Expect(!unavailableFolder.available &&
        unavailableFolder.kind ==
            FolderTargetKind::None,
        "unavailable direct paths are not guessed to be folders");

    DeleteFileW(folderShortcutPath.c_str());
    DeleteFileW(missingFolderShortcutPath.c_str());
    DeleteFileW(shortcutPath.c_str());
    DeleteFileW(targetPath.c_str());
    RemoveDirectoryW(targetDirectory.c_str());
    RemoveDirectoryW(directory.c_str());
    if (SUCCEEDED(comResult))
        CoUninitialize();

    if (failures == 0)
        std::cout << "All item-location tests passed.\n";
    return failures == 0 ? 0 : 1;
}
