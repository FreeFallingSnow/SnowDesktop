#include "shell_context_menu_invoke.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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

    std::cout << "shell context-menu invocation tests passed\n";
    return 0;
}
