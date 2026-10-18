#include "desktop/desktop_membership_recovery.h"
#include "icons/icon_customization_lifecycle.h"
#include "desktop/desktop_item_reference_migration.h"
#include "data/layout_storage.h"
#include "widgets/widget_pair_drop.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace
{
namespace recovery = snowdesktop::desktop_membership_recovery;
namespace storage = snowdesktop::layout_storage;
constexpr std::int64_t kStart = 1800000000;
const std::wstring a = L"C:\\DESKTOP\\A.LNK";
const std::wstring b = L"C:\\DESKTOP\\B.LNK";
const std::wstring c = L"C:\\DESKTOP\\C.LNK";
const std::wstring d = L"C:\\DESKTOP\\D.LNK";
const std::wstring e = L"C:\\DESKTOP\\E.LNK";
const std::wstring f = L"C:\\DESKTOP\\F.LNK";
int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

DesktopWidget Collection(std::vector<std::wstring> keys, const wchar_t* id = L"collection")
{
    DesktopWidget widget;
    widget.id = id;
    widget.itemKeys = std::move(keys);
    return widget;
}

// Replace only desktop enumeration and wall time. Exercise the same model
// transaction used before automatic collection and free-desktop placement.
bool Refresh(std::vector<DesktopWidget>& widgets,
    const std::vector<std::wstring>& keys, std::int64_t now = kStart,
    const std::vector<DockEntry>& dock = {}, bool complete = true)
{
    std::vector<DesktopItem> snapshot;
    for (const auto& key : keys)
    {
        DesktopItem item;
        item.layoutKey = item.parsingName = key;
        snapshot.push_back(std::move(item));
    }
    return recovery::Reconcile(widgets, snapshot, dock, now, complete);
}

std::string Utf8(const std::wstring& value)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& value)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string Document(const DesktopWidget& widget)
{
    std::vector<storage::MissingItemRecord> missing;
    for (const auto& item : widget.missingItems)
        missing.push_back({Utf8(item.key),
            static_cast<int>(item.index), item.missingSince});
    std::ostringstream out;
    out << R"({"widgets":[{"id":"collection","type":"collection","page":"primary","x":0,"y":0,"items":[)";
    for (size_t index = 0; index < widget.itemKeys.size(); ++index)
    {
        if (index) out << ',';
        const auto& key = widget.itemKeys[index];
        out << std::quoted(Utf8(key));
    }
    out << "],\"missingItems\":" << storage::SerializeMissingItems(missing) << "}]}";
    return out.str();
}

DesktopWidget Restore(const storage::WidgetRecord& saved)
{
    auto widget = Collection({});
    for (const auto& key : saved.items)
        widget.itemKeys.push_back(Wide(key));
    for (const auto& item : saved.missingItems)
        widget.missingItems.push_back({Wide(item.key),
            static_cast<size_t>(item.index), item.missingSince});
    return widget;
}

void TestUpdateGapAndRestart(const std::filesystem::path& root)
{
    std::vector<DesktopWidget> widgets{Collection({a, b, c, d})};
    Expect(Refresh(widgets, {a, c, d}) &&
        widgets[0].itemKeys == std::vector<std::wstring>{a, c, d} &&
        widgets[0].missingItems == std::vector<MissingDesktopWidgetItem>{{b, 1, kStart}},
        "an installer gap hides the missing shortcut without losing its collection index");
    Refresh(widgets, {d, a}, kStart + 30);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, d} &&
        widgets[0].missingItems == std::vector<MissingDesktopWidgetItem>{{b, 1, kStart}, {c, 2, kStart + 30}},
        "successive gaps retain independent first-missing times and logical order");
    Refresh(widgets, {a, c, d}, kStart + 60);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, c, d} &&
        widgets[0].missingItems == std::vector<MissingDesktopWidgetItem>{{b, 1, kStart}},
        "shortcuts returning in reverse order do not swap positions or reset the remaining deadline");

    const auto path = root / L"membership-recovery" / L"layout.json";
    Expect(storage::SaveDocument(path, Document(widgets[0])),
        "the production atomic layout writer accepts missing ownership metadata");
    storage::Document saved;
    const auto loaded = storage::LoadDocument(path, saved);
    Expect(loaded.status == storage::LoadStatus::LoadedPrimary && saved.widgets.size() == 1,
        "the restart fixture loads the persisted collection from disk");
    if (saved.widgets.size() != 1) return;
    widgets = {Restore(saved.widgets[0])};
    // A startup partial read is allowed to recover observed cached members.
    Refresh(widgets, {L"c:\\desktop\\b.lnk"}, kStart + 90, {}, false);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, b, c, d} && widgets[0].missingItems.empty(),
        "same-path replacement after a restart returns to the original position, case insensitively");
    Expect(!Refresh(widgets, {d, c, b, a}, kStart + 100) && widgets[0].missingItems.empty(),
        "repeated refresh and different enumeration order cannot duplicate restored collection members");

    widgets = {Collection({a, b, c})};
    Refresh(widgets, {a, c});
    const std::wstring renamed = L"C:\\DESKTOP\\B-NEW.LNK";
    Refresh(widgets, {a, renamed, c}, kStart + 10);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, c} && widgets[0].missingItems.size() == 1,
        "a different shortcut path is not guessed to be the cached application");
}

