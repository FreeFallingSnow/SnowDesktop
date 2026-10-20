#include "shell/shortcut_application_rules.h"
#include "layout/category_collection_rules.h"
#include "widgets/file_category_auto_collect_rules.h"
#include "desktop/desktop_category_item_rules.h"
#include "shell/shortcut_category_target.h"
#include "drag_drop/empty_group_drop_rules.h"
#include "icons/shortcut_icon_resource.h"
#include "icons/large_icon_steam.h"
#include "shell/shortcut_category_pidl_cases.h"

#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <algorithm>
#include <cstdint>
#include <fstream>
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

void CheckShortcutCategoryTargets()
{
    namespace collection = snowdesktop::category_collection_rules;
    Check(collection::NormalizeExtensionToken(L"lnk:pdf") == L".LNK:.PDF" &&
        collection::NormalizeExtensionToken(L"*.lnk:folder") == L".LNK:FOLDER" &&
        collection::NormalizeExtensionToken(L"url:steam") == L".URL:STEAM",
        "editable shortcut selectors accept optional dots and normalize case");
    const auto merged = collection::MergeLegacyProgramShortcutRules({L".PY", L".LNK", L".URL", L".LNK:APP"});
    Check(merged == std::vector<std::wstring>({L".PY", L".LNK:APP", L".URL:STEAM"}) &&
        collection::MergeLegacyProgramShortcutRules(merged) == merged,
        "upgrade merges target selectors without losing custom suffixes or duplicating rules");
    TemporaryDirectory temporary;
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized) && !temporary.Path().empty(), "shortcut category fixture initializes COM and a private directory");
    if (FAILED(initialized) || temporary.Path().empty()) return;
    const auto folder = temporary.Path() / L"folder.with.dots";
    std::filesystem::create_directory(folder);
    const auto document = temporary.Path() / L"report.PDF";
    const auto executable = temporary.Path() / L"program.EXE";
    std::ofstream(document) << "document";
    std::ofstream(executable) << "fixture only, never launched";
    for (const auto& target : {folder, document, executable})
    {
        const auto shortcut = temporary.Path() / (target.filename().wstring() + L".lnk");
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        const bool saved = SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&link))) && SUCCEEDED(link->SetPath(target.c_str())) &&
            SUCCEEDED(link.As(&file)) && SUCCEEDED(file->Save(shortcut.c_str(), TRUE));
        Check(saved, "real Shell links are saved for folder, document and executable targets");
        if (!saved) continue;
        const auto resolved = collection::ReadShortcutTarget(shortcut.wstring());
        Check(resolved.classified, "saved links complete target classification");
        const bool isFolder = target == folder;
        const bool isProgram = target == executable;
        Check(resolved.directory == isFolder && collection::IsProgramItem(L".LNK", false,
            {L".EXE", L".LNK:APP", L".URL:STEAM"}, resolved) == isProgram,
            "only executable links require the default program opt-in");
        Check(collection::MatchesExtensionRule(L".LNK:FOLDER", L".LNK", resolved) == isFolder,
            "the editable folder-target rule distinguishes a dotted directory from files");
        if (target == document)
            Check(collection::MatchesExtensionRule(L".PDF", L".LNK", resolved) &&
                collection::MatchesExtensionRule(L".LNK:.PDF", L".LNK", resolved) &&
                !collection::MatchesExtensionRule(L".LNK:.EXE", L".LNK", resolved),
                "plain and link-only user suffix rules classify document targets");
    }
    const auto broken = collection::ReadShortcutTarget((temporary.Path() / L"missing.lnk").wstring());
    Check(!collection::IsProgramItem(L".LNK", false, {L".LNK:APP"}, broken),
        "an unresolved link is not silently treated as a program");
    shortcut_category_pidl_cases::Run(temporary.Path(), Check);
    CoUninitialize();
}
#include "local_shortcut_icon_cases.h"
} // namespace

int RunWebsiteIconTests();
int RunWebsiteIconProbe(const wchar_t* shortcutPath, const wchar_t* outputDirectory);

