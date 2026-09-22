#include "app.h"
#include "startup_diagnostics.h"

#include <atomic>
#include <new>
#include <shldisp.h>
#include <utility>
#include <filesystem>

// Asynchronous path-based Shell file operations.

namespace
{

void SetPerformedDropEffectData(IDataObject* dataObject, DWORD effect)
{
    if (!dataObject)
        return;
    const CLIPFORMAT format = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));
    if (!format)
        return;

    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!storage)
        return;
    auto* value = static_cast<DWORD*>(GlobalLock(storage));
    if (!value)
    {
        GlobalFree(storage);
        return;
    }
    *value = effect;
    GlobalUnlock(storage);

    FORMATETC formatEtc{
        format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = storage;
    if (FAILED(dataObject->SetData(&formatEtc, &medium, TRUE)))
        ReleaseStgMedium(&medium);
}

class OleAsyncFileOperationCompletion final
{
public:
    OleAsyncFileOperationCompletion(
        ComPtr<IStream> stream, DWORD effect)
        : stream_(std::move(stream)), effect_(effect)
    {
    }

    ~OleAsyncFileOperationCompletion()
    {
        Finish(false);
    }

    void Finish(bool succeeded)
    {
        if (finished_.exchange(true))
            return;
        ComPtr<IDataObjectAsyncCapability> asyncCapability;
        if (stream_)
        {
            CoGetInterfaceAndReleaseStream(
                stream_.Detach(), IID_PPV_ARGS(&asyncCapability));
        }
        if (asyncCapability)
        {
            // A successful target-side move is optimized: the target already
            // moved/deleted the source, so the source must not delete it again.
            if (succeeded && effect_ == DROPEFFECT_NONE)
            {
                ComPtr<IDataObject> dataObject;
                if (SUCCEEDED(asyncCapability.As(&dataObject)))
                    SetPerformedDropEffectData(
                        dataObject.Get(), DROPEFFECT_NONE);
            }
            asyncCapability->EndOperation(
                succeeded ? S_OK : E_ABORT,
                nullptr,
                succeeded ? effect_ : DROPEFFECT_NONE);
        }
    }

private:
    std::atomic_bool finished_ = false;
    ComPtr<IStream> stream_;
    DWORD effect_ = DROPEFFECT_NONE;
};

}

void DesktopApp::ReportPerformedDropEffect(
    IDataObject* dataObject, DWORD effect)
{
    SetPerformedDropEffectData(dataObject, effect);
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
        return false;

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
        delete result;
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    ApplyFloatingDockLayerPolicy();
    return true;
}

bool DesktopApp::QueueShellDrop(
    std::vector<std::wstring> sourcePaths,
    std::wstring targetParsingName,
    DWORD keyState,
    POINTL screenPoint,
    DWORD allowedEffects,
    FileOperationCompletion completion)
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
        return false;

    snowdesktop::ShellDropRequest request;
    request.sources = std::move(sourcePaths);
    request.targetParsingName = std::move(targetParsingName);
    request.keyState = keyState;
    request.screenPoint = screenPoint;
    request.allowedEffects = allowedEffects;
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
        delete result;
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
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
        return false;

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
        return false;
    }
    ++shellFileOperationInFlight_;
    shellRefreshRevision_.Invalidate();
    readyShellRefresh_.reset();
    ApplyFloatingDockLayerPolicy();
    return true;
}

bool DesktopApp::PrepareOleAsyncFileOperation(
    IDataObject* dataObject,
    DWORD completionEffect,
    FileOperationCompletion completion,
    FileOperationCompletion& asyncCompletion)
{
    asyncCompletion = {};
    const DWORD effect = completionEffect &
        (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (!dataObject)
        return false;

    ComPtr<IDataObjectAsyncCapability> asyncCapability;
    BOOL asyncMode = FALSE;
    if (FAILED(dataObject->QueryInterface(
            IID_PPV_ARGS(&asyncCapability))) ||
        !asyncCapability ||
        FAILED(asyncCapability->GetAsyncMode(&asyncMode)) ||
        !asyncMode)
        return false;

    ComPtr<IStream> asyncStream;
    if (FAILED(CoMarshalInterThreadInterfaceInStream(
            IID_IDataObjectAsyncCapability,
            asyncCapability.Get(), &asyncStream)) ||
        !asyncStream)
        return false;
    if (FAILED(asyncCapability->StartOperation(nullptr)))
    {
        ComPtr<IDataObjectAsyncCapability> discarded;
        CoGetInterfaceAndReleaseStream(
            asyncStream.Detach(), IID_PPV_ARGS(&discarded));
        return false;
    }

    std::shared_ptr<OleAsyncFileOperationCompletion> state;
    try
    {
        state = std::make_shared<OleAsyncFileOperationCompletion>(
            std::move(asyncStream), effect);
        asyncCompletion = [state,
            completion = std::move(completion)](bool succeeded) mutable {
            state->Finish(succeeded);
            if (completion)
                completion(succeeded);
        };
    }
    catch (...)
    {
        if (state)
        {
            state->Finish(false);
            return false;
        }
        ComPtr<IDataObjectAsyncCapability> cancelCapability;
        CoGetInterfaceAndReleaseStream(
            asyncStream.Detach(),
            IID_PPV_ARGS(&cancelCapability));
        if (cancelCapability)
            cancelCapability->EndOperation(
                E_OUTOFMEMORY, nullptr, DROPEFFECT_NONE);
        return false;
    }
    return true;
}

void DesktopApp::OnShellFileOperationCompleted(LPARAM lParam)
{
    std::unique_ptr<ShellFileOperationUiCompletion> result(
        reinterpret_cast<ShellFileOperationUiCompletion*>(lParam));
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
    while (PeekMessageW(
        &message, completionWindow,
        kShellFileOperationCompletedMessage,
        kShellFileOperationCompletedMessage,
        PM_REMOVE))
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
        },
        [](const std::wstring& path) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
                error == ERROR_INVALID_NAME;
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

void DesktopApp::QueueFolderRead(const std::wstring& path)
{
    if (exitRequested_ || path.empty()) return;
    const auto key = snowdesktop::shell_refresh::FolderKey(path);
    if (!folderReadsPending_.insert(key).second) return;
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
        if (snapshot) ApplyFolderRefresh(*snapshot);
    }, hwnd_, kBackgroundShellReadyMessage)) folderReadsPending_.erase(key);
}

