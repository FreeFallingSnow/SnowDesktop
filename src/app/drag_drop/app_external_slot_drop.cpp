#include "app/app.h"
#include "drag_drop/external_drop_resources.h"
#include "drag_drop/external_slot_transfer.h"
#include "drag_drop/external_file_destination.h"
#include "drag_drop/drop_staging.h"
#include "drag_drop/default_file_drop_action.h"
#include "drag_drop/ole_drag_rules.h"
#include "widgets/lua_logical_slot.h"

#include <stdexcept>

namespace
{
namespace content = snowdesktop::external_drop_content;

content::Paths DownloadSlotUrls(const content::Paths& urls, std::stop_token stop)
{
    for (const auto& url : urls)
    {
        if (stop.stop_requested()) throw std::runtime_error("drop cancelled");
        snowdesktop::UrlDropDownloadRequest request;
        request.url = url;
        request.destinationDirectory = snowdesktop::drop_staging::Directory();
        if (request.destinationDirectory.empty()) return {};
        const auto result = snowdesktop::UrlDropDownloadWorker::Execute(request, stop);
        if (result.outcome == snowdesktop::UrlDropDownloadOutcome::Downloaded)
            return {result.localPath};
        if (!result.CanRetryAlternateUrl()) break;
    }
    if (stop.stop_requested()) throw std::runtime_error("drop cancelled");
    return {};
}
}

DWORD DesktopApp::ExternalFileDropPreferredEffect(POINT point) const
{
    const auto* target = dragSession_.TargetContainer();
    const auto* desktop = containers_.empty() ? nullptr : containers_.front().get();
    if (dragSession_.TargetRegion() == HitRegion::Handoff) return DROPEFFECT_MOVE;
    if (!externalLuaFileDropAvailable_)
        return target == desktop ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
    std::wstring directory;
    if (target && target != desktop)
    {
        DragSourceList placeholder;
        placeholder.hasExternalFiles = true;
        DragSourceEntry entry; entry.kind = DropSourceKind::ExternalFile; entry.displayName = L"drop";
        placeholder.entries.push_back(std::move(entry));
        const auto preview = BuildDropPreviewList(placeholder, dragSession_.TargetContainer(),
            dragSession_.TargetSlot(), dragSession_.TargetRegion(), MK_CONTROL, point);
        if (preview.targetKind == DropTargetKind::FolderMapping && preview.targetWidget)
            directory = preview.targetWidget->sourceFolderPath;
        else if (preview.targetKind != DropTargetKind::KeyedWidget) return DROPEFFECT_MOVE;
    }
    if (directory.empty())
    {
        wchar_t path[MAX_PATH]{};
        if (snowdesktop::desktop_source::CopyDirectory(path)) directory = path;
    }
    // Delayed paths are unavailable during hover. Preserve non-destructive
    // provisional feedback; the real commit resolves them after source read.
    return snowdesktop::default_file_drop_action::Preferred(externalFileDropPaths_, directory)
        .value_or(DROPEFFECT_COPY);
}

