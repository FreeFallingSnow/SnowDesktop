#include <windows.h>

#include <filesystem>
#include <iostream>

#include "resource.h"

namespace
{
bool HasGroupIconResource(HMODULE module, WORD identifier)
{
    return FindResourceW(module, MAKEINTRESOURCEW(identifier),
               RT_GROUP_ICON) != nullptr;
}
}

int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: launcher_icon_resource_tests <launcher.exe>\n";
        return 2;
    }

    const std::filesystem::path launcherPath = argv[1];
    const HMODULE module = LoadLibraryExW(launcherPath.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module)
    {
        std::cerr << "FAIL: unable to load launcher resources from "
                  << launcherPath.string() << '\n';
        return 1;
    }

    const bool hasAppIcon = HasGroupIconResource(module, IDI_APPICON);
    const bool hasSmallAppIcon =
        HasGroupIconResource(module, IDI_APPICON_SMALL);
    FreeLibrary(module);

    if (!hasAppIcon)
        std::cerr << "FAIL: launcher is missing IDI_APPICON\n";
    if (!hasSmallAppIcon)
        std::cerr << "FAIL: launcher is missing IDI_APPICON_SMALL\n";

    if (!hasAppIcon || !hasSmallAppIcon)
        return 1;

    std::cout << "Steam launcher contains both SnowDesktop icon resources\n";
    return 0;
}