int wmain(int argc, wchar_t** argv)
{
    if (argc == 4 && std::wstring_view(argv[1]) == L"--website-probe") return RunWebsiteIconProbe(argv[2], argv[3]);
    CheckLocalShortcutIcons();
    CheckLocalFolderAndDocumentIcons();
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    failures += RunWebsiteIconTests();
    if (SUCCEEDED(initialized)) CoUninitialize();
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
    CheckShortcutCategoryTargets();

    // The opt-in must cover native executables and app links, including
    // links that have no filesystem extension and user-defined suffixes.
    namespace collection = snowdesktop::category_collection_rules;
    Check(collection::IsProgramItem(L".exe", false, {}), "executables require the program opt-in");
    Check(collection::IsProgramItem(L"", true, {}), "Shell app identities require the program opt-in");
    Check(!collection::IsProgramItem(L".LNK", false, {}), "a shortcut suffix alone does not require program collection");
    Check(!collection::IsProgramItem(L".URL", false, {L".URL:STEAM"}), "ordinary web links remain collectable with programs disabled");
    Check(collection::IsProgramItem(L".PY", false, {L".PY"}), "custom program suffixes require the same opt-in");
    Check(!collection::IsProgramItem(L".PDF", false, {L".PY"}), "ordinary documents stay collectable while programs are disabled");
    // These are the production predicates used by FileCategories admission,
    // category snapshots and the commit-time program opt-in. No Shell/UI is
    // simulated here; actual drop execution still needs desktop validation.
    for (const auto* clsid : {kDesktopIconClsidThisPC, kDesktopIconClsidUserFiles,
            kDesktopIconClsidNetwork, kDesktopIconClsidControlPanel, kDesktopIconClsidRecycleBin})
    {
        DesktopItem item;
        item.layoutKey = L"SYSTEM-ICON";
        item.desktopIconClsid = clsid;
        item.parsingName = L"C:\\Users\\Example";
        Check(collection::IsDesktopNamespaceProgram(item),
            "each standard desktop namespace icon requires program collection");
        Check(collection::DesktopNamespaceCategory(item, true) == L"programs" &&
            collection::DesktopNamespaceCategory(item, false) == L"others",
            "namespace icons use Programs, falling back to Others when that category is disabled");
        for (const bool folder : {false, true})
        {
            Check(!collection::IsCollectableDesktopItem(item, false, folder, L"", {}),
                "namespace icons cannot bypass the program opt-in through a directory-backed PIDL");
            Check(collection::IsCollectableDesktopItem(item, true, folder, L"", {}),
                "enabling program collection admits system icons with or without filesystem paths");
        }
        item.desktopIconClsid.clear();
        item.parsingName = L"::" + std::wstring(clsid);
        Check(collection::IsDesktopNamespaceProgram(item) &&
            collection::IsCollectableDesktopItem(item, true, false, L"", {}) &&
            !collection::IsCollectableDesktopItem(item, false, false, L"", {}),
            "a Shell parsing identity retains the same opt-in before CLSID enrichment");
        item.layoutKey.clear();
        Check(!collection::IsCollectableDesktopItem(item, true, false, L"", {}),
            "even an enabled system icon needs a persistent layout key");
    }
    DesktopItem ordinary;
    ordinary.layoutKey = L"ordinary";
    ordinary.parsingName = L"C:\\Docs\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}.pdf";
    Check(!collection::IsDesktopNamespaceProgram(ordinary) &&
        collection::DesktopNamespaceCategory(ordinary, true).empty() &&
        collection::IsCollectableDesktopItem(ordinary, false, false, L".PDF", {}),
        "a document whose filename contains a CLSID remains an ordinary collectable document");
    Check(collection::IsCollectableDesktopItem(ordinary, false, true, L"", {}) &&
        !collection::IsCollectableDesktopItem(ordinary, false, false, L".EXE", {}) &&
        collection::IsCollectableDesktopItem(ordinary, true, false, L".EXE", {}),
        "ordinary folders retain their admission and executables still require the opt-in");
    const std::vector<std::wstring> defaults{L"all", L"folders", L"programs", L"images", L"others"};
    auto order = collection::ResolveTabOrder(defaults, {L"images", L"all", L"images", L"removed"});
    Check(order == std::vector<std::wstring>({L"images", L"all", L"folders", L"programs", L"others"}),
        "saved tab order deduplicates IDs, drops stale IDs, and retains new categories");
    Check(collection::MoveTab(order, L"all", L"others") && order.back() == L"all",
        "All can be moved to the end without changing category matching rules");
    Check(collection::MoveTab(order, L"all", L"images") && order.front() == L"all",
        "All can be moved back to the beginning");
    Check(!collection::MoveTab(order, L"all", L"missing"), "an invalid target cannot discard a tab");

    namespace autoCollect = snowdesktop::file_category_auto_collect_rules;
    DesktopWidget scope;
    Check(autoCollect::Matches(scope, L"images") && autoCollect::Matches(scope, L"custom-new"),
        "legacy empty selection includes all current and future categories");
    autoCollect::ToggleCategory(scope, L"images");
    Check(scope.autoCollect && autoCollect::Matches(scope, L"images") &&
        !autoCollect::Matches(scope, L"documents"),
        "selecting a category from All starts an explicit selection");
    autoCollect::ToggleCategory(scope, L"documents");
    autoCollect::ToggleCategory(scope, L"images");
    Check(scope.autoCollect && scope.autoCollectCategoryIds == std::vector<std::wstring>{L"documents"},
        "category selection supports adding and removing individual categories");
    autoCollect::ToggleCategory(scope, L"documents");
    Check(!scope.autoCollect, "removing the last category disables automatic collection");

    const std::vector<std::wstring> available{L"all", L"images", L"documents", L"custom-work", L"others"};
    std::vector<DesktopWidget> collectors(4);
    for (size_t index = 0; index < collectors.size(); ++index)
    {
        collectors[index].type = DesktopWidgetType::FileCategories;
        collectors[index].id = std::to_wstring(index);
        collectors[index].autoCollect = index < 3;
    }
    collectors[1].autoCollectCategoryIds = {L"images"};
    collectors[2].autoCollectCategoryIds = {L"documents"};
    autoCollect::EnforceExclusiveSelection(collectors, 1, available);
    Check(collectors[0].autoCollect && !autoCollect::Matches(collectors[0], L"images") &&
        autoCollect::Matches(collectors[0], L"custom-work") && collectors[2].autoCollect,
        "claiming Images removes only that option from an earlier All selection");
    autoCollect::EnforceExclusiveSelection(collectors, 2, available);
    Check(!autoCollect::Matches(collectors[0], L"documents") && collectors[1].autoCollect,
        "different components keep disjoint enabled categories");
    collectors[1].autoCollectCategoryIds.push_back(L"custom-work");
    autoCollect::EnforceExclusiveSelection(collectors, 1, available);
    autoCollect::ToggleCategory(collectors[2], L"custom-work");
    autoCollect::EnforceExclusiveSelection(collectors, 2, available);
    Check(collectors[1].autoCollect &&
        collectors[1].autoCollectCategoryIds == std::vector<std::wstring>{L"images"} &&
        collectors[2].autoCollectCategoryIds == std::vector<std::wstring>({L"documents", L"custom-work"}),
        "transferring a custom category keeps the previous owner's remaining categories enabled");
    autoCollect::ToggleCategory(collectors[3], L"images");
    collectors[1].itemKeys = {L"PHOTO.PNG"};
    collectors[1].missingItems.push_back({L"MISSING-PHOTO.PNG", {}, 0});
    const auto displaced = autoCollect::EnforceExclusiveSelection(collectors, 3, available);
    Check(!collectors[1].autoCollect && collectors[3].autoCollect &&
        collectors[1].itemKeys.empty() && collectors[1].missingItems.empty() &&
        std::find(displaced.begin(), displaced.end(), 1) != displaced.end(),
        "the latest Images selection disables the previous owner and releases live and missing memberships");
    collectors[1].autoCollect = true;
    collectors[1].autoCollectCategoryIds.clear();
    autoCollect::EnforceExclusiveSelection(collectors, 1, available);
    Check(collectors[1].autoCollect && !collectors[0].autoCollect &&
        !collectors[2].autoCollect && !collectors[3].autoCollect,
        "choosing All disables every previous automatic collector");
    collectors[0].autoCollectCategoryIds = {L"removed-custom"};
    Check(!autoCollect::Matches(collectors[0], L"others"),
        "a deleted custom category never silently broadens into All or Others");
    collectors[0].itemKeys = {L"MANUALLY-KEPT"};
    Check(autoCollect::EnforceExclusiveSelection(collectors, 1, available).empty() &&
        collectors[0].itemKeys == std::vector<std::wstring>{L"MANUALLY-KEPT"},
        "inactive components keep their contents and repeated enforcement is stable");

    namespace emptyGroup = snowdesktop::empty_group_drop_rules;
    for (const auto groupType : {DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    {
        for (const bool programs : {false, true})
        {
            DesktopWidget group;
            group.type = groupType;
            group.id = L"retained-id";
            group.gridCell.pageId = L"retained-page";
            group.gridCell.column = 3;
            group.gridCell.row = 4;
            group.gridSpan = {5, 2};
            group.bounds = RECT{111, 222, 777, 555};
            group.userRenamed = true;
            group.title = L"My group";
            group.showSearchBox = true;
            group.showFileCategories = true;
            group.childWidgetIds = {L"stale-child"};
            const std::vector<DesktopWidget> noSources;
            Check(emptyGroup::Convert(group, noSources, programs, L"Default title") &&
                group.type == (programs ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories),
                "either empty group can convert to the matching drop type");
            Check(group.id == L"retained-id" && group.gridCell.pageId == L"retained-page" &&
                group.gridCell.column == 3 && group.gridCell.row == 4 &&
                group.gridSpan.columns == 5 && group.gridSpan.rows == 2 &&
                group.bounds.left == 111 && group.bounds.top == 222 &&
                group.bounds.right == 777 && group.bounds.bottom == 555,
                "conversion preserves identity, page, position, size and runtime bounds");
            Check(group.title == L"My group" && group.showSearchBox && group.showFileCategories &&
                group.childWidgetIds.empty(), "conversion retains renamed titles and display options, clearing stale sources");
        }
        DesktopWidget group;
        group.type = groupType;
        group.childWidgetIds = {L"valid-child"};
        DesktopWidget child;
        child.id = L"valid-child";
        child.type = groupType == DesktopWidgetType::CollectionGroup
            ? DesktopWidgetType::Collection : DesktopWidgetType::FolderMapping;
        const std::vector<DesktopWidget> validSources{child};
        Check(!emptyGroup::Convert(group, validSources, false, L"Files") && group.type == groupType &&
            group.childWidgetIds == std::vector<std::wstring>{L"valid-child"},
            "an existing empty child source prevents conversion and is never discarded");
    }

    if (failures != 0)
    {
        std::cerr << failures << " shortcut application rule test(s) failed\n";
        return 1;
    }
    std::cout << "Shortcut application rule tests passed\n";
    return 0;
}
