#include "widget_package.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
void PrintUsage()
{
    std::cout
        << "SnowDesktop widget package tool\n"
        << "  snowwidget validate <package-directory>\n"
        << "  snowwidget pack <package-directory> <output.snowwidget>\n"
        << "  snowwidget publish-local <package-directory> <catalog-directory>\n";
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3)
    {
        PrintUsage();
        return 2;
    }
    const std::wstring command = argv[1];
    const std::filesystem::path source = argv[2];
    snowdesktop::widget::PackagePaths paths;
    snowdesktop::widget::WidgetPackageManager manager(paths);
    snowdesktop::widget::ValidationReport report;
    snowdesktop::widget::PackageArtifact artifact;
    std::string error;

    if (command == L"validate")
    {
        report = source.extension() == L".snowwidget"
            ? manager.ValidateArchive(source)
            : manager.ValidateDirectory(source);
        std::cout << report.ToJson() << '\n';
        return report.Ok() ? 0 : 1;
    }
    if (command == L"pack")
    {
        if (argc != 4)
        {
            PrintUsage();
            return 2;
        }
        if (!manager.ExportDirectory(source, argv[3], artifact, report, error))
        {
            std::cerr << report.ToJson() << '\n' << error << '\n';
            return 1;
        }
        std::cout << "{\"ok\":true,\"packageId\":\"" << artifact.packageId
            << "\",\"version\":\"" << artifact.version
            << "\",\"sha256\":\"" << artifact.sha256
            << "\",\"path\":\""
            << WideToUtf8(artifact.localPath.wstring()) << "\"}\n";
        return 0;
    }
    if (command == L"publish-local")
    {
        if (argc != 4)
        {
            PrintUsage();
            return 2;
        }
        snowdesktop::widget::PackageManifest manifest;
        report = manager.ValidateDirectory(source, &manifest);
        if (!report.Ok())
        {
            std::cerr << report.ToJson() << '\n';
            return 1;
        }
        wchar_t temporaryRoot[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporaryRoot))
        {
            std::cerr << "cannot resolve temporary directory\n";
            return 1;
        }
        const auto temporary = std::filesystem::path(temporaryRoot) /
            (L"SnowDesktop-" + std::wstring(argv[2]).substr(
                std::wstring(argv[2]).find_last_of(L"\\/") + 1) +
                L".snowwidget");
        if (!manager.ExportDirectory(source, temporary, artifact, report, error))
        {
            std::cerr << report.ToJson() << '\n' << error << '\n';
            return 1;
        }
        snowdesktop::widget::LocalCatalogPublisher publisher(argv[3]);
        snowdesktop::widget::PublishRequest request;
        request.artifact = artifact;
        request.title = manifest.name;
        request.description = manifest.description;
        const auto result = publisher.Publish(request);
        std::error_code ec;
        std::filesystem::remove(temporary, ec);
        if (!result.ok)
        {
            std::cerr << result.error << '\n';
            return 1;
        }
        std::cout << "{\"ok\":true,\"externalItemId\":\""
            << result.externalItemId << "\"}\n";
        return 0;
    }
    PrintUsage();
    return 2;
}