std::optional<DesktopApp::ExternalSlotDestination>
DesktopApp::CaptureExternalSlotDestination(POINT point, DWORD keyState)
{
    Container* target = dragSession_.TargetContainer();
    Slot* slot = dragSession_.TargetSlot();
    const HitRegion region = dragSession_.TargetRegion();
    if (!target || target == GetDesktopGrid() || region == HitRegion::None ||
        region == HitRegion::Blocked || region == HitRegion::Handoff)
        return std::nullopt;
    ExternalSlotDestination result;
    if (auto* lua = dynamic_cast<LuaLogicalSlotContainer*>(target))
    {
        result.luaWidgetId = lua->WidgetId();
        result.luaSlotId = lua->SlotId();
        result.insertIndex = lua->GetDropInsertIndex(slot, region);
        result.preview.action = DropAction::Copy;
        return result;
    }

    DragSourceList placeholder;
    placeholder.hasExternalFiles = true;
    DragSourceEntry entry;
    entry.kind = DropSourceKind::ExternalFile;
    entry.displayName = L"drop";
    placeholder.entries.push_back(std::move(entry));
    if (auto* dock = dynamic_cast<DockContainer*>(target))
    {
        result.preview = BuildDropPreviewList(placeholder, GetDesktopGrid(),
            nullptr, HitRegion::Empty, MK_ALT, point);
        result.preview.action = DropAction::Link;
        result.preview.fileBacked = true;
        result.preview.pinMaterializedItemsToDock = true;
        result.preview.dockInsertIndex = dock->GetExternalDropInsertIndex(slot, region);
    }
    else
    {
        result.preview = BuildDropPreviewList(placeholder, target, slot, region,
            static_cast<int>(keyState), point);
        if (result.preview.Empty() || !result.preview.targetWidget)
            return std::nullopt;
        const auto& widget = *result.preview.targetWidget;
        result.widgetId = widget.id;
        result.widgetType = widget.type;
        result.folderPath = widget.sourceFolderPath;
        result.dockFolderPopup = target == dockFolderPopupContainer_.get();
        result.volumeDefault = (keyState & (MK_CONTROL | MK_SHIFT | MK_ALT)) == 0;
    }
    // The read/download may outlive a popup, page, container tree or vector
    // reallocation. No pointers from the drag session cross that boundary.
    result.preview.targetContainer = nullptr;
    result.preview.targetWidget = nullptr;
    for (auto& landing : result.preview.landings) landing.widget = nullptr;
    return result;
}

bool DesktopApp::CommitExternalSlotPaths(const ExternalSlotDestination& destination,
    const std::vector<std::wstring>& paths, bool owned, bool copyOnly,
    FileOperationCompletion completion, bool synchronously,
    std::shared_ptr<snowdesktop::ShellFileOperationResult> result)
{
    if (exitRequested_ || paths.empty()) return false;
    if (owned && !destination.luaWidgetId.empty())
    {
        DragSourceList sources;
        sources.hasExternalFiles = true;
        for (const auto& path : paths)
        {
            DragSourceEntry entry;
            entry.kind = DropSourceKind::ExternalFile;
            entry.sourceIndex = sources.entries.size(); entry.filePath = path;
            sources.entries.push_back(std::move(entry));
        }
        auto outputs = std::make_shared<snowdesktop::ShellFileOperationResult>();
        auto committedResult = std::make_shared<bool>(false);
        // A logical slot stores paths rather than copying files itself. Publish
        // actual desktop files before handing those paths to the component.
        const bool queued = MaterializeFilesToDesktop(sources, DropAction::Copy, false, nullptr,
            [this, destination, paths, outputs, committedResult, synchronously, completion = std::move(completion)](bool succeeded) mutable {
                std::vector<std::wstring> published;
                for (const auto& path : paths)
                    for (const auto& output : outputs->outputs)
                        if (_wcsicmp(output.source.c_str(), path.c_str()) == 0 && !output.destination.empty())
                        { published.push_back(output.destination); break; }
                const bool committed = succeeded && published.size() == paths.size() &&
                    CommitExternalSlotPaths(destination, published, false, true, {}, true, {});
                *committedResult = committed;
                if (!outputs->outputs.empty()) RequestShellRefresh();
                if (completion && (committed || !synchronously)) completion(committed);
            }, synchronously, outputs);
        return queued && (!synchronously || *committedResult);
    }
    if (destination.fileDropTarget)
    {
        const auto& target = *destination.fileDropTarget;
        const auto index = FindWidgetIndexById(target.widgetId);
        if (!widgetEngine_ || index >= widgets_.size() ||
            widgets_[index].type != DesktopWidgetType::LuaScript) return false;
        const bool committed = widgetEngine_->InvokeFileDrop(target, paths);
        if (committed && completion) completion(true);
        return committed;
    }
    if (!destination.luaWidgetId.empty())
    {
        for (const auto& candidate : containers_)
        {
            auto* lua = dynamic_cast<LuaLogicalSlotContainer*>(candidate.get());
            if (!lua || lua->WidgetId() != destination.luaWidgetId ||
                lua->SlotId() != destination.luaSlotId) continue;
            if (!lua->AcceptsDragPayload(
                    snowdesktop::slot_contract::DragPayloadKind::ExternalFile,
                    paths.size())) return false;
            std::vector<std::unique_ptr<ExternalFileItem>> ownedItems;
            std::vector<Item*> items;
            for (const auto& path : paths)
            {
                ownedItems.push_back(std::make_unique<ExternalFileItem>(path));
                items.push_back(ownedItems.back().get());
            }
            Slot boundary(lua, RECT{}, destination.insertIndex);
            const bool committed = lua->CommitItems(items, &boundary, HitRegion::SortBefore);
            if (completion) completion(committed);
            return committed;
        }
        return false;
    }

    snowdesktop::external_file_destination::Services services;
    services.resolveTarget = [this](const std::wstring& id, bool popup) -> DesktopWidget* {
        if (popup)
            return dockFolderPopupOpen_ && dockFolderPopupWidget_.id == id
                ? &dockFolderPopupWidget_ : nullptr;
        const size_t index = FindWidgetIndexById(id);
        return index < widgets_.size() ? &widgets_[index] : nullptr;
    };
    services.desktopPreview = [this](const GridCell& anchor, size_t count) {
        return BuildExternalDesktopPreviewList(anchor, count);
    };
    services.canPinExisting = [this](const auto& files) { return CanPinExistingDesktopPaths(files); };
    services.pinExisting = [this](const auto& files, size_t index) {
        return AddMaterializedItemsToDock(files, index, true);
    };
    services.dockHasCapacity = [this](size_t count) {
        const auto* dock = GetDockContainer();
        return dock && dock->HasCapacity(count);
    };
    services.afterPin = [this] { SaveLayoutSlots(); RequestShellRefresh(); };
    services.fileName = FileNameFromPath;
    services.execute = [this](const DragSourceList& sources, const DropPreviewList& preview,
        FileOperationCompletion done, bool sync, auto outputs) {
        return ExecuteDropPipeline(sources, preview, std::move(done), sync, std::move(outputs));
    };
    return snowdesktop::external_file_destination::Commit(destination, paths, owned, copyOnly,
        std::move(completion), synchronously, std::move(result), services);
}

