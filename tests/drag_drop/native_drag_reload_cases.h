#pragma once

namespace native_drag_reload_cases
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
using native_drag_commit_cases::Require;
using native_drag_commit_cases::Read;

inline snowdesktop::shell_refresh::Snapshot Snapshot(const std::vector<fs::path>& files,
    bool incremental = false)
{
    snowdesktop::shell_refresh::Snapshot snapshot;
    snapshot.desktopComplete = !incremental;
    snapshot.desktopIncremental = incremental;
    // A refresh may enumerate in a different order from the gesture. Partial
    // reads supply one observed identity; production must retain the others.
    for (auto it = files.rbegin(); it != files.rend(); ++it)
    {
        snapshot.desktopItems.push_back(Access::FileItem(*it));
        if (incremental) break;
    }
    return snapshot;
}

inline const DesktopItem& Find(const DesktopApp& app, const std::wstring& key)
{
    const auto& items = Access::Items(app);
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.layoutKey == key; });
    Require(found != items.end(), "native whole reload retains each captured stable file identity");
    return *found;
}

inline void Check(DesktopApp& app, const std::vector<std::wstring>& keys,
    const DropPreviewList& preview, int source, std::size_t count, std::size_t itemCount = 2)
{
    Require(Access::Items(app).size() == itemCount, "native whole reload cannot delete an unobserved file from an incremental snapshot");
    const auto page = std::find_if(Access::Pages(app).begin(), Access::Pages(app).end(),
        [](const auto& value) { return value.id == L"__page:1"; });
    Require(page != Access::Pages(app).end() && page->columns == 4 && page->rows == 3,
        "native whole reload keeps the saved page accessible with its established dimensions");
    Require(preview.landings.size() == count, "native reload scenario requires the previously accepted source set");
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& item = Find(app, keys[i]);
        const auto landing = std::find_if(preview.landings.begin(), preview.landings.end(),
            [&](const auto& value) { return value.sourceIndex == i; });
        Require(landing != preview.landings.end() && item.gridCell.pageId == landing->cell.pageId &&
            item.gridCell.column == landing->cell.column && item.gridCell.row == landing->cell.row,
            "native whole reload must retain the accepted file's captured page and cell by stable identity");
        Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
            "native whole reload must rebuild each committed file into exactly one matching visible runtime slot");
    }
    if (!source && count == 1)
    {
        const auto& other = Find(app, keys[1]);
        Require(other.gridCell.pageId == L"__page:1" && other.gridCell.column == 2 && other.gridCell.row == 0 &&
            Access::HasVisibleIcon(app, other) && Access::HasMatchingRuntimeSlot(app, other),
            "native whole reload preserves the unrelated free desktop identity and its literal original position");
    }
    const auto& widgets = Access::Widgets(app);
    if (source)
    {
        Require(widgets.size() == 1 && widgets[0].id == L"native-origin" &&
            widgets[0].type == (source == 1 ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories) &&
            widgets[0].itemKeys.size() == 2 - count && !widgets[0].autoCollect &&
            widgets[0].gridCell.pageId == L"__page:1" && widgets[0].gridCell.column == 0 && widgets[0].gridCell.row == 0 &&
            widgets[0].gridSpan.columns == 1 && widgets[0].gridSpan.rows == 1,
            "native whole reload preserves the source component identity, position and selective membership removal");
        if (count == 1)
            Require(widgets[0].itemKeys[0] == keys[1] && !Access::HasVisibleIcon(app, Find(app, keys[1])),
                "native whole reload preserves the remaining logical member order without a desktop duplicate");
    }
    else Require(widgets.empty(), "native whole reload cannot invent a component for free desktop movement");
}

