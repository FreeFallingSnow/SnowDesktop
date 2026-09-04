#include "widget_package.h"
#include "widget_author_lint.h"
#include "widget_author_permissions.h"
#include "widget_author_test.h"
#include "widget_api_contract_json.h"
#include "widget_system_contract_json.h"
#include "widget_view_contract_json.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{
void PrintUsage()
{
    std::cout
        << "SnowDesktop widget package tool\n"
        << "  snowwidget --version\n"
        << "  snowwidget capabilities\n"
        << "  snowwidget api-contract\n"
        << "  snowwidget system-contract\n"
        << "  snowwidget view-contract\n"
        << "  snowwidget inspect <package-directory>\n"
        << "  snowwidget lint <package-directory>\n"
        << "  snowwidget quality <package-directory>\n"
        << "  snowwidget test <package-directory>\n"
        << "  snowwidget permissions <package-directory>\n"
        << "  snowwidget preview <package-directory> <output.png>"
           " [--columns N] [--rows N] [--dpi N]"
           " [--canvas-size N] [--padding N]"
           " [--locale CODE]"
           " [--appearance dark|light|glass-dark|glass-light|acrylic-dark|acrylic-light]"
           " [--theme dark|light]"
           " [--data-state ready|empty|loading|error|stale|permission-denied]"
           " [--background image-file]"
           " [--storage key=value] [--host SnowDesktop.exe]\n"
        << "  snowwidget preview-native <collection|collection-group|file-group|file-categories|folder-mapping|all> <output-directory>"
           " [--dpi N] [--locale CODE]"
           " [--appearance dark|light|glass-dark|glass-light|acrylic-dark|acrylic-light]"
           " [--background image-file]"
           " [--transparent]"
           " [--canvas-width N] [--canvas-height N] [--padding N]"
           " [--host SnowDesktop.exe]\n"
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

std::filesystem::path CurrentExecutablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1)
            return std::filesystem::path(
                std::wstring(buffer.data(), length));
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

std::optional<std::filesystem::path> ToolOwnedDirectory(
    std::wstring_view name)
{
    const auto executable = CurrentExecutablePath();
    if (executable.empty()) return std::nullopt;
    const auto directory = executable.parent_path() / L"data" /
        L"snowwidget" / std::wstring(name);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return std::nullopt;
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return std::nullopt;
    return directory;
}

std::optional<std::filesystem::path> CreateOwnedTransactionDirectory(
    std::wstring_view area, std::wstring_view prefix)
{
    const auto root = ToolOwnedDirectory(area);
    if (!root) return std::nullopt;
    for (unsigned int attempt = 0; attempt < 100; ++attempt)
    {
        const auto candidate = *root /
            (std::wstring(prefix) + L"-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" +
                std::to_wstring(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error))
            return candidate;
        if (error && error != std::errc::file_exists)
            return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> FindPreviewHost(
    const std::filesystem::path& explicitHost)
{
    std::error_code error;
    const auto accept = [&](const std::filesystem::path& candidate)
        -> std::optional<std::filesystem::path> {
        if (candidate.empty() ||
            !std::filesystem::is_regular_file(candidate, error))
        {
            error.clear();
            return std::nullopt;
        }
        return std::filesystem::weakly_canonical(candidate, error);
    };
    if (!explicitHost.empty()) return accept(explicitHost);

    wchar_t environmentHost[32768]{};
    const DWORD environmentLength = GetEnvironmentVariableW(
        L"SNOWDESKTOP_HOST", environmentHost,
        static_cast<DWORD>(std::size(environmentHost)));
    if (environmentLength > 0 && environmentLength < std::size(environmentHost))
        if (auto host = accept(environmentHost)) return host;

    const auto executable = CurrentExecutablePath();
    if (!executable.empty())
    {
        const auto bin = executable.parent_path();
        if (auto host = accept(bin / L"SnowDesktop.exe")) return host;
        auto bundledRoot = bin;
        for (int level = 0; level < 3 && bundledRoot.has_parent_path(); ++level)
            bundledRoot = bundledRoot.parent_path();
        if (auto host = accept(bundledRoot / L"SnowDesktop.exe")) return host;
    }

    wchar_t searched[32768]{};
    const DWORD searchedLength = SearchPathW(nullptr, L"SnowDesktop.exe",
        nullptr, static_cast<DWORD>(std::size(searched)), searched, nullptr);
    if (searchedLength > 0 && searchedLength < std::size(searched))
        return accept(searched);
    return std::nullopt;
}

std::wstring QuoteWindowsArgument(std::wstring_view argument)
{
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool ParseInteger(std::wstring_view text, int minimum,
    int maximum, int& output)
{
    if (text.empty()) return false;
    const std::wstring owned(text);
    wchar_t* end = nullptr;
    errno = 0;
    const long value = std::wcstol(owned.c_str(), &end, 10);
    if (errno != 0 || !end || *end != L'\0' ||
        value < minimum || value > maximum)
        return false;
    output = static_cast<int>(value);
    return true;
}

std::optional<std::filesystem::path> CreateResultPath()
{
    const auto directory = ToolOwnedDirectory(L"preview-results");
    if (!directory) return std::nullopt;
    for (unsigned int attempt = 0; attempt < 100; ++attempt)
    {
        const auto candidate = *directory /
            (L"result-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" +
                std::to_wstring(attempt) + L".json");
        HANDLE file = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            CloseHandle(file);
            return candidate;
        }
        const DWORD createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS &&
            createError != ERROR_ALREADY_EXISTS)
            return std::nullopt;
    }
    return std::nullopt;
}

std::string ReadUtf8File(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), {});
}

