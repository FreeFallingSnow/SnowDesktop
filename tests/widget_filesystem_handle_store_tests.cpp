#include "widget_filesystem_handle_store.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path CreateProbeDirectory()
{
    wchar_t temporary[MAX_PATH]{};
    Expect(GetTempPathW(MAX_PATH, temporary) > 0,
        "temporary directory is available");
    const auto root = std::filesystem::path(temporary) /
        (L"SnowDesktopWidgetFilesystemHandleStoreTests-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    Expect(std::filesystem::create_directories(root, error) && !error,
        "probe directory is created");
    return root;
}
}

int main()
{
    using namespace snowdesktop::widget_runtime;
    const auto root = CreateProbeDirectory();
    const auto registry = root / L"handles.json";
    const auto folder = root / L"selected";
    const auto file = folder / L"note.txt";
    std::filesystem::create_directories(folder);
    std::ofstream(file, std::ios::binary) << "hello";

    const WidgetFilesystemHandleOwner ownerA{ "instance-a", "package-a" };
    const WidgetFilesystemHandleOwner ownerB{ "instance-b", "package-a" };
    std::string error;
    WidgetFilesystemHandleStore store(registry);
    Expect(store.Load(error) && error.empty(),
        "a missing registry loads as empty");

    const auto fileGrant = store.Grant(ownerA, file,
        WidgetFilesystemHandleKind::File,
        WidgetFilesystemHandleAccess::Read);
    Expect(fileGrant &&
            WidgetFilesystemHandleStore::IsOpaqueHandle(
                fileGrant.entry->handle) &&
            fileGrant.entry->handle.find("note") == std::string::npos,
        "grant returns a path-free opaque handle");
    const auto duplicateGrant = store.Grant(ownerA, file,
        WidgetFilesystemHandleKind::File,
        WidgetFilesystemHandleAccess::Read);
    Expect(duplicateGrant && duplicateGrant.entry->handle ==
            fileGrant.entry->handle && store.Size() == 1,
        "identical grants reuse the existing handle");
    const auto isolatedGrant = store.Grant(ownerA, file,
        WidgetFilesystemHandleKind::File,
        WidgetFilesystemHandleAccess::Read, false);
    Expect(isolatedGrant && isolatedGrant.created &&
            isolatedGrant.entry->handle != fileGrant.entry->handle &&
            store.Size() == 2,
        "host-managed settings can request independently revocable handles");
    Expect(store.Resolve(ownerA, fileGrant.entry->handle).has_value() &&
            !store.Resolve(ownerB, fileGrant.entry->handle).has_value(),
        "handles are scoped to both instance and package ownership");

    const auto folderGrant = store.Grant(ownerA, folder,
        WidgetFilesystemHandleKind::Folder,
        WidgetFilesystemHandleAccess::ReadWrite);
    Expect(folderGrant && store.Size() == 3,
        "folder and file grants remain distinct");

    WidgetFilesystemHandleStore reloaded(registry);
    Expect(reloaded.Load(error) && error.empty() &&
            reloaded.Resolve(ownerA, fileGrant.entry->handle).has_value() &&
            reloaded.Resolve(ownerA, folderGrant.entry->handle).has_value(),
        "opaque grants survive a store reload");
    Expect(reloaded.Revoke(ownerA, fileGrant.entry->handle, error) &&
            !reloaded.Resolve(ownerA, fileGrant.entry->handle).has_value(),
        "an owner can revoke its own handle");
    Expect(!reloaded.Revoke(ownerB, folderGrant.entry->handle, error) &&
            reloaded.Resolve(ownerA, folderGrant.entry->handle).has_value(),
        "another instance cannot revoke a handle");
    Expect(reloaded.RevokeInstance(ownerA.instanceId, error) == 2 &&
            reloaded.Size() == 0,
        "deleting an instance revokes its remaining grants");

    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::cout << "widget filesystem handle store tests passed\n";
    return 0;
}
