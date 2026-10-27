#pragma once

#include "widget/packages/widget_package_source_snapshot.h"
#include "app/widgets/widget_package_source_capture.h"
#include "../test_temporary_directory.h"

#include <fstream>
#include <stdexcept>

namespace package_source_lookup_cases
{
using namespace snowdesktop::widget;

inline void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

inline void Write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    Require(static_cast<bool>(output), "source lookup fixture writes successfully");
}

inline void MakePackage(const std::filesystem::path& path,
    const std::string& id, const std::string& version = "1.0.0")
{
    Write(path / L"widget.json",
        "{\"schemaVersion\":2,\"apiVersion\":2,\"dataVersion\":1,"
        "\"id\":\"" + id + "\",\"slug\":\"source-lookup\","
        "\"name\":\"Source lookup\",\"version\":\"" + version +
        "\",\"entry\":\"main.lua\",\"permissions\":[]}");
    Write(path / L"main.lua", "return widget.define({})");
}

inline std::optional<PackageSourceRef> Lookup(
    const WidgetPackageManager& manager, const std::string& id)
{
    return PackageSourceSnapshot(manager.ListPackages()).Find(id);
}

inline void ExpectSource(const WidgetPackageManager& manager,
    const std::string& id, const PackageSourceRef& expected)
{
    Require(Lookup(manager, id) == std::optional(expected),
        "source snapshot preserves the independently expected provider and external item ID");
}

