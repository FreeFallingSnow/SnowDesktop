// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "package_tool.h"
#include "bridge_json.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>
#include <utility>

namespace snowdesktop::steam_bridge
{
namespace
{
constexpr std::size_t kMaximumToolOutputBytes = 16u * 1024u * 1024u;

std::filesystem::path SiblingSnowwidget()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path() / L"snowwidget.exe";
}

bool IsRegularNonReparseFile(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring output(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), output.data(), length) != length)
        return {};
    return output;
}

bool ReadPipeAvailable(HANDLE pipe, std::string& output, std::string& error)
{
    std::array<char, 8192> buffer{};
    while (true)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE) return true;
            error = "cannot read snowwidget output";
            return false;
        }
        if (available == 0) return true;
        DWORD read = 0;
        const DWORD requested = std::min<DWORD>(available,
            static_cast<DWORD>(buffer.size()));
        if (!ReadFile(pipe, buffer.data(), requested, &read, nullptr))
        {
            if (GetLastError() == ERROR_BROKEN_PIPE) return true;
            error = "cannot read snowwidget output";
            return false;
        }
        if (output.size() + read > kMaximumToolOutputBytes)
        {
            error = "snowwidget output exceeds the 16 MiB safety limit";
            return false;
        }
        output.append(buffer.data(), read);
    }
}

std::optional<std::string> RequiredString(const JsonValue& object,
    std::string_view key, std::string& error)
{
    auto value = JsonString(object, key);
    if (!value)
        error = "snowwidget JSON field is missing or invalid: " +
            std::string(key);
    return value;
}
}

PackagedWidget::~PackagedWidget()
{
    Cleanup();
}

PackagedWidget::PackagedWidget(PackagedWidget&& other) noexcept
    : temporaryDirectory(std::move(other.temporaryDirectory)),
      packagePath(std::move(other.packagePath)),
      packageId(std::move(other.packageId)),
      version(std::move(other.version)), sha256(std::move(other.sha256))
{
    other.temporaryDirectory.clear();
    other.packagePath.clear();
}

PackagedWidget& PackagedWidget::operator=(PackagedWidget&& other) noexcept
{
    if (this == &other) return *this;
    Cleanup();
    temporaryDirectory = std::move(other.temporaryDirectory);
    packagePath = std::move(other.packagePath);
    packageId = std::move(other.packageId);
    version = std::move(other.version);
    sha256 = std::move(other.sha256);
    other.temporaryDirectory.clear();
    other.packagePath.clear();
    return *this;
}

void PackagedWidget::Cleanup()
{
    if (temporaryDirectory.empty()) return;
    std::error_code ec;
    if (!packagePath.empty()) std::filesystem::remove(packagePath, ec);
    ec.clear();
    std::filesystem::remove(temporaryDirectory, ec);
    temporaryDirectory.clear();
    packagePath.clear();
}

PackageTool::PackageTool(std::filesystem::path executable,
    std::filesystem::path stagingRoot)
    : executable_(executable.empty() ? SiblingSnowwidget() :
        std::move(executable)),
      stagingRoot_(stagingRoot.empty()
        ? executable_.parent_path() / L"data" / L"SteamWorkshopManager" /
            L"staging" / L"packages"
        : std::move(stagingRoot))
{
}

std::wstring QuoteWindowsArgument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") ==
        std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'"')
        {
            output.append(slashes * 2 + 1, L'\\');
            output.push_back(L'"');
            slashes = 0;
            continue;
        }
        output.append(slashes, L'\\');
        slashes = 0;
        output.push_back(character);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

bool PackageTool::Run(const std::vector<std::wstring>& arguments,
    std::string& output, std::string& error, unsigned timeoutMs) const
{
    output.clear();
    if (!IsRegularNonReparseFile(executable_))
    {
        error = "snowwidget.exe is missing or is a reparse point: " +
            executable_.string();
        return false;
    }
    SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
    HANDLE readPipeRaw = nullptr;
    HANDLE writePipeRaw = nullptr;
    if (!CreatePipe(&readPipeRaw, &writePipeRaw, &security, 0))
    {
        error = "cannot create the snowwidget output pipe";
        return false;
    }
    const auto close = [](HANDLE value)
    { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); };
    if (!SetHandleInformation(readPipeRaw, HANDLE_FLAG_INHERIT, 0))
    {
        close(readPipeRaw);
        close(writePipeRaw);
        error = "cannot protect the snowwidget output pipe";
        return false;
    }
    std::wstring commandLine = QuoteWindowsArgument(executable_.wstring());
    for (const auto& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += QuoteWindowsArgument(argument);
    }
    STARTUPINFOW startup{ sizeof(startup) };
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipeRaw;
    startup.hStdError = writePipeRaw;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(executable_.c_str(),
        mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
        &startup, &process);
    close(writePipeRaw);
    writePipeRaw = nullptr;
    if (!created)
    {
        close(readPipeRaw);
        error = "cannot start snowwidget.exe";
        return false;
    }
    const auto deadline = GetTickCount64() + timeoutMs;
    bool readOk = true;
    while (WaitForSingleObject(process.hProcess, 20) == WAIT_TIMEOUT)
    {
        if (!ReadPipeAvailable(readPipeRaw, output, error))
        {
            readOk = false;
            TerminateProcess(process.hProcess, 1);
            break;
        }
        if (GetTickCount64() >= deadline)
        {
            error = "snowwidget operation timed out";
            TerminateProcess(process.hProcess, 1);
            readOk = false;
            break;
        }
    }
    WaitForSingleObject(process.hProcess, 5000);
    if (readOk) readOk = ReadPipeAvailable(readPipeRaw, output, error);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    close(readPipeRaw);
    close(process.hThread);
    close(process.hProcess);
    if (!readOk) return false;
    if (exitCode != 0)
    {
        error = output.empty() ? "snowwidget failed" : output;
        return false;
    }
    return true;
}

