#include "app/app.h"
#include "dock/dock_window_transition.h"
#include "common/l10n.h"
#include "diagnostics/diagnostic_log.h"
#include "widget/runtime/widget_engine.h"
#include "app/shell/icon_load_task.h"
#include "common/utils.h"
#include "desktop/desktop_source.h"
#include "drag_drop/ole_async_operation.h"
#include "platform/pending_window_message.h"
#include "app/lifecycle/startup_diagnostics.h"
#include "diagnostics/performance_trace.h"
#include "shell/shell_file_history.h"

#include <atomic>
#include <new>
#include <shldisp.h>
#include <utility>
#include <filesystem>

// Asynchronous path-based Shell file operations.

namespace
{
// The Shell can cancel or skip individual members. Observe the source names
// on the worker before posting completion, including for opaque IDropTarget
// handoffs that do not return per-item receipts.
struct ShellRemovalSources
{
    std::vector<std::wstring> paths;
    std::vector<std::wstring> departed;

    void ReadDepartures()
    {
        for (const auto& path : paths)
        {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                departed.push_back(path);
        }
    }
};
}

bool DesktopApp::CanApplyFileHistory(bool redo) const
{
    return shellFileOperationInFlight_ == 0 && pendingRenames_.empty() &&
        !renameController_.IsActive() && !dragSession_.HasContext() &&
        widgetAction_ == WidgetAction::None && !largeIconGesture_ &&
        !dragDropController_.IsTransportActive() &&
        (redo ? shellFileOperationWorker_.CanRedo() : shellFileOperationWorker_.CanUndo());
}

void DesktopApp::AppendFileHistoryMenu(HMENU menu)
{
    const auto append = [&](bool redo, UINT command, const char* key, const wchar_t* shortcut, const wchar_t* glyph) {
        if (!CanApplyFileHistory(redo)) return;
        const std::wstring label = std::wstring(_LW(key)) + L"\t" + shortcut;
        AppendMenuW(menu, MF_STRING, command, label.c_str());
        SetMenuItemIcon(menu, command, glyph, MenuIconFont::FluentRegular);
        SetMenuItemQuickAction(menu, command);
    };
    append(false, kFileUndoCommand, "app.menu.undo", L"Ctrl+Z", L"\uF199");
    append(true, kFileRedoCommand, "app.menu.redo", L"Ctrl+Y", L"\uF16E");
}