void TestDeadlinesAndPartialReads()
{
    std::vector<DesktopWidget> widgets{Collection({a, b, c})};
    Refresh(widgets, {a}, kStart, {}, false);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, b, c} && widgets[0].missingItems.empty(),
        "an incremental snapshot cannot turn unobserved live members into deleted items");
    Refresh(widgets, {a, c});
    Refresh(widgets, {a, c}, kStart + recovery::kRetentionSeconds - 1);
    Expect(widgets[0].missingItems.size() == 1 && widgets[0].missingItems[0].missingSince == kStart,
        "repeated scans never extend a missing shortcut's 24-hour retention period");
    auto beforeDeadline = widgets;
    Refresh(beforeDeadline, {a, b, c}, kStart + recovery::kRetentionSeconds - 1);
    Expect(beforeDeadline[0].itemKeys == std::vector<std::wstring>{a, b, c},
        "a replacement just before the deadline recovers its collection");
    Refresh(widgets, {a, b, c}, kStart + recovery::kRetentionSeconds);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, c} && widgets[0].missingItems.empty(),
        "a replacement at or after the deadline remains a free-desktop item");

    widgets = {Collection({a, b, c, d})};
    Refresh(widgets, {a, c, d});
    Refresh(widgets, {a, d}, kStart + 60);
    Expect(recovery::PruneExpired(widgets, kStart + recovery::kRetentionSeconds) &&
        widgets[0].missingItems == std::vector<MissingDesktopWidgetItem>{{c, 1, kStart + 60}},
        "maintenance removes expired metadata and closes its logical-order hole");
    Refresh(widgets, {a, c, d}, kStart + recovery::kRetentionSeconds + 1);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, c, d},
        "an expired earlier gap does not displace a later restored member");

    widgets = {Collection({a, b})};
    Refresh(widgets, {a});
    recovery::PruneExpired(widgets, kStart - 60);
    Expect(widgets[0].missingItems.size() == 1 && widgets[0].missingItems[0].missingSince == kStart - 60,
        "a backward wall-clock adjustment rebases a future deadline without losing ownership");
}