bool PackageTool::Inspect(const std::filesystem::path& source,
    WidgetInspection& inspection, std::string& error) const
{
    inspection = {};
    std::string output;
    if (!Run({ L"inspect", source.wstring() }, output, error, 60000))
        return false;
    JsonValue root;
    if (!ParseJson(output, root, error) || !root.IsObject())
    {
        if (error.empty()) error = "snowwidget inspect returned malformed JSON";
        return false;
    }
    const auto ok = JsonBoolean(root, "ok");
    const JsonValue* manifest = root.Find("manifest");
    const JsonValue* validation = root.Find("validation");
    if (!ok || !manifest || !manifest->IsObject() || !validation)
    {
        error = "snowwidget inspect returned an incomplete JSON result";
        return false;
    }
    inspection.valid = *ok;
    inspection.validationJson = WriteJson(*validation, -1);
    auto id = RequiredString(*manifest, "id", error);
    auto slug = RequiredString(*manifest, "slug", error);
    auto version = RequiredString(*manifest, "version", error);
    auto name = RequiredString(*manifest, "name", error);
    auto description = RequiredString(*manifest, "description", error);
    auto author = RequiredString(*manifest, "author", error);
    auto license = RequiredString(*manifest, "license", error);
    auto preview = RequiredString(*manifest, "preview", error);
    if (!id || !slug || !version || !name || !description || !author ||
        !license || !preview)
        return false;
    inspection.packageId = *id;
    inspection.slug = *slug;
    inspection.version = *version;
    inspection.name = *name;
    inspection.description = *description;
    inspection.author = *author;
    inspection.license = *license;
    if (!preview->empty()) inspection.preview = source / Utf8ToWide(*preview);
    const auto copyArray = [&](std::string_view key,
        std::vector<std::string>& destination)
    {
        const JsonValue* values = manifest->Find(key);
        if (!values || !values->IsArray()) return false;
        for (const auto& value : values->array)
        {
            if (!value.IsString()) return false;
            destination.push_back(value.string);
        }
        return true;
    };
    if (!copyArray("permissions", inspection.permissions) ||
        !copyArray("networkDomains", inspection.networkDomains))
    {
        error = "snowwidget inspect returned an invalid manifest array";
        return false;
    }
    if (const JsonValue* locales = manifest->Find("locales"))
    {
        if (!locales->IsObject())
        {
            error = "snowwidget inspect returned an invalid locales object";
            return false;
        }
        for (const auto& [locale, value] : locales->object)
        {
            if (!value.IsObject())
            {
                error = "snowwidget inspect returned invalid locale metadata";
                return false;
            }
            const auto title = JsonString(value, "title");
            const auto localizedDescription =
                JsonString(value, "description");
            if (!title || !localizedDescription)
            {
                error = "snowwidget inspect returned incomplete locale metadata";
                return false;
            }
            inspection.localizations.push_back(
                WidgetLocalization{ locale, *title,
                    *localizedDescription });
        }
    }
    if (!inspection.valid)
    {
        error = inspection.validationJson;
        return false;
    }
    return true;
}

bool PackageTool::Pack(const std::filesystem::path& source,
    const WidgetInspection& expected, PackagedWidget& package,
    std::string& error) const
{
    package.Cleanup();
    std::error_code ec;
    std::filesystem::create_directories(stagingRoot_, ec);
    if (ec || stagingRoot_.empty())
    {
        error = "cannot prepare the data package staging directory";
        return false;
    }
    const DWORD stagingAttributes = GetFileAttributesW(stagingRoot_.c_str());
    if (stagingAttributes == INVALID_FILE_ATTRIBUTES ||
        (stagingAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (stagingAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        error = "the data package staging directory is unsafe";
        return false;
    }
    package.temporaryDirectory = stagingRoot_ /
        (L"package-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(std::chrono::steady_clock::now()
             .time_since_epoch().count()));
    std::filesystem::create_directory(package.temporaryDirectory, ec);
    if (ec)
    {
        package.temporaryDirectory.clear();
        error = "cannot create the unique package directory";
        return false;
    }
    package.packagePath = package.temporaryDirectory / L"package.snowwidget";
    std::string output;
    if (!Run({ L"pack", source.wstring(), package.packagePath.wstring() },
            output, error, 120000))
    {
        package.Cleanup();
        return false;
    }
    JsonValue root;
    if (!ParseJson(output, root, error) || !root.IsObject() ||
        JsonBoolean(root, "ok") != true)
    {
        if (error.empty()) error = "snowwidget pack returned malformed JSON";
        package.Cleanup();
        return false;
    }
    auto id = RequiredString(root, "packageId", error);
    auto version = RequiredString(root, "version", error);
    auto sha256 = RequiredString(root, "sha256", error);
    if (!id || !version || !sha256 || *id != expected.packageId ||
        *version != expected.version || sha256->size() != 64 ||
        !std::filesystem::is_regular_file(package.packagePath, ec) || ec)
    {
        if (error.empty()) error = "packed artifact does not match inspection";
        package.Cleanup();
        return false;
    }
    package.packageId = *id;
    package.version = *version;
    package.sha256 = *sha256;
    return true;
}
}
