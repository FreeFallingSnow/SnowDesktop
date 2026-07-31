#include "widget_package.h"

#include "data_paths.h"

namespace snowdesktop::widget
{
PackagePaths PackagePaths::ForCurrentDeployment()
{
    const std::filesystem::path executable(GetExecutableDirectoryPath());
    const std::filesystem::path data(GetDataDirectoryPath());
    PackagePaths paths;
    paths.builtin = executable / L"widgets";
    paths.installed = data / L"widgets" / L"installed";
    paths.development = data / L"widgets" / L"dev";
    paths.staging = data / L"widgets" / L"staging";
    paths.quarantine = data / L"widgets" / L"quarantine";
    paths.migrations = data / L"widgets" / L"migrations";
    paths.registry = data / L"widgets" / L"packages.json";
    return paths;
}
}