inline void TestSelectionAndCapture()
{
    const auto candidate = [](const char* id, PackageSourceRef source,
                              bool active, bool enabled) {
        InstalledPackage package;
        package.manifest.id = id;
        package.source = std::move(source);
        package.active = active;
        package.enabled = enabled;
        return package;
    };
    const std::vector<InstalledPackage> packages{
        candidate("fallback", {"local", "first"}, false, true),
        candidate("fallback", {"local", "ignored"}, false, true),
        candidate("fallback", {"local", "active-before"}, true, true),
        candidate("fallback", {"local", "active"}, true, true),
        candidate("fallback", {"local", "disabled"}, true, false),
        candidate("fallback", {"", "incomplete"}, true, true),
        candidate("fallback", {"local", ""}, true, true),
        candidate("first", {"local", "first"}, false, false),
        candidate("first", {"local", "second"}, false, true),
        candidate("workshop", {"local", "active"}, true, true),
        candidate("workshop", {"steam-workshop", "123@42"}, false, false),
        candidate("workshop", {"steam-workshop", "456@99"}, true, true),
        candidate("invalid", {"steam-workshop", "invalid"}, false, true),
        candidate("invalid", {"local-directory", "active"}, true, true)};
    PackageSourceSnapshot snapshot(packages);
    Require(snapshot.Find("fallback") == std::optional(PackageSourceRef{"local", "active"}) &&
        snapshot.Find("first") == std::optional(PackageSourceRef{"local", "first"}),
        "fallback chooses the last enabled active candidate or the first complete source");
    Require(snapshot.Find("workshop") == std::optional(PackageSourceRef{"steam-workshop", "123@42"}),
        "the first valid Workshop source wins even while inactive or disabled");
    Require(snapshot.Find("invalid") == std::optional(PackageSourceRef{"local-directory", "active"}) &&
        !snapshot.Find("") && !snapshot.Find("missing"),
        "invalid Workshop IDs remain fallback candidates and unknown IDs stay absent");
    auto copied = snapshot;
    auto moved = std::move(copied);
    Require(moved.Find("workshop") == snapshot.Find("workshop"),
        "snapshot copy and move retain owned source strings");

    // These seams exercise field update and batch capture. Host callers keep
    // their original UTF converters and Steam URL builder.
    const auto encode = [](const std::wstring& value) {
        std::string result;
        for (const wchar_t character : value) result.push_back(static_cast<char>(character));
        return result;
    };
    const auto decode = [](const std::string& value) {
        return std::wstring(value.begin(), value.end());
    };
    const auto itemUrl = [&](const std::string& id) {
        return L"https://steamcommunity.com/sharedfiles/filedetails/?id=" + decode(id);
    };
    DesktopWidget widget;
    widget.id = L"lookup-widget";
    widget.type = DesktopWidgetType::LuaScript;
    widget.packageId = L"workshop";
    snowdesktop::widget_source_capture::Apply(widget, snapshot.Find("workshop"),
        encode, decode, itemUrl);
    Require(widget.packageSourceProvider == L"steam-workshop" &&
        widget.packageSourceExternalItemId == L"123@42" &&
        widget.packageSourceUrl == L"https://steamcommunity.com/sharedfiles/filedetails/?id=123",
        "capture stores the full external identity and builds the recovery URL from its published ID");
    widget.packageSourceUrl = L"remembered-url";
    snowdesktop::widget_source_capture::Apply(widget, snapshot.Find("fallback"), encode, decode, itemUrl);
    Require(widget.packageSourceExternalItemId == L"123@42" && widget.packageSourceUrl == L"remembered-url",
        "local development fallback preserves a remembered valid Workshop recovery address");
    snowdesktop::widget_source_capture::Apply(widget, std::nullopt, encode, decode, itemUrl);
    Require(widget.packageSourceUrl == L"remembered-url",
        "missing packages preserve existing Workshop recovery fields");
    snowdesktop::widget_source_capture::Apply(widget,
        std::optional(PackageSourceRef{"steam-workshop", "invalid"}), encode, decode, itemUrl);
    Require(widget.packageSourceExternalItemId == L"invalid" && widget.packageSourceUrl.empty(),
        "invalid Workshop sources clear the URL without changing stored source identity");
    snowdesktop::widget_source_capture::Apply(widget, snapshot.Find("fallback"), encode, decode, itemUrl);
    Require(widget.packageSourceProvider.empty() && widget.packageSourceExternalItemId.empty() &&
        widget.packageSourceUrl.empty(), "ordinary local sources clear provider-specific noise");
    std::vector<DesktopWidget> widgets(5);
    widgets[0].type = DesktopWidgetType::LuaScript;
    widgets[0].packageId = L"workshop";
    widgets[1].type = DesktopWidgetType::LuaScript;
    widgets[1].packageId = L"fallback";
    widgets[2].type = DesktopWidgetType::LuaScript;
    widgets[2].packageId = L"workshop";
    widgets[3].type = DesktopWidgetType::Collection;
    widgets[3].packageId = L"workshop";
    widgets[3].packageSourceUrl = L"leave-non-Lua";
    widgets[4].type = DesktopWidgetType::LuaScript;
    widgets[4].packageSourceUrl = L"leave-empty-ID";
    int catalogReads = 0;
    const auto listPackages = [&] { ++catalogReads; return packages; };
    snowdesktop::widget_source_capture::CaptureForSave(
        widgets, listPackages, encode, decode, itemUrl);
    Require(catalogReads == 1 &&
        widgets[0].packageSourceExternalItemId == L"123@42" &&
        widgets[2].packageSourceExternalItemId == L"123@42" &&
        widgets[1].packageSourceProvider.empty() &&
        widgets[3].packageSourceUrl == L"leave-non-Lua" &&
        widgets[4].packageSourceUrl == L"leave-empty-ID",
        "the production save capture reads the catalogue once and preserves non-Lua and empty-ID records");
    std::vector<DesktopWidget> withoutPackages{widgets[3], widgets[4]};
    snowdesktop::widget_source_capture::CaptureForSave(
        withoutPackages, listPackages, encode, decode, itemUrl);
    Require(catalogReads == 1, "saves without package IDs never read the catalogue");
}