inline void Run()
{
    native_drag_commit_cases::Profile profile;
    std::size_t cases = 0;
    for (const bool secondary : {false, true})
        for (const int source : {0, 1, 2})
            for (const std::size_t count : {1u, 2u})
            {
                auto& context = native_drag_commit_cases::context;
                const auto scenario = "reload source=" + std::to_string(source) + " count=" + std::to_string(count) +
                    " secondary=" + std::to_string(secondary);
                context = scenario + " phase=commit";
                const auto directory = profile.root / (L"reload-" + std::to_wstring(cases++));
                const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
                const std::vector<fs::path> files{desktop / L"A.txt", desktop / L"B.txt"};
                for (std::size_t i = 0; i < files.size(); ++i)
                { std::ofstream stream(files[i], std::ios::binary); stream << "reload-file-" << i; }
                auto& app = profile.Model(secondary, directory / L"layout");
                Access::PopulateModel(app, files, source);
                Access::PrepareReload(app);
                Require(Access::Save(app), "save the actual native pre-drag model before the reload scenario");
                std::vector<std::wstring> keys;
                for (const auto& item : Access::Items(app)) keys.push_back(item.layoutKey);
                Access::BeginModel(app, source, count);
                const auto preview = Access::ObserveModelPreview(app, {secondary ? -650L : 150L, 150});
                Access::DropModel(app);
                Check(app, keys, preview, source, count);

                context = scenario + " phase=memory-refresh";
                auto full = Snapshot(files);
                Access::Reload(app, full, false);
                Require(Access::Ready(app) && !Access::ReloadPending(app), "complete ordinary native reload must release the save barrier");
                Check(app, keys, preview, source, count);

                context = scenario + " phase=active-drag";
                const auto savedBeforeDrag = Read(Access::LayoutPath(app));
                Access::BeginDesktopGesture(app, keys[0]);
                const auto* runtime = Access::Desktop(app);
                auto deferred = Snapshot(files);
                Access::Reload(app, deferred, false);
                Require(Access::ShellReloadPending(app) && deferred.desktopItems.size() == 2 &&
                    Access::Desktop(app) == runtime && Read(Access::LayoutPath(app)) == savedBeforeDrag,
                    "native whole reload must defer snapshot consumption, runtime replacement and disk writes during an active drag");
                // End only the supplied gesture; drain the same unconsumed
                // snapshot through the real reload entry after its lifetime.
                Access::EndGesture(app);
                Access::Reload(app, deferred, false);
                Require(!Access::ShellReloadPending(app), "native deferred refresh must drain after drag ownership ends");
                Check(app, keys, preview, source, count);

                context = scenario + " phase=incremental-refresh";
                const auto saved = Read(Access::LayoutPath(app));
                auto partial = Snapshot(files, true);
                Access::Reload(app, partial, false);
                Require(!Access::Ready(app) && !Access::Save(app) && Read(Access::LayoutPath(app)) == saved,
                    "native incremental reload cannot overwrite the complete saved layout");
                Check(app, keys, preview, source, count);

                context = scenario + " phase=pending-disk-partial";
                auto diskPartial = Snapshot(files, true);
                Access::Reload(app, diskPartial, true);
                Require(Access::ReloadPending(app) && !Access::Ready(app) && !Access::Save(app) &&
                    Read(Access::LayoutPath(app)) == saved,
                    "native disk reload must preserve its write barrier until a complete model and settings synchronization");

                context = scenario + " phase=pending-disk-complete";
                auto complete = Snapshot(files);
                Access::Reload(app, complete, false);
                Require(Access::Ready(app) && !Access::ReloadPending(app) && Access::ReloadSettingsAgree(app),
                    "native disk reload completion must synchronize actual settings mirrors and release its write barrier");
                Check(app, keys, preview, source, count);

                context = scenario + " phase=disk-restoration";
                Access::PerturbBeforeDiskRead(app);
                auto restored = Snapshot(files);
                Access::Reload(app, restored, true);
                Require(Access::Ready(app) && !Access::ReloadPending(app) && Access::Save(app),
                    "native complete disk reload must finish rebuilding and permit a subsequent save");
                Check(app, keys, preview, source, count);

                context = scenario + " phase=newer-disk-positions";
                // A separate owner writes a newer document through actual
                // native persistence. This app still holds the old records;
                // restoring from its cache is insufficient evidence of a read.
                auto& writer = profile.Model(secondary, directory / L"layout");
                Access::Load(writer);
                Access::ReadSnapshot(writer, files, true);
                Require(Access::WriteReplacementPositions(writer, keys, count),
                    "a separate native owner must save the literal replacement positions");
                auto replacement = Snapshot(files);
                Access::Reload(app, replacement, true);
                Require(Access::Ready(app) && !Access::ReloadPending(app), "newer native disk records must complete the reload barrier");
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto& item = Find(app, keys[i]);
                    Require(item.gridCell.pageId == L"__page:1" && item.gridCell.column == static_cast<int>(i) * 2 + 1 &&
                        item.gridCell.row == 2 && Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                        "native disk reload must observe newer file positions instead of cached layout records");
                }
                Access::StopModel(writer);
                for (std::size_t i = 0; i < files.size(); ++i)
                    Require(Read(files[i]) == "reload-file-" + std::to_string(i),
                        "native whole reload cannot modify the original logical files");
                Access::StopModel(app);
                for (const auto& file : files) fs::remove(file);
            }
    std::cout << cases << " actual native whole-reload, drag-deferral, incremental and disk-restoration scenes passed\n";
}
}
