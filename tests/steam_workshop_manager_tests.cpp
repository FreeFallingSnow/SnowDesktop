#include "bridge_json.h"
#include "manager_localization.h"
#include "package_tool.h"
#include "publish_lifecycle.h"
#include "steam_workshop_sync.h"
#include "workshop_project.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <iostream>
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

void TestManagerLocalization()
{
    TemporaryDirectory temporary;
    const auto languages = temporary.path / L"lang";
    std::filesystem::create_directory(languages);
    std::ofstream(languages / L"en-US.json", std::ios::binary) <<
        R"({"workshop_manager.open":"Open Workshop","unrelated":"same"})";
    std::ofstream(languages / L"zh-CN.json", std::ios::binary) <<
        R"({"workshop_manager.open":"打开网页工坊","unrelated":"不同"})";
    ManagerLocalization localization;
    std::string error;
    Check(localization.Load(languages, "zh-Hans-CN", error),
        "manager localization loads the shared flat JSON catalogs");
    Check(std::string(localization.Translate("Open Workshop", "内置中文")) ==
        "打开网页工坊",
        "manager localization resolves a compatible requested language");
    Check(std::string(localization.Translate("same", "内置中文")) ==
        "内置中文",
        "manager localization only consumes its own key namespace");
    std::ofstream(languages / L"zh-TW.json", std::ios::binary) <<
        R"({"workshop_manager.open":"開啟網頁工坊","unrelated":"不同"})";
    Check(localization.Load(languages, "zh-HK", error),
        "manager localization reloads after adding regional catalogs");
    Check(std::string(localization.Translate("Open Workshop", "內置中文")) ==
        "開啟網頁工坊",
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
    auto plan = BuildSteamWorkshopSyncPlan({}, snapshot);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Install,
        "a newly subscribed Workshop component is installed automatically");

    auto current = Installed("package-a", "1.0.0",
        "steam-workshop", "100@42");
    SteamWorkshopSubscriptionSnapshot downloading;
    downloading.authoritative = true;
    downloading.subscribedPublishedFileIds = { "100" };
    plan = BuildSteamWorkshopSyncPlan({ current }, downloading);
    Check(plan.actions.empty(),
        "a subscribed item still downloading is not mistaken for an unsubscribe");

    SteamWorkshopSubscriptionSnapshot unsubscribed;
    unsubscribed.authoritative = true;
    auto shadowed = current;
    shadowed.active = false;
    plan = BuildSteamWorkshopSyncPlan({ shadowed }, unsubscribed);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Uninstall,
        "unsubscription removes a Workshop package hidden by a development copy");

    snapshot.installable[0].manifest.version = "1.1.0";
    plan = BuildSteamWorkshopSyncPlan({ shadowed }, snapshot);
    Check(plan.actions.size() == 1 && plan.actions[0].kind ==
        SteamWorkshopSyncActionKind::Update,
        "a shadowed Workshop component still follows the published version");

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
    Check(loaded.Remove(loaded.Projects()[0].localId, error) &&
        std::filesystem::is_directory(source),
        "removing a record does not delete source content");
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

void TestRealPackageTool(const std::filesystem::path& executable,
    const std::filesystem::path& repositoryRoot)
{
    PackageTool tool(executable);
    WidgetInspection inspection;
    PackagedWidget package;
    std::string error;
    const auto source = repositoryRoot / L"widgets" / L"analog-clock";
    Check(tool.Inspect(source, inspection, error),
        "snowwidget inspect returns a validated manifest JSON object");
    if (!inspection.valid) return;
    Check(tool.Pack(source, inspection, package, error),
        "package tool validates the pack result against inspect");
    Check(std::filesystem::is_regular_file(package.packagePath) &&
        package.packageId == inspection.packageId &&
        package.version == inspection.version && package.sha256.size() == 64,
        "packed package has matching ID, version, hash, and output file");
    const auto temporary = package.temporaryDirectory;
    package.Cleanup();
    Check(!std::filesystem::exists(temporary),
        "package cleanup deletes only its package and empty unique directory");
}
}

int wmain(int argc, wchar_t** argv)
{
    TestJson();
    TestManagerLocalization();
    TestSteamSubscriptionSyncPlan();
    TestProjectStore();
    TestMetadataBinding();
    TestCommandLineQuoting();
    TestPublishLifecycle();
    if (argc == 3) TestRealPackageTool(argv[1], argv[2]);
    else Check(false, "test requires snowwidget.exe and repository root arguments");
    if (failures == 0)
        std::cout << "Steam Workshop manager tests passed\n";
    return failures == 0 ? 0 : 1;
}