int RunPreviewHost(const std::filesystem::path& host,
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    int columns, int rows, int dpi,
    std::wstring_view locale,
    std::wstring_view theme,
    std::wstring_view appearance,
    std::wstring_view dataState,
    const std::filesystem::path& backgroundImage,
    int canvasSize, int padding,
    const std::vector<std::wstring>& storage)
{
    const auto resultPath = CreateResultPath();
    if (!resultPath)
    {
        std::cerr << "{\"ok\":false,\"stage\":\"host.result\","
            "\"error\":\"cannot create the preview result file\"}\n";
        return 1;
    }
    std::wstring commandLine = QuoteWindowsArgument(host.wstring());
    const auto append = [&](std::wstring_view value) {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArgument(value);
    };
    append(L"--widget-author-preview");
    append(source.wstring());
    append(output.wstring());
    append(std::to_wstring(columns));
    append(std::to_wstring(rows));
    append(std::to_wstring(dpi));
    append(resultPath->wstring());
    append(locale);
    append(theme);
    append(appearance);
    append(dataState);
    append(backgroundImage.wstring());
    if (canvasSize > 0)
        append(L"@preview.canvasSize=" + std::to_wstring(canvasSize));
    if (padding > 0)
        append(L"@preview.padding=" + std::to_wstring(padding));
    for (const auto& pair : storage) append(pair);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const BOOL launched = CreateProcessW(host.c_str(),
        mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!launched)
    {
        std::error_code removeError;
        std::filesystem::remove(*resultPath, removeError);
        std::cerr << "{\"ok\":false,\"stage\":\"host.launch\","
            "\"error\":\"cannot launch SnowDesktop preview host (Windows error "
            << GetLastError() << ")\"}\n";
        return 1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 120000);
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    const std::string json = ReadUtf8File(*resultPath);
    std::error_code removeError;
    std::filesystem::remove(*resultPath, removeError);
    if (!json.empty())
        std::cout << json << (json.back() == '\n' ? "" : "\n");
    else
        std::cerr << "{\"ok\":false,\"stage\":\"host.result\","
            "\"error\":\"preview host exited without a result\","
            "\"exitCode\":" << exitCode << "}\n";
    return wait == WAIT_OBJECT_0 && exitCode == 0 && !json.empty()
        ? 0 : 1;
}