bool DesktopApp::QueueFileHistory(bool redo)
{
    if (!CanApplyFileHistory(redo)) return false;
    const HWND window = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    if (!window || !IsWindow(window)) return false;
    auto historyResult = std::make_shared<snowdesktop::file_history::Result>();
    auto* completion = new (std::nothrow) ShellFileOperationUiCompletion{
        false, [this, historyResult](bool) {
            for (const auto& [from, to] : historyResult->renames)
            {
                auto renamed = std::make_shared<snowdesktop::ShellRenameResult>();
                renamed->status = S_OK;
                renamed->sourcePath = from;
                renamed->path = to;
                // Apply the existing reference migration before the guarded
                // reload; file history does not restore a stale layout snapshot.
                pendingRenames_.push_back(std::move(renamed));
            }
            if (!historyResult->removed.empty()) RemoveDesktopKeysFromWidgets(historyResult->removed);
            ApplyPendingRenames();
            if (folderPreview_)
            {
                const auto& directory = dockFolderPopupWidget_.sourceFolderPath;
                const auto key = snowdesktop::shell_refresh::FolderKey(directory);
                const auto inDirectory = [&](const std::wstring& path) {
                    return snowdesktop::shell_refresh::FolderKey(std::filesystem::path(path).parent_path().wstring()) == key;
                };
                if (std::ranges::any_of(historyResult->renames, [&](const auto& rename) {
                        return inDirectory(rename.first) || inDirectory(rename.second);
                    }) || std::ranges::any_of(historyResult->removed, inDirectory) ||
                    std::ranges::any_of(historyResult->restored, inDirectory))
                    RequestFolderRefresh({directory});
            }
            RequestShellRefresh();
        }};
    if (!completion) return false;
    if (!shellFileOperationWorker_.EnqueueHistory(redo, historyResult, [window, completion](bool succeeded) {
        completion->succeeded = succeeded;
        if (!PostMessageW(window, kShellFileOperationCompletedMessage, 0, reinterpret_cast<LPARAM>(completion)))
            delete completion;
    }))
    {
        delete completion;
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    return true;
}

void DesktopApp::ReportPerformedDropEffect(
    IDataObject* dataObject, DWORD effect)
{
    snowdesktop::ole_async_operation::SetPerformedDropEffectData(dataObject, effect);
}

bool DesktopApp::QueueFileDeletion(std::vector<std::wstring> paths, bool permanentDelete,
    FILEOP_FLAGS extraRecycleFlags, std::wstring refreshFolder)
{
    if (paths.empty()) return false;

    const auto flags = static_cast<FILEOP_FLAGS>(permanentDelete
        ? FOF_WANTNUKEWARNING
        : (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | extraRecycleFlags));
    std::vector<snowdesktop::ShellFileOperationStep> steps;
    steps.push_back({FO_DELETE, std::move(paths), {}, flags});
    return QueueShellFileOperation(std::move(steps),
        [this, refreshFolder = std::move(refreshFolder)](bool succeeded) {
            if (!succeeded) return;
            if (refreshFolder.empty()) RequestShellRefresh();
            else RequestFolderRefresh({refreshFolder});
        });
}

bool DesktopApp::QueueShellFileOperation(
    std::vector<snowdesktop::ShellFileOperationStep> steps,
    FileOperationCompletion completion)
{
    snowdesktop::ShellFileOperationRequest request;
    request.steps = std::move(steps);
    return QueueShellFileOperation(
        std::move(request), std::move(completion));
}

bool DesktopApp::DispatchMaterializedFileOperation(snowdesktop::ShellFileOperationRequest request,
    FileOperationCompletion completion, bool synchronously)
{
    if (!synchronously) return QueueShellFileOperation(std::move(request), std::move(completion));
    std::vector<std::wstring> sources;
    for (const auto& step : request.steps)
        if (step.function == FO_MOVE) sources.insert(sources.end(), step.sources.begin(), step.sources.end());
    if (!sources.empty())
    {
        if (!request.result) request.result = std::make_shared<snowdesktop::ShellFileOperationResult>();
        BeginFileRemovalVisibility(sources);
    }
    const bool succeeded = snowdesktop::ShellFileOperationWorker::Execute(request);
    if (!sources.empty())
    {
        std::vector<std::wstring> departed;
        for (const auto& output : request.result->outputs)
            if (!output.referencesSource && !output.destination.empty() && !PathsEqualInsensitive(output.source, output.destination))
                departed.push_back(output.source);
        FinishFileRemovalVisibility(sources, departed);
    }
    if (completion) completion(succeeded);
    return succeeded;
}

bool DesktopApp::QueueShellFileOperation(
    snowdesktop::ShellFileOperationRequest request,
    FileOperationCompletion completion)
{
    HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    if (!completionWindow || !IsWindow(completionWindow) ||
        (request.steps.empty() && request.exactFileCopies.empty() &&
         request.shortcuts.empty()))
        return false;

    auto* result = new (std::nothrow)
        ShellFileOperationUiCompletion{
            false, std::move(completion) };
    if (!result)
    {
        snowdesktop::operation_feedback::Report({"app.operation.fileFailed", L"Queue allocation", ERROR_NOT_ENOUGH_MEMORY});
        return false;
    }

    std::vector<std::wstring> removedPaths;
    std::vector<DockEntry> removalPins;
    std::shared_ptr<ShellRemovalSources> removal;
    for (const auto& step : request.steps)
        if (step.function == FO_DELETE || step.function == FO_MOVE)
            removedPaths.insert(removedPaths.end(), step.sources.begin(), step.sources.end());
    if (!removedPaths.empty())
    {
        removal = std::make_shared<ShellRemovalSources>();
        removal->paths = std::move(removedPaths);
        for (size_t i = 0; i < dockEntries_.size(); ++i)
        {
            const auto& entry = dockEntries_[i];
            if (entry.type != DockEntryType::DesktopItem || IsRecycleBinDockEntry(entry)) continue;
            const auto itemIndex = FindItemIndexByKey(entry.reference);
            const auto& path = itemIndex < items_.size() ? items_[itemIndex].parsingName : entry.reference;
            if (std::none_of(removal->paths.begin(), removal->paths.end(), [&](const auto& removed) {
                    return _wcsicmp(path.c_str(), removed.c_str()) == 0;
                })) continue;
            removalPins.push_back(entry);
            for (const auto& container : containers_)
                if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                    dock->PrepareEntryRemoval(i);
        }
        auto callback = std::move(result->callback);
        result->callback = [this, removal, removalPins,
            callback = std::move(callback)](bool succeeded) {
            FinishFileRemovalVisibility(removal->paths, removal->departed);
            // A cancelled/failed operation must retain its original ownership.
            // Successful user deletions/moves must not become recoverable gaps.
            if (succeeded) RemoveDesktopKeysFromWidgets(removal->departed);
            else
                for (const auto& container : containers_)
                    if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                        for (const auto& pin : removalPins) dock->CancelEntryRemoval(pin);
            if (callback) callback(succeeded);
        };
    }
    const bool queued = shellFileOperationWorker_.Enqueue(
        std::move(request),
        [completionWindow, result, removal](bool succeeded) {
            if (removal) removal->ReadDepartures();
            result->succeeded = succeeded;
            if (!PostMessageW(
                    completionWindow,
                    kShellFileOperationCompletedMessage,
                    0,
                    reinterpret_cast<LPARAM>(result)))
                delete result;
        });
    if (!queued)
    {
        for (const auto& container : containers_)
            if (auto* dock = dynamic_cast<DockContainer*>(container.get()))
                for (const auto& pin : removalPins) dock->CancelEntryRemoval(pin);
        delete result;
        snowdesktop::operation_feedback::Report({"app.operation.fileFailed", L"File operation queue unavailable", ERROR_NOT_READY});
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    if (removal) BeginFileRemovalVisibility(removal->paths);
    ApplyFloatingDockLayerPolicy();
    return true;
}

bool DesktopApp::IsRecycleDropTarget(Item* item) const
{
    if (const auto* icon = dynamic_cast<DesktopIcon*>(item))
    {
        const auto* model = icon->GetDesktopItem();
        return model && _wcsicmp(model->desktopIconClsid.c_str(), kDesktopIconClsidRecycleBin) == 0;
    }
    const auto* pin = dynamic_cast<DockEntryItem*>(item);
    return pin && pin->GetEntryIndex() < dockEntries_.size() && IsRecycleBinDockEntry(dockEntries_[pin->GetEntryIndex()]);
}

bool DesktopApp::QueueShellDrop(
    std::vector<std::wstring> sourcePaths,
    std::wstring targetParsingName,
    DWORD keyState,
    POINTL screenPoint,
    DWORD allowedEffects,
    FileOperationCompletion completion, bool recycleTarget)
{
    HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    if (!completionWindow || !IsWindow(completionWindow) ||
        sourcePaths.empty() || targetParsingName.empty())
        return false;

    auto* result = new (std::nothrow)
        ShellFileOperationUiCompletion{
            false, std::move(completion) };
    if (!result)
    {
        snowdesktop::operation_feedback::Report({"app.operation.fileFailed", L"Queue allocation", ERROR_NOT_ENOUGH_MEMORY});
        return false;
    }

    snowdesktop::ShellDropRequest request;
    std::shared_ptr<ShellRemovalSources> removal;
    if (recycleTarget && ChooseDropEffect(keyState, allowedEffects) == DROPEFFECT_MOVE)
    {
        removal = std::make_shared<ShellRemovalSources>();
        removal->paths = sourcePaths;
        result->callback = [this, removal, done = std::move(result->callback)](bool succeeded) {
            FinishFileRemovalVisibility(removal->paths, removal->departed);
            if (done) done(succeeded);
        };
    }
    request.sources = std::move(sourcePaths);
    request.targetParsingName = std::move(targetParsingName);
    request.keyState = keyState;
    request.screenPoint = screenPoint;
    request.allowedEffects = allowedEffects;
    const bool queued = shellFileOperationWorker_.Enqueue(
        std::move(request),
        [completionWindow, result, removal](bool succeeded) {
            if (removal) removal->ReadDepartures();
            result->succeeded = succeeded;
            if (!PostMessageW(
                    completionWindow,
                    kShellFileOperationCompletedMessage,
                    0,
                    reinterpret_cast<LPARAM>(result)))
                delete result;
        });
    if (!queued)
    {
        delete result;
        snowdesktop::operation_feedback::Report({"app.operation.fileFailed", L"File operation queue unavailable", ERROR_NOT_READY});
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    if (removal) BeginFileRemovalVisibility(removal->paths);
    ApplyFloatingDockLayerPolicy();
    return true;
}

bool DesktopApp::QueueAsyncShellDrop(
    IDataObject* dataObject,
    std::wstring targetParsingName,
    DWORD keyState,
    POINTL screenPoint,
    DWORD allowedEffects,
    FileOperationCompletion completion,
    std::function<bool(IDataObject*)> dataObjectPreflight,
    bool allowSynchronousClipboardSource)
{
    HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    const DWORD effects = allowedEffects &
        (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (!completionWindow || !IsWindow(completionWindow) ||
        !dataObject || targetParsingName.empty() ||
        effects == DROPEFFECT_NONE)
        return false;

    ComPtr<IDataObjectAsyncCapability> asyncCapability;
    BOOL asyncMode = FALSE;
    if (FAILED(dataObject->QueryInterface(
            IID_PPV_ARGS(&asyncCapability))) ||
        !asyncCapability ||
        asyncCapability->GetAsyncMode(&asyncMode) != S_OK ||
        !asyncMode)
    {
        // Unlike a live drag, paste retains the copied IDataObject through
        // the marshal packet even when the source has no async protocol.
        // Existing drag callers still require the Start/EndOperation pair.
        if (!allowSynchronousClipboardSource)
            return false;
        asyncCapability.Reset();
    }

    auto* result = new (std::nothrow)
        ShellFileOperationUiCompletion{
            false, std::move(completion) };
    if (!result)
    {
        snowdesktop::operation_feedback::Report({"app.operation.fileFailed", L"Queue allocation", ERROR_NOT_ENOUGH_MEMORY});
        return false;
    }

    ComPtr<IStream> dataStream;
    HRESULT marshalResult = CoMarshalInterThreadInterfaceInStream(
        IID_IDataObject, dataObject, &dataStream);
    if (FAILED(marshalResult) || !dataStream)
    {
        delete result;
        return false;
    }
    ComPtr<IStream> asyncStream;
    if (asyncCapability)
    {
        marshalResult = CoMarshalInterThreadInterfaceInStream(
            IID_IDataObjectAsyncCapability,
            asyncCapability.Get(), &asyncStream);
        if (FAILED(marshalResult) || !asyncStream)
        {
            ComPtr<IDataObject> discarded;
            CoGetInterfaceAndReleaseStream(
                dataStream.Detach(), IID_PPV_ARGS(&discarded));
            delete result;
            return false;
        }

        const HRESULT startResult =
            asyncCapability->StartOperation(nullptr);
        if (FAILED(startResult))
        {
            ComPtr<IDataObject> discardedData;
            CoGetInterfaceAndReleaseStream(
                dataStream.Detach(), IID_PPV_ARGS(&discardedData));
            ComPtr<IDataObjectAsyncCapability> discardedAsync;
            CoGetInterfaceAndReleaseStream(
                asyncStream.Detach(), IID_PPV_ARGS(&discardedAsync));
            delete result;
            return false;
        }
    }

    snowdesktop::ShellDropRequest request;
    request.marshaledDataObject = std::move(dataStream);
    request.marshaledAsyncCapability = std::move(asyncStream);
    request.targetParsingName = std::move(targetParsingName);
    request.keyState = keyState;
    request.screenPoint = screenPoint;
    request.allowedEffects = effects;
    request.clipboardPaste = allowSynchronousClipboardSource;
    request.dataObjectPreflight = std::move(dataObjectPreflight);
    const bool queued = shellFileOperationWorker_.Enqueue(
        std::move(request),
        [completionWindow, result](bool succeeded) {
            result->succeeded = succeeded;
            if (!PostMessageW(
                    completionWindow,
                    kShellFileOperationCompletedMessage,
                    0,
                    reinterpret_cast<LPARAM>(result)))
                delete result;
        });
    if (!queued)
    {
        // Enqueue consumes the marshal packets and balances StartOperation on
        // every rejection path.
        delete result;
        snowdesktop::operation_feedback::Report({"app.operation.fileFailed", L"File operation queue unavailable", ERROR_NOT_READY});
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    ApplyFloatingDockLayerPolicy();
    return true;
}

bool DesktopApp::PrepareOleAsyncFileOperation(
    IDataObject* dataObject, DWORD completionEffect,
    FileOperationCompletion completion, FileOperationCompletion& asyncCompletion)
{
    return snowdesktop::ole_async_operation::Prepare(dataObject, completionEffect,
        std::move(completion), asyncCompletion);
}

void DesktopApp::OnShellFileOperationCompleted(LPARAM lParam)
{
    auto result = snowdesktop::TakeShellFileOperationCompletion(lParam);
    if (!result || exitRequested_)
        return;
    if (result->fileOperation && shellFileOperationInFlight_ > 0)
        --shellFileOperationInFlight_;
    // The completed request must no longer block its own callback from
    // reloading Shell state.  Other queued requests still keep the counter
    // non-zero and preserve the existing debounce behavior.
    if (result->callback)
        result->callback(result->succeeded);
    if (shellFileOperationInFlight_ > 0)
        return;

    if ((shellReloadPending_ ||
         shellDockFolderPopupRefreshPending_) &&
        hwnd_ && IsWindow(hwnd_))
    {
        // We already know that the operation/read has finished. Run the same
        // guarded drain as the timer now, instead of adding a second debounce.
        // It still defers model replacement during edits, menus and drags.
        OnTimer(kShellChangeTimerId);
    }
    // SHFileOperationW can promote the Explorer/foreground window while the
    // worker thread owns its progress UI. Restore the floating Dock layer once
    // after the final operation instead of forcing a DWM restack per task.
    if (!result->fileOperation)
        return; // A metadata read must never activate or restack the desktop.
    RestoreDesktopWindowLayer();
    if (snowdesktop::floating_dock_rules::
            ShouldRefocusFloatingDockKeyboardSession(
                floatingDockVisible_,
                floatingDockKeyboardSessionActive_,
                shellFileOperationInFlight_,
                shellPopupMenuLayerDepth_))
        RefocusFloatingDockKeyboardSession();
}

void DesktopApp::StopShellFileOperationWorker()
{
    initialShellRead_.Stop();
    for (auto& reader : initialLocalReads_) reader.Stop();
    externalSlotReadStopSource_.request_stop();
    externalSlotReadWorker_.Stop();
    shellFileOperationWorker_.Stop();
    readyShellRefresh_.reset();

    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    // The worker Stop() path runs queued completions without going through
    // OnShellFileOperationCompleted, so clear the UI-side in-flight state
    // even when the completion window is already gone during shutdown.
    shellFileOperationInFlight_ = 0;
    if (!completionWindow || !IsWindow(completionWindow))
        return;

    MSG message{};
    while (snowdesktop::TakePendingWindowMessage(message, completionWindow,
        kShellFileOperationCompletedMessage) == snowdesktop::PendingWindowMessage::Ready)
    {
        delete reinterpret_cast<ShellFileOperationUiCompletion*>(
            message.lParam);
    }
}

bool snowdesktop::shell_refresh::Read(const Request& request, Snapshot& snapshot)
{
    const bool showHidden = AreExplorerHiddenItemsVisible();
    return ReadSources(request, snapshot,
        [showHidden](const Request& input, Snapshot& output) {
            return ReadDesktop(input.iconVisibility, showHidden, output.desktopItems,
                &output.metadata, input.publishDesktopItem);
        },
        [showHidden](const std::wstring& path, MetadataCache& metadata) {
            return ReadFolder(path, showHidden, &metadata);
        });
}

void DesktopApp::RequestShellRefresh()
{
    if (exitRequested_)
        return;
    shellRefreshScope_.Full();
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    const bool alreadyPending = shellReloadPending_;
    shellReloadPending_ = true;
    // A filesystem event says nothing about saved layout or Lua storage.
    // Preserve any separately requested full reload, but never introduce one.
    // Keep the first wake deadline. Resetting it for every notification can
    // indefinitely postpone feedback during a stream of filesystem changes.
    // An active read/operation will drain pending work from its completion.
    if (!alreadyPending && hwnd_ && IsWindow(hwnd_))
        SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
}

void DesktopApp::RequestFolderRefresh(const std::vector<std::wstring>& paths)
{
    if (exitRequested_) return;
    for (const auto& path : paths)
    {
        if (snowdesktop::debug_profile::Enabled() &&
            snowdesktop::shell_refresh::FolderKey(path) ==
                snowdesktop::shell_refresh::FolderKey(snowdesktop::desktop_source::Directory()))
        {
            RequestShellRefresh();
            continue;
        }
        ++folderReadVersions_[snowdesktop::shell_refresh::FolderKey(path)];
        QueueFolderRead(path);
    }
}

bool DesktopApp::QueueFolderRead(const std::wstring& path)
{
    if (exitRequested_ || path.empty()) return false;
    const auto key = snowdesktop::shell_refresh::FolderKey(path);
    if (!folderReadsPending_.insert(key).second)
    {
        folderReadRetries_.Forget(key);
        return true;
    }
    const auto version = folderReadVersions_[key];
    const bool hidden = AreExplorerHiddenItemsVisible();
    if (!folderReadWork_.Submit(L"folder:" + key, [path, hidden] {
        auto snapshot = std::make_shared<snowdesktop::shell_refresh::Snapshot>();
        snapshot->foldersOnly = snapshot->desktopComplete = true;
        const auto started = GetTickCount64();
        snapshot->folders.emplace(snowdesktop::shell_refresh::FolderKey(path),
            snowdesktop::shell_refresh::ReadFolder(path, hidden, &snapshot->metadata));
        snapshot->readMs = GetTickCount64() - started;
        return snapshot;
    }, [this, key, path, version](auto snapshot) {
        folderReadsPending_.erase(key);
        if (folderReadVersions_[key] != version) { QueueFolderRead(path); return; }
        const bool pointerInteraction = dragSession_.HasContext() ||
            dragDropController_.IsTransportActive() || mouseDown_;
        const bool deferModel = pointerInteraction || shellFileOperationInFlight_ > 0;
        folderReadDelivery_.Deliver(key, version, std::move(snapshot), deferModel,
            [this, pointerInteraction](auto& ready) {
                // First content can populate the transient popup. Keep the
                // shared folder model and pending drop placements deferred.
                if (!dockFolderPopupOpen_ || !dockFolderPopupLoading_)
                    return;
                if (pointerInteraction &&
                    (dragSession_.Source() == dockFolderPopupContainer_.get() ||
                     !(dragSession_.IsActive() || dragDropController_.IsExternalDragActive()) ||
                     !((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)))
                    return;
                const auto folder = ready.folders.find(
                    snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
                if (folder == ready.folders.end()) return;
                RefreshDockFolderPopup(&folder->second);
                POINT point{};
                if (pointerInteraction && GetCursorPos(&point) && ScreenToClient(hwnd_, &point))
                    RefreshDwellDragTarget(point);
            },
            [this](auto& ready) { ApplyFolderRefresh(ready); });
    }, hwnd_, kBackgroundShellReadyMessage))
    {
        folderReadsPending_.erase(key);
        folderReadRetries_.Remember(key, path);
        return false;
    }
    folderReadRetries_.Forget(key);
    return true;
}

void DesktopApp::RetryFolderReads()
{
    // A rejected request is retried after Drain frees capacity, even without
    // another filesystem notification. Bound work per maintenance pass.
    folderReadRetries_.Retry([this](const auto& path) {
        return QueueFolderRead(path);
    });
}

void DesktopApp::ApplyFolderRefresh(snowdesktop::shell_refresh::Snapshot& snapshot)
{
    snowdesktop::performance::Scope trace("shell.refresh", "folder.snapshot.apply");
    // The common timer has fenced drags, menus, edits and file operations.
    // Only replace the affected folder models; leave desktop placement and
    // unrelated containers untouched.
    reloading_ = true;
    ClearPopupDragTarget();
    ClearPopupMouseDownItem();
    mouseDownHit_ = nullptr;
    pendingCtrlToggleWidgetItem_ = nullptr;
    std::unordered_set<std::wstring> changed;
    bool orderChanged = false;
    for (const auto& [path, folder] : snapshot.folders)
    {
        if (!folder.complete)
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, (L"Folder refresh failed; retaining current entries: " + path).c_str(), DiagnosticLogLevel::Warning);
        else
            ApplyPendingFolderPreviewPlacements(folder);
    }
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping) continue;
        const auto folder = snapshot.folders.find(
            snowdesktop::shell_refresh::FolderKey(widget.sourceFolderPath));
        if (folder == snapshot.folders.end() || !folder->second.complete) continue;
        const auto oldOrder = widget.itemKeys;
        EnumerateFolderMappingEntries(widget, true, &folder->second);
        // A folder-only result may arrive without a structural desktop reload.
        // Reconcile the actual outputs before publishing the mapped view and
        // saving its order; alphabetical enumeration is not the chosen drop.
        ApplyPendingFolderPlacements(widget, widget.id, {});
        orderChanged |= oldOrder != widget.itemKeys;
        changed.insert(widget.id);
    }
    std::vector<RECT> desktopDamage;
    for (auto& container : containers_)
    {
        auto* widget = dynamic_cast<WidgetContainer*>(container.get());
        if (!widget || !widget->GetWidgetData()) continue;
        const auto& data = *widget->GetWidgetData();
        auto* search = dynamic_cast<ScrollingItemWidget*>(widget);
        const bool searchActive = search && search->IsSearchActive();
        if (search) search->RefreshFolderSearch();
        auto* mapping = dynamic_cast<FolderMapping*>(widget);
        const bool mappingChanged = mapping && changed.contains(data.id);
        if (mappingChanged)
            mapping->InvalidateFilterCache();
        auto* group = dynamic_cast<FileGroup*>(widget);
        const bool groupChanged = group && std::any_of(data.childWidgetIds.begin(), data.childWidgetIds.end(),
            [&](const auto& id) { return changed.contains(id); });
        if (groupChanged)
            group->InvalidateHostedView();
        if (searchActive || mappingChanged || groupChanged)
        {
            RECT dirty = GetStandaloneWidgetFrameRect(data);
            if (IsRectEmpty(&dirty)) continue;
            InflateRect(&dirty, 4, 4);
            desktopDamage.push_back(dirty);
        }
    }
    std::unordered_set<std::wstring> cachePaths;
    for (const auto& widget : widgets_)
        if (widget.type == DesktopWidgetType::FolderMapping)
            cachePaths.insert(snowdesktop::shell_refresh::FolderKey(widget.sourceFolderPath));
    // The preview uses the live snapshot below. Caching its transient paths
    // here would retain file metadata from every directory visited.
    if (dockFolderPopupOpen_ && !folderPreview_)
        cachePaths.insert(snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
    for (auto& [key, metadata] : snapshot.metadata.folders)
        if (cachePaths.contains(key))
            shellMetadataCache_.folders.insert_or_assign(key, std::move(metadata));
    if (dockFolderPopupOpen_)
    {
        const auto folder = snapshot.folders.find(
            snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
        if (folder != snapshot.folders.end())
            RefreshDockFolderPopup(&folder->second);
    }
    RefreshInactiveInlineFolderPreviews(snapshot);
    ApplyFileRemovalRefresh(snapshot);
    if (orderChanged) SaveLayoutSlots();
    // The matching Dock folder snapshot has already published its geometry.
    if (!dockFolderPopupOpen_) RefreshOpenCollectionPopupGeometry();
    InvalidateDragStaticScene(false);
    reloading_ = false;
    if (hwnd_)
        for (const auto& dirty : desktopDamage)
            InvalidateRect(hwnd_, &dirty, FALSE);
    // A Dock folder publishes only when its own snapshot changes. A second
    // invalidation here would also repaint unchanged or unrelated results.
    if (!dockFolderPopupOpen_ || !popupAnchoredToDock_ || !dockFolderPopupMappingWidgetId_.empty() ||
        IsLuaPanelHostedByFloatingWindow() || IsLuaStackIndicatorVisible())
        InvalidateFloatingPopupWindow(false);
    InvalidateQuickNavigationWindow();
    wchar_t timing[192]{};
    swprintf_s(timing, L"Folder refresh async: folders=%zu readMs=%llu metadataHits=%zu metadataQueries=%zu",
        snapshot.folders.size(), snapshot.readMs, snapshot.metadata.hits, snapshot.metadata.queries);
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, timing);
}

void DesktopApp::RefreshShellItemsAsync()
{
    if (dockWindowTransition_ && dockWindowTransition_->IsActive())
    {
        // A completed filesystem read may otherwise rebuild and save the
        // entire desktop in the middle of a restore animation.
        if (hwnd_) SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
        snowdesktop::performance::Value("shell.refresh", "animation_deferred", {}, 1);
        return;
    }
    snowdesktop::performance::Scope refreshScope("shell.refresh", "dispatch");
    if (initialShellReadPending_)
    {
        StartInitialShellRead();
        return;
    }
    if (readyShellRefresh_)
    {
        auto snapshot = std::exchange(readyShellRefresh_, {});
        if (!snapshot->desktopComplete)
        {
            shellReloadPending_ = false;
            shellDockFolderPopupRefreshPending_ = false;
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"Shell refresh read failed; retaining current model", DiagnosticLogLevel::Warning);
            CompleteLayoutRestore(snowdesktop::SettingsActionResult::Failure(
                _LW("settings.backup.restoreLayout.commitFailed")));
            return; // Retry on a new event, not in an unbounded timer loop.
        }
        if (snapshot->foldersOnly)
        {
            ApplyFolderRefresh(*snapshot);
            return;
        }
        for (const auto& [path, folder] : snapshot->folders)
            ApplyPendingFolderPreviewPlacements(folder);
        const ULONGLONG started = GetTickCount64();
        const size_t count = snapshot->desktopItems.size();
        const size_t metadataHits = snapshot->metadata.hits;
        const size_t metadataQueries = snapshot->metadata.queries;
        shellMetadataCache_ = std::move(snapshot->metadata);
        const bool contentOnly = desktopItemsReady_ && !layoutReload_.Pending() && !HasPendingDesktopNewItems() &&
            snowdesktop::shell_refresh::ApplyDesktopContentRefresh(items_, *snapshot,
                [this](const DesktopItem& item) {
                    IconLoadTask task;
                    task.sourceStamp = snowdesktop::shell_icon_request::Stamp(item);
                    task.serial = iconLoadSerial_;
                    task.layoutKey = item.layoutKey;
                    task.absolutePidl.reset(ILCloneFull(item.absolutePidl.get()));
                    task.sysIconIndex = item.sysIconIndex;
                    task.parsingName = item.parsingName;
                    task.isDesktopItem = true;
                    task.phase = item.iconState == IconState::IconReady
                        ? IconLoadPhase::Phase2 : IconLoadPhase::Phase1;
                    EnqueueIconLoad(std::move(task));
                });
        if (contentOnly)
        {
            ApplyFileRemovalRefresh(*snapshot);
            // Nested changes do not alter desktop membership, but can change
            // recursive matches. Refresh queries without replacing their state.
            for (auto& container : containers_)
                if (auto* search = dynamic_cast<ScrollingItemWidget*>(container.get())) search->RefreshFolderSearch();
            shellReloadPending_ = false;
            shellRefreshScope_.Full();
            InvalidateDragStaticScene();
            InvalidateRect(hwnd_, nullptr, FALSE);
            InvalidateDockRects();
            if (widgetEngine_) widgetEngine_->NotifyDesktopChanged("reload");
        }
        else
            ReloadItems(false, snapshot.get());
        snowdesktop::performance::Value("shell.refresh", "content_only", {}, contentOnly ? 1 : 0);
        if (dockFolderPopupOpen_)
        {
            const auto folder = snapshot->folders.find(
                snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
            if (folder != snapshot->folders.end())
                RefreshDockFolderPopup(&folder->second);
        }
        InvalidateFloatingPopupWindow(false);
        InvalidateQuickNavigationWindow();
        wchar_t timing[512]{};
        swprintf_s(timing, L"Shell refresh async: items=%zu readMs=%llu applyMs=%llu contentOnly=%d "
            L"desktopMs=%llu foldersMs=%llu metadataHits=%zu metadataQueries=%zu "
            L"modelMs=%llu layoutMs=%llu saveMs=%llu rebuildMs=%llu notifyMs=%llu",
            count, snapshot->readMs, GetTickCount64() - started, contentOnly ? 1 : 0,
            snapshot->desktopReadMs, snapshot->folderReadMs,
            metadataHits, metadataQueries, snapshot->modelMs, snapshot->layoutMs,
            snapshot->saveMs, snapshot->rebuildMs, snapshot->notifyMs);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, timing);
        return;
    }

    const auto revision = shellRefreshRevision_.Begin();
    if (!revision)
        return;
    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    auto request = BuildShellRefreshRequest();
    snowdesktop::shell_refresh::SelectFolders(request, shellRefreshScope_);

    auto snapshot = std::make_shared<snowdesktop::shell_refresh::Snapshot>();
    // Copy value metadata/PIDLs; the worker never observes mutable UI storage.
    // Startup/manual enumeration seeds this cache, so the first file change is warm too.
    if (request.foldersOnly)
    {
        for (const auto& path : request.folders)
        {
            const auto key = snowdesktop::shell_refresh::FolderKey(path);
            if (const auto cached = shellMetadataCache_.folders.find(key);
                cached != shellMetadataCache_.folders.end())
                snapshot->metadata.folders.try_emplace(key, cached->second);
        }
    }
    else
        snapshot->metadata = shellMetadataCache_;
    RequestFolderRefresh(request.folders);
    request.folders.clear();
    const bool queued = shellModelWork_.Submit(L"desktop-read", [request = std::move(request), snapshot] {
        snapshot->desktopComplete = snowdesktop::shell_refresh::Read(request, *snapshot);
        return snapshot;
    }, [this, revision = *revision](auto result) {
        const bool current = shellRefreshRevision_.Finish(revision);
        if (current && result) readyShellRefresh_ = std::move(result);
        else if (current)
            CompleteLayoutRestore(snowdesktop::SettingsActionResult::Failure(
                _LW("settings.backup.restoreLayout.commitFailed")));
        shellReloadPending_ = true;
        if (hwnd_) SetTimer(hwnd_, kShellChangeTimerId, 1, nullptr);
    }, completionWindow, kBackgroundShellReadyMessage);
    if (!queued)
    {
        shellRefreshRevision_.Finish(*revision);
        shellReloadPending_ = false;
        CompleteLayoutRestore(snowdesktop::SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed")));
        return;
    }
    shellReloadPending_ = true;
}

snowdesktop::shell_refresh::Request DesktopApp::BuildShellRefreshRequest() const
{
    snowdesktop::shell_refresh::Request request;
    request.iconVisibility = settingsIconVisibility_;
    for (const auto& widget : widgets_)
        if (widget.type == DesktopWidgetType::FolderMapping)
            request.folders.push_back(widget.sourceFolderPath);
    if (dockFolderPopupOpen_)
        request.folders.push_back(dockFolderPopupWidget_.sourceFolderPath);
    return request;
}

void DesktopApp::StartInitialShellRead()
{
    if (exitRequested_ || !initialShellReadPending_ || initialShellRead_.Pending())
        return;
    layoutReload_.ApplyPendingRead([this] { ReloadLayoutStateFromDisk(); });
    const auto revision = shellRefreshRevision_.Begin();
    if (!revision) return;
    initialShellReadRevision_ = *revision;
    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    for (size_t i = 0; i < std::size(initialLocalReads_); ++i)
    {
        if (initialLocalReads_[i].Pending()) continue;
        initialLocalReadRevisions_[i] = *revision;
        initialLocalReads_[i].Start(BuildShellRefreshRequest(), completionWindow,
            kBackgroundShellReadyMessage);
    }
    auto request = BuildShellRefreshRequest();
    RequestFolderRefresh(request.folders);
    request.folders.clear();
    if (!initialShellRead_.Start(std::move(request), completionWindow, kBackgroundShellReadyMessage))
    {
        shellRefreshRevision_.Finish(*revision);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"Startup Shell reader could not start", DiagnosticLogLevel::Warning);
        CompleteLayoutRestore(snowdesktop::SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed")));
    }
}

void DesktopApp::PollInitialShellRead(std::chrono::milliseconds budget)
{
    // Commit on the UI thread only, using the same revision/interaction fences
    // as normal refresh. The worker must not overwrite edits made after timeout.
    if (exitRequested_ || !initialShellReadPending_ || reloading_ ||
        compositionPaintInProgress_ || mouseDown_ || renameController_.IsActive() ||
        HasActiveContextMenuSession() || shellFileOperationInFlight_ > 0 ||
        !pendingRenames_.empty() || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive())
        return;
    auto snapshot = initialShellRead_.TakeReady(budget);
    snowdesktop::shell_refresh::Snapshot progress;
    progress.desktopIncremental = true;
    std::unordered_set<std::wstring> seen;
    const auto append = [&](std::vector<DesktopItem> items) {
        for (auto& item : items)
            if (seen.insert(ToUpperInvariant(item.layoutKey)).second)
                progress.desktopItems.push_back(std::move(item));
    };
    for (size_t i = 0; i < std::size(initialLocalReads_); ++i)
    {
        auto& reader = initialLocalReads_[i];
        auto local = reader.TakeReady();
        auto items = local ? std::move(local->desktopItems) : reader.TakeProgress();
        if (shellRefreshRevision_.IsCurrent(initialLocalReadRevisions_[i]))
            append(std::move(items));
        else if (!reader.Pending())
        {
            // Local changes can still refresh while the virtual namespace is
            // blocked. Never wait for that older root read to retire first.
            initialLocalReadRevisions_[i] = shellRefreshRevision_.Current();
            const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
            reader.Start(BuildShellRefreshRequest(), completionWindow, kBackgroundShellReadyMessage);
        }
    }
    auto shellItems = initialShellRead_.TakeProgress();
    if (shellRefreshRevision_.IsCurrent(initialShellReadRevision_))
    {
        append(std::move(shellItems));
        if (snapshot && !snapshot->desktopComplete)
            append(std::move(snapshot->desktopItems));
    }
    if ((!snapshot || !snapshot->desktopComplete ||
            !shellRefreshRevision_.IsCurrent(initialShellReadRevision_)) &&
        !progress.desktopItems.empty())
    {
        const size_t count = progress.desktopItems.size();
        ReloadItems(false, &progress);
        wchar_t timing[320]{};
        swprintf_s(timing, L"Startup Shell partial: items=%zu modelMs=%llu layoutMs=%llu "
            L"saveMs=%llu rebuildMs=%llu notifyMs=%llu", count,
            progress.modelMs, progress.layoutMs, progress.saveMs,
            progress.rebuildMs, progress.notifyMs);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, timing);
    }
    if (!snapshot) return;
    if (!shellRefreshRevision_.Finish(initialShellReadRevision_))
    {
        const auto message = L"Startup Shell snapshot superseded: readRevision=" +
            std::to_wstring(initialShellReadRevision_) + L" currentRevision=" +
            std::to_wstring(shellRefreshRevision_.Current()) + L" items=" +
            std::to_wstring(snapshot->desktopItems.size());
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, message.c_str());
        StartInitialShellRead();
        return;
    }
    if (!snapshot->desktopComplete)
    {
        shellReloadPending_ = false;
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell,
            L"Startup Shell read failed; saved layout retained, retry on next refresh",
            DiagnosticLogLevel::Warning);
        CompleteLayoutRestore(snowdesktop::SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed")));
        return;
    }
    shellMetadataCache_ = std::move(snapshot->metadata);
    snowdesktop::startup_diagnostics::Scope startup(L"ApplyStartupSnapshot", true,
        snapshot->desktopItems.size());
    ReloadItems(false, snapshot.get());
    if (desktopItemsReady_)
    {
        initialShellReadPending_ = false;
        for (auto& reader : initialLocalReads_) reader.Stop();
        InvalidateRect(hwnd_, nullptr, TRUE);
        InvalidateFloatingDockWindow(false);
        snowdesktop::startup_diagnostics::Call(L"RefreshDockRunningWindows", [&] {
            RefreshDockRunningWindows(false);
        });
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Shell, L"Startup Shell snapshot applied");
    }
}