void DesktopApp::QueueDockPathChecks(const std::vector<std::wstring>& paths)
{
    for (const auto& path : paths)
    {
        const auto revision = shellRefreshRevision_.Current();
        folderReadWork_.Submit(L"dock-exists:" + ToUpperInvariant(path), [path] {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
            const auto error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME;
        }, [this, path, revision](bool missing) {
            if (!missing || !shellRefreshRevision_.IsCurrent(revision)) return;
            const auto erased = std::erase_if(dockEntries_, [&](const auto& entry) {
                return entry.type == DockEntryType::DesktopItem && entry.reference == path;
            });
            if (!erased) return;
            InvalidateDockContainers();
            UpdateFloatingDockWindowBounds(false);
            SaveLayoutSlots();
            InvalidateDockRects();
        }, hwnd_, kBackgroundShellReadyMessage);
    }
}

void DesktopApp::ApplyFolderRefresh(snowdesktop::shell_refresh::Snapshot& snapshot)
{
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
        if (!folder.complete)
            WriteDiagnosticLogEntry((L"Folder refresh failed; retaining current entries: " + path).c_str());
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping) continue;
        const auto folder = snapshot.folders.find(
            snowdesktop::shell_refresh::FolderKey(widget.sourceFolderPath));
        if (folder == snapshot.folders.end() || !folder->second.complete) continue;
        const auto oldOrder = widget.itemKeys;
        EnumerateFolderMappingEntries(widget, true, &folder->second);
        orderChanged |= oldOrder != widget.itemKeys;
        changed.insert(widget.id);
    }
    for (auto& container : containers_)
    {
        auto* widget = dynamic_cast<WidgetContainer*>(container.get());
        if (!widget || !widget->GetWidgetData()) continue;
        if (auto* mapping = dynamic_cast<FolderMapping*>(widget);
            mapping && changed.contains(widget->GetWidgetData()->id))
            mapping->InvalidateFilterCache();
        if (auto* group = dynamic_cast<FileGroup*>(widget);
            group && std::any_of(widget->GetWidgetData()->childWidgetIds.begin(),
                widget->GetWidgetData()->childWidgetIds.end(),
                [&](const auto& id) { return changed.contains(id); }))
            group->InvalidateHostedView();
    }
    std::unordered_set<std::wstring> activePaths;
    for (const auto& widget : widgets_)
        if (widget.type == DesktopWidgetType::FolderMapping)
            activePaths.insert(snowdesktop::shell_refresh::FolderKey(widget.sourceFolderPath));
    if (dockFolderPopupOpen_)
        activePaths.insert(snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
    for (auto& [key, metadata] : snapshot.metadata.folders)
        if (activePaths.contains(key))
            shellMetadataCache_.folders.insert_or_assign(key, std::move(metadata));
    if (dockFolderPopupOpen_)
    {
        const auto folder = snapshot.folders.find(
            snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
        if (folder != snapshot.folders.end())
            RefreshDockFolderPopup(&folder->second);
    }
    if (orderChanged) SaveLayoutSlots();
    RefreshOpenCollectionPopupGeometry();
    InvalidateDragStaticScene();
    reloading_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateFloatingPopupWindow(false);
    InvalidateQuickNavigationWindow();
    wchar_t timing[192]{};
    swprintf_s(timing, L"Folder refresh async: folders=%zu readMs=%llu metadataHits=%zu metadataQueries=%zu",
        snapshot.folders.size(), snapshot.readMs, snapshot.metadata.hits, snapshot.metadata.queries);
    WriteDiagnosticLogEntry(timing);
}

void DesktopApp::RefreshShellItemsAsync()
{
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
            WriteDiagnosticLogEntry(L"Shell refresh read failed; retaining current model");
            return; // Retry on a new event, not in an unbounded timer loop.
        }
        if (snapshot->foldersOnly)
        {
            ApplyFolderRefresh(*snapshot);
            return;
        }
        const ULONGLONG started = GetTickCount64();
        const size_t count = snapshot->desktopItems.size();
        const size_t metadataHits = snapshot->metadata.hits;
        const size_t metadataQueries = snapshot->metadata.queries;
        shellMetadataCache_ = std::move(snapshot->metadata);
        ReloadItems(shellReloadLayoutFromDiskPending_, snapshot.get());
        if (dockFolderPopupOpen_)
        {
            const auto folder = snapshot->folders.find(
                snowdesktop::shell_refresh::FolderKey(dockFolderPopupWidget_.sourceFolderPath));
            if (folder != snapshot->folders.end())
                RefreshDockFolderPopup(&folder->second);
            else
                QueueFolderRead(dockFolderPopupWidget_.sourceFolderPath);
        }
        InvalidateFloatingPopupWindow(false);
        InvalidateQuickNavigationWindow();
        wchar_t timing[512]{};
        swprintf_s(timing, L"Shell refresh async: items=%zu readMs=%llu applyMs=%llu "
            L"desktopMs=%llu foldersMs=%llu dockMs=%llu metadataHits=%zu metadataQueries=%zu "
            L"modelMs=%llu layoutMs=%llu saveMs=%llu rebuildMs=%llu notifyMs=%llu",
            count, snapshot->readMs, GetTickCount64() - started,
            snapshot->desktopReadMs, snapshot->folderReadMs, snapshot->dockReadMs,
            metadataHits, metadataQueries, snapshot->modelMs, snapshot->layoutMs,
            snapshot->saveMs, snapshot->rebuildMs, snapshot->notifyMs);
        WriteDiagnosticLogEntry(timing);
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
    QueueDockPathChecks(request.dockPaths);
    request.folders.clear();
    request.dockPaths.clear();
    const bool queued = shellModelWork_.Submit(L"desktop-read", [request = std::move(request), snapshot] {
        snapshot->desktopComplete = snowdesktop::shell_refresh::Read(request, *snapshot);
        return snapshot;
    }, [this, revision = *revision](auto result) {
        if (shellRefreshRevision_.Finish(revision) && result) readyShellRefresh_ = std::move(result);
        shellReloadPending_ = true;
        if (hwnd_) SetTimer(hwnd_, kShellChangeTimerId, 1, nullptr);
    }, completionWindow, kBackgroundShellReadyMessage);
    if (!queued)
    {
        shellRefreshRevision_.Finish(*revision);
        shellReloadPending_ = false;
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
    for (const auto& entry : dockEntries_)
        if (entry.type == DockEntryType::DesktopItem &&
            std::filesystem::path(entry.reference).is_absolute())
            request.dockPaths.push_back(entry.reference);

    return request;
}

void DesktopApp::StartInitialShellRead()
{
    if (exitRequested_ || !initialShellReadPending_ || initialShellRead_.Pending())
        return;
    if (shellReloadLayoutFromDiskPending_)
    {
        LoadLayoutSlots();
        RecreateItemTextFormat();
        RecreateComponentListTextFormat();
        UpdateLayoutWorkArea(false);
        if (widgetEngine_) widgetEngine_->ReloadStorage();
        shellReloadLayoutFromDiskPending_ = false;
    }
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
    QueueDockPathChecks(request.dockPaths);
    request.folders.clear();
    request.dockPaths.clear();
    if (!initialShellRead_.Start(std::move(request), completionWindow, kBackgroundShellReadyMessage))
    {
        shellRefreshRevision_.Finish(*revision);
        WriteDiagnosticLogEntry(L"Startup Shell reader could not start", DiagnosticLogLevel::Warning);
    }
}

void DesktopApp::PollInitialShellRead(std::chrono::milliseconds budget)
{
    // Commit on the UI thread only, using the same revision/interaction fences
    // as normal refresh. The worker must not overwrite edits made after timeout.
    if (exitRequested_ || !initialShellReadPending_ || reloading_ ||
        compositionPaintInProgress_ || mouseDown_ || renameEdit_ ||
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
        WriteDiagnosticLogEntry(timing);
    }
    if (!snapshot) return;
    if (!shellRefreshRevision_.Finish(initialShellReadRevision_))
    {
        const auto message = L"Startup Shell snapshot superseded: readRevision=" +
            std::to_wstring(initialShellReadRevision_) + L" currentRevision=" +
            std::to_wstring(shellRefreshRevision_.Current()) + L" items=" +
            std::to_wstring(snapshot->desktopItems.size());
        WriteDiagnosticLogEntry(message.c_str());
        StartInitialShellRead();
        return;
    }
    if (!snapshot->desktopComplete)
    {
        shellReloadPending_ = false;
        WriteDiagnosticLogEntry(
            L"Startup Shell read failed; saved layout retained, retry on next refresh",
            DiagnosticLogLevel::Warning);
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
        WriteDiagnosticLogEntry(L"Startup Shell snapshot applied");
    }
}