int RunNativePreviewHost(const std::filesystem::path& host,
    std::wstring_view component,
    const std::filesystem::path& outputDirectory,
    int dpi, std::wstring_view locale, std::wstring_view appearance,
    const std::filesystem::path& backgroundImage,
    int canvasWidth, int canvasHeight, int padding, bool transparent)
{
    const auto resultPath = CreateResultPath();
    if (!resultPath)
    {
        std::cerr << "{\"ok\":false,\"stage\":\"host.result\","
            "\"error\":\"cannot create the native preview result file\"}\n";
        return 1;
    }
    std::wstring commandLine = QuoteWindowsArgument(host.wstring());
    const auto append = [&](std::wstring_view value) {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArgument(value);
    };
    append(L"--native-component-preview");
    append(component);
    append(outputDirectory.wstring());
    append(std::to_wstring(dpi));
    append(locale);
    append(appearance);
    append(backgroundImage.wstring());
    append(std::to_wstring(canvasWidth));
    append(std::to_wstring(canvasHeight));
    append(std::to_wstring(padding));
    append(transparent ? L"1" : L"0");
    append(resultPath->wstring());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const BOOL launched = CreateProcessW(host.c_str(),
        mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!launched)
    {
        std::error_code removeError;
        std::filesystem::remove(*resultPath, removeError);
        std::cerr << "{\"ok\":false,\"stage\":\"host.launch\","
            "\"error\":\"cannot launch SnowDesktop native preview host "
            "(Windows error " << GetLastError() << ")\"}\n";
        return 1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 120000);
    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    const std::string json = ReadUtf8File(*resultPath);
    std::error_code removeError;
    std::filesystem::remove(*resultPath, removeError);
    if (!json.empty())
        std::cout << json << (json.back() == '\n' ? "" : "\n");
    else
        std::cerr << "{\"ok\":false,\"stage\":\"host.result\","
            "\"error\":\"native preview host exited without a result\","
            "\"exitCode\":" << exitCode << "}\n";
    return wait == WAIT_OBJECT_0 && exitCode == 0 && !json.empty()
        ? 0 : 1;
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
            << "{\"ok\":true,\"protocolVersion\":2,\"version\":"
            << JsonEscape(SNOWDESKTOP_VERSION)
            << ",\"format\":\"snowdesktop-widget\","
               "\"authoringSkill\":{\"id\":\"snowdesktop-lua-widget\","
               "\"revision\":"
            << SNOWDESKTOP_WIDGET_SKILL_REVISION
            << "},\"recommendedSchemaVersion\":2,"
               "\"recommendedApiVersion\":2,"
               "\"executableSchemaVersions\":[2],"
               "\"executableApiVersions\":[2],\"commands\":["
               "\"api-contract\",\"system-contract\",\"view-contract\",\"inspect\","
               "\"lint\",\"quality\",\"test\",\"preview\",\"permissions\","
               "\"preview-native\",\"validate\",\"pack\",\"publish-local\"]}"
            << '\n';
        return 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"api-contract")
    {
        std::cout << snowdesktop::widget_api::
            SerializePublicApiContractJson() << '\n';
        return 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"system-contract")
    {
        std::cout
            << snowdesktop::widget_api::
                SerializeSystemCapabilityContractJson()
            << '\n';
        return 0;
    }
    if (argc == 2 && std::wstring_view(argv[1]) == L"view-contract")
    {
        std::cout
            << snowdesktop::widget_runtime::SerializeViewContractJson()
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
    if (const auto staging = ToolOwnedDirectory(L"package-staging"))
        paths.staging = *staging;
    snowdesktop::widget::WidgetPackageManager manager(paths);
    snowdesktop::widget::ValidationReport report;
    snowdesktop::widget::PackageArtifact artifact;
    std::string error;

    if (command == L"preview-native")
    {
        const std::wstring_view component = argc >= 3
            ? std::wstring_view(argv[2]) : std::wstring_view{};
        if (argc < 4 ||
            (component != L"collection" &&
             component != L"collection-group" &&
             component != L"file-group" &&
             component != L"file-categories" &&
             component != L"folder-mapping" && component != L"all"))
        {
            std::cerr << "{\"ok\":false,\"error\":\"preview-native requires a supported component or all and an output directory\"}\n";
            return 2;
        }
        const std::filesystem::path outputDirectory = argv[3];
        int dpi = 144;
        int canvasWidth = 1280;
        int canvasHeight = 720;
        int padding = 72;
        std::wstring locale = L"en-US";
        std::wstring appearance = L"dark";
        std::filesystem::path backgroundImage;
        std::filesystem::path explicitHost;
        bool transparent = false;
        for (int index = 4; index < argc; ++index)
        {
            const std::wstring_view option(argv[index]);
            if ((option == L"--dpi" || option == L"--locale" ||
                    option == L"--appearance" ||
                    option == L"--background" ||
                    option == L"--canvas-width" ||
                    option == L"--canvas-height" ||
                    option == L"--padding" || option == L"--host") &&
                index + 1 >= argc)
            {
                std::cerr << "{\"ok\":false,\"error\":\"preview-native option is missing its value\"}\n";
                return 2;
            }
            if (option == L"--dpi")
            {
                if (!ParseInteger(argv[++index], 96, 480, dpi))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"DPI must be an integer from 96 to 480\"}\n";
                    return 2;
                }
            }
            else if (option == L"--locale")
            {
                locale = argv[++index];
                if (locale.empty() || locale.size() > 35)
                {
                    std::cerr << "{\"ok\":false,\"error\":\"locale must contain 1 to 35 characters\"}\n";
                    return 2;
                }
            }
            else if (option == L"--appearance")
            {
                appearance = argv[++index];
                if (appearance != L"dark" && appearance != L"light" &&
                    appearance != L"glass-dark" &&
                    appearance != L"glass-light" &&
                    appearance != L"acrylic-dark" &&
                    appearance != L"acrylic-light")
                {
                    std::cerr << "{\"ok\":false,\"error\":\"appearance must be dark, light, glass-dark, glass-light, acrylic-dark, or acrylic-light\"}\n";
                    return 2;
                }
            }
            else if (option == L"--background")
                backgroundImage = argv[++index];
            else if (option == L"--transparent")
                transparent = true;
            else if (option == L"--canvas-width")
            {
                if (!ParseInteger(argv[++index], 320, 8192, canvasWidth))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"canvas width must be an integer from 320 to 8192\"}\n";
                    return 2;
                }
            }
            else if (option == L"--canvas-height")
            {
                if (!ParseInteger(argv[++index], 240, 8192, canvasHeight))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"canvas height must be an integer from 240 to 8192\"}\n";
                    return 2;
                }
            }
            else if (option == L"--padding")
            {
                if (!ParseInteger(argv[++index], 0, 4096, padding))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"padding must be an integer from 0 to 4096\"}\n";
                    return 2;
                }
            }
            else if (option == L"--host")
                explicitHost = argv[++index];
            else
            {
                std::cerr << "{\"ok\":false,\"error\":\"unknown preview-native option\"}\n";
                return 2;
            }
        }
        if (padding * 2 >= canvasWidth || padding * 2 >= canvasHeight)
        {
            std::cerr << "{\"ok\":false,\"error\":\"padding must leave a positive canvas content area\"}\n";
            return 2;
        }
        if (transparent && !backgroundImage.empty())
        {
            std::cerr << "{\"ok\":false,\"error\":\"--transparent cannot be combined with --background\"}\n";
            return 2;
        }
        const auto host = FindPreviewHost(explicitHost);
        if (!host)
        {
            std::cerr << "{\"ok\":false,\"stage\":\"host.locate\","
                "\"error\":\"SnowDesktop.exe was not found; pass --host or set SNOWDESKTOP_HOST\"}\n";
            return 1;
        }
        return RunNativePreviewHost(*host, argv[2], outputDirectory,
            dpi, locale, appearance, backgroundImage,
            canvasWidth, canvasHeight, padding, transparent);
    }

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
        std::vector<std::pair<std::string,
            const snowdesktop::widget::LocalizedMetadata*>> locales;
        locales.reserve(manifest.locales.size());
        for (const auto& [locale, localized] : manifest.locales)
            locales.emplace_back(locale, &localized);
        std::sort(locales.begin(), locales.end(),
            [](const auto& left, const auto& right)
            { return left.first < right.first; });
        std::cout << ",\"locales\":{";
        for (std::size_t index = 0; index < locales.size(); ++index)
        {
            if (index) std::cout << ',';
            std::cout << JsonEscape(locales[index].first)
                << ":{\"title\":"
                << JsonEscape(locales[index].second->title)
                << ",\"description\":"
                << JsonEscape(locales[index].second->description) << '}';
        }
        std::cout << '}';
        std::cout << "},\"migration\":{\"required\":"
            << (manifest.schemaVersion < 2 || manifest.apiVersion < 2
                ? "true" : "false")
            << ",\"targetSchemaVersion\":2,\"targetApiVersion\":2,"
               "\"entryContract\":\"return widget.define({...})\"},"
               "\"validation\":" << report.ToJson() << "}\n";
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
    if (command == L"lint")
    {
        if (argc != 3 || source.extension() == L".snowwidget")
        {
            std::cerr << "{\"ok\":false,\"error\":\"lint requires an unpacked component directory\"}\n";
            return 2;
        }
        snowdesktop::widget::PackageManifest manifest;
        report = manager.ValidateDirectory(source, &manifest);
        if (!report.Ok())
        {
            std::cout << "{\"ok\":false,\"validation\":"
                << report.ToJson() << ",\"lint\":null}\n";
            return 1;
        }
        const auto lint = snowdesktop::widget_authoring::
            LintWidgetDirectory(source, manifest);
        std::cout << lint.ToJson() << '\n';
        return lint.Ok() ? 0 : 1;
    }
    if (command == L"quality")
    {
        if (argc != 3 || source.extension() == L".snowwidget")
        {
            std::cerr << "{\"ok\":false,\"error\":\"quality requires an unpacked component directory\"}\n";
            return 2;
        }
        snowdesktop::widget::PackageManifest manifest;
        report = manager.ValidateDirectory(source, &manifest);
        if (!report.Ok())
        {
            std::cout << "{\"ok\":false,\"validation\":"
                << report.ToJson() << ",\"lint\":null}\n";
            return 1;
        }
        const auto lint = snowdesktop::widget_authoring::
            LintWidgetDirectory(source, manifest);
        const bool qualityOk = lint.Ok() && lint.WarningCount() == 0;
        std::cout << "{\"ok\":" << (qualityOk ? "true" : "false")
            << ",\"validation\":" << report.ToJson()
            << ",\"lint\":" << lint.ToJson() << "}\n";
        return qualityOk ? 0 : 1;
    }
    if (command == L"test")
    {
        if (argc != 3 || source.extension() == L".snowwidget")
        {
            std::cerr << "{\"ok\":false,\"error\":\"test requires an unpacked component directory\"}\n";
            return 2;
        }
        report = manager.ValidateDirectory(source);
        if (!report.Ok())
        {
            std::cout << "{\"ok\":false,\"validation\":"
                << report.ToJson() << ",\"tests\":null}\n";
            return 1;
        }
        const auto tests = snowdesktop::widget_authoring::
            RunWidgetTests(source);
        std::cout << tests.ToJson() << '\n';
        return tests.Ok() ? 0 : 1;
    }
    if (command == L"permissions")
    {
        if (argc != 3 || source.extension() == L".snowwidget")
        {
            std::cerr << "{\"ok\":false,\"error\":\"permissions requires an unpacked component directory\"}\n";
            return 2;
        }
        snowdesktop::widget::PackageManifest manifest;
        report = manager.ValidateDirectory(source, &manifest);
        if (!report.Ok())
        {
            std::cout << "{\"ok\":false,\"validation\":"
                << report.ToJson() << "}\n";
            return 1;
        }
        const auto permissionReport = snowdesktop::widget_authoring::
            BuildPermissionReport(manifest);
        std::cout << permissionReport.json << '\n';
        return permissionReport.ok ? 0 : 1;
    }
    if (command == L"preview")
    {
        if (argc < 4 || source.extension() == L".snowwidget")
        {
            std::cerr << "{\"ok\":false,\"error\":\"preview requires an unpacked component directory and output PNG\"}\n";
            return 2;
        }
        const std::filesystem::path output = argv[3];
        std::wstring extension = output.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](wchar_t character) { return static_cast<wchar_t>(
                std::towlower(character)); });
        if (extension != L".png")
        {
            std::cerr << "{\"ok\":false,\"error\":\"preview output must use the .png extension\"}\n";
            return 2;
        }
        snowdesktop::widget::PackageManifest manifest;
        report = manager.ValidateDirectory(source, &manifest);
        if (!report.Ok())
        {
            std::cout << "{\"ok\":false,\"stage\":\"package.validate\",\"validation\":"
                << report.ToJson() << "}\n";
            return 1;
        }
        int columns = manifest.defaultColumns;
        int rows = manifest.defaultRows;
        int dpi = 96;
        int canvasSize = 0;
        int padding = 0;
        std::wstring locale = L"en-US";
        std::wstring theme = L"dark";
        std::wstring appearance = L"dark";
        std::wstring dataState = L"ready";
        std::filesystem::path backgroundImage;
        bool themeSpecified = false;
        bool appearanceSpecified = false;
        std::filesystem::path explicitHost;
        std::vector<std::wstring> storage;
        for (int index = 4; index < argc; ++index)
        {
            const std::wstring_view option(argv[index]);
            if ((option == L"--columns" || option == L"--rows" ||
                    option == L"--dpi" || option == L"--canvas-size" ||
                    option == L"--padding" || option == L"--storage" ||
                    option == L"--host" || option == L"--locale" ||
                    option == L"--theme" || option == L"--appearance" ||
                    option == L"--data-state" ||
                    option == L"--background") &&
                index + 1 >= argc)
            {
                std::cerr << "{\"ok\":false,\"error\":\"preview option is missing its value\"}\n";
                return 2;
            }
            if (option == L"--columns")
            {
                if (!ParseInteger(argv[++index], 1, 8, columns))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"columns must be an integer from 1 to 8\"}\n";
                    return 2;
                }
            }
            else if (option == L"--rows")
            {
                if (!ParseInteger(argv[++index], 1, 8, rows))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"rows must be an integer from 1 to 8\"}\n";
                    return 2;
                }
            }
            else if (option == L"--dpi")
            {
                if (!ParseInteger(argv[++index], 96, 480, dpi))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"DPI must be an integer from 96 to 480\"}\n";
                    return 2;
                }
            }
            else if (option == L"--canvas-size")
            {
                if (!ParseInteger(argv[++index], 64, 8192, canvasSize))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"canvas size must be an integer from 64 to 8192\"}\n";
                    return 2;
                }
            }
            else if (option == L"--padding")
            {
                if (!ParseInteger(argv[++index], 0, 4096, padding))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"padding must be an integer from 0 to 4096\"}\n";
                    return 2;
                }
            }
            else if (option == L"--storage")
            {
                const std::wstring pair = argv[++index];
                if (pair.find(L'=') == std::wstring::npos ||
                    pair.starts_with(L"="))
                {
                    std::cerr << "{\"ok\":false,\"error\":\"storage must use key=value\"}\n";
                    return 2;
                }
                storage.push_back(pair);
            }
            else if (option == L"--locale")
            {
                locale = argv[++index];
                if (locale.empty() || locale.size() > 35)
                {
                    std::cerr << "{\"ok\":false,\"error\":\"locale must contain 1 to 35 characters\"}\n";
                    return 2;
                }
            }
            else if (option == L"--theme")
            {
                if (appearanceSpecified)
                {
                    std::cerr << "{\"ok\":false,\"error\":\"theme and appearance cannot be used together\"}\n";
                    return 2;
                }
                themeSpecified = true;
                theme = argv[++index];
                if (theme != L"dark" && theme != L"light")
                {
                    std::cerr << "{\"ok\":false,\"error\":\"theme must be dark or light\"}\n";
                    return 2;
                }
                appearance = theme;
            }
            else if (option == L"--appearance")
            {
                if (themeSpecified)
                {
                    std::cerr << "{\"ok\":false,\"error\":\"theme and appearance cannot be used together\"}\n";
                    return 2;
                }
                appearanceSpecified = true;
                appearance = argv[++index];
                if (appearance != L"dark" && appearance != L"light" &&
                    appearance != L"glass-dark" &&
                    appearance != L"glass-light" &&
                    appearance != L"acrylic-dark" &&
                    appearance != L"acrylic-light")
                {
                    std::cerr << "{\"ok\":false,\"error\":\"appearance must be dark, light, glass-dark, glass-light, acrylic-dark, or acrylic-light\"}\n";
                    return 2;
                }
                theme = appearance.ends_with(L"light")
                    ? L"light" : L"dark";
            }
            else if (option == L"--data-state")
            {
                dataState = argv[++index];
                if (dataState != L"ready" && dataState != L"empty" &&
                    dataState != L"loading" && dataState != L"error" &&
                    dataState != L"stale" &&
                    dataState != L"permission-denied")
                {
                    std::cerr << "{\"ok\":false,\"error\":\"data state must be ready, empty, loading, error, stale, or permission-denied\"}\n";
                    return 2;
                }
            }
            else if (option == L"--host")
                explicitHost = argv[++index];
            else if (option == L"--background")
                backgroundImage = argv[++index];
            else
            {
                std::cerr << "{\"ok\":false,\"error\":\"unknown preview option\"}\n";
                return 2;
            }
        }
        if (columns < manifest.minColumns || rows < manifest.minRows ||
            (manifest.maxColumns > 0 && columns > manifest.maxColumns) ||
            (manifest.maxRows > 0 && rows > manifest.maxRows))
        {
            std::cerr << "{\"ok\":false,\"stage\":\"request.size\",\"error\":\"preview size is outside the component manifest bounds\"}\n";
            return 2;
        }
        if (padding > 0 && canvasSize == 0)
        {
            std::cerr << "{\"ok\":false,\"error\":\"padding requires canvas-size\"}\n";
            return 2;
        }
        if (canvasSize > 0 && padding * 2 >= canvasSize)
        {
            std::cerr << "{\"ok\":false,\"error\":\"padding must leave a positive canvas content area\"}\n";
            return 2;
        }
        const auto host = FindPreviewHost(explicitHost);
        if (!host)
        {
            std::cerr << "{\"ok\":false,\"stage\":\"host.locate\","
                "\"error\":\"SnowDesktop.exe was not found; pass --host or set SNOWDESKTOP_HOST\"}\n";
            return 1;
        }
        return RunPreviewHost(*host, source, output,
            columns, rows, dpi, locale, theme, appearance,
            dataState, backgroundImage, canvasSize, padding, storage);
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
        const auto transaction = CreateOwnedTransactionDirectory(
            L"publish-staging", L"publish");
        if (!transaction)
        {
            std::cerr << "cannot create the data staging directory\n";
            return 1;
        }
        const auto temporary = *transaction / L"package.snowwidget";
        if (!manager.ExportDirectory(source, temporary, artifact, report, error))
        {
            std::error_code cleanupError;
            std::filesystem::remove_all(*transaction, cleanupError);
            std::cerr << report.ToJson() << '\n' << error << '\n';
            return 1;
        }
        snowdesktop::widget::LocalCatalogPublisher publisher(
            argv[3], paths.staging);
        snowdesktop::widget::PublishRequest request;
        request.artifact = artifact;
        request.title = manifest.name;
        request.description = manifest.description;
        const auto result = publisher.Publish(request);
        std::error_code ec;
        std::filesystem::remove_all(*transaction, ec);
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
