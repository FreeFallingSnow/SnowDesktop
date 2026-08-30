#include "authoring_toolchain.h"
#include "bridge_json.h"
#include "manager_localization.h"
#include "package_tool.h"
#include "publish_lifecycle.h"
#include "steam_app_identity.h"
#include "steam_child_environment.h"
#include "steam_workshop_cache.h"
#include "steam_workshop_sync.h"
#include "workshop_localization.h"
#include "workshop_project.h"

#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace snowdesktop::steam_bridge;
using namespace snowdesktop::widget;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

struct TemporaryDirectory
{
    std::filesystem::path path;
    TemporaryDirectory()
    {
        path = std::filesystem::temp_directory_path() /
            (L"SnowDesktopWorkshopTests-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        std::filesystem::create_directory(path);
    }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void TestJson()
{
    JsonValue value;
    std::string error;
    Check(ParseJson("{\"中文\":\"雪\\n桌面\",\"n\":1,\"ok\":true}",
        value, error), "JSON parser accepts UTF-8 and escapes");
    Check(JsonBoolean(value, "ok") == true,
        "JSON boolean helper reads a boolean");
    Check(JsonUnsigned(value, "n") == 1,
        "JSON unsigned helper reads an integer");
    JsonValue duplicate;
    Check(!ParseJson("{\"a\":1,\"a\":2}", duplicate, error),
        "JSON parser rejects duplicate keys");
    Check(!ParseJson("{\"n\":9007199254740992}", duplicate, error) ||
        !JsonUnsigned(duplicate, "n"),
        "large identifiers are not accepted as lossy JSON numbers");
}

void TestSteamIdentity()
{
    Check(kSteamAppId == 5080330u,
        "the compiled Steam identity uses the production App ID");
    Check(kSteamWindowsDepotId == 5080331u,
        "the compiled Steam identity uses the production Windows depot");
    Check(IsExpectedSteamAppId(5080330u) &&
        !IsExpectedSteamAppId(480u),
        "Steam runtime identity rejects placeholder App IDs");
    Check(SteamWorkshopHomeUrl() ==
        "https://steamcommunity.com/app/5080330/workshop/",
        "Workshop links target the SnowDesktop application hub");
    Check(SteamWorkshopClientUrl() ==
        "steam://openurl/https://steamcommunity.com/app/5080330/workshop/",
        "Workshop home links prefer the Steam client");
    Check(SteamCommunityItemClientUrl(1234567890) ==
        "steam://url/CommunityFilePage/1234567890",
        "Workshop item links use Valve's Steam client protocol");
    const std::string mismatch = SteamAppIdMismatchMessage(480u);
    Check(mismatch.find("5080330") != std::string::npos &&
        mismatch.find("480") != std::string::npos,
        "App ID mismatch diagnostics identify expected and actual values");
}

void TestSteamChildEnvironment()
{
    const std::vector<wchar_t> block =
        snowdesktop::BuildSnowDesktopSteamChildEnvironment();
    Check(block.size() >= 2 && block[block.size() - 1] == L'\0' &&
        block[block.size() - 2] == L'\0',
        "Steam child environment is a double-null-terminated Unicode block");
    std::size_t appIdEntries = 0;
    std::size_t gameIdEntries = 0;
    for (const wchar_t* current = block.data(); *current != L'\0';
         current += std::wcslen(current) + 1)
    {
        const std::wstring_view entry(current);
        if (entry == L"SteamAppId=5080330") ++appIdEntries;
        if (entry == L"SteamGameId=5080330") ++gameIdEntries;
    }
    Check(appIdEntries == 1 && gameIdEntries == 1,
        "Steam child environment carries exactly one production App ID context");
}

void TestManagerLocalization()
{
    TemporaryDirectory temporary;
    const auto languages = temporary.path / L"lang";
    std::filesystem::create_directory(languages);
    std::ofstream(languages / L"en-US.json", std::ios::binary) <<
        R"({"workshop_manager.open":"Open Workshop","unrelated":"same"})";
    std::ofstream(languages / L"zh-CN.json", std::ios::binary) <<
        R"({"workshop_manager.open":"打开创意工坊","unrelated":"不同"})";
    ManagerLocalization localization;
    std::string error;
    Check(localization.Load(languages, "zh-Hans-CN", error),
        "manager localization loads the shared flat JSON catalogs");
    Check(std::string(localization.Translate("Open Workshop", "内置中文")) ==
        "打开创意工坊",
        "manager localization resolves a compatible requested language");
    Check(std::string(localization.Translate("same", "内置中文")) ==
        "内置中文",
        "manager localization only consumes its own key namespace");
    std::ofstream(languages / L"zh-TW.json", std::ios::binary) <<
        R"({"workshop_manager.open":"開啟創意工坊","unrelated":"不同"})";
    Check(localization.Load(languages, "zh-HK", error),
        "manager localization reloads after adding regional catalogs");
    Check(std::string(localization.Translate("Open Workshop", "內置中文")) ==
        "開啟創意工坊",
        "manager localization prefers traditional Chinese for Hong Kong");
    localization.SelectLanguage("en-US");
    Check(std::string(localization.Translate("Open Workshop", "内置中文")) ==
        "Open Workshop",
        "manager localization follows a selected main catalog");
}

PackageDetails WorkshopDetails(std::string packageId, std::string version,
    std::string itemId)
{
    PackageDetails details;
    details.manifest.id = std::move(packageId);
    details.manifest.version = std::move(version);
    details.source = { "steam-workshop", std::move(itemId) + "@42" };
    return details;
}

InstalledPackage Installed(std::string packageId, std::string version,
    std::string provider, std::string itemId)
{
    InstalledPackage package;
    package.manifest.id = std::move(packageId);
    package.manifest.version = std::move(version);
    package.source = { std::move(provider), std::move(itemId) };
    package.active = true;
    return package;
}

void TestSteamSubscriptionSyncPlan()
{
    SteamWorkshopSubscriptionSnapshot snapshot;
    snapshot.authoritative = true;
    snapshot.subscribedPublishedFileIds = { "100" };
    snapshot.installable.push_back(
        WorkshopDetails("package-a", "1.0.0", "100"));
    auto associations =
        BuildSteamWorkshopPackageAssociations(snapshot);
    Check(associations.size() == 1 &&
        associations["package-a"] == "100@42",
        "a subscribed Workshop item is associated with its package UUID");
    auto duplicate = WorkshopDetails("package-a", "1.0.0", "200");
    snapshot.installable.push_back(duplicate);
    associations = BuildSteamWorkshopPackageAssociations(snapshot);
    Check(!associations.contains("package-a"),
        "duplicate subscribed package UUIDs are never associated arbitrarily");
    snapshot.installable.pop_back();
    SteamWorkshopSubscriptionSnapshot failedSnapshot;
    failedSnapshot.authoritative = true;
    PackageManifest failedManifest;
    failedManifest.id = "package-failed";
    failedManifest.name = "Failed package";
    failedSnapshot.discoveryFailures.push_back({ "package-failed",
        "300@42", std::move(failedManifest), "validation failed" });
    associations = BuildSteamWorkshopPackageAssociations(failedSnapshot);
    Check(associations.size() == 1 &&
        associations["package-failed"] == "300@42",
        "a discovered invalid Workshop package retains its item association");
    auto plan = BuildSteamWorkshopSyncPlan({}, snapshot);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Install,
        "a newly subscribed Workshop component is installed automatically");

    auto current = Installed("package-a", "1.0.0",
        "steam-workshop", "100@42");
    auto development = Installed("package-a", "1.0.0",
        "local-directory", "package-a-dev");
    development.development = true;
    plan = BuildSteamWorkshopSyncPlan({ development }, snapshot);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Install && plan.conflicts.empty(),
        "a development candidate does not block Workshop installation");

