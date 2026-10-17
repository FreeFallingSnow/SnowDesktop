#pragma once

#include "placement_cases.h"
#include "layout/widget_displacement.h"
#include "data/layout_geometry_codec.h"
#include <map>
#include <set>
#include <tuple>

namespace widget_displacement_cases
{
using namespace drop_placement_cases;

inline std::map<std::filesystem::path, std::string> Files(const std::filesystem::path& root)
{
    std::map<std::filesystem::path, std::string> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        files[entry.path().lexically_relative(root)] = entry.is_directory() ? "<directory>" : Bytes(entry.path());
    return files;
}
inline std::string Widgets(const std::vector<DesktopWidget>& widgets)
{
    std::ostringstream stream;
    for (const auto& widget : widgets)
    {
        widget_layout_codec::Write(stream, widget,
            [](const auto& value) { return snowdesktop::json::EscapeString(Encode(value)); }, Encode);
        stream << widget.selected;
    }
    return stream.str();
}

struct World
{
    std::vector<DesktopWidget> widgets;
    std::vector<DesktopItem> items;
    std::vector<GridPage> pages;
    std::vector<size_t> order{0, 1};
    std::vector<std::wstring> savedIds;
    std::unordered_map<std::wstring, int> columns, rows;
    std::unordered_map<std::wstring, std::wstring> names;
    std::filesystem::path root;
    bool persist = true, reverseItems = false;
    int commits = 0, generated = 0;

