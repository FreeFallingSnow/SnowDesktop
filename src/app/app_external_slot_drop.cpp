#include "app.h"
#include "../external_drop_resources.h"
#include "../widgets/lua_logical_slot.h"

#include <stdexcept>

namespace
{
namespace content = snowdesktop::external_drop_content;

std::shared_ptr<content::Content> NewSlotContent(
    const std::shared_ptr<snowdesktop::ShellFileOperationResult>& result)
{
    return {new content::Content, [result](content::Content* value) {
        content::Cleanup(*value, *result);
        delete value;
    }};
}

struct MarshaledSlotSource
{
    ComPtr<IStream> stream;
    ~MarshaledSlotSource()
    {
        // Releasing an unread stream alone does not release its marshal data.
        ComPtr<IDataObject> discarded;
        if (stream)
            CoGetInterfaceAndReleaseStream(stream.Detach(), IID_PPV_ARGS(&discarded));
    }
    ComPtr<IDataObject> Take()
    {
        ComPtr<IDataObject> source;
        if (stream)
            CoGetInterfaceAndReleaseStream(stream.Detach(), IID_PPV_ARGS(&source));
        return source;
    }
};

content::Paths DownloadSlotUrls(const content::Paths& urls, std::stop_token stop)
{
    for (const auto& url : urls)
    {
        if (stop.stop_requested()) throw std::runtime_error("drop cancelled");
        snowdesktop::UrlDropDownloadRequest request;
        request.url = url;
        request.destinationDirectory = GetDataSubdirectoryPath(L"DropContent");
        const auto result = snowdesktop::UrlDropDownloadWorker::Execute(request, stop);
        if (result.outcome == snowdesktop::UrlDropDownloadOutcome::Downloaded)
            return {result.localPath};
        if (!result.CanRetryAlternateUrl()) break;
    }
    if (stop.stop_requested()) throw std::runtime_error("drop cancelled");
    return {};
}
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
        result.preview.dockInsertIndex = dock->GetDropInsertIndex(slot, region);
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

    DropPreviewList preview = destination.preview;
    DesktopWidget* widget = nullptr;
    if (!destination.widgetId.empty() || destination.dockFolderPopup)
    {
        if (destination.dockFolderPopup)
        {
            if (!dockFolderPopupOpen_ ||
                dockFolderPopupWidget_.id != destination.widgetId) return false;
            widget = &dockFolderPopupWidget_;
        }
        else
        {
            const size_t index = FindWidgetIndexById(destination.widgetId);
            if (index >= widgets_.size()) return false;
            widget = &widgets_[index];
        }
        if (widget->type != destination.widgetType ||
            widget->sourceFolderPath != destination.folderPath) return false;
        if (widget->type == DesktopWidgetType::FileCategories &&
            ((!owned && preview.action == DropAction::Link) ||
             std::any_of(paths.begin(), paths.end(), [](const auto& path) {
                 return _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
             }))) return false;
        preview.targetWidget = widget;
        preview.landings.clear();
        for (size_t index = 0; index < paths.size(); ++index)
        {
            DropLanding landing;
            landing.kind = preview.targetKind == DropTargetKind::FolderMapping
                ? DropLandingKind::Folder : DropLandingKind::WidgetIndex;
            landing.sourceIndex = index;
            landing.widget = widget;
            landing.widgetId = widget->id;
            landing.insertIndex = preview.insertIndex + index;
            landing.cell = widget->gridCell;
            preview.landings.push_back(std::move(landing));
        }
    }
    else if (preview.pinMaterializedItemsToDock)
    {
        auto* dock = GetDockContainer();
        if (!dock || !dock->HasCapacity(paths.size())) return false;
        const auto action = preview.action;
        const auto insertIndex = preview.dockInsertIndex;
        preview = BuildExternalDesktopPreviewList(preview.anchorCell, paths.size());
        preview.pinMaterializedItemsToDock = true;
        preview.dockInsertIndex = insertIndex;
        preview.action = action;
    }
    else return false;

    DragSourceList sources;
    sources.hasExternalFiles = true;
    for (const auto& path : paths)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = sources.entries.size();
        entry.filePath = path;
        entry.displayName = FileNameFromPath(path);
        sources.entries.push_back(std::move(entry));
    }
    if ((owned || copyOnly) && !preview.pinMaterializedItemsToDock)
        preview.action = DropAction::Copy;
    preview.fileBacked = true;
    return ExecuteDropPipeline(sources, preview, std::move(completion), synchronously, std::move(result));
}

