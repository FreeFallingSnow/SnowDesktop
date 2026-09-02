#include "shell_file_operation_worker.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shldisp.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace snowdesktop
{
namespace
{

using Microsoft::WRL::ComPtr;

class FileDropDataObject final : public IDataObject
{
public:
    explicit FileDropDataObject(std::vector<std::wstring> paths)
        : paths_(std::move(paths)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject)
        {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(
            InterlockedIncrement(&referenceCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG remaining = InterlockedDecrement(&referenceCount_);
        if (remaining == 0)
            delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* format, STGMEDIUM* medium) override
    {
        if (!format || !medium)
            return E_POINTER;
        const HRESULT supported = QueryGetData(format);
        if (FAILED(supported))
            return supported;

        size_t characterCount = 1;
        for (const auto& path : paths_)
            characterCount += path.size() + 1;
        const SIZE_T byteCount = sizeof(DROPFILES) +
            characterCount * sizeof(wchar_t);
        HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
            byteCount);
        if (!storage)
            return STG_E_MEDIUMFULL;

        auto* drop = static_cast<DROPFILES*>(GlobalLock(storage));
        if (!drop)
        {
            GlobalFree(storage);
            return STG_E_MEDIUMFULL;
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        auto* destination = reinterpret_cast<wchar_t*>(
            reinterpret_cast<BYTE*>(drop) + drop->pFiles);
        for (const auto& path : paths_)
        {
            std::copy(path.begin(), path.end(), destination);
            destination += path.size();
            *destination++ = L'\0';
        }
        *destination = L'\0';
        GlobalUnlock(storage);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = storage;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC*, STGMEDIUM*) override
    {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        if (!format)
            return E_POINTER;
        if (format->cfFormat != CF_HDROP ||
            format->dwAspect != DVASPECT_CONTENT ||
            format->lindex != -1 ||
            (format->tymed & TYMED_HGLOBAL) == 0)
            return DV_E_FORMATETC;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC*, FORMATETC* output) override
    {
        if (!output)
            return E_POINTER;
        output->ptd = nullptr;
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC*, STGMEDIUM*, BOOL) override
    {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD direction, IEnumFORMATETC** enumerator) override
    {
        if (!enumerator)
            return E_POINTER;
        *enumerator = nullptr;
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        FORMATETC format{
            CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
            TYMED_HGLOBAL };
        return SHCreateStdEnumFmtEtc(1, &format, enumerator);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
        FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA**) override
    {
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~FileDropDataObject() = default;

    LONG referenceCount_ = 1;
    std::vector<std::wstring> paths_;
};

// The outer SnowDesktop drop target owns the asynchronous OLE operation.  A
// Shell target invoked on the worker must therefore see an ordinary
// IDataObject; otherwise it can start a second asynchronous operation against
// the same source and race our EndOperation call.
class SynchronousDataObjectView final : public IDataObject
{
public:
    explicit SynchronousDataObjectView(ComPtr<IDataObject> inner)
        : inner_(std::move(inner)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid, void** object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject)
        {
            *object = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IDataObjectAsyncCapability)
            return E_NOINTERFACE;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(
            InterlockedIncrement(&referenceCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const LONG remaining = InterlockedDecrement(&referenceCount_);
        if (remaining == 0)
            delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE GetData(
        FORMATETC* format, STGMEDIUM* medium) override
    {
        return inner_ ? inner_->GetData(format, medium) : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(
        FORMATETC* format, STGMEDIUM* medium) override
    {
        return inner_ ? inner_->GetDataHere(format, medium) : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        return inner_ ? inner_->QueryGetData(format) : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(
        FORMATETC* input, FORMATETC* output) override
    {
        return inner_ ? inner_->GetCanonicalFormatEtc(input, output)
                      : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE SetData(
        FORMATETC* format, STGMEDIUM* medium, BOOL release) override
    {
        const CLIPFORMAT performedFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));
        if (format && medium && performedFormat &&
            format->cfFormat == performedFormat &&
            medium->tymed == TYMED_HGLOBAL && medium->hGlobal &&
            GlobalSize(medium->hGlobal) >= sizeof(DWORD))
        {
            const auto* value = static_cast<const DWORD*>(
                GlobalLock(medium->hGlobal));
            if (value)
            {
                performedEffect_ = *value &
                    (DROPEFFECT_COPY | DROPEFFECT_MOVE |
                     DROPEFFECT_LINK);
                hasPerformedEffect_ = true;
                GlobalUnlock(medium->hGlobal);
            }
        }
        return inner_ ? inner_->SetData(format, medium, release)
                      : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(
        DWORD direction, IEnumFORMATETC** enumerator) override
    {
        return inner_ ? inner_->EnumFormatEtc(direction, enumerator)
                      : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE DAdvise(
        FORMATETC* format, DWORD flags, IAdviseSink* sink,
        DWORD* connection) override
    {
        return inner_ ? inner_->DAdvise(
            format, flags, sink, connection) : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD connection) override
    {
        return inner_ ? inner_->DUnadvise(connection) : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE EnumDAdvise(
        IEnumSTATDATA** enumerator) override
    {
        return inner_ ? inner_->EnumDAdvise(enumerator) : E_UNEXPECTED;
    }

    bool TryGetPerformedEffect(DWORD& effect) const
    {
        if (!hasPerformedEffect_)
            return false;
        effect = performedEffect_;
        return true;
    }

private:
    ~SynchronousDataObjectView() = default;

    LONG referenceCount_ = 1;
    ComPtr<IDataObject> inner_;
    DWORD performedEffect_ = DROPEFFECT_NONE;
    bool hasPerformedEffect_ = false;
};

ComPtr<IDataObject> CreateHDropDataObject(
    const std::vector<std::wstring>& paths)
{
    ComPtr<IDataObject> dataObject;
    dataObject.Attach(new (std::nothrow) FileDropDataObject(paths));
    return dataObject;
}

std::wstring BuildPathList(const std::vector<std::wstring>& paths)
{
    std::wstring result;
    for (const auto& path : paths)
    {
        if (path.empty())
            continue;
        result.append(path);
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

std::wstring BuildDestination(const std::wstring& destination)
{
    if (destination.empty())
        return {};
    std::wstring result = destination;
    result.push_back(L'\0');
    result.push_back(L'\0');
    return result;
}

ComPtr<IDataObject> CreateFileDataObject(
    const std::vector<std::wstring>& paths)
{
    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& path : paths)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (path.empty() || FAILED(SHParseDisplayName(
                path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        {
            if (pidl)
                CoTaskMemFree(pidl);
            for (PIDLIST_ABSOLUTE parsed : pidls)
                CoTaskMemFree(parsed);
            return nullptr;
        }
        pidls.push_back(pidl);
    }

    const auto releasePidls = [&pidls]() {
        for (PIDLIST_ABSOLUTE pidl : pidls)
            CoTaskMemFree(pidl);
    };
    if (pidls.empty())
        return nullptr;

    PIDLIST_ABSOLUTE parent = ILCloneFull(pidls.front());
    if (!parent || !ILRemoveLastID(parent))
    {
        if (parent)
            CoTaskMemFree(parent);
        releasePidls();
        return nullptr;
    }
    std::vector<PCUITEMID_CHILD> childItems;
    childItems.reserve(pidls.size());
    for (PIDLIST_ABSOLUTE pidl : pidls)
    {
        PIDLIST_ABSOLUTE candidateParent = ILCloneFull(pidl);
        if (!candidateParent || !ILRemoveLastID(candidateParent) ||
            !ILIsEqual(parent, candidateParent))
        {
            if (candidateParent)
                CoTaskMemFree(candidateParent);
            CoTaskMemFree(parent);
            releasePidls();
            return CreateHDropDataObject(paths);
        }
        CoTaskMemFree(candidateParent);
        childItems.push_back(ILFindLastID(pidl));
    }
    ComPtr<IDataObject> dataObject;
    const HRESULT dataResult = SHCreateDataObject(
        parent,
        static_cast<UINT>(childItems.size()),
        childItems.data(),
        nullptr,
        IID_PPV_ARGS(&dataObject));
    CoTaskMemFree(parent);
    releasePidls();
    if (FAILED(dataResult))
        return CreateHDropDataObject(paths);
    return dataObject;
}

template<typename Interface>
ComPtr<Interface> ConsumeMarshaledInterface(ComPtr<IStream>& stream)
{
    ComPtr<Interface> result;
    if (!stream)
        return result;
    IStream* rawStream = stream.Detach();
    if (FAILED(CoGetInterfaceAndReleaseStream(
            rawStream, IID_PPV_ARGS(&result))))
        result.Reset();
    return result;
}

void CancelShellDropRequest(ShellDropRequest request)
{
    ComPtr<IDataObjectAsyncCapability> asyncCapability =
        ConsumeMarshaledInterface<IDataObjectAsyncCapability>(
            request.marshaledAsyncCapability);
    // Consume the data-object marshal packet even though the cancelled task
    // will not use it. Releasing IStream alone does not release marshal data.
    (void)ConsumeMarshaledInterface<IDataObject>(
        request.marshaledDataObject);
    if (asyncCapability)
        asyncCapability->EndOperation(E_ABORT, nullptr, DROPEFFECT_NONE);
}

bool TryGetDropEffectData(IDataObject* dataObject, DWORD& effect)
{
    effect = DROPEFFECT_NONE;
    if (!dataObject)
        return false;
    const CLIPFORMAT format = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT));
    if (!format)
        return false;

    FORMATETC formatEtc{
        format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(&formatEtc, &medium)) ||
        medium.tymed != TYMED_HGLOBAL || !medium.hGlobal)
        return false;

    const auto* value = static_cast<const DWORD*>(
        GlobalLock(medium.hGlobal));
    if (!value || GlobalSize(medium.hGlobal) < sizeof(DWORD))
    {
        if (value)
            GlobalUnlock(medium.hGlobal);
        ReleaseStgMedium(&medium);
        return false;
    }
    effect = *value &
        (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    return true;
}

} // namespace

ShellFileOperationRequest CreateRecycleBinDeleteRequest(
    std::vector<std::wstring> sources)
{
    ShellFileOperationRequest request;
    if (sources.empty())
        return request;

    request.steps.push_back({
        FO_DELETE,
        std::move(sources),
        {},
        static_cast<FILEOP_FLAGS>(
            FOF_ALLOWUNDO |
            FOF_NOCONFIRMATION |
            FOF_WANTNUKEWARNING) });
    return request;
}

ShellFileOperationWorker::~ShellFileOperationWorker()
{
    Stop();
}

bool ShellFileOperationWorker::Enqueue(
    ShellFileOperationRequest request, Completion completion)
{
    if (request.steps.empty() && request.exactFileCopies.empty() &&
        request.shortcuts.empty())
        return false;

    Task pending{ std::move(request), std::move(completion) };
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return false;
        try
        {
            tasks_.push_back(std::move(pending));
        }
        catch (...)
        {
            return false;
        }
        if (!started_)
        {
            try
            {
                thread_ = std::thread(&ShellFileOperationWorker::Run, this);
                started_ = true;
            }
            catch (...)
            {
                tasks_.pop_back();
                return false;
            }
        }
    }
    cv_.notify_one();
    return true;
}

bool ShellFileOperationWorker::Enqueue(
    ShellDropRequest request, Completion completion)
{
    if ((request.sources.empty() && !request.marshaledDataObject) ||
        request.targetParsingName.empty())
    {
        CancelShellDropRequest(std::move(request));
        return false;
    }

    Task pending{ std::move(request), std::move(completion) };
    Task cancelledTask;
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
        {
            cancelledTask = std::move(pending);
            cancelled = true;
        }
        else
        {
            try
            {
                tasks_.push_back(std::move(pending));
            }
            catch (...)
            {
                cancelledTask = std::move(pending);
                cancelled = true;
            }
            if (!cancelled && !started_)
            {
                try
                {
                    thread_ = std::thread(
                        &ShellFileOperationWorker::Run, this);
                    started_ = true;
                }
                catch (...)
                {
                    cancelledTask = std::move(tasks_.back());
                    tasks_.pop_back();
                    cancelled = true;
                }
            }
        }
    }
    if (cancelled)
    {
        if (auto* drop = std::get_if<ShellDropRequest>(
                &cancelledTask.request))
            CancelShellDropRequest(std::move(*drop));
        return false;
    }
    cv_.notify_one();
    return true;
}

void ShellFileOperationWorker::Stop()
{
    std::deque<Task> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        cancelled.swap(tasks_);
    }

    for (auto& task : cancelled)
    {
        if (auto* drop = std::get_if<ShellDropRequest>(&task.request))
            CancelShellDropRequest(std::move(*drop));
        if (task.completion)
            task.completion(false);
    }

    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool ShellFileOperationWorker::Execute(
    const ShellFileOperationRequest& request)
{
    bool attempted = false;
    bool allSucceeded = true;
    for (const auto& step : request.steps)
    {
        if (step.function == 0 || step.sources.empty())
            continue;
        attempted = true;

        const std::wstring sources = BuildPathList(step.sources);
        const std::wstring destination =
            BuildDestination(step.destination);
        SHFILEOPSTRUCTW operation{};
        operation.hwnd = nullptr;
        operation.wFunc = step.function;
        operation.pFrom = sources.c_str();
        operation.pTo = destination.empty()
            ? nullptr : destination.c_str();
        operation.fFlags = step.flags;

        const int result = SHFileOperationW(&operation);
        if (result != 0 || operation.fAnyOperationsAborted)
            allSucceeded = false;
    }
    for (const auto& copy : request.exactFileCopies)
    {
        if (copy.source.empty() || copy.destination.empty())
            continue;
        attempted = true;
        // The destination was selected before the request was queued. Never
        // let Shell collision renaming make the completion path inaccurate.
        if (!CopyFileW(
                copy.source.c_str(), copy.destination.c_str(), TRUE))
            allSucceeded = false;
    }
    for (const auto& shortcut : request.shortcuts)
    {
        if (shortcut.source.empty() || shortcut.destination.empty())
            continue;
        attempted = true;

        ComPtr<IShellLinkW> shellLink;
        if (FAILED(CoCreateInstance(
                CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&shellLink))) || !shellLink)
        {
            allSucceeded = false;
            continue;
        }
        if (FAILED(shellLink->SetPath(shortcut.source.c_str())))
        {
            allSucceeded = false;
            continue;
        }
        if (!shortcut.workingDirectory.empty() &&
            FAILED(shellLink->SetWorkingDirectory(
                shortcut.workingDirectory.c_str())))
        {
            allSucceeded = false;
            continue;
        }

        ComPtr<IPersistFile> persistFile;
        if (FAILED(shellLink.As(&persistFile)) || !persistFile)
        {
            allSucceeded = false;
            continue;
        }

        bool destinationClaimed = false;
        if (shortcut.failIfDestinationExists)
        {
            HANDLE destination = CreateFileW(
                shortcut.destination.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (destination == INVALID_HANDLE_VALUE)
            {
                allSucceeded = false;
                continue;
            }
            CloseHandle(destination);
            destinationClaimed = true;
        }

        if (FAILED(persistFile->Save(
                shortcut.destination.c_str(), TRUE)))
        {
            if (destinationClaimed)
                DeleteFileW(shortcut.destination.c_str());
            allSucceeded = false;
        }
    }
    return attempted && allSucceeded;
}

bool ShellFileOperationWorker::Execute(
    ShellDropRequest request)
{
    ComPtr<IDataObjectAsyncCapability> asyncCapability =
        ConsumeMarshaledInterface<IDataObjectAsyncCapability>(
            request.marshaledAsyncCapability);
    ComPtr<IDataObject> dataObject =
        ConsumeMarshaledInterface<IDataObject>(
            request.marshaledDataObject);
    const auto finish = [&asyncCapability](
            HRESULT result, DWORD effect) {
        const bool succeeded = SUCCEEDED(result);
        if (asyncCapability)
        {
            asyncCapability->EndOperation(
                succeeded ? S_OK : result,
                nullptr,
                succeeded ? effect : DROPEFFECT_NONE);
        }
        return succeeded;
    };

    if ((request.sources.empty() && !dataObject) ||
        request.targetParsingName.empty())
        return finish(E_INVALIDARG, DROPEFFECT_NONE);

    const DWORD allowedEffects = request.allowedEffects &
        (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (allowedEffects == DROPEFFECT_NONE)
        return finish(E_INVALIDARG, DROPEFFECT_NONE);

    if (!dataObject)
        dataObject = CreateFileDataObject(request.sources);
    if (!dataObject)
        return finish(E_FAIL, DROPEFFECT_NONE);

    if ((allowedEffects & DROPEFFECT_COPY) != 0 &&
        request.dataObjectPreflight)
    {
        try
        {
            if (request.dataObjectPreflight(dataObject.Get()))
                return finish(S_OK, DROPEFFECT_COPY);
        }
        catch (...)
        {
            // A failed optional preflight must not suppress the normal Shell
            // handoff. Its implementation owns cleanup of partial output.
        }
    }

    ComPtr<IDataObject> dropDataObject = dataObject;
    SynchronousDataObjectView* synchronousView = nullptr;
    if (asyncCapability)
    {
        synchronousView = new (std::nothrow)
            SynchronousDataObjectView(dataObject);
        dropDataObject.Attach(synchronousView);
        if (!dropDataObject)
            return finish(E_OUTOFMEMORY, DROPEFFECT_NONE);
    }

    ComPtr<IShellItem> targetItem;
    HRESULT result = SHCreateItemFromParsingName(
        request.targetParsingName.c_str(), nullptr,
        IID_PPV_ARGS(&targetItem));
    if (FAILED(result) || !targetItem)
        return finish(result, DROPEFFECT_NONE);

    ComPtr<IDropTarget> dropTarget;
    result = targetItem->BindToHandler(
        nullptr, BHID_SFUIObject,
        IID_PPV_ARGS(&dropTarget));
    if (FAILED(result) || !dropTarget)
        return finish(result, DROPEFFECT_NONE);

    DWORD effect = allowedEffects;
    result = dropTarget->DragEnter(
        dropDataObject.Get(), request.keyState,
        request.screenPoint, &effect);
    effect &= allowedEffects;
    if (FAILED(result) || effect == DROPEFFECT_NONE)
    {
        dropTarget->DragLeave();
        return finish(
            FAILED(result) ? result : E_FAIL,
            DROPEFFECT_NONE);
    }

    effect = allowedEffects;
    result = dropTarget->DragOver(
        request.keyState, request.screenPoint, &effect);
    effect &= allowedEffects;
    if (FAILED(result) || effect == DROPEFFECT_NONE)
    {
        dropTarget->DragLeave();
        return finish(
            FAILED(result) ? result : E_FAIL,
            DROPEFFECT_NONE);
    }

    effect = allowedEffects;
    result = dropTarget->Drop(
        dropDataObject.Get(), request.keyState,
        request.screenPoint, &effect);
    effect &= allowedEffects;
    DWORD performedEffect = DROPEFFECT_NONE;
    const bool hasPerformedEffect = SUCCEEDED(result) &&
        ((synchronousView && synchronousView->TryGetPerformedEffect(
            performedEffect)) ||
         TryGetDropEffectData(dataObject.Get(), performedEffect));
    if (hasPerformedEffect)
        return finish(result, performedEffect);
    if (SUCCEEDED(result) && effect == DROPEFFECT_NONE && asyncCapability)
        return finish(E_FAIL, DROPEFFECT_NONE);
    return finish(result, effect);
}

void ShellFileOperationWorker::Run()
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    for (;;)
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });
            if (stopping_ && tasks_.empty())
                break;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        const bool succeeded = std::visit(
            [](auto& request) {
                using Request = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Request, ShellDropRequest>)
                    return ShellFileOperationWorker::Execute(
                        std::move(request));
                else
                    return ShellFileOperationWorker::Execute(request);
            },
            task.request);
        if (task.completion)
            task.completion(succeeded);
    }

    if (SUCCEEDED(comResult))
        CoUninitialize();
}

} // namespace snowdesktop
