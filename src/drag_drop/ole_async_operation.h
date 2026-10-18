#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shldisp.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace snowdesktop::ole_async_operation
{
using Microsoft::WRL::ComPtr;
using Completion = std::function<void(bool)>;

inline void SetPerformedDropEffectData(IDataObject* dataObject, DWORD effect)
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
        ComPtr<IStream> stream, DWORD effect, std::function<DWORD()> resolvedEffect = {})
        : stream_(std::move(stream)), effect_(effect), resolvedEffect_(std::move(resolvedEffect))
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
            const DWORD effect = succeeded && resolvedEffect_ ? resolvedEffect_() : effect_;
            // A successful target-side move is optimized: the target already
            // moved/deleted the source, so the source must not delete it again.
            if (succeeded && effect == DROPEFFECT_NONE)
            {
                ComPtr<IDataObject> dataObject;
                if (SUCCEEDED(asyncCapability.As(&dataObject)))
                    SetPerformedDropEffectData(
                        dataObject.Get(), DROPEFFECT_NONE);
            }
            asyncCapability->EndOperation(
                succeeded ? S_OK : E_ABORT,
                nullptr,
                succeeded ? effect : DROPEFFECT_NONE);
        }
    }

private:
    std::atomic_bool finished_ = false;
    ComPtr<IStream> stream_;
    DWORD effect_ = DROPEFFECT_NONE;
    std::function<DWORD()> resolvedEffect_;
};


inline bool Prepare(
    IDataObject* dataObject,
    DWORD completionEffect,
    Completion completion,
    Completion& asyncCompletion,
    std::function<DWORD()> resolvedEffect = {})
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
            std::move(asyncStream), effect, std::move(resolvedEffect));
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
}
