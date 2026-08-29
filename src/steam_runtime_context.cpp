#include "steam_runtime_context.h"

#include "json_value.h"

#include <windows.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>

namespace
{
using snowdesktop::deployment::RuntimeDeploymentContext;
using snowdesktop::deployment::RuntimeDeploymentKind;

bool IsSafeIdentifier(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 96)
        return false;
    for (const unsigned char character : value)
    {
        if (!std::isalnum(character) && character != '-' &&
            character != '_' && character != '.')
        {
            return false;
        }
    }
    return true;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty())
        return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), result.data(),
            required) != required)
    {
        return {};
    }
    return result;
}

const JsonValue* RequiredField(const JsonValue& object,
    std::string_view name, JsonValue::Type type) noexcept
{
    const JsonValue* value = object.Find(name);
    return value && value->type == type ? value : nullptr;
}

bool IsSafeRelativePath(const std::filesystem::path& path) noexcept
{
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.has_root_directory())
    {
        return false;
    }
    for (const auto& part : path)
    {
        if (part == L".." || part == L"." || part.empty())
            return false;
    }
    return true;
}

bool EqualPathComponent(const std::filesystem::path& left,
    std::wstring_view right) noexcept
{
    const std::wstring text = left.wstring();
    return CompareStringOrdinal(text.c_str(), static_cast<int>(text.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool IsExpectedRuntimeLocation(const std::filesystem::path& executablePath,
    const std::filesystem::path& installRoot,
    RuntimeDeploymentKind kind) noexcept
{
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(
        executablePath.parent_path(), installRoot, error);
    if (error)
        return false;

    std::vector<std::filesystem::path> parts;
    for (const auto& part : relative)
        parts.push_back(part);
    if (parts.size() != 3 || !EqualPathComponent(parts[0], L".snowdesktop"))
        return false;
    const std::wstring_view expected =
        kind == RuntimeDeploymentKind::SteamManaged ? L"runtime" : L"dev";
    return EqualPathComponent(parts[1], expected) &&
        IsSafeIdentifier(parts[2].string());
}

bool IsPathInside(const std::filesystem::path& child,
    const std::filesystem::path& parent) noexcept
{
    std::error_code error;
    const auto relative = std::filesystem::relative(child, parent, error);
    if (error || relative.empty() || relative.is_absolute())
        return false;
    for (const auto& part : relative)
    {
        if (part == L"..")
            return false;
    }
    return true;
}

RuntimeDeploymentContext Invalid(std::string message)
{
    RuntimeDeploymentContext result;
    result.kind = RuntimeDeploymentKind::Invalid;
    result.explicitContext = true;
    result.error = std::move(message);
    return result;
}

std::string WindowsError(DWORD value)
{
    const std::error_code error(
        static_cast<int>(value), std::system_category());
    return std::to_string(value) + " (" + error.message() + ")";
}
}

namespace snowdesktop::deployment
{
RuntimeDeploymentContext ResolveRuntimeDeploymentContext(
    const std::filesystem::path& executablePath, bool packaged)
{
    RuntimeDeploymentContext result;
    if (packaged)
    {
        result.kind = RuntimeDeploymentKind::Packaged;
        return result;
    }

    const std::filesystem::path sidecar =
        executablePath.parent_path() / kSteamRuntimeContextFilename;
    const std::filesystem::path sidecarParent = sidecar.parent_path();
    const DWORD parentAttributes =
        GetFileAttributesW(sidecarParent.c_str());
    if (parentAttributes == INVALID_FILE_ATTRIBUTES)
    {
        return Invalid("Steam runtime context directory cannot be inspected: " +
            WindowsError(GetLastError()));
    }
    if ((parentAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return Invalid("Steam runtime context parent is not a directory");

    const DWORD sidecarAttributes = GetFileAttributesW(sidecar.c_str());
    if (sidecarAttributes == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD probeError = GetLastError();
        // The parent was just confirmed to be an accessible directory, so a
        // missing leaf is the only conclusive absence. A disappeared parent,
        // access denial, invalid path, or any other I/O error fails closed.
        if (probeError == ERROR_FILE_NOT_FOUND)
            return result;
        return Invalid("Steam runtime context presence cannot be determined: " +
            WindowsError(probeError));
    }

    result.explicitContext = true;
    if ((sidecarAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return Invalid("Steam runtime context is not a regular file");

    std::error_code error;
    const bool regularSidecar =
        std::filesystem::is_regular_file(sidecar, error);
    if (error)
    {
        return Invalid("Steam runtime context type cannot be determined: " +
            error.message());
    }
    if (!regularSidecar)
        return Invalid("Steam runtime context is not a regular file");

    std::ifstream stream(sidecar, std::ios::binary);
    if (!stream)
        return Invalid("Steam runtime context cannot be opened");
    std::ostringstream contents;
    contents << stream.rdbuf();
    if (!stream.good() && !stream.eof())
        return Invalid("Steam runtime context cannot be read");

    JsonValue root;
    std::string parseError;
    if (!ParseJson(contents.str(), root, &parseError) || !root.IsObject())
        return Invalid("Steam runtime context is invalid JSON: " + parseError);

    const JsonValue* schema = RequiredField(
        root, "schemaVersion", JsonValue::Type::Number);
    const JsonValue* kind = RequiredField(
        root, "kind", JsonValue::Type::String);
    const JsonValue* installRootRelative = RequiredField(
        root, "installRootRelative", JsonValue::Type::String);
    const JsonValue* dataRootRelative = RequiredField(
        root, "dataRootRelative", JsonValue::Type::String);
    const JsonValue* launcherRelative = RequiredField(
        root, "launcherRelative", JsonValue::Type::String);
    if (!schema || schema->number != 1.0 || !kind ||
        !installRootRelative || !dataRootRelative || !launcherRelative)
    {
        return Invalid("Steam runtime context has missing or unsupported fields");
    }

    if (kind->string == "steam-managed")
        result.kind = RuntimeDeploymentKind::SteamManaged;
    else if (kind->string == "steam-local-dev")
        result.kind = RuntimeDeploymentKind::SteamLocalDevelopment;
    else
        return Invalid("Steam runtime context has an unknown deployment kind");

    // Both supported layouts are deliberately three levels below the stable
    // Steam root. Do not accept an arbitrary upward traversal from a sidecar.
    if (installRootRelative->string != "../../..")
        return Invalid("Steam runtime context has an invalid install root");

    const std::wstring dataText = Utf8ToWide(dataRootRelative->string);
    const std::wstring launcherText = Utf8ToWide(launcherRelative->string);
    if ((dataRootRelative->string.size() != 0 && dataText.empty()) ||
        (launcherRelative->string.size() != 0 && launcherText.empty()))
    {
        return Invalid("Steam runtime context contains invalid UTF-8 paths");
    }
    const std::filesystem::path dataRelative(dataText);
    const std::filesystem::path launcherRelativePath(launcherText);
    if (!IsSafeRelativePath(dataRelative) ||
        !IsSafeRelativePath(launcherRelativePath))
    {
        return Invalid("Steam runtime context contains an unsafe relative path");
    }

    error.clear();
    result.installRoot = std::filesystem::weakly_canonical(
        executablePath.parent_path() / L".." / L".." / L"..", error);
    if (error || result.installRoot.empty() ||
        !IsExpectedRuntimeLocation(executablePath, result.installRoot,
            result.kind))
    {
        return Invalid("Steam runtime executable is outside its expected layout");
    }

    result.dataRoot = std::filesystem::weakly_canonical(
        result.installRoot / dataRelative, error);
    if (error || !IsPathInside(result.dataRoot, result.installRoot))
        return Invalid("Steam data path escapes the install root");
    result.launcher = std::filesystem::weakly_canonical(
        result.installRoot / launcherRelativePath, error);
    if (error || !IsPathInside(result.launcher, result.installRoot) ||
        !std::filesystem::is_regular_file(result.launcher, error))
    {
        return Invalid("Steam launcher is missing or outside the install root");
    }

    if (result.kind == RuntimeDeploymentKind::SteamManaged)
    {
        if (dataRelative != L"data" ||
            launcherRelativePath != kSteamLauncherFilename)
        {
            return Invalid("Steam managed paths do not match the stable layout");
        }
    }
    else
    {
        const JsonValue* profile = RequiredField(
            root, "profileId", JsonValue::Type::String);
        if (!profile || !IsSafeIdentifier(profile->string))
            return Invalid("Steam local development profile is invalid");
        result.profileId = profile->string;
        const std::filesystem::path expected =
            std::filesystem::path(L".snowdesktop") / L"dev-data" /
            Utf8ToWide(profile->string);
        std::error_code relativeError;
        const std::filesystem::path expectedLauncher =
            std::filesystem::relative(
                executablePath, result.installRoot, relativeError);
        if (dataRelative != expected || relativeError ||
            launcherRelativePath != expectedLauncher)
        {
            return Invalid("Steam local development paths do not match the profile");
        }
    }
    return result;
}

bool IsSteamDeployment(RuntimeDeploymentKind kind) noexcept
{
    return kind == RuntimeDeploymentKind::SteamManaged ||
        kind == RuntimeDeploymentKind::SteamLocalDevelopment;
}

bool CanOwnProductionAutoStart(RuntimeDeploymentKind kind) noexcept
{
    return kind == RuntimeDeploymentKind::Portable ||
        kind == RuntimeDeploymentKind::Packaged ||
        kind == RuntimeDeploymentKind::SteamManaged;
}
}
