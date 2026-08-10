#include "widget_package.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
void PrintUsage()
{
    std::cout
        << "SnowDesktop widget package tool\n"
        << "  snowwidget --version\n"
        << "  snowwidget capabilities\n"
        << "  snowwidget inspect <package-directory>\n"
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

std::string JsonEscape(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20)
            {
                output += "\\u00";
                output.push_back(hex[character >> 4]);
                output.push_back(hex[character & 0x0f]);
            }
            else output.push_back(static_cast<char>(character));
            break;
        }
    }
    output.push_back('"');
    return output;
}

void WriteStringArray(std::ostream& output,
    const std::vector<std::string>& values)
{
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index) output << ',';
        output << JsonEscape(values[index]);
    }
    output << ']';
}
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--version")
    {
        std::cout << SNOWDESKTOP_VERSION << '\n';
        return 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"capabilities")
    {
        std::cout
            << "{\"ok\":true,\"protocolVersion\":1,\"version\":"
            << JsonEscape(SNOWDESKTOP_VERSION)
            << ",\"format\":\"snowdesktop-widget\","
               "\"authoringSkill\":{\"id\":\"snowdesktop-lua-widget\","
               "\"revision\":1},\"commands\":["
               "\"inspect\",\"validate\",\"pack\",\"publish-local\"]}"
            << '\n';
        return 0;
    }
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

    if (command == L"inspect")
    {
        if (argc != 3 || source.extension() == L".snowwidget")
        {
            std::cerr << "{\"ok\":false,\"error\":\"inspect requires an unpacked component directory\"}\n";
            return 2;
        }
        snowdesktop::widget::PackageManifest manifest;
        report = manager.ValidateDirectory(source, &manifest);
        std::cout << "{\"ok\":" << (report.Ok() ? "true" : "false")
            << ",\"manifest\":{"
            << "\"schemaVersion\":" << manifest.schemaVersion
            << ",\"apiVersion\":" << manifest.apiVersion
            << ",\"id\":" << JsonEscape(manifest.id)
            << ",\"slug\":" << JsonEscape(manifest.slug)
            << ",\"version\":" << JsonEscape(manifest.version)
            << ",\"name\":" << JsonEscape(manifest.name)
            << ",\"description\":" << JsonEscape(manifest.description)
            << ",\"author\":" << JsonEscape(manifest.author)
            << ",\"license\":" << JsonEscape(manifest.license)
            << ",\"preview\":" << JsonEscape(manifest.preview)
            << ",\"permissions\":";
        WriteStringArray(std::cout, manifest.permissions);
        std::cout << ",\"networkDomains\":";
        WriteStringArray(std::cout, manifest.networkDomains);
        std::cout << "},\"validation\":" << report.ToJson() << "}\n";
        return report.Ok() ? 0 : 1;
    }

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
        std::cout << "{\"ok\":true,\"packageId\":"
            << JsonEscape(artifact.packageId)
            << ",\"version\":" << JsonEscape(artifact.version)
            << ",\"sha256\":" << JsonEscape(artifact.sha256)
            << ",\"path\":"
            << JsonEscape(WideToUtf8(artifact.localPath.wstring())) << "}\n";
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
