#pragma once

#include "core/drop_model.h"
#include "empty_group_drop_rules.h"
#include "desktop/desktop_category_item_rules.h"
#include "shell/shortcut_category_target.h"
#include "shell/shell_file_operation_worker.h"
#include <functional>
#include <memory>

namespace snowdesktop::drop_pipeline
{
using Completion = std::function<void(bool)>;

inline bool AutoCollectSource(const DragSourceList& sources, const std::vector<DesktopWidget>& widgets)
{
    if (!sources.hasOriginWidget || sources.originWidgetType != DesktopWidgetType::FileCategories) return false;
    const auto widget = std::find_if(widgets.begin(), widgets.end(),
        [&](const auto& candidate) { return candidate.id == sources.originWidgetId; });
    return widget != widgets.end() && widget->autoCollect;
}

template<class FindDesktopItem, class Normalize>
bool ContainsPrograms(const DragSourceList& sources,
    const std::vector<std::wstring>& extensions, FindDesktopItem findItem, Normalize normalize)
{
    return std::any_of(sources.entries.begin(), sources.entries.end(), [&](const auto& entry) {
        auto* item = findItem(entry.desktopKey);
        if (item && category_collection_rules::IsDesktopNamespaceProgram(*item)) return true;
        const bool application = item && item->isApplicationShortcut;
        const auto& path = entry.filePath.empty() ? entry.displayName : entry.filePath;
        const DWORD attributes = path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
        const auto target = item && item->shortcutTarget.classified
            ? item->shortcutTarget : category_collection_rules::ReadShortcutTarget(path);
        if (item && target.classified)
        {
            item->shortcutTarget = target;
            item->isApplicationShortcut = category_collection_rules::IsProgramItem(
                normalize(PathFindExtensionW(path.c_str())), false, {}, target);
        }
        return category_collection_rules::IsProgramItem(
            normalize(PathFindExtensionW(path.c_str())), application, extensions, target);
    });
}

struct ConvertedView
{
    std::shared_ptr<void> lifetime;
    Container* container = nullptr;
};

struct Services
{
    std::function<bool(const DragSourceList&)> containsPrograms;
    std::function<bool(bool)> confirmConversion;
    std::function<bool()> confirmPrograms;
    std::function<bool()> exiting;
    std::function<bool(Container*)> runtimeLive;
    std::function<ConvertedView(DesktopWidget*)> createConvertedView;
    std::function<void()> saveLayout;
    std::function<void()> convertedLayout;
    std::function<bool()> programsEnabled;
    std::function<bool()> enablePrograms;
    std::function<bool(const DragSourceList&)> autoCollectSource;
    std::function<std::wstring(std::wstring)> normalize;
    std::function<void()> refreshCollectedKeys;
    std::function<void()> dockChanged;
    std::function<bool(const DragSourceList&, const DropPreviewList&, Completion, bool,
        std::shared_ptr<ShellFileOperationResult>)> files;
    std::function<bool(const DragSourceList&, const DropPreviewList&)> internal;
    std::wstring collectionTitle, filesTitle;
};

struct TargetSnapshot
{
    DesktopWidget* pointer;
    std::wstring id;
    DesktopWidgetType type;
    explicit TargetSnapshot(DesktopWidget* widget) : pointer(widget), id(widget->id), type(widget->type) {}
};

inline bool CurrentTarget(const TargetSnapshot& snapshot, const std::vector<DesktopWidget>& widgets,
    const DragSourceList& sources, const DropPreviewList& preview, const Services& services)
{
    if (services.exiting()) return false;
    const auto current = std::find_if(widgets.begin(), widgets.end(),
        [&](const auto& widget) { return widget.id == snapshot.id; });
    if (current == widgets.end() || &*current != snapshot.pointer || current->type != snapshot.type)
        return false;
    return services.runtimeLive(sources.origin) && services.runtimeLive(preview.targetContainer);
}

// This is the host's commit-time decision and dispatch path. Dialog display,
// settings IO and native view creation are services; decision ordering, stable
// target checks, model conversion, Dock cleanup and dispatch are shared.
inline bool Execute(const DragSourceList& sources, const DropPreviewList& requestedPreview,
    std::vector<DesktopWidget>& widgets, std::vector<DockEntry>& dock, const Services& services,
    Completion completion, bool synchronously, std::shared_ptr<ShellFileOperationResult> result)
{
    if (sources.HasDesktopWidgetIcons() &&
        (requestedPreview.targetKind != DropTargetKind::Desktop || requestedPreview.action != DropAction::Move))
        return Execute(sources.WithoutDesktopWidgetIcons(), requestedPreview, widgets, dock, services,
            std::move(completion), synchronously, std::move(result));
    if (sources.Empty()) return false;
    DropPreviewList preview = requestedPreview;
    bool convertedGroup = false;
    ConvertedView convertedView;
    if (!preview.Empty() && preview.targetWidget && !sources.hasWidgets &&
        !sources.hasCollectionGroupEntries && !sources.hasFileGroupEntries &&
        empty_group_drop_rules::IsEmptyGroup(*preview.targetWidget, widgets))
    {
        const TargetSnapshot captured(preview.targetWidget);
        const bool programs = services.containsPrograms(sources);
        if (!services.confirmConversion(programs)) return false;
        if (!CurrentTarget(captured, widgets, sources, preview, services)) return false;
        if (!empty_group_drop_rules::Convert(*preview.targetWidget, widgets, programs,
            programs ? services.collectionTitle : services.filesTitle)) return false;
        convertedGroup = true;
        convertedView = services.createConvertedView(preview.targetWidget);
        preview.targetContainer = convertedView.container;
        services.saveLayout();
    }
    if (preview.targetWidget && preview.targetWidget->type == DesktopWidgetType::FileCategories &&
        !services.programsEnabled() && services.containsPrograms(sources))
    {
        const TargetSnapshot captured(preview.targetWidget);
        if (!services.confirmPrograms()) return false;
        if (!CurrentTarget(captured, widgets, sources, preview, services)) return false;
        if (!services.enablePrograms()) return false;
    }
    if (preview.Empty() && !(preview.fileBacked && preview.targetKind == DropTargetKind::Desktop)) return false;
    if (preview.targetKind == DropTargetKind::Desktop && preview.action == DropAction::Move &&
        services.autoCollectSource(sources)) return false;

    std::unordered_set<std::wstring> movedDockEntries;
    if (preview.action == DropAction::Move || preview.consumeDockSource)
        for (const auto& entry : sources.entries)
            if (entry.fromDock && !entry.dockReference.empty())
                movedDockEntries.insert(std::to_wstring(static_cast<int>(entry.dockEntryType)) +
                    L":" + services.normalize(entry.dockReference));
    auto finish = [&dock, services, convertedGroup, movedDockEntries = std::move(movedDockEntries),
        completion = std::move(completion)](bool succeeded) mutable {
        if (succeeded && !movedDockEntries.empty())
        {
            const size_t previousSize = dock.size();
            std::erase_if(dock, [&](const auto& entry) {
                return movedDockEntries.contains(std::to_wstring(static_cast<int>(entry.type)) +
                    L":" + services.normalize(entry.reference));
            });
            services.refreshCollectedKeys();
            if (dock.size() != previousSize) services.dockChanged();
        }
        if (convertedGroup) services.convertedLayout();
        if (completion) completion(succeeded);
    };
    if (preview.fileBacked)
    {
        const bool queued = services.files(sources, preview, std::move(finish), synchronously, std::move(result));
        if (convertedGroup && queued && !synchronously) services.convertedLayout();
        return queued;
    }
    const bool executed = services.internal(sources, preview);
    finish(executed);
    return executed;
}
}