    explicit World(std::filesystem::path path) : root(std::move(path))
    {
        std::filesystem::create_directories(root / L"sources" / L"mapped");
        std::ofstream(root / L"sources" / L"mapped" / L"inside.txt") << "mapped bytes";
        pages.resize(2);
        pages[0].id = L"left"; pages[0].columns = 8; pages[0].rows = 8;
        pages[1].id = L"right"; pages[1].columns = 6; pages[1].rows = 6;
        for (const auto& page : pages) { savedIds.push_back(page.id); columns[page.id] = page.columns; rows[page.id] = page.rows; }
        names[L"left"] = L"Named page";
    }
    bool Grouped(const DesktopWidget& widget) const
    {
        return std::any_of(widgets.begin(), widgets.end(), [&](const auto& group) {
            return std::find(group.childWidgetIds.begin(), group.childWidgetIds.end(), widget.id) != group.childWidgetIds.end();
        });
    }
    bool Owned(const DesktopItem& item) const
    {
        return std::any_of(widgets.begin(), widgets.end(), [&](const auto& widget) {
            return widget.type != DesktopWidgetType::FolderMapping && std::find(widget.itemKeys.begin(), widget.itemKeys.end(), item.layoutKey) != widget.itemKeys.end();
        });
    }
    void Item(const std::wstring& name, GridCell cell, GridSpan span = {1, 1})
    {
        const auto path = root / L"sources" / name;
        std::ofstream(path) << "original-" << items.size();
        DesktopItem item;
        item.name = name; item.parsingName = path.wstring(); item.layoutKey = Key(path.wstring()); item.typeName = L"original type";
        item.gridCell = std::move(cell); item.gridSpan = span; item.slot = widget_displacement::Slot(pages, item.gridCell);
        item.fileSize = std::filesystem::file_size(path); item.selected = items.size() % 2 == 0; item.isCut = true;
        items.push_back(std::move(item));
    }
    std::string Metadata() const
    {
        std::ostringstream stream;
        for (const auto& item : items)
        {
            stream << Encode(item.name) << Encode(item.parsingName) << Encode(item.layoutKey) << Encode(item.typeName)
                << item.fileSize.value_or(0) << item.selected << item.isCut << item.gridSpan.columns << item.gridSpan.rows;
            if (item.largeIcon) stream << EncodeLargeIconConfig(*item.largeIcon);
        }
        return stream.str();
    }
    std::vector<std::optional<LargeIconConfig>> LargeConfigs() const
    {
        std::vector<std::optional<LargeIconConfig>> result;
        for (const auto& item : items) result.push_back(item.largeIcon);
        return result;
    }
    std::string Snapshot() const
    {
        std::ostringstream stream;
        stream << Widgets(widgets) << Metadata();
        for (const auto& item : items) stream << Encode(item.gridCell.pageId) << ':' << item.gridCell.column << ':' << item.gridCell.row << ':' << item.slot << ';';
        for (const auto& id : savedIds) stream << Encode(id) << ';';
        for (const auto& [id, value] : std::map<std::wstring, int>(columns.begin(), columns.end())) stream << Encode(id) << ':' << value << ';';
        for (const auto& [id, value] : std::map<std::wstring, int>(rows.begin(), rows.end())) stream << Encode(id) << ':' << value << ';';
        return stream.str();
    }
    std::vector<GridPage> AllPages() const
    {
        auto all = pages;
        for (const auto& id : savedIds)
            if (!widget_displacement::FindPage(all, id) && columns.contains(id) && rows.contains(id))
            {
                GridPage page; page.id = id; page.columns = columns.at(id); page.rows = rows.at(id); all.push_back(page);
            }
        return all;
    }
    void Legal() const
    {
        const auto all = AllPages();
        std::set<std::tuple<std::wstring, int, int>> occupied;
        const auto add = [&](const GridCell& cell, GridSpan span) {
            const auto page = std::find_if(all.begin(), all.end(), [&](const auto& value) { return value.id == cell.pageId; });
            Require(page != all.end() && span.columns > 0 && span.rows > 0,
                "successful component placement keeps every visible object within persisted pages without overlap");
            // Independent occupancy oracle: enumerate cells, rather than ask
            // the production Fits/Occupied helpers to grade their own output.
            for (int column = 0; column < span.columns; ++column)
                for (int row = 0; row < span.rows; ++row)
                    Require(cell.column + column >= 0 && cell.column + column < page->columns &&
                        cell.row + row >= 0 && cell.row + row < page->rows &&
                        occupied.emplace(cell.pageId, cell.column + column, cell.row + row).second,
                        "successful component placement keeps every visible object within persisted pages without overlap");
        };
        for (const auto& widget : widgets) if (!Grouped(widget)) add(widget.gridCell, widget.gridSpan);
        for (const auto& item : items) if (!item.name.empty() && !Owned(item)) add(item.gridCell, item.gridSpan);
    }
    void Save()
    {
        auto escape = [](const auto& value) { return snowdesktop::json::EscapeString(Encode(value)); };
        std::ostringstream stream;
        stream << "{\"schemaVersion\":1,\"pages\":[";
        for (size_t index = 0; index < savedIds.size(); ++index)
        {
            if (index) stream << ',';
            const auto& id = savedIds[index];
            const auto* page = widget_displacement::FindPage(pages, id);
            layout_geometry_codec::WritePage(stream, id, page ? page->monitorId : L"",
                columns.at(id), rows.at(id), names.contains(id) ? &names.at(id) : nullptr, escape);
        }
        stream << "],\"widgets\":[";
        for (size_t index = 0; index < widgets.size(); ++index)
        {
            if (index) stream << ',';
            widget_layout_codec::Write(stream, widgets[index], escape, Encode);
        }
        stream << "],\"items\":[";
        bool first = true;
        for (size_t index = 0; index < items.size(); ++index)
        {
            const auto& item = items[reverseItems ? items.size() - 1 - index : index];
            if (Owned(item)) continue;
            if (!first) stream << ',';
            first = false;
            layout_geometry_codec::WriteItem(stream, item, escape);
        }
        stream << "]}";
        Require(layout_storage::SaveDocument(root / L"layout.json", stream.str()), "persist actual component and displaced-item geometry atomically");
    }
    void Reload() const
    {
        layout_storage::Document document;
        Require(layout_storage::LoadDocument(root / L"layout.json", document).status == layout_storage::LoadStatus::LoadedPrimary,
            "read actual component placement, desktop item and page records");
        Require(document.pages.size() == savedIds.size() && document.widgets.size() == widgets.size(),
            "reload retains each original component and every published page");
        for (const auto& page : document.pages)
        {
            Require(page.columns == columns.at(Decode(page.id)) && page.rows == rows.at(Decode(page.id)),
                "published page dimensions survive real layout storage");
            const auto name = names.find(Decode(page.id));
            Require(name == names.end() ? !page.name : page.name == Encode(name->second),
                "geometry persistence preserves named pages and absent names");
        }
        size_t expectedItems = 0;
        for (const auto& item : items)
        {
            if (Owned(item)) continue;
            ++expectedItems;
            const auto saved = std::find_if(document.items.begin(), document.items.end(), [&](const auto& record) { return Decode(record.key) == item.layoutKey; });
            Require(saved != document.items.end(), "displaced or stationary item retains its original persisted identity");
            const auto restored = layout_geometry_codec::RestoreItem(*saved, Decode,
                [&](const auto& cell) { return widget_displacement::Slot(pages, cell); });
            Require(restored.hasGrid && restored.cell.pageId == item.gridCell.pageId && restored.cell.column == item.gridCell.column &&
                restored.cell.row == item.gridCell.row && restored.span.columns == item.gridSpan.columns && restored.span.rows == item.gridSpan.rows &&
                restored.largeIcon.has_value() == item.largeIcon.has_value(),
                "desktop item page, exact cell, footprint and large-icon state survive reload");
            if (item.largeIcon) Require(*restored.largeIcon == *item.largeIcon,
                "component displacement does not rewrite large-icon configuration during storage");
        }
        Require(document.items.size() == expectedItems, "reload neither duplicates nor loses desktop item identities");
        for (const auto& widget : widgets)
        {
            const auto saved = std::find_if(document.widgets.begin(), document.widgets.end(), [&](const auto& record) { return Decode(record.id) == widget.id; });
            Require(saved != document.widgets.end(), "component identity survives displacement storage");
            DesktopWidget restored;
            widget_layout_codec::RestorePlacement(*saved, restored, Decode);
            widget_layout_codec::RestoreMembers(*saved, restored, Decode, Key);
            Require(restored.gridCell.pageId == widget.gridCell.pageId && restored.gridCell.column == widget.gridCell.column &&
                restored.gridCell.row == widget.gridCell.row && restored.gridSpan.columns == widget.gridSpan.columns &&
                restored.gridSpan.rows == widget.gridSpan.rows && restored.itemKeys == widget.itemKeys &&
                restored.childWidgetIds == widget.childWidgetIds && restored.activeCategoryId == widget.activeCategoryId,
                "component landing and logical members survive the same persisted layout as displaced files");
        }
    }
    bool Place(GridCell cell, GridSpan span, bool move = true, size_t index = 0)
    {
        widget_displacement::Services services;
        services.grouped = [&](const auto& widget) { return Grouped(widget); };
        services.owned = [&](const auto& item) { return Owned(item); };
        services.generatePageId = [&] { return L"created-" + std::to_wstring(++generated); };
        services.committed = [&] { ++commits; if (persist) Save(); };
        return widget_displacement::Place({widgets, items, pages, order, savedIds, columns, rows}, index, std::move(cell), span, move, services);
    }
};

inline void Seed(World& world, DesktopWidgetType type, bool icon, const std::wstring& targetPage, size_t displaced)
{
    world.widgets.resize(2);
    auto& moving = world.widgets[0];
    moving.id = L"moving"; moving.type = type; moving.title = L"Original title"; moving.customTitle = L"Custom";
    moving.gridCell = {L"left", 0, 0}; moving.gridSpan = icon ? GridSpan{1, 1} : GridSpan{2, 2};
    moving.privacyMode = true; moving.listMode = true; moving.fanPopup = true; moving.selected = true;
    moving.singleCellIconMode = icon; moving.scrollContainerMode = type == DesktopWidgetType::Collection && !icon;
    if (type == DesktopWidgetType::FolderMapping) moving.sourceFolderPath = (world.root / L"sources" / L"mapped").wstring();
    auto& owner = world.widgets[1];
    owner.id = L"owner"; owner.type = DesktopWidgetType::Collection; owner.gridCell = {L"left", 7, 6}; owner.gridSpan = {1, 1};
    world.Item(L"a.txt", {targetPage, 2, 2});
    world.Item(L"b.txt", displaced > 1 ? GridCell{targetPage, 3, 2} : GridCell{L"right", 5, 5});
    world.Item(L"keep.txt", {L"right", 0, 5});
    world.Item(L"inside.txt", {L"left", 0, 0});
    world.Item(L"hidden.txt", {targetPage, 2, 2}); world.items.back().name.clear();
    owner.itemKeys = {world.items[3].layoutKey};
    if (type == DesktopWidgetType::Collection || type == DesktopWidgetType::FileCategories)
    { moving.itemKeys = owner.itemKeys; owner.itemKeys.clear(); }
    if (IsGroupWidgetType(type))
    {
        moving.childWidgetIds = {L"child"}; moving.activeCategoryId = L"child";
        DesktopWidget child;
        child.id = L"child"; child.type = type == DesktopWidgetType::CollectionGroup ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
        child.gridCell = {L"left", 0, 0}; child.gridSpan = {2, 2}; child.itemKeys = owner.itemKeys;
        owner.itemKeys.clear(); world.widgets.push_back(std::move(child));
    }
    world.items[0].largeIcon = LargeIconConfig{};
    world.items[0].largeIcon->autoColor = false;
    world.items[0].largeIcon->manualColor = 0x123456;
    world.items[0].largeIcon->radius = 17;
}

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    for (auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories, DesktopWidgetType::FolderMapping,
        DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (bool icon : {false, true})
    for (const auto& page : {L"left", L"right"})
    for (size_t count : {size_t{1}, size_t{2}})
    {
        if (icon && count > 1) continue;
        World world(root / (L"move-" + std::to_wstring(serial++)));
        Seed(world, type, icon, page, count); world.reverseItems = serial % 2 == 0;
        const auto originals = Files(world.root / L"sources");
        world.Legal();
        const auto before = world.widgets;
        const auto metadata = world.Metadata();
        const auto largeConfigs = world.LargeConfigs();
        const auto span = before[0].gridSpan;
        std::vector<GridCell> oldCells;
        for (const auto& item : world.items) oldCells.push_back(item.gridCell);
        Require(world.Place({page, 2, 2}, span) && world.commits == 1 &&
            world.widgets[0].gridCell.pageId == page && world.widgets[0].gridCell.column == 2 && world.widgets[0].gridCell.row == 2 &&
            world.widgets[0].gridSpan.columns == span.columns && world.widgets[0].gridSpan.rows == span.rows,
            "an admitted component move publishes its captured page, exact cell and footprint once");
        auto normalized = world.widgets; normalized[0].gridCell = before[0].gridCell; normalized[0].gridSpan = before[0].gridSpan;
        Require(Widgets(normalized) == Widgets(before) && world.Metadata() == metadata && world.LargeConfigs() == largeConfigs && Files(world.root / L"sources") == originals,
            "component movement preserves every original identity, configuration, logical member and file byte");
        for (size_t index = count; index < world.items.size(); ++index)
            Require(world.items[index].gridCell.pageId == oldCells[index].pageId && world.items[index].gridCell.column == oldCells[index].column &&
                world.items[index].gridCell.row == oldCells[index].row, "unrelated, owned and hidden file positions remain unchanged by component movement");
        world.Legal(); world.Reload();
    }
    for (int invalid = 0; invalid < 5; ++invalid)
    {
        World world(root / (L"reject-" + std::to_wstring(serial++)));
        Seed(world, DesktopWidgetType::Collection, false, L"right", 2); world.persist = false;
        GridCell target{L"right", 2, 2};
        if (invalid == 0) target.column = -1;
        if (invalid == 1) target.column = 6;
        if (invalid == 2) target.pageId = L"missing";
        if (invalid == 3) world.widgets[1].gridCell = target;
        const auto before = world.Snapshot();
        Require(!world.Place(target, {2, 2}, true, invalid == 4 ? world.widgets.size() : 0) && world.Snapshot() == before && world.commits == 0,
            "invalid or blocked component target rejects without changing geometry, membership, saved pages or callbacks");
    }
    for (bool resize : {false, true})
    {
        World world(root / (L"unfit-overflow-" + std::to_wstring(serial++)));
        world.persist = false;
        world.pages[0].columns = world.pages[0].rows = 5; world.pages[1].columns = world.pages[1].rows = 2;
        world.columns[L"left"] = world.rows[L"left"] = 5; world.columns[L"right"] = world.rows[L"right"] = 2;
        world.widgets.resize(2);
        world.widgets[0].id = L"moving"; world.widgets[0].type = DesktopWidgetType::Collection;
        world.widgets[0].gridCell = {L"left", 4, 4}; world.widgets[0].gridSpan = {1, 1};
        world.widgets[1].id = L"blocker"; world.widgets[1].gridCell = {L"right", 0, 0}; world.widgets[1].gridSpan = {2, 2};
        if (resize) world.Item(L"small.txt", {L"left", 0, 0});
        world.Item(L"large.txt", {L"left", 1, 1}, {3, 3}); world.items.back().largeIcon = LargeIconConfig{};
        world.items.back().largeIcon->columns = 3; world.items.back().largeIcon->rows = 3;
        world.Legal();
        const auto before = world.Snapshot(); const auto files = Files(world.root / L"sources");
        const auto largeConfigs = world.LargeConfigs();
        const bool placed = world.Place(resize ? GridCell{L"left", 0, 0} : GridCell{L"left", 2, 2}, resize ? GridSpan{5, 5} : GridSpan{1, 1}, !resize);
        if (placed) world.Legal();
        else Require(world.Snapshot() == before && world.commits == 0,
            "failed component displacement leaves the component, earlier item plans and saved page records unchanged");
        Require(Files(world.root / L"sources") == files, "failed or successful displacement preserves all physical original files");
        Require(world.LargeConfigs() == largeConfigs, "component displacement preserves the original declared large-icon footprint and configuration");
    }
    for (bool saved : {false, true})
    {
        World world(root / (L"overflow-" + std::to_wstring(serial++)));
        world.pages[0].columns = world.pages[0].rows = 4; world.pages[1].columns = world.pages[1].rows = 2;
        world.columns[L"left"] = world.rows[L"left"] = 4; world.columns[L"right"] = world.rows[L"right"] = 2;
        if (saved) { world.savedIds.push_back(L"offline"); world.columns[L"offline"] = world.rows[L"offline"] = 5; }
        world.widgets.resize(2);
        world.widgets[0].id = L"moving"; world.widgets[0].gridCell = {L"left", 3, 3}; world.widgets[0].gridSpan = {1, 1};
        world.widgets[1].id = L"blocker"; world.widgets[1].gridCell = {L"right", 0, 0}; world.widgets[1].gridSpan = {2, 2};
        for (int index = 0; index < 9; ++index) world.Item(L"entry-" + std::to_wstring(index) + L".txt", {L"left", index / 3, index % 3});
        const auto before = world.Snapshot(); const auto files = Files(world.root / L"sources");
        const bool placed = world.Place({L"left", 0, 0}, {4, 4}, false);
        std::cout << "overflow saved=" << saved << " accepted=" << placed << " generated=" << world.generated << '\n';
        // Q08 does not yet prescribe accepting every full-page resize or a
        // particular fallback order. Success must be legal/persistent; refusal
        // must leave the complete model unchanged.
        if (placed) { world.Legal(); world.Reload(); }
        else Require(world.Snapshot() == before && world.commits == 0, "declined overflow placement leaves all prior data intact");
        Require(Files(world.root / L"sources") == files, "overflow pages retain each original file and byte");
    }
    std::cout << "component movement, displaced items, rejection, overflow and shared layout storage cases passed\n";
}
}