DWORD DesktopApp::DropExternalSlotContent(IDataObject* dataObject,
    const ExternalSlotDestination& destination, DWORD allowedEffects,
    bool asynchronousSource, const std::vector<std::wstring>& knownPaths)
{
    if (!dataObject) return DROPEFFECT_NONE;
    const bool volumeDefault = destination.volumeDefault &&
        (allowedEffects & (DROPEFFECT_MOVE | DROPEFFECT_COPY)) == (DROPEFFECT_MOVE | DROPEFFECT_COPY) &&
        (!knownPaths.empty() || content::ProbeFileSource(dataObject).available);
    std::wstring physicalDirectory = destination.preview.targetKind == DropTargetKind::FolderMapping
        ? destination.folderPath : std::wstring{};
    if (volumeDefault && physicalDirectory.empty())
    {
        wchar_t path[MAX_PATH]{};
        if (snowdesktop::desktop_source::CopyDirectory(path)) physicalDirectory = path;
    }
    const bool allowContent = (allowedEffects & DROPEFFECT_COPY) != 0;
    DWORD effect = destination.preview.action == DropAction::Move ? DROPEFFECT_MOVE :
        destination.preview.action == DropAction::Link ? DROPEFFECT_LINK : DROPEFFECT_COPY;
    if (destination.preview.pinMaterializedItemsToDock)
        effect = snowdesktop::dock_drop_rules::ChooseExternalMappingEffect(allowedEffects);
    else if (volumeDefault)
        effect = snowdesktop::default_file_drop_action::Preferred(knownPaths, physicalDirectory).value_or(DROPEFFECT_COPY);
    if ((effect & allowedEffects) == 0) return DROPEFFECT_NONE;

    // An explicit drop on an inactive tab activates that captured child only
    // after a successful commit. Ordinary content drops must not undo a later
    // user tab switch while their external source is still being read.
    std::wstring activationGroupId;
    DesktopWidgetType activationGroupType = DesktopWidgetType::CollectionGroup;
    if (auto* group = dynamic_cast<CollectionGroup*>(dragSession_.TargetContainer()))
    {
        const auto* data = group->GetWidgetData();
        const auto hit = group->CategoryIdAtPoint(dragSession_.CurrentPoint());
        if (data && !hit.empty() && hit == destination.widgetId && hit != group->GetActiveCollectionId())
        { activationGroupId = data->id; activationGroupType = data->type; }
    }
    else if (auto* fileGroup = dynamic_cast<FileGroup*>(dragSession_.TargetContainer()))
    {
        const auto* data = fileGroup->GetWidgetData();
        const auto hit = fileGroup->SourceIdAtPoint(dragSession_.CurrentPoint());
        if (data && !hit.empty() && hit == destination.widgetId && hit != fileGroup->GetActiveSourceId())
        { activationGroupId = data->id; activationGroupType = data->type; }
    }
    const auto resolveActivationGroup = [this, activationGroupId, activationGroupType, destination]() -> DesktopWidget* {
        const auto parent = FindWidgetIndexById(activationGroupId);
        const auto child = FindWidgetIndexById(destination.widgetId);
        if (parent >= widgets_.size() || child >= widgets_.size()) return nullptr;
        auto& group = widgets_[parent];
        const auto& target = widgets_[child];
        if (group.type != activationGroupType || target.type != destination.widgetType ||
            target.sourceFolderPath != destination.folderPath ||
            std::find(group.childWidgetIds.begin(), group.childWidgetIds.end(), destination.widgetId) == group.childWidgetIds.end())
            return nullptr;
        return &group;
    };

    // A slot pointer and its numeric index cannot survive the delayed read.
    // Keep only the indicated member identity and side; the original target
    // is independently revalidated below before its current order is used.
    std::wstring insertionAnchor;
    const bool insertAfterAnchor = dragSession_.TargetRegion() == HitRegion::SortAfter;
    if (!destination.widgetId.empty())
        if (auto* slot = dragSession_.TargetSlot())
            if (auto* item = slot->GetItem()) insertionAnchor = item->GetPath();

    auto result = std::make_shared<snowdesktop::ShellFileOperationResult>();
    auto value = content::MakeContent(result);
    const auto stop = externalSlotReadStopSource_.get_token();
    const auto downloadUrls = [stop](const content::Paths& urls) {
        return DownloadSlotUrls(urls, stop);
    };
    auto read = [allowContent, downloadUrls, filesOnly = destination.fileDropTarget.has_value()](IDataObject* source, bool download,
        const content::Paths& files) {
        const auto resolvedFiles = files.empty() ? GetDropPaths(source) : files;
        if (filesOnly) return content::Content{resolvedFiles, {}, false, true};
        const auto snapshot = resolvedFiles.empty()
            ? ReadDropReferenceSnapshot(source) : DropReferenceSnapshot{};
        const auto urls = ExtractDropUrls(snapshot);
        auto descriptors = resolvedFiles.empty()
            ? snowdesktop::virtual_file_drop::ReadDescriptors(source)
            : std::vector<snowdesktop::virtual_file_drop::VirtualFileDescriptor>{};
        if (descriptors.size() == 1 && !urls.empty())
        {
            const auto extension = std::filesystem::path(
                descriptors.front().suggestedFileName).extension().wstring();
            if (_wcsicmp(extension.c_str(), L".url") == 0 ||
                _wcsicmp(extension.c_str(), L".website") == 0 ||
                _wcsicmp(extension.c_str(), L".lnk") == 0)
                descriptors.clear();
        }
        content::Readers readers;
        readers.files = [&] { return resolvedFiles; };
        readers.localFileUrls = [&] { return TryExtractLocalFileUrlFromDataObject(snapshot); };
        readers.image = [&] { return TryExtractImageFromDataObject(source); };
        readers.dataUrl = [&] { return TryExtractDataUrlFromDataObject(snapshot); };
        readers.virtualFiles = [&]() -> content::Paths {
            if (descriptors.empty()) return {};
            bool complete = false;
            auto paths = TryMaterializeVirtualFilesFromDataObject(source, descriptors, &complete);
            if (!complete)
            {
                for (const auto& path : paths) DeleteFileW(path.c_str());
                throw std::runtime_error("incomplete virtual file drop");
            }
            return paths;
        };
        readers.virtualFileCount = descriptors.size();
        readers.urls = [&] { return urls; };
        if (download) readers.download = downloadUrls;
        readers.shortcut = [&] { return TryExtractUrlFromDataObject(snapshot); };
        readers.text = [&] { return TryExtractTextFromDataObject(snapshot); };
        return content::Read(false, allowContent, readers);
    };

    snowdesktop::external_slot_transfer::Callbacks callbacks;
    callbacks.read = std::move(read);
    callbacks.resolveUrls = [downloadUrls](const content::Paths& urls) {
        content::Readers readers;
        readers.urls = [urls] { return urls; };
        readers.download = downloadUrls;
        readers.shortcut = [urls]() -> content::Paths {
            const auto path = urls.empty() ? std::wstring{} : CreateUrlShortcut(urls.front());
            return path.empty() ? content::Paths{} : content::Paths{path};
        };
        return content::Read(false, true, readers);
    };
    const auto committedEffect = std::make_shared<DWORD>(effect);
    if (volumeDefault) callbacks.committedEffect = [committedEffect] { return *committedEffect; };
    callbacks.commit = [this, destination, activationGroupId, resolveActivationGroup, insertionAnchor, insertAfterAnchor, result, committedEffect, volumeDefault, physicalDirectory](const content::Content& input,
        FileOperationCompletion finished, bool synchronously) {
        // The tab's original owner is part of this explicit destination. A
        // replacement group or detached child cannot receive a pending drop.
        if (!activationGroupId.empty() && !resolveActivationGroup()) return false;
        auto resolved = destination;
        if (!insertionAnchor.empty())
        {
            const auto index = FindWidgetIndexById(destination.widgetId);
            const auto* target = destination.dockFolderPopup
                ? dockFolderPopupOpen_ ? &dockFolderPopupWidget_ : nullptr
                : index < widgets_.size() ? &widgets_[index] : nullptr;
            if (target && target->id == destination.widgetId && target->type == destination.widgetType &&
                target->sourceFolderPath == destination.folderPath)
            {
                const bool mapped = target->type == DesktopWidgetType::FolderMapping;
                const auto count = mapped ? target->folderEntries.size() : target->itemKeys.size();
                std::size_t anchor = 0;
                for (; anchor < count; ++anchor)
                {
                    const auto& key = mapped ? target->folderEntries[anchor].fullPath : target->itemKeys[anchor];
                    if (ToUpperInvariant(key) == ToUpperInvariant(insertionAnchor)) break;
                }
                resolved.preview.insertIndex = anchor == count ? count : anchor + (insertAfterAnchor ? 1u : 0u);
            }
        }
        if (volumeDefault && !input.owned && !input.copyOnly)
        {
            *committedEffect = snowdesktop::default_file_drop_action::Preferred(input.paths, physicalDirectory)
                .value_or(DROPEFFECT_COPY);
            resolved.preview.action = DropActionFromMods(static_cast<int>(
                snowdesktop::ole_drag_rules::ModifiersForDropEffect(*committedEffect)));
        }
        auto completed = [this, destination, activationGroupId, resolveActivationGroup,
            finished = std::move(finished)](bool succeeded) mutable {
            if (succeeded && !activationGroupId.empty())
                if (auto* group = resolveActivationGroup())
                {
                    group->activeCategoryId = destination.widgetId;
                    for (const auto& container : containers_)
                        if (auto* view = dynamic_cast<WidgetContainer*>(container.get());
                            view && view->GetWidgetData() == group)
                        {
                            if (auto* collections = dynamic_cast<CollectionGroup*>(view)) collections->InvalidateFilterCache();
                            else if (auto* files = dynamic_cast<FileGroup*>(view)) files->InvalidateHostedView();
                        }
                    SaveLayoutSlots();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            if (finished) finished(succeeded);
        };
        return CommitExternalSlotPaths(resolved, input.paths, input.owned,
            input.copyOnly, std::move(completed), synchronously, result);
    };
    callbacks.completed = [](bool succeeded) {
        if (!succeeded)
            WriteDiagnosticLogEntry(L"External file drop failed during content reading or destination commit");
    };
    const HWND window = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    return snowdesktop::external_slot_transfer::Transfer(dataObject, effect,
        asynchronousSource, knownPaths, std::move(value), externalSlotReadWorker_,
        stop, window, kShellFileOperationCompletedMessage, std::move(callbacks));
}