DWORD DesktopApp::DropExternalSlotContent(IDataObject* dataObject,
    const ExternalSlotDestination& destination, DWORD allowedEffects,
    bool asynchronousSource, const std::vector<std::wstring>& knownPaths)
{
    if (!dataObject) return DROPEFFECT_NONE;
    const bool allowContent = (allowedEffects & DROPEFFECT_COPY) != 0;
    DWORD effect = destination.preview.action == DropAction::Move ? DROPEFFECT_MOVE :
        destination.preview.action == DropAction::Link ? DROPEFFECT_LINK : DROPEFFECT_COPY;
    if (destination.preview.pinMaterializedItemsToDock)
        effect = snowdesktop::dock_drop_rules::ChooseExternalMappingEffect(allowedEffects);
    if ((effect & allowedEffects) == 0) return DROPEFFECT_NONE;

    auto result = std::make_shared<snowdesktop::ShellFileOperationResult>();
    auto value = NewSlotContent(result);
    const auto stop = externalSlotReadStopSource_.get_token();
    const auto downloadUrls = [stop](const content::Paths& urls) {
        return DownloadSlotUrls(urls, stop);
    };
    auto read = [allowContent, downloadUrls](IDataObject* source, bool download,
        const content::Paths& files) {
        const auto resolvedFiles = files.empty() ? GetDropPaths(source) : files;
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

    FileOperationCompletion oleCompletion;
    auto marshaled = std::make_shared<MarshaledSlotSource>();
    if (asynchronousSource)
    {
        if (FAILED(CoMarshalInterThreadInterfaceInStream(IID_IDataObject,
                dataObject, &marshaled->stream)) ||
            !PrepareOleAsyncFileOperation(dataObject,
                effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect, {}, oleCompletion))
            return DROPEFFECT_NONE;
    }
    else
    {
        try { *value = read(dataObject, false, knownPaths); }
        catch (...) { return DROPEFFECT_NONE; }
    }
    auto finished = [value, keepReference = !destination.luaWidgetId.empty(),
        oleCompletion](bool succeeded) {
        if (succeeded && keepReference) value->owned = false;
        if (!succeeded)
            WriteDiagnosticLogEntry(L"External component drop failed during content reading or destination commit");
        if (oleCompletion) oleCompletion(succeeded);
    };
    if (!asynchronousSource && value->pendingUrls.empty())
    {
        const bool committed = CommitExternalSlotPaths(destination, value->paths,
            value->owned, value->copyOnly, finished, true, result);
        if (!committed) finished(false);
        return committed ? (effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect) : DROPEFFECT_NONE;
    }

    HWND window = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    if (!window || !IsWindow(window)) return DROPEFFECT_NONE;
    auto* completion = new (std::nothrow) ShellFileOperationUiCompletion{
        false, [this, destination, value, finished, result](bool succeeded) {
            if (!succeeded || !CommitExternalSlotPaths(destination, value->paths,
                    value->owned, value->copyOnly, finished, false, result))
                finished(false);
        }, false};
    if (!completion) return DROPEFFECT_NONE;
    const bool queued = externalSlotReadWorker_.Enqueue(
        snowdesktop::ShellReadRequest{[asynchronousSource, marshaled, value, read, downloadUrls, stop] {
            try
            {
                if (stop.stop_requested()) return false;
                if (asynchronousSource)
                {
                    auto source = marshaled->Take();
                    if (!source) return false;
                    *value = read(source.Get(), true, {});
                }
                else
                {
                    const auto urls = value->pendingUrls;
                    content::Readers readers;
                    readers.urls = [urls] { return urls; };
                    readers.download = downloadUrls;
                    readers.shortcut = [urls]() -> content::Paths {
                        const auto path = urls.empty() ? std::wstring{} : CreateUrlShortcut(urls.front());
                        return path.empty() ? content::Paths{} : content::Paths{path};
                    };
                    *value = content::Read(false, true, readers);
                }
                return !value->paths.empty();
            }
            catch (...) { return false; }
        }}, [window, completion](bool succeeded) {
            completion->succeeded = succeeded;
            if (!PostMessageW(window, kShellFileOperationCompletedMessage, 0,
                    reinterpret_cast<LPARAM>(completion))) delete completion;
        });
    if (!queued) delete completion;
    return queued ? effect : DROPEFFECT_NONE;
}
