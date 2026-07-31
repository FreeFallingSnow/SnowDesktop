/**
 * @file data_paths.cpp
 * @brief SnowDesktop 用户数据路径与旧版路径迁移实现。
 */

#include "data_paths.h"
#include "deployment_context.h"
#include "portable_data_migration.h"

#include <windows.h>
#include <shlwapi.h>

#include <cwchar>
#include <iterator>

namespace
{
    bool PathExistsLocal(const std::wstring& path)
    {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool DirectoryExistsLocal(const std::wstring& path)
    {
        DWORD attrs = GetFileAttributesW(path.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring JoinPathLocal(std::wstring base, const wchar_t* leaf)
    {
        if (base.empty())
            return leaf ? std::wstring(leaf) : std::wstring();
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
            base.push_back(L'\\');
        if (leaf)
            base += leaf;
        return base;
    }

    bool LooksLikeBareName(const wchar_t* name)
    {
        if (!name || !*name)
            return false;
        for (const wchar_t* p = name; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/' || *p == L':')
                return false;
        }
        return true;
    }

    void EnsureDirectoryLocal(const std::wstring& path)
    {
        if (path.empty() || DirectoryExistsLocal(path))
            return;
        CreateDirectoryW(path.c_str(), nullptr);
    }

    void MigratePathIfNeeded(const std::wstring& legacyPath, const std::wstring& currentPath)
    {
        if (!PathExistsLocal(legacyPath) || PathExistsLocal(currentPath))
            return;

        MoveFileExW(
            legacyPath.c_str(),
            currentPath.c_str(),
            MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
    }

    void MigrateDirectoryPathIfNeeded(const std::wstring& legacyPath, const std::wstring& currentPath)
    {
        if (!DirectoryExistsLocal(legacyPath))
            return;

        if (!PathExistsLocal(currentPath))
        {
            MoveFileExW(
                legacyPath.c_str(),
                currentPath.c_str(),
                MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
            return;
        }

        if (!DirectoryExistsLocal(currentPath))
            return;

        std::wstring searchPath = JoinPathLocal(legacyPath, L"*");
        WIN32_FIND_DATAW findData{};
        HANDLE find = FindFirstFileW(searchPath.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE)
            return;

        do
        {
            if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0)
                continue;

            std::wstring source = JoinPathLocal(legacyPath, findData.cFileName);
            std::wstring target = JoinPathLocal(currentPath, findData.cFileName);
            if (!PathExistsLocal(target))
            {
                MoveFileExW(
                    source.c_str(),
                    target.c_str(),
                    MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH);
            }
        } while (FindNextFileW(find, &findData));

        FindClose(find);
        RemoveDirectoryW(legacyPath.c_str());
    }
}

std::wstring GetExecutableDirectoryPath()
{
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= static_cast<DWORD>(std::size(path)))
        return L".";
    PathRemoveFileSpecW(path);
    return path;
}

std::wstring GetDataDirectoryPath()
{
    const std::wstring packageLocalState =
        snowdesktop::deployment::GetPackageLocalStatePath();
    std::wstring dataDir;
    if (!packageLocalState.empty())
        dataDir = JoinPathLocal(packageLocalState, L"data");
    else
        dataDir = JoinPathLocal(GetExecutableDirectoryPath(), L"data");
    EnsureDirectoryLocal(dataDir);
    return dataDir;
}

std::wstring GetDataFilePath(const wchar_t* filename)
{
    if (!LooksLikeBareName(filename))
        return filename ? std::wstring(filename) : std::wstring();

    std::wstring legacyDir =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (legacyDir.empty())
        legacyDir = GetExecutableDirectoryPath();
    const std::wstring dataDir = GetDataDirectoryPath();
    const std::wstring legacyPath = JoinPathLocal(legacyDir, filename);
    const std::wstring currentPath = JoinPathLocal(dataDir, filename);
    MigratePathIfNeeded(legacyPath, currentPath);
    return currentPath;
}

std::wstring GetDataSubdirectoryPath(const wchar_t* dirname)
{
    if (!LooksLikeBareName(dirname))
        return dirname ? std::wstring(dirname) : std::wstring();

    std::wstring legacyDir =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (legacyDir.empty())
        legacyDir = GetExecutableDirectoryPath();
    const std::wstring dataDir = GetDataDirectoryPath();
    const std::wstring legacyPath = JoinPathLocal(legacyDir, dirname);
    const std::wstring currentPath = JoinPathLocal(dataDir, dirname);
    MigrateDirectoryPathIfNeeded(legacyPath, currentPath);
    EnsureDirectoryLocal(currentPath);
    return currentPath;
}

void MigrateLegacyDataPaths()
{
    std::filesystem::path stateRoot =
        snowdesktop::deployment::GetPackageLocalStatePath();
    if (stateRoot.empty())
        stateRoot = GetExecutableDirectoryPath();
    const auto portableMigration =
        snowdesktop::migration::ApplyPending(stateRoot);
    if (!portableMigration.ok)
    {
        OutputDebugStringA(("SnowDesktop: pending portable data migration "
            "failed: " + portableMigration.error + "\n").c_str());
    }

    static const wchar_t* files[] = {
        L"SnowDesktop.layout.json",
        L"SnowDesktop.storage.json",
        L"SnowDesktop.general.json",
        L"SnowDesktop.navigation.json",
        L"SnowDesktop.categories.json",
        L"SnowDesktop.personalization.json",
        L"SnowDesktop.dock.json",
        L"SnowDesktop.dock-usage.json",
        L"SnowDesktop.log",
        L"SnowDesktop_crash.log",
    };

    for (const wchar_t* file : files)
        GetDataFilePath(file);

    GetDataSubdirectoryPath(L"backups");
    // Older MSIX builds exposed user-created loose Lua components directly
    // under LocalState\widgets. Move that directory before the package manager
    // scans LocalState\data\widgets so the migration wizard can still find it.
    // Portable builds keep their read-only-in-practice bundled packages beside
    // the executable in widgets\, so treating that directory as legacy user
    // data would move every built-in package out of the scan root.
    if (snowdesktop::deployment::IsPackaged())
        GetDataSubdirectoryPath(L"widgets");
}
