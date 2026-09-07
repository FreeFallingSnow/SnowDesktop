#include "shortcut_application_rules.h"
#include "shortcut_icon_resource.h"
#include "large_icon_steam.h"

#include <windows.h>

#include <filesystem>
#include <iostream>

namespace rules =
    snowdesktop::shortcut_application_rules;

namespace
{
int failures = 0;

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        wchar_t temporaryPath[MAX_PATH]{};
        wchar_t temporaryFile[MAX_PATH]{};
        if (GetTempPathW(static_cast<DWORD>(std::size(temporaryPath)),
                temporaryPath) == 0 ||
            GetTempFileNameW(temporaryPath, L"sdi", 0, temporaryFile) == 0)
            return;
        DeleteFileW(temporaryFile);
        if (CreateDirectoryW(temporaryFile, nullptr))
            path_ = temporaryFile;
    }

    ~TemporaryDirectory()
    {
        if (!path_.empty())
        {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    const std::filesystem::path& Path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void CheckInternetShortcutIconResource()
{
    TemporaryDirectory temporary;
    Check(!temporary.Path().empty(),
        "a temporary directory must be available for Internet shortcut tests");
    if (temporary.Path().empty())
        return;

    const auto shortcutPath = temporary.Path() / L"Website.url";
    Check(WritePrivateProfileStringW(L"InternetShortcut", L"URL",
              L"https://example.com", shortcutPath.c_str()) != FALSE &&
            WritePrivateProfileStringW(L"InternetShortcut", L"IconFile",
              L"icons\\website.ico", shortcutPath.c_str()) != FALSE &&
            WritePrivateProfileStringW(L"InternetShortcut", L"IconIndex",
              L"-7", shortcutPath.c_str()) != FALSE,
        "an Internet shortcut fixture must be writable");

    const auto resource = snowdesktop::shortcut_icon_resource::
        ReadInternetShortcutIconResource(shortcutPath.wstring());
    const std::wstring expectedPath =
        (temporary.Path() / L"icons" / L"website.ico").wstring();
    Check(resource && resource->path == expectedPath && resource->index == -7,
        "Internet shortcuts must resolve their raw IconFile and IconIndex");

    const auto missingPath = temporary.Path() / L"MissingIcon.url";
    WritePrivateProfileStringW(L"InternetShortcut", L"URL",
        L"https://example.com", missingPath.c_str());
    Check(!snowdesktop::shortcut_icon_resource::
              ReadInternetShortcutIconResource(missingPath.wstring()),
        "Internet shortcuts without IconFile must use the Shell fallback");
}
} // namespace

int main()
{
    namespace steam = snowdesktop::large_icon_steam;
    Check(steam::AppId(L"steam://rungameid/570") == 570u && steam::AppId(L"STEAM://RUN/730//") == 730u,
        "large icon Steam covers recognize both launch URL forms");
    for (const auto url : {L"steam://rungameid/12345678901234567", L"steam://run/0", L"steam://run/-1", L"https://store.steampowered.com/app/570", L"steam://run/123evil"})
        Check(!steam::AppId(url), "cover fetching rejects non-store shortcut IDs and malformed launch IDs");
    JsonValue metadata;
    Check(ParseJson(R"({"response":{"store_items":[{"appid":570,"assets":{"asset_url_format":"steam/apps/570/${FILENAME}?t=123","library_capsule_2x":"abc123/library_600x900_2x.jpg","header":"def456/header.jpg"}}]}})", metadata),
        "hashed Steam asset metadata fixture is valid JSON");
    Check(steam::AssetUrl(metadata, 570, true) == "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/570/abc123/library_600x900_2x.jpg?t=123" &&
        steam::AssetUrl(metadata, 570, false).find("def456/header.jpg") != std::string::npos && steam::AssetUrl(metadata, 730, true).empty(),
        "cover metadata keeps hash directories, orientation and exact AppID identity");
    Check(!steam::SafeAssetPath("../private") && !steam::SafeAssetPath("https://other.example/image.jpg") && !steam::SafeAssetPath("/root/image.png"),
        "Steam metadata cannot replace the trusted download host or escape relative asset paths");
    constexpr std::wstring_view appUserModelId =
        L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App";

    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", appUserModelId, L"", false),
        "an AppUserModelID must identify a Microsoft Store shortcut");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", L"", appUserModelId, false),
        "an AUMID target parsing path must identify an Applications shortcut");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", L"",
            L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
            false),
        "shell:AppsFolder targets must identify Applications shortcuts");
    Check(
        rules::LooksLikeApplicationsParsingName(
            L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\"
            L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"),
        "the Applications known-folder CLSID must be recognized");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"", L"", L"", L"", true),
        "an Applications PIDL must identify an Applications shortcut");
    Check(
        rules::IsApplicationsShellLinkTarget(
            L"C:\\Windows\\explorer.exe",
            L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App",
            L"", L"", false),
        "Explorer AppsFolder launch arguments must identify an application");
    Check(
        !rules::IsApplicationsShellLinkTarget(
            L"C:\\Windows\\explorer.exe", L"C:\\Temp", L"", L"", false),
        "ordinary Explorer shortcuts must not be classified as applications");
    Check(
        !rules::IsApplicationsShellLinkTarget(
            L"", L"", L"", L"C:\\Docs\\notes.txt", false),
        "document shortcuts must not be classified as Applications targets");

    Check(
        rules::ShouldUseShellIconOnly(L"C:\\Apps\\Editor.EXE"),
        "executables must avoid the Shell thumbnail representation");
    Check(
        rules::ShouldUseShellIconOnly(L"C:\\Desktop\\Editor.lnk"),
        "shortcuts must keep the native icon representation");
    Check(
        rules::ShouldUseShellIconOnly(L"C:\\Desktop\\Website.url"),
        "Internet shortcuts must keep the native icon representation");
    Check(
        rules::ShouldUseShellIconOnly(
            L"shell:AppsFolder\\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"),
        "Applications items must keep the native icon representation");
    Check(
        !rules::ShouldUseShellIconOnly(L"C:\\Pictures\\mountain.png"),
        "media files must remain eligible for Shell thumbnails");
    Check(
        !rules::ShouldUseShellIconOnly(L"C:\\Docs\\report.pdf"),
        "ordinary documents must remain eligible for Shell thumbnails");

    Check(
        rules::IsSteamApplicationUrl(L"steam://rungameid/730"),
        "Steam rungameid URLs must identify application shortcuts");
    Check(
        rules::IsSteamApplicationUrl(L"  StEaM://run/570/  "),
        "Steam run URLs must be matched case-insensitively");
    Check(
        !rules::IsSteamApplicationUrl(L"steam://rungameid/not-a-number"),
        "invalid Steam application IDs must be rejected");
    Check(
        !rules::IsSteamApplicationUrl(L"https://store.steampowered.com/app/730"),
        "Steam web links must remain ordinary Internet shortcuts");
    Check(
        !rules::IsSteamApplicationUrl(L"steam://open/store/730"),
        "non-launch Steam URLs must remain ordinary Internet shortcuts");

    CheckInternetShortcutIconResource();

    if (failures != 0)
    {
        std::cerr << failures << " shortcut application rule test(s) failed\n";
        return 1;
    }
    std::cout << "Shortcut application rule tests passed\n";
    return 0;
}
