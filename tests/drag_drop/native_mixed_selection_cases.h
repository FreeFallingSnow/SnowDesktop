#pragma once

#include "data/widget_layout_codec.h"
#include <sstream>

namespace native_mixed_selection_cases
{
using namespace native_ole_file_cases;

inline std::string OriginalComponents(const DesktopApp& app)
{
    std::ostringstream stream;
    std::vector<const DesktopWidget*> originals;
    for (const auto& widget : Access::Widgets(app))
        if (widget.id != L"mixed-target") originals.push_back(&widget);
    std::sort(originals.begin(), originals.end(), [](const auto* left, const auto* right) { return left->id < right->id; });
    for (const auto* widget : originals)
        snowdesktop::widget_layout_codec::Write(stream, *widget, JsonEscapeUtf8, WideToUtf8);
    return stream.str();
}
inline void Check(DesktopApp& app, const std::vector<fs::path>& files, std::size_t count,
    const std::string& original, DesktopWidgetType targetType)
{
    Require(OriginalComponents(app) == original,
        "D01: an unsupported component, its children, members, placement and settings must remain unchanged");
    const auto* target = Access::MemberContainer(app, L"mixed-target")->GetWidgetData();
    Require(target && target->type == targetType && target->itemKeys.size() == count,
        "D01: the actual native target must receive every eligible selected file despite unsupported mixed members");
    for (std::size_t i = 0; i < files.size(); ++i)
    {
        const auto key = ToUpperInvariant(files[i].wstring());
        const auto occurrences = std::count(target->itemKeys.begin(), target->itemKeys.end(), key);
        const auto& item = Find(app, key);
        Require(occurrences == (i < count ? 1 : 0) && Read(files[i]) == "mixed-file-" + std::to_string(i),
            "D01: logical file transfer preserves bytes and assigns only the supported selected identities once");
        if (i < count)
            Require(!Access::HasVisibleIcon(app, item) && item.gridCell.pageId == target->gridCell.pageId &&
                item.gridCell.column == target->gridCell.column && item.gridCell.row == target->gridCell.row,
                "D01: accepted files must belong to the intended container without a free-desktop duplicate");
        else
            Require(Access::HasVisibleIcon(app, item) && item.gridCell.pageId == L"__page:1" &&
                item.gridCell.column == static_cast<int>(i) * 2 && item.gridCell.row == 0,
                "D01: an unselected ordinary file must retain its original visible desktop cell");
    }
    std::unordered_set<std::wstring> visible;
    for (const auto& slot : Access::MemberContainer(app, L"mixed-target")->GetSlots())
        if (const auto* item = slot->GetItem()) visible.insert(ToUpperInvariant(item->GetPath()));
    Require(visible.size() == count && std::all_of(target->itemKeys.begin(), target->itemKeys.end(),
        [&](const auto& key) { return visible.contains(ToUpperInvariant(key)); }),
        "D01: eligible identities must be accessible through the real target member slots");
    Require(Access::NoPresentationWindows(app), "native mixed-selection tests cannot open desktop presentation windows");
}
inline void Run(std::string_view typeName)
{
    DesktopWidgetType targetType;
    if (typeName == "collection") targetType = DesktopWidgetType::Collection;
    else if (typeName == "files") targetType = DesktopWidgetType::FileCategories;
    else throw std::runtime_error("unknown native mixed-selection target");
    native_drag_commit_cases::Profile profile;
    std::size_t scenes = 0;
    for (const auto type : {DesktopWidgetType::Collection, DesktopWidgetType::FileCategories,
        DesktopWidgetType::FolderMapping, DesktopWidgetType::CollectionGroup, DesktopWidgetType::FileGroup})
    for (const std::size_t count : {0u, 1u, 2u})
    for (const bool reverse : {false, true})
    {
        native_drag_commit_cases::context = "mixed target=" + std::string(typeName) + " component=" +
            std::to_string(static_cast<int>(type)) + " files=" + std::to_string(count) + " reverse=" + std::to_string(reverse);
        const auto directory = profile.root / (L"mixed-" + std::to_wstring(scenes++));
        fs::create_directories(directory / L"component");
        const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
        const std::vector<fs::path> files{desktop / L"A.txt", desktop / L"B.txt"};
        std::vector<fs::path> inside;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            { std::ofstream stream(files[i], std::ios::binary); stream << "mixed-file-" << i; }
            inside.push_back((type == DesktopWidgetType::FolderMapping ? directory / L"component" : desktop) /
                (L"Inside-" + std::to_wstring(i) + L".txt"));
            { std::ofstream stream(inside.back(), std::ios::binary); stream << "inside-" << i; }
        }
        auto& app = profile.Model(false, directory / L"layout");
        Access::SeedMixedModel(app, files, inside, type, targetType);
        Access::PrepareReload(app); Access::BindAsync(app);
        native_drag_async_reload_cases::Refresh(app);
        Require(Access::Save(app), "save original native mixed-selection model");
        const auto original = OriginalComponents(app);
        const auto captured = Access::BeginMixed(app, files, count, reverse);
        Require(captured.hasWidgets && captured.HasDesktopWidgetIcons() && captured.entries.size() == count + 1,
            "D01: the production source capture must retain the complete mixed selection before filtering");
        const auto preview = Access::MixedPreview(app);
        Require(preview.landings.size() == count && (count == 0 || preview.action == DropAction::Move),
            "D01: native feedback must accept eligible members without claiming unsupported members");
        for (const auto& landing : preview.landings)
        {
            const auto source = std::find_if(captured.entries.begin(), captured.entries.end(),
                [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
            Require(source != captured.entries.end() && source->kind == DropSourceKind::DesktopIcon &&
                !source->desktopKey.empty() && landing.widgetId == L"mixed-target",
                "D01: eligible landing indices must retain their full-selection identities and the intended destination");
        }
        Access::DropMixed(app);
        Check(app, files, count, original, targetType);
        Require(Access::Save(app), "save actual partial mixed-selection result through native host storage");
        native_drag_async_reload_cases::Refresh(app);
        Check(app, files, count, original, targetType);
        Access::Load(app);
        native_drag_async_reload_cases::Refresh(app, true);
        Check(app, files, count, original, targetType);
        for (std::size_t i = 0; i < inside.size(); ++i)
            Require(Read(inside[i]) == "inside-" + std::to_string(i),
                "D01: ignoring an unsupported component cannot alter any of its physical member files");
        Require(Outputs(desktop, [&] { auto all = files; if (type != DesktopWidgetType::FolderMapping)
            all.insert(all.end(), inside.begin(), inside.end()); return all; }()).empty(),
            "D01: logical eligible-subset transfer cannot create extra physical file outputs");
        Access::StopModel(app);
        for (const auto& file : files) fs::remove(file);
        for (const auto& file : inside) fs::remove(file);
    }
    std::cout << scenes << " actual native complete-selection/subset/model/storage scenes passed: " << typeName << '\n';
}
}