    SteamWorkshopSubscriptionSnapshot downloading;
    downloading.authoritative = true;
    downloading.subscribedPublishedFileIds = { "100" };
    plan = BuildSteamWorkshopSyncPlan({ current }, downloading);
    Check(plan.actions.empty(),
        "a subscribed item still downloading is not mistaken for an unsubscribe");

    SteamWorkshopSubscriptionSnapshot unsubscribed;
    unsubscribed.authoritative = true;
    unsubscribed.activeSteamAccountId = "111";
    const SteamWorkshopSubscriptionHistory subscriptionHistory{
        { "111", { "100" } },
    };
    ResolveSteamWorkshopSubscriptionRemovals(
        unsubscribed, subscriptionHistory);
    auto shadowed = current;
    shadowed.active = false;
    plan = BuildSteamWorkshopSyncPlan({ shadowed }, unsubscribed);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Uninstall,
        "unsubscription removes a Workshop package hidden by a development copy");

    snapshot.installable[0].manifest.version = "1.1.0";
    snapshot.installable[0].source.externalItemId = "100";
    plan = BuildSteamWorkshopSyncPlan({ shadowed }, snapshot);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Update &&
        plan.actions[0].externalItemId == "100@42",
        "a cached Workshop update preserves the verified creator binding");

    plan = BuildSteamWorkshopSyncPlan({ current }, unsubscribed);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Uninstall,
        "an unsubscribed Workshop component is uninstalled automatically");
    auto previousVersion = current;
    previousVersion.manifest.version = "0.9.0";
    previousVersion.active = false;
    plan = BuildSteamWorkshopSyncPlan(
        { current, previousVersion }, unsubscribed);
    Check(plan.actions.size() == 1,
        "unsubscription schedules one removal for all retained versions");

    SteamWorkshopSubscriptionSnapshot switchedAccount;
    switchedAccount.authoritative = true;
    switchedAccount.activeSteamAccountId = "222";
    ResolveSteamWorkshopSubscriptionRemovals(
        switchedAccount, subscriptionHistory);
    plan = BuildSteamWorkshopSyncPlan({ current }, switchedAccount);
    Check(plan.actions.empty(),
        "an empty cache after switching Steam accounts preserves local components");

    SteamWorkshopSubscriptionSnapshot sharedAcrossAccounts = unsubscribed;
    ResolveSteamWorkshopSubscriptionRemovals(sharedAcrossAccounts,
        { { "111", { "100" } }, { "222", { "100" } } });
    plan = BuildSteamWorkshopSyncPlan({ current }, sharedAcrossAccounts);
    Check(plan.actions.empty(),
        "another account's remembered subscription preserves the local component");

    unsubscribed.authoritative = false;
    plan = BuildSteamWorkshopSyncPlan({ current }, unsubscribed);
    Check(plan.actions.empty(),
        "a failed/non-authoritative Steam query never removes components");

    auto local = Installed("package-a", "1.0.0", "local-import", "package-a");
    plan = BuildSteamWorkshopSyncPlan({ local }, snapshot);
    Check(plan.actions.empty() && plan.conflicts.size() == 1,
        "automatic subscription sync does not replace another package source");
}