void TestExplicitOwnershipChanges()
{
    std::vector<DesktopWidget> widgets{Collection({a, b, c})};
    Refresh(widgets, {a, c});
    recovery::Forget(widgets[0], {L"c:\\desktop\\b.lnk", a});
    Refresh(widgets, {a, b, c}, kStart + 1);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{c} && widgets[0].missingItems.empty(),
        "explicit removal revokes live and cached membership so refresh cannot undo user intent");
    // This is the same revocation used after successful host delete/move operations.
    recovery::Forget(widgets[0], {c});
    Refresh(widgets, {a, b}, kStart + 2);
    Refresh(widgets, {a, b, c}, kStart + 3);
    Expect(widgets[0].itemKeys.empty() && widgets[0].missingItems.empty(),
        "a deliberately deleted member has no recovery record when its path is later recreated");

    widgets = {Collection({a, b}), Collection({}, L"other")};
    Refresh(widgets, {a});
    widgets[1].itemKeys = {b};
    Refresh(widgets, {a, b}, kStart + 1);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a} && widgets[0].missingItems.empty() &&
        widgets[1].itemKeys == std::vector<std::wstring>{b},
        "an application manually collected elsewhere cannot be stolen by its previous collection");

    DockEntry pin;
    pin.type = DockEntryType::DesktopItem;
    pin.reference = b;
    widgets = {Collection({a, b})};
    Refresh(widgets, {a});
    auto mapping = widgets;
    Refresh(widgets, {a, b}, kStart + 1, {pin});
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a} && widgets[0].missingItems.empty(),
        "a Dock-owned replacement cancels its stale collection claim");
    pin.keepOnDesktop = true;
    Refresh(mapping, {a, b}, kStart + 1, {pin});
    Expect(mapping[0].itemKeys == std::vector<std::wstring>{a, b},
        "a Dock mapping which retains desktop ownership does not block collection recovery");

    widgets = {Collection({a, b})};
    Refresh(widgets, {a});
    std::vector<DockEntry> dock;
    const auto renamed = L"C:\\DESKTOP\\RENAMED.LNK";
    const auto migration = snowdesktop::desktop_item_reference_migration::MigrateReferences(
        widgets, dock, b, renamed);
    Refresh(widgets, {a, renamed}, kStart + 1);
    Expect(migration.widgetReferences == 1 && widgets[0].itemKeys == std::vector<std::wstring>{a, renamed},
        "an explicit host rename migrates cached ownership using the confirmed old and new paths");

    widgets = {Collection({a, b})};
    widgets[0].type = DesktopWidgetType::FolderMapping;
    Refresh(widgets, {});
    recovery::Forget(widgets[0], {a});
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, b} && widgets[0].missingItems.empty(),
        "mapped-folder content remains outside desktop ownership recovery and revocation");
}

void TestMergeAndCollectionDeletion()
{
    std::vector<DesktopWidget> widgets{Collection({d, e, f}, L"source"), Collection({a, b, c}, L"target")};
    Refresh(widgets, {a, c, d, f});
    std::vector<DockEntry> dock;
    const auto normalize = [](const std::wstring& key) { return key; };
    Expect(snowdesktop::widget_pair_drop::Apply(widgets, dock, 0, 1,
        snowdesktop::widget_pair_drop::Action::Merge, {}, normalize),
        "the production collection merge accepts the two collections with hidden members");
    Expect(widgets.size() == 1 && widgets[0].itemKeys == std::vector<std::wstring>{a, c, d, f},
        "a merge continues to expose only the existing visible members");
    Refresh(widgets, {a, b, c, d, e, f}, kStart + 1);
    Expect(widgets[0].itemKeys == std::vector<std::wstring>{a, b, c, d, e, f} && widgets[0].missingItems.empty(),
        "merging collections transfers missing-member ownership and both original relative positions");
    Refresh(widgets, {a, c, d, e, f}, kStart + 2);
    widgets.clear();
    Refresh(widgets, {a, b, c, d, e, f}, kStart + 3);
    Expect(widgets.empty(), "deleting a collection also deletes its pending recovery claims");
}

