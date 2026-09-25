#pragma once

#include "external_drop_content.h"
#include "folder_self_drop_rules.h"
#include "shell_file_operation_worker.h"

#include <optional>
#include <utility>

namespace snowdesktop
{
// Called on the Shell worker after the clipboard object's async operation has
// started. Local paths use a tracked file operation whose return means the
// transfer finished, not an IDropTarget handoff that can outlive Drop().
// nullopt permits a namespace/link handoff; false is a terminal path failure
// and must never retry a partially completed batch through another mechanism.
template<class Execute>
std::optional<bool> TryExecuteClipboardFileOperation(
    IDataObject* dataObject, const std::wstring& folder, DWORD effect,
    Execute&& execute)
{
    if (!dataObject || (effect != DROPEFFECT_COPY && effect != DROPEFFECT_MOVE))
        return std::nullopt;
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (dataObject->QueryGetData(&format) != S_OK)
        return std::nullopt;

    auto paths = external_drop_content::ReadFilePaths(dataObject);
    if (paths.empty() || folder.empty() ||
        folder_self_drop_rules::IsSelfContainedFolderDrop(paths, folder))
        return false;

    std::wstring destination = folder;
    if (destination.back() != L'\\' && destination.back() != L'/')
        destination += L'\\';
    ShellFileOperationRequest request;
    request.result = std::make_shared<ShellFileOperationResult>();
    request.steps.push_back({
        static_cast<UINT>(effect == DROPEFFECT_MOVE ? FO_MOVE : FO_COPY),
        std::move(paths), std::move(destination),
        static_cast<FILEOP_FLAGS>(FOF_NOCONFIRMATION | FOF_NOERRORUI |
            FOF_RENAMEONCOLLISION)});
    return std::forward<Execute>(execute)(request);
}

// A native move already removed the source. Match the optimized-move protocol
// used by the host's other async file operations so the source cannot delete
// it a second time when EndOperation is delivered.
inline void ReportClipboardFileOperationEffect(IDataObject* dataObject, DWORD effect)
{
    if (!dataObject) return;
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!storage) return;
    auto* value = static_cast<DWORD*>(GlobalLock(storage));
    if (!value) { GlobalFree(storage); return; }
    *value = effect;
    GlobalUnlock(storage);
    FORMATETC format{static_cast<CLIPFORMAT>(RegisterClipboardFormatW(
        L"Performed DropEffect")), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = storage;
    if (FAILED(dataObject->SetData(&format, &medium, TRUE)))
        ReleaseStgMedium(&medium);
}
}