inline void Run()
{
    TestSelectionAndCapture();
    snowdesktop::test::TemporaryDirectory temporary;
    PackagePaths paths;
    paths.builtin = temporary.path / L"builtin";
    paths.installed = temporary.path / L"installed";
    paths.development = temporary.path / L"dev";
    paths.staging = temporary.path / L"staging";
    paths.quarantine = temporary.path / L"quarantine";
    paths.migrations = temporary.path / L"migrations";
    paths.registry = temporary.path / L"packages.json";
    const std::string id = "3fbb18cd-7c46-4a9f-9fe3-3e2c19facb23";
    const std::string laterId = "29d5fca2-c367-49d7-8b05-2b49b7ef9efc";
    const auto builtin = paths.builtin / L"builtin-copy";
    const auto development = paths.development / L"dev-copy";
    const auto source = temporary.path / L"source";
    WidgetPackageManager manager(paths);
    std::string error;
    Require(manager.Initialize(error) && !Lookup(manager, id), "empty lookup catalogue initializes");
    MakePackage(builtin, id);
    MakePackage(development, id);
    Require(manager.RefreshCatalog(error), "external sources are discovered");
    ExpectSource(manager, id, {"local-directory", "dev-copy"});
    Require(manager.SetDevelopmentOverride(id, false, error), "development can be deactivated");
    ExpectSource(manager, id, {"builtin", "builtin-copy"});
    MakePackage(source, id);
    InstalledPackage installed;
    ValidationReport report;
    Require(manager.InstallDirectory(source, {"steam-workshop", "123456789@42"},
        false, installed, report, error), "Workshop fixture installs");
    Require(manager.SetDevelopmentOverride(id, true, error), "development can override managed execution");
    ExpectSource(manager, id, {"steam-workshop", "123456789@42"});
    Require(manager.SetEnabled(id, false, error), "managed package can be disabled");
    ExpectSource(manager, id, {"steam-workshop", "123456789@42"});

    PackageSourceSnapshot previousSave(manager.ListPackages());
    const auto returnedSource = previousSave.Find(id);
    Require(manager.Uninstall(id, error), "managed source uninstalls");
    ExpectSource(manager, id, {"local-directory", "dev-copy"});
    Require(previousSave.Find(id) == returnedSource &&
        returnedSource == std::optional(PackageSourceRef{"steam-workshop", "123456789@42"}),
        "one save owns a consistent source snapshot while a subsequent save sees removal");
    Require(manager.SetDevelopmentOverride(id, false, error), "builtin fallback is restored");
    Require(manager.InstallDirectory(source, {"steam-workshop", "invalid"},
        false, installed, report, error), "invalid published identity remains a fallback source");
    ExpectSource(manager, id, {"steam-workshop", "invalid"});
    MakePackage(source, id, "2.0.0");
    Require(manager.InstallDirectory(source, {"local", "new-source"},
        true, installed, report, error), "another source and managed version install");
    ExpectSource(manager, id, {"local", "new-source"});
    Require(manager.Rollback(id, "1.0.0", error), "managed version rolls back");
    ExpectSource(manager, id, {"local", "new-source"});

    const auto beforeFailure = Lookup(manager, id);
    struct RegistryLock
    {
        HANDLE handle;
        ~RegistryLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    };
    {
        RegistryLock lock{CreateFileW(paths.registry.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Require(lock.handle != INVALID_HANDLE_VALUE, "registry failure fixture locks file");
        MakePackage(paths.development / L"later", laterId);
        Require(!manager.RefreshCatalog(error) && !error.empty(),
            "discovery records a real registry replacement failure");
        Require(Lookup(manager, id) == beforeFailure && !Lookup(manager, laterId),
            "a failed rescan preserves the prior catalogue for the next save snapshot");
    }
    Require(manager.RefreshCatalog(error) && Lookup(manager, laterId).has_value(),
        "the next save sees new sources after discovery recovers");
    Require(manager.Uninstall(id, error), "managed lookup source uninstalls");
    Write(development / L"native.dll", "invalid package file");
    Require(manager.RefreshCatalog(error), "invalid package rescan completes");
    ExpectSource(manager, id, {"builtin", "builtin-copy"});
    std::filesystem::remove(development / L"native.dll");
    Require(manager.RefreshCatalog(error), "repaired package is rescanned");
    Require(manager.SetDevelopmentOverride(id, true, error), "repaired development source activates");
    ExpectSource(manager, id, {"local-directory", "dev-copy"});
    std::filesystem::remove_all(builtin);
    std::filesystem::remove_all(development);
    Require(manager.RefreshCatalog(error) && !Lookup(manager, id),
        "deleted packages cannot survive in a subsequent save snapshot");
}
}