void TestStorageContracts(const std::filesystem::path& root)
{
    {
        const std::string target = "item:C:\\Desktop\\易标\"工具.lnk";
        const auto pending = storage::SerializeMissingIconTargets({{target, 2200000000}});
        storage::Document parsed;
        Expect(storage::ParseDocument("{}", parsed) && parsed.missingIconTargets.empty(),
            "legacy layouts do not invent icon-customization deletion deadlines");
        Expect(storage::ParseDocument("{\"missingIconTargets\":" + pending + "}", parsed) &&
            parsed.missingIconTargets.size() == 1 && parsed.missingIconTargets[0].target == target &&
            parsed.missingIconTargets[0].missingSince == 2200000000,
            "icon-customization deadlines round-trip Unicode, quoting and post-2038 timestamps");
        const auto guarded = root / L"icon-customization-retention.json";
        Expect(storage::SaveDocument(guarded, "{\"missingIconTargets\":" + pending + "}"),
            "pending customization deadlines use the atomic layout save path");
        for (const auto* invalid : {"null", "[1]", R"([{"target":"item:a","missingSince":0}])",
            R"([{"target":"item:a","missingSince":-1}])", R"([{"target":"item:a","missingSince":1.5}])",
            R"([{"target":"item:a","missingSince":"42"}])", R"([{"target":"item:a","missingSince":9007199254740992}])",
            R"([{"target":"widget:","missingSince":42}])",
            R"([{"target":"item:a","missingSince":42},{"target":"item:a","missingSince":43}])"})
            Expect(!storage::SaveDocument(guarded, std::string("{\"missingIconTargets\":") + invalid + "}"),
                "malformed or duplicate customization deadlines cannot replace valid saved state");
        Expect(storage::LoadDocument(guarded, parsed).status == storage::LoadStatus::LoadedPrimary &&
            parsed.missingIconTargets.size() == 1 && parsed.missingIconTargets[0].missingSince == 2200000000,
            "restarting after a rejected save keeps the original customization expiry");
        namespace lifecycle = snowdesktop::icon_customization;
        const auto source = root / L"restored-icon-source.tmp";
        { std::ofstream file(source); file << "source"; }
        Expect(lifecycle::Probe(source) == lifecycle::Presence::Present,
            "the worker observes an existing file independently of folder enumeration");
        std::filesystem::remove(source);
        Expect(lifecycle::Probe(source) == lifecycle::Presence::Missing,
            "the worker confirms deletion of a source file on accessible local storage");
        Expect(lifecycle::Probe(root / L"removed-tree" / L"source.tmp") == lifecycle::Presence::Missing,
            "deleting the whole containing folder also starts expiry for its customizations");
    }
    const std::string prefix = R"({"widgets":[{"id":"collection","page":"primary","x":0,"y":0,"missingItems":)";
    const std::string suffix = "}]}";
    storage::Document decoded;
    Expect(storage::ParseDocument(R"({"widgets":[{"id":"old","page":"primary","x":0,"y":0,"items":["old.lnk"]}]})", decoded) &&
        decoded.widgets.size() == 1 && decoded.widgets[0].missingItems.empty(),
        "legacy layouts load without a migration or an invented missing deadline");
    const std::string unicodeKey = "C:\\Desktop\\易标\"工具.lnk";
    const auto metadata = storage::SerializeMissingItems({{unicodeKey, 2, 2200000000}});
    Expect(storage::ParseDocument(prefix + metadata + suffix, decoded) &&
        decoded.widgets.size() == 1 && decoded.widgets[0].missingItems.size() == 1 &&
        decoded.widgets[0].missingItems[0].key == unicodeKey &&
        decoded.widgets[0].missingItems[0].missingSince == 2200000000,
        "Unicode paths, quoting and timestamps beyond 2038 round-trip in recovery metadata");

    const auto path = root / L"membership-recovery" / L"guarded.json";
    const auto valid = prefix + metadata + suffix;
    Expect(storage::SaveDocument(path, valid), "the guarded layout fixture starts with valid ownership data");
    for (const std::string invalid : {"null", "[1]", R"([{"key":"app.lnk","index":0}])",
             R"([{"key":"","index":0,"missingSince":1800000000}])",
             R"([{"key":"app.lnk","index":-1,"missingSince":1800000000}])",
             R"([{"key":"app.lnk","index":0.5,"missingSince":1800000000}])",
             R"([{"key":"app.lnk","index":0,"missingSince":0}])",
             R"([{"key":"app.lnk","index":0,"missingSince":-1}])",
             R"([{"key":"app.lnk","index":0,"missingSince":1.5}])",
             R"([{"key":"app.lnk","index":0,"missingSince":"1800000000"}])",
             R"([{"key":"app.lnk","index":0,"missingSince":9007199254740992}])"})
    {
        Expect(!storage::SaveDocument(path, prefix + invalid + suffix),
            "malformed recovery metadata is rejected before replacing the existing layout");
    }
    Expect(storage::LoadDocument(path, decoded).status == storage::LoadStatus::LoadedPrimary &&
        decoded.widgets.size() == 1 && decoded.widgets[0].missingItems.size() == 1 &&
        decoded.widgets[0].missingItems[0].key == unicodeKey,
        "rejected recovery metadata cannot destroy the prior valid collection");
    std::string cleared;
    Expect(storage::BuildClearedDocument(valid, cleared) && storage::ParseDocument(cleared, decoded) &&
        decoded.widgets.empty(), "clearing the layout also clears all persistent membership recovery data");
}
}

int RunDesktopMembershipRecoveryTests(const std::filesystem::path& root)
{
    failures = 0;
    TestUpdateGapAndRestart(root);
    TestDeadlinesAndPartialReads();
    TestExplicitOwnershipChanges();
    TestMergeAndCollectionDeletion();
    TestStorageContracts(root);
    return failures;
}
