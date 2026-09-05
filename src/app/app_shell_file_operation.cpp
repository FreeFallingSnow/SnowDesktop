#include "app.h"

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
    std::function<bool(IDataObject*)> dataObjectPreflight)
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
        FAILED(asyncCapability->GetAsyncMode(&asyncMode)) ||
        !asyncMode)
        return false;

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

    snowdesktop::ShellDropRequest request;
    request.marshaledDataObject = std::move(dataStream);
    request.marshaledAsyncCapability = std::move(asyncStream);
    request.targetParsingName = std::move(targetParsingName);
    request.keyState = keyState;
    request.screenPoint = screenPoint;
    request.allowedEffects = effects;
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
    shellRefreshWorker_.Stop();
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
    const ULONGLONG started = GetTickCount64();
    const bool showHidden = AreExplorerHiddenItemsVisible();
    snapshot.desktopComplete = ReadDesktop(
        request.iconVisibility, showHidden, snapshot.desktopItems);
    for (const auto& path : request.folders)
    {
        const auto [entry, inserted] = snapshot.folders.try_emplace(ToUpperInvariant(path));
        if (inserted)
            entry->second = ReadFolder(path, showHidden);
    }
    for (const auto& path : request.dockPaths)
    {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INVALID_NAME)
            snapshot.missingDockPaths.insert(ToUpperInvariant(path));
    }
    snapshot.readMs = GetTickCount64() - started;
    return snapshot.desktopComplete;
}

void DesktopApp::RequestShellRefresh()
{
    if (exitRequested_)
        return;
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

void DesktopApp::RefreshShellItemsAsync()
{
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
        const ULONGLONG started = GetTickCount64();
        const size_t count = snapshot->desktopItems.size();
        ReloadItems(false, snapshot.get());
        if (dockFolderPopupOpen_)
        {
            const auto folder = snapshot->folders.find(
                ToUpperInvariant(dockFolderPopupWidget_.sourceFolderPath));
            if (folder != snapshot->folders.end())
                RefreshDockFolderPopup(&folder->second);
            else
                RequestShellRefresh();
        }
        InvalidateFloatingPopupWindow(false);
        InvalidateQuickNavigationWindow();
        wchar_t timing[256]{};
        swprintf_s(timing, L"Shell refresh async: items=%zu readMs=%llu applyMs=%llu",
            count, snapshot->readMs, GetTickCount64() - started);
        WriteDiagnosticLogEntry(timing);
        return;
    }

    const auto revision = shellRefreshRevision_.Begin();
    if (!revision)
        return;
    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
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

    auto snapshot = std::make_shared<snowdesktop::shell_refresh::Snapshot>();
    auto* completion = new (std::nothrow) ShellFileOperationUiCompletion{
        false, [this, snapshot, revision = *revision](bool succeeded) {
            if (shellRefreshRevision_.Finish(revision))
            {
                snapshot->desktopComplete &= succeeded;
                readyShellRefresh_ = snapshot;
                shellReloadPending_ = true;
            }
        }, false };
    if (!completion || !completionWindow || !IsWindow(completionWindow))
    {
        delete completion;
        shellRefreshRevision_.Finish(*revision);
        shellReloadPending_ = false;
        return;
    }
    const bool queued = shellRefreshWorker_.Enqueue(
        snowdesktop::ShellReadRequest{
            [request = std::move(request), snapshot] {
                return snowdesktop::shell_refresh::Read(request, *snapshot);
            } },
        [completionWindow, completion](bool succeeded) {
            completion->succeeded = succeeded;
            if (!PostMessageW(completionWindow, kShellFileOperationCompletedMessage,
                    0, reinterpret_cast<LPARAM>(completion)))
                delete completion;
        });
    if (!queued)
    {
        delete completion;
        shellRefreshRevision_.Finish(*revision);
        shellReloadPending_ = false;
        return;
    }
    shellReloadPending_ = true;
}