void TestProjectStore()
{
    TemporaryDirectory temporary;
    const auto source = temporary.path / L"project with spaces";
    std::filesystem::create_directory(source);
    std::ofstream(source / L"widget.json") << "{}";
    ProjectStore store(temporary.path / L"store");
    WorkshopProject* project = nullptr;
    std::string error;
    Check(store.AddDirectory(source, project, error),
        "project store adds a component directory");
    Check(project && !project->localId.empty(),
        "project store creates a persistent local UUID");
    if (!project) return;
    project->packageId = "11111111-2222-3333-4444-555555555555";
    project->publishedFileId = 76561198000000001ull;
    project->tags = { "Widget", "Clock" };
    Check(store.Save(error), "project store saves schema v1");
    project->lastPublishedVersion = "1.2.3";
    Check(store.Save(error), "second save creates a backup and replaces atomically");
    Check(std::filesystem::is_regular_file(
        store.Root() / L"projects.json.bak"), "project store keeps .bak");
    ProjectStore loaded(store.Root());
    Check(loaded.Load(error), "project store loads schema v1");
    Check(loaded.Projects().size() == 1 &&
        loaded.Projects()[0].publishedFileId == 76561198000000001ull &&
        loaded.Projects()[0].tags.size() == 2,
        "project store round-trips strings, 64-bit IDs, and tags");
    WorkshopProject* duplicate = nullptr;
    Check(loaded.AddDirectory(source, duplicate, error) &&
        loaded.Projects().size() == 1,
        "adding the same canonical directory is idempotent");
    WorkshopProject* invalid = nullptr;
    Check(!loaded.AddDirectory(temporary.path / L"missing", invalid, error),
        "project store rejects missing paths");
    const auto linked = temporary.path / L"linked-project";
    if (CreateSymbolicLinkW(linked.c_str(), source.c_str(),
            SYMBOLIC_LINK_FLAG_DIRECTORY |
            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        Check(!loaded.AddDirectory(linked, invalid, error),
            "project store rejects project paths containing a reparse point");
    const auto second = temporary.path / L"second";
    std::filesystem::create_directory(second);
    std::ofstream(second / L"widget.json") << "{}";
    std::size_t discovered = 0;
    Check(loaded.Discover(temporary.path, discovered, error) && discovered == 1,
        "development-root discovery adds immediate component children");
    const auto bundledRoot = temporary.path / L"bundled";
    const auto bundledComponent = bundledRoot / L"clock";
    std::filesystem::create_directories(
        bundledRoot / L"snowdesktop-lua-widget");
    std::filesystem::create_directories(bundledComponent);
    std::ofstream(bundledRoot / L"snowdesktop-lua-widget" / L"SKILL.md")
        << "bundled marker";
    std::ofstream(bundledComponent / L"widget.json") << "{}";
    WorkshopProject* bundledProject = nullptr;
    Check(!loaded.AddDirectory(bundledComponent, bundledProject, error),
        "bundled components cannot be added as creator projects");
    discovered = 0;
    Check(loaded.Discover(bundledRoot, discovered, error) && discovered == 0,
        "bundled component roots are not discovered by the creator manager");
    Check(loaded.Remove(loaded.Projects()[0].localId, error) &&
        std::filesystem::is_directory(source),
        "removing a record does not delete source content");
}

void TestWorkshopManagerDataMigration()
{
    TemporaryDirectory temporary;
    const auto legacyRoot = temporary.path / L"legacy-manager";
    const auto dataDirectory = temporary.path / L"data";
    const auto targetRoot = WorkshopManagerDataRoot(dataDirectory);
    ProjectStore legacyStore(legacyRoot);
    std::string error;
    Check(legacyStore.Save(error),
        "legacy Workshop Manager project store can be created");
    Check(legacyStore.Save(error),
        "legacy Workshop Manager project backup can be created");
    std::ofstream(legacyRoot / L"projects.json.tmp", std::ios::binary)
        << "stale temporary data";
    std::filesystem::create_directory(legacyRoot / L"preview-cache");
    std::ofstream(legacyRoot / L"preview-cache" / L"123.preview",
        std::ios::binary) << "preview data";

    Check(MigrateWorkshopManagerDataOnce(
            targetRoot, error, legacyRoot),
        "Workshop Manager data migrates into the SnowDesktop data root");
    Check(targetRoot == dataDirectory / L"SteamWorkshopManager" &&
            std::filesystem::is_regular_file(
                targetRoot / L"projects.json") &&
            std::filesystem::is_regular_file(
                targetRoot / L"projects.json.bak") &&
            std::filesystem::is_regular_file(
                targetRoot / L"projects.json.tmp") &&
            std::filesystem::is_regular_file(
                targetRoot / L"preview-cache" / L"123.preview") &&
            std::filesystem::is_regular_file(targetRoot /
                L".legacy-localappdata-migrated-v1"),
        "project state, backup, temporary state, and previews share the data directory");
    Check(!std::filesystem::exists(legacyRoot),
        "successful Workshop Manager migration removes the empty legacy root");
    ProjectStore migratedStore(targetRoot);
    Check(migratedStore.Load(error),
        "the migrated Workshop Manager project store remains readable");
    std::filesystem::create_directories(legacyRoot);
    std::ofstream(legacyRoot / L"projects.json.tmp", std::ios::binary)
        << "must remain untouched after the migration marker";
    Check(MigrateWorkshopManagerDataOnce(
            targetRoot, error, legacyRoot) &&
            std::filesystem::is_regular_file(
                legacyRoot / L"projects.json.tmp"),
        "the completed migration marker prevents later legacy-directory access");
}

void TestMetadataBinding()
{
    const std::string packageId =
        "11111111-2222-3333-4444-555555555555";
    const std::string metadata = BuildWorkshopMetadata(packageId, "1.2.3");
    std::string error;
    const auto parsed = ParseWorkshopMetadata(metadata, error);
    Check(parsed && parsed->packageId == packageId &&
        parsed->version == "1.2.3", "association metadata round-trips");
    WorkshopProject project;
    project.packageId = packageId;
    Check(CanBindWorkshopItem(project, metadata, 10, 10, 20, 20, error),
        "owned item with matching App ID and UUID can bind");
    Check(!CanBindWorkshopItem(project, metadata, 11, 10, 20, 20, error),
        "foreign owner cannot bind");
    Check(!CanBindWorkshopItem(project, metadata, 10, 10, 21, 20, error),
        "foreign Consumer App ID cannot bind");
    WorkshopProject other;
    other.packageId = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    Check(!CanBindWorkshopItem(other, metadata, 10, 10, 20, 20, error),
        "mismatched package UUID cannot bind");
}

void TestCommandLineQuoting()
{
    const std::vector<std::wstring> values = {
        L"plain", L"path with spaces\\widget", L"", L"a\\\\\"b",
        L"trailing\\" };
    for (const auto& value : values)
    {
        const std::wstring command = L"tool.exe " + QuoteWindowsArgument(value);
        int count = 0;
        wchar_t** parsed = CommandLineToArgvW(command.c_str(), &count);
        Check(parsed && count == 2 && parsed[1] == value,
            "Windows argument quoting round-trips through CommandLineToArgvW");
        if (parsed) LocalFree(parsed);
    }
}

enum class FakeResult
{
    Success,
    CreateFailed,
    UploadFailed,
    EulaRequired,
    Banned,
    Offline,
    Timeout,
};

struct FakePublishOutcome
{
    PublishLifecycle lifecycle;
    bool eulaRequired = false;
};

FakePublishOutcome RunFakeBackend(FakeResult result, bool creating)
{
    FakePublishOutcome outcome;
    outcome.lifecycle.Begin(creating);
    if (result == FakeResult::CreateFailed || result == FakeResult::Offline)
    {
        outcome.lifecycle.Fail();
        return outcome;
    }
    if (creating) outcome.lifecycle.ItemCreated(12345678901234567ull);
    else outcome.lifecycle.BindExisting(12345678901234567ull);
    if (result == FakeResult::UploadFailed || result == FakeResult::Banned)
    {
        outcome.lifecycle.Fail();
        return outcome;
    }
    outcome.lifecycle.SubmitStarted();
    if (result == FakeResult::Timeout)
    {
        outcome.lifecycle.Fail();
        return outcome;
    }
    outcome.eulaRequired = result == FakeResult::EulaRequired;
    outcome.lifecycle.Succeed();
    return outcome;
}

void TestPublishLifecycle()
{
    auto success = RunFakeBackend(FakeResult::Success, true);
    Check(success.lifecycle.State() == PublishLifecycleState::Succeeded,
        "fake backend covers successful create and upload");
    Check(success.lifecycle.MustPersistCreatedItem(),
        "created PublishedFileId remains persistable");
    auto failed = RunFakeBackend(FakeResult::UploadFailed, true);
    Check(failed.lifecycle.State() == PublishLifecycleState::Failed &&
        failed.lifecycle.PublishedFileId() != 0 &&
        failed.lifecycle.MustPersistCreatedItem(),
        "upload failure retains the immediately-created PublishedFileId");
    auto eula = RunFakeBackend(FakeResult::EulaRequired, false);
    Check(eula.eulaRequired &&
        eula.lifecycle.State() == PublishLifecycleState::Succeeded,
        "fake backend carries legal-agreement status");
    for (const auto result : { FakeResult::Banned, FakeResult::Offline,
            FakeResult::Timeout })
        Check(RunFakeBackend(result, true).lifecycle.State() ==
            PublishLifecycleState::Failed,
            "fake backend covers banned, offline, and timeout failures");
    PublishLifecycle submitting;
    submitting.Begin(false);
    submitting.BindExisting(99);
    Check(submitting.CanCancel(), "publish can cancel before SubmitItemUpdate");
    submitting.SubmitStarted();
    Check(!submitting.CanCancel(),
        "publish cannot cancel after SubmitItemUpdate starts");
}

void TestWorkshopLocalization()
{
    Check(SteamApiLanguageForLocale("en-US") == "english" &&
        SteamApiLanguageForLocale("zh-Hans-CN") == "schinese" &&
        SteamApiLanguageForLocale("zh-Hant-HK") == "tchinese" &&
        SteamApiLanguageForLocale("pt-BR") == "brazilian" &&
        SteamApiLanguageForLocale("es-419") == "latam" &&
        SteamApiLanguageForLocale("ko-KR") == "koreana" &&
        !SteamApiLanguageForLocale("eo-001"),
        "BCP-47 component locales map to Steam API language codes");

    const std::vector<WidgetLocalization> source = {
        { "zh-TW", "音訊頻譜", "繁體說明" },
        { "es-419", "Espectro", "Descripción" },
        { "en-US", "Audio Spectrum", "English description" },
        { "eo-001", "Spektro", "Priskribo" },
    };
    const auto localized = BuildSteamWorkshopLocalizations(
        "Fallback", "Fallback description", source);
    Check(localized.size() == 3 &&
        localized[0].language == "english" &&
        localized[0].title == "Audio Spectrum" &&
        localized[1].language == "latam" &&
        localized[2].language == "tchinese",
        "Workshop localizations are deduplicated and order English first");

    const auto withFallback = BuildSteamWorkshopLocalizations(
        "Fallback title", "Fallback description",
        { { "ja-JP", "オーディオスペクトラム", "日本語の説明" } });
    Check(withFallback.size() == 2 &&
        withFallback[0].language == "english" &&
        withFallback[0].title == "Fallback title" &&
        withFallback[1].language == "japanese",
        "manifest defaults supply the required English Workshop fallback");

}

void TestSteamWorkshopLocalCache()
{
    TemporaryDirectory temporary;
    const auto workshop = temporary.path / L"steamapps" / L"workshop";
    const auto content = workshop / L"content" / L"5080330";
    std::filesystem::create_directories(content / L"100");
    std::filesystem::create_directories(content / L"300");
    std::filesystem::create_directories(content / L"400");
    std::ofstream(workshop / L"appworkshop_5080330.acf",
        std::ios::binary) << R"VDF(
"AppWorkshop"
{
    "appid" "5080330"
    "NeedsUpdate" "0"
    "NeedsDownload" "1"
    "WorkshopItemsInstalled"
    {
        "100" { "manifest" "11" }
        "300" { "manifest" "30" }
        "400" { "manifest" "40" }
    }
    "WorkshopItemDetails"
    {
        "100" { "latest_manifest" "11" }
        "200" { "latest_manifest" "20" "subscribedby" "123" }
        "300" { "latest_manifest" "31" "subscribedby" "123" }
        "400" { "latest_manifest" "40" "subscribedby" "0" }
    }
}
)VDF";

    auto cache = ReadSteamWorkshopLocalCache(
        { temporary.path }, 5080330u);
    Check(cache.authoritative &&
        cache.subscribedPublishedFileIds ==
            std::vector<std::string>({ "100", "200", "300" }),
        "local Workshop cache is an authoritative subscription snapshot");
    Check(cache.readyItems.size() == 1 &&
        cache.readyItems[0].publishedFileId == "100" &&
        cache.readyItems[0].contentDirectory == content / L"100",
        "local Workshop cache keeps current subscribed items ready while another item downloads and ignores stale unsubscribed details");

    std::ofstream(workshop / L"appworkshop_5080330.acf",
        std::ios::binary | std::ios::trunc) <<
        "\"AppWorkshop\" { \"appid\" \"5080330\"";
    cache = ReadSteamWorkshopLocalCache({ temporary.path }, 5080330u);
    Check(!cache.authoritative && !cache.error.empty(),
        "a partially written Workshop cache never authorizes removals");
}

void TestAuthoringToolchain(const std::filesystem::path& repositoryRoot,
    const std::filesystem::path& snowwidget)
{
    TemporaryDirectory temporary;
    const auto bundled = repositoryRoot / L"widgets" /
        L"snowdesktop-lua-widget";
    const std::array kinds = {
        AgentSkillTargetKind::Shared,
        AgentSkillTargetKind::Codex,
        AgentSkillTargetKind::ClaudeCode,
        AgentSkillTargetKind::Cursor,
        AgentSkillTargetKind::GitHubCopilot,
        AgentSkillTargetKind::GeminiCli,
    };
    for (std::size_t index = 0; index < kinds.size(); ++index)
    {
        AgentSkillTarget target;
        target.kind = kinds[index];
        target.id = "test-agent-" + std::to_string(index);
        target.skillsRoot = temporary.path /
            std::filesystem::path(target.id) / L"skills";
        std::string error;
        auto status = InspectAgentSkill(
            bundled, snowwidget, target, error);
        Check(status.state == SkillInstallState::NotInstalled &&
            status.bundledRevision == 5,
            "each supported agent reports a clean not-installed state");
        Check(InstallOrUpdateAgentSkill(status, error),
            "Agent Skill installs transactionally into every selected root");
        status = InspectAgentSkill(bundled, snowwidget, target, error);
        Check(status.state == SkillInstallState::Current &&
            status.installedRevision == status.bundledRevision &&
            std::filesystem::is_regular_file(status.target / L"SKILL.md") &&
            std::filesystem::is_regular_file(
                status.target / L"bin" / L"snowwidget.exe"),
            "installed Agent Skill is current and contains its CLI");
        Check(UninstallAgentSkill(status, error),
            "an unselected Agent Skill target is removed safely");
        status = InspectAgentSkill(bundled, snowwidget, target, error);
        Check(status.state == SkillInstallState::NotInstalled &&
            !std::filesystem::exists(status.target),
            "an uninstalled Agent Skill target returns to not-installed");
    }
}

void TestRealPackageTool(const std::filesystem::path& executable,
    const std::filesystem::path& repositoryRoot)
{
    TemporaryDirectory temporaryRoot;
    const auto stagingRoot = temporaryRoot.path / L"data" /
        L"SteamWorkshopManager" / L"staging" / L"packages";
    PackageTool tool(executable, stagingRoot);
    const std::wstring capabilitiesCommand = L"\"" + executable.wstring() +
        L"\" capabilities";
    FILE* capabilitiesPipe = _wpopen(capabilitiesCommand.c_str(), L"rt");
    Check(capabilitiesPipe != nullptr,
        "snowwidget capabilities can be launched by AI tooling");
    if (capabilitiesPipe)
    {
        std::string capabilitiesText;
        std::array<char, 1024> capabilitiesBuffer{};
        while (std::fgets(capabilitiesBuffer.data(),
                static_cast<int>(capabilitiesBuffer.size()), capabilitiesPipe))
            capabilitiesText += capabilitiesBuffer.data();
        const int capabilitiesExit = _pclose(capabilitiesPipe);
        JsonValue capabilities;
        std::string capabilitiesError;
        Check(capabilitiesExit == 0 &&
            ParseJson(capabilitiesText, capabilities, capabilitiesError) &&
            JsonUnsigned(capabilities, "protocolVersion") == 2u &&
            JsonUnsigned(capabilities, "recommendedSchemaVersion") == 2u &&
            JsonUnsigned(capabilities, "recommendedApiVersion") == 2u &&
            capabilities.Find("executableSchemaVersions") &&
            capabilities.Find("executableSchemaVersions")->IsArray() &&
            capabilities.Find("executableSchemaVersions")->array.size() == 1 &&
            capabilities.Find("executableSchemaVersions")->array[0].number == 2 &&
            capabilities.Find("executableApiVersions") &&
            capabilities.Find("executableApiVersions")->IsArray() &&
            capabilities.Find("executableApiVersions")->array.size() == 1 &&
            capabilities.Find("executableApiVersions")->array[0].number == 2 &&
            !capabilities.Find("migrationInputSchemaVersions") &&
            !capabilities.Find("migrationInputApiVersions") &&
            !capabilities.Find("supportedSchemaVersions") &&
            !capabilities.Find("supportedApiVersions") &&
            JsonString(capabilities, "format") == "snowdesktop-widget",
            "snowwidget capabilities publishes a v2-only package contract");
    }
    WidgetInspection inspection;
    PackagedWidget package;
    std::string error;
    const auto source = repositoryRoot / L"widgets" / L"analog-clock";
    Check(tool.Inspect(source, inspection, error),
        "snowwidget inspect returns a validated manifest JSON object");
    if (!inspection.valid) return;
    const auto localizations = BuildSteamWorkshopLocalizations(
        inspection.name, inspection.description, inspection.localizations);
    Check(inspection.localizations.size() >= 5 &&
        localizations.size() >= 5 &&
        localizations.front().language == "english",
        "snowwidget inspect exposes reusable component package localizations");
    Check(tool.Pack(source, inspection, package, error),
        "package tool validates the pack result against inspect");
    Check(std::filesystem::is_regular_file(package.packagePath) &&
        package.temporaryDirectory.parent_path() == stagingRoot &&
        package.packageId == inspection.packageId &&
        package.version == inspection.version && package.sha256.size() == 64,
        "packed package has matching ID, version, hash, and output file");
    const auto temporary = package.temporaryDirectory;
    package.Cleanup();
    Check(!std::filesystem::exists(temporary),
        "package cleanup deletes only its package and empty unique directory");
}

void TestManagerFontCoverage(const std::filesystem::path& repositoryRoot)
{
    std::ifstream input(repositoryRoot / L"steam_bridge" / L"src" /
        L"manager_main.cpp", std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    Check(input.good() || input.eof(),
        "Workshop Manager source is readable for the font contract");
    Check(source.find("malgun.ttf") != std::string::npos &&
            source.find("MergeMode = true") != std::string::npos &&
            source.find("GetGlyphRangesKorean()") != std::string::npos,
        "Workshop Manager merges a Korean system font and Hangul glyph range");
}
}

int wmain(int argc, wchar_t** argv)
{
    TestJson();
    TestSteamIdentity();
    TestSteamChildEnvironment();
    TestManagerLocalization();
    TestSteamSubscriptionSyncPlan();
    TestSteamWorkshopLocalCache();
    TestProjectStore();
    TestWorkshopManagerDataMigration();
    TestMetadataBinding();
    TestCommandLineQuoting();
    TestPublishLifecycle();
    TestWorkshopLocalization();
    if (argc == 3)
    {
        TestAuthoringToolchain(argv[2], argv[1]);
        TestRealPackageTool(argv[1], argv[2]);
        TestManagerFontCoverage(argv[2]);
    }
    else Check(false, "test requires snowwidget.exe and repository root arguments");
    if (failures == 0)
        std::cout << "Steam Workshop manager tests passed\n";
    return failures == 0 ? 0 : 1;
}
