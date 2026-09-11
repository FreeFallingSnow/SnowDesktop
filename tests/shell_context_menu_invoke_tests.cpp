#include "shell_context_menu_invoke.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <wrl/client.h>

namespace
{

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int wmain()
{
    Expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        "Shell command tests initialize COM");
    const std::wstring currentDirectory =
        std::filesystem::current_path().wstring();
    Expect(snowdesktop::ShellInvocationDirectoryForItem(
            currentDirectory) == currentDirectory,
        "a directory item invokes Shell commands in that directory");

    const std::wstring filePath =
        (std::filesystem::current_path() /
            L"snowdesktop-shell-invoke-probe.txt").wstring();
    Expect(snowdesktop::ShellInvocationDirectoryForItem(filePath) ==
            currentDirectory,
        "a file item invokes Shell commands in its parent directory");

    const std::wstring desktopDirectory =
        snowdesktop::DesktopShellInvocationDirectory();
    Expect(!desktopDirectory.empty() &&
            std::filesystem::is_directory(desktopDirectory),
        "the physical desktop directory is available for background verbs");

    CMINVOKECOMMANDINFOEX invoke{};
    invoke.cbSize = sizeof(invoke);
    invoke.fMask = CMIC_MASK_UNICODE;
    std::string ansiDirectory;
    snowdesktop::SetShellInvocationDirectory(
        invoke, currentDirectory, ansiDirectory);
    Expect(invoke.lpDirectoryW == currentDirectory.c_str(),
        "the Unicode Shell invocation directory uses stable caller storage");
    Expect(invoke.lpDirectory != nullptr &&
            !ansiDirectory.empty(),
        "legacy Shell handlers also receive an ANSI invocation directory");

    // Query the real Windows New handler without displaying a menu or creating
    // files. This catches unsupported canonical verbs and lazy submenu loading.
    {
        Microsoft::WRL::ComPtr<IContextMenu> context;
        Expect(SUCCEEDED(CoCreateInstance(CLSID_NewMenu, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(context.GetAddressOf()))),
            "Windows New-menu handler is available");
        Microsoft::WRL::ComPtr<IShellExtInit> initializer;
        Expect(SUCCEEDED(context.As(&initializer)), "New handler accepts a folder");
        PIDLIST_ABSOLUTE folder = nullptr;
        Expect(SUCCEEDED(SHParseDisplayName(currentDirectory.c_str(), nullptr,
                &folder, 0, nullptr)), "test folder resolves to a PIDL");
        const HRESULT initialized = initializer->Initialize(folder, nullptr, 0);
        CoTaskMemFree(folder);
        Expect(SUCCEEDED(initialized), "New handler initializes for the target folder");
        HMENU root = CreatePopupMenu();
        Expect(SUCCEEDED(context->QueryContextMenu(root, 0, 1, 0x7FFF, CMF_NORMAL)),
            "New handler builds its commands");
        HMENU submenu = GetSubMenu(root, 0);
        Microsoft::WRL::ComPtr<IContextMenu2> messages;
        if (SUCCEEDED(context.As(&messages)))
            messages->HandleMenuMsg(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(submenu), 0);
        const UINT command = snowdesktop::FindNewFolderCommand(context.Get(), submenu);
        Expect(command != 0, "Ctrl+Shift+N resolves the real NewFolder command");
        EnableMenuItem(submenu, command, MF_BYCOMMAND | MF_GRAYED);
        Expect(snowdesktop::FindNewFolderCommand(context.Get(), submenu) == 0,
            "a disabled NewFolder command cannot be executed");
        DestroyMenu(root);
    }
    CoUninitialize();
    std::cout << "shell context-menu invocation tests passed\n";
    return 0;
}
