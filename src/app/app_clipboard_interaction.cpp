#include "app.h"
#include "external_drop_content.h"

// Folder-entry clipboard, delete and paste operations.

bool DesktopApp::HasPasteableFileClipboardData() const
{
    ComPtr<IDataObject> clipboard;
    if (FAILED(OleGetClipboard(&clipboard)) || !clipboard)
        return false;
    return snowdesktop::external_drop_content::ProbeClipboardFileSource(
        clipboard.Get()) !=
        snowdesktop::external_drop_content::ClipboardFileSource::None;
}

bool DesktopApp::SuppressDesktopWidgetDragTargets() const
{
    if (!dragSession_.IsActive()) return false;
    return std::any_of(dragSession_.Items().begin(), dragSession_.Items().end(),
        [this](Item* item) {
            if (dynamic_cast<DockFrequentItem*>(item)) return true;
            const auto* dockItem = dynamic_cast<DockEntryItem*>(item);
            const size_t index = dockItem
                ? dockItem->GetEntryIndex() : static_cast<size_t>(-1);
            return index < dockEntries_.size() && dockEntries_[index].keepOnDesktop;
        });
}

std::wstring DesktopApp::GetDockDragOutRemovalHint(POINT point) const
{
    const auto* sourceDock = dynamic_cast<DockContainer*>(dragSession_.Source());
    if (!sourceDock) return L"";
    // A replicated Dock on another monitor is still a valid Dock target, not
    // a drag-out removal area.
    if (GetDockContainerAtPoint(point)) return L"";
    RECT sourceBounds = sourceDock->GetBounds();
    if (PtInRect(&sourceBounds, point)) return L"";

    for (Item* item : dragSession_.Items())
    {
        if (dynamic_cast<DockFrequentItem*>(item))
            return _LW("core.drag.remove_frequent");
        const auto* dockItem = dynamic_cast<DockEntryItem*>(item);
        const size_t index = dockItem
            ? dockItem->GetEntryIndex() : static_cast<size_t>(-1);
        if (index < dockEntries_.size() && dockEntries_[index].keepOnDesktop)
            return _LW("core.drag.remove_dock_map");
    }
    return L"";
}

/**
 * @brief 获取所有选中的文件夹条目路径
 * @param firstWidgetIndex [out] 第一个包含选中条目的部件索引
 * @return 选中的文件路径列表
 */
std::vector<std::wstring> DesktopApp::GetSelectedFolderEntryPaths(size_t* firstWidgetIndex) const
{
    if (firstWidgetIndex)
        *firstWidgetIndex = static_cast<size_t>(-1);

    if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_)
    {
        std::vector<std::wstring> paths;
        for (const auto& entry :
             dockFolderPopupWidget_.
                folderEntries)
        {
            if (entry.selected &&
                !entry.fullPath.empty())
                paths.push_back(
                    entry.fullPath);
        }
        return paths;
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;

        std::vector<std::wstring> paths;
        for (const auto& entry : widget.folderEntries)
            if (entry.selected && !entry.fullPath.empty())
                paths.push_back(entry.fullPath);

        if (!paths.empty())
        {
            if (firstWidgetIndex)
                *firstWidgetIndex = i;
            return paths;
        }
    }

    return {};
}

// New commands target the visible file source, including a FileGroup tab or
// the Desktop Files popup in Dock, before considering other selected sources.
size_t DesktopApp::FindNewItemShortcutTarget() const
{
    constexpr size_t missing = static_cast<size_t>(-1);
    auto resolve = [&](const DesktopWidget& widget) -> size_t {
        size_t index = FindWidgetIndexById(widget.type == DesktopWidgetType::FileGroup
            ? widget.activeCategoryId : widget.id);
        if (index >= widgets_.size()) return missing;
        const auto& source = widgets_[index];
        return source.type == DesktopWidgetType::FileCategories ||
            (source.type == DesktopWidgetType::FolderMapping && !source.sourceFolderPath.empty())
            ? index : missing;
    };
    if (const auto* popup = GetOpenPopupWidget())
        if (const auto index = resolve(*popup); index < widgets_.size()) return index;
    if (keyboardNavInsideWidget_ && keyboardNavWidgetIndex_ < widgets_.size())
        if (const auto index = resolve(widgets_[keyboardNavWidgetIndex_]); index < widgets_.size()) return index;
    for (size_t i = widgets_.size(); i-- > 0;)
        if (!IsGroupedWidget(widgets_[i]) && widgets_[i].gridCell.pageId != kDockPageId &&
            PtInRect(&widgets_[i].bounds, lastMousePoint_))
            return resolve(widgets_[i]);
    for (const auto& widget : widgets_)
        if (widget.selected)
            if (const auto index = resolve(widget); index < widgets_.size()) return index;
    return FindFolderMappingShortcutTarget();
}

/**
 * @brief 查找文件夹映射的快捷操作目标部件
 * @return 部件索引，未找到返回 (size_t)-1
 */
size_t DesktopApp::FindFolderMappingShortcutTarget() const
{
    size_t selectedEntryWidget = static_cast<size_t>(-1);
    (void)GetSelectedFolderEntryPaths(&selectedEntryWidget);
    if (selectedEntryWidget < widgets_.size())
        return selectedEntryWidget;

    auto activeFileGroupMapping =
        [&](const DesktopWidget& group)
            -> size_t {
        if (group.type !=
            DesktopWidgetType::FileGroup)
            return static_cast<size_t>(-1);
        const size_t childIndex =
            FindWidgetIndexById(
                group.activeCategoryId);
        return childIndex < widgets_.size() &&
            widgets_[childIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[childIndex].
                sourceFolderPath.empty()
            ? childIndex
            : static_cast<size_t>(-1);
    };
    if (keyboardNavInsideWidget_ &&
        keyboardNavWidgetIndex_ < widgets_.size())
    {
        const size_t childIndex =
            activeFileGroupMapping(
                widgets_[keyboardNavWidgetIndex_]);
        if (childIndex < widgets_.size())
            return childIndex;
    }
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type ==
                DesktopWidgetType::FileGroup &&
            PtInRect(&widget.bounds,
                lastMousePoint_))
        {
            const size_t childIndex =
                activeFileGroupMapping(widget);
            if (childIndex < widgets_.size())
                return childIndex;
        }
        if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
            continue;
        if (PtInRect(&widget.bounds, lastMousePoint_))
            return i;
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type ==
                DesktopWidgetType::FileGroup &&
            widget.selected)
        {
            const size_t childIndex =
                activeFileGroupMapping(widget);
            if (childIndex < widgets_.size())
                return childIndex;
        }
        if (widget.type == DesktopWidgetType::FolderMapping &&
            widget.selected && !widget.sourceFolderPath.empty())
            return i;
    }

    return static_cast<size_t>(-1);
}

/**
 * @brief 复制或剪切选中的文件夹条目到剪贴板
 * @param cut true 为剪切，false 为复制
 * @return 是否成功
 */
bool DesktopApp::CopyCutSelectedFolderEntries(bool cut)
{
    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths();
    if (paths.empty()) return false;

    ComPtr<IDataObject> dataObj = CreateFileDropDataObject(paths);
    if (!dataObj) return false;

    cutPaths_.clear();
    if (cut)
    {
        CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fmt{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        med.tymed = TYMED_HGLOBAL;
        med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (med.hGlobal)
        {
            *static_cast<DWORD*>(GlobalLock(med.hGlobal)) = DROPEFFECT_MOVE;
            GlobalUnlock(med.hGlobal);
            dataObj->SetData(&fmt, &med, TRUE);
        }

        for (const auto& path : paths)
            cutPaths_.insert(path);
    }

    OleSetClipboard(dataObj.Get());
    OleFlushClipboard();
    UpdateCutState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateFloatingDockWindow();
    return true;
}

/**
 * @brief 删除选中的文件夹条目
 * @param permanentDelete true 时永久删除并显示 Shell 确认对话框
 * @return 是否执行了删除操作
 */
bool DesktopApp::DeleteSelectedFolderEntries(bool permanentDelete)
{
    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths();
    if (paths.empty()) return false;

    for (const auto& path : paths)
        cutPaths_.erase(path);

    std::vector<snowdesktop::ShellFileOperationStep> steps;
    steps.push_back({
        FO_DELETE,
        std::move(paths),
        {},
        static_cast<FILEOP_FLAGS>(permanentDelete
            ? FOF_WANTNUKEWARNING
            : (FOF_ALLOWUNDO |
               FOF_NOCONFIRMATION |
               FOF_NOERRORUI)) });
    (void)QueueShellFileOperation(
        std::move(steps),
        [this](bool succeeded) {
            if (!succeeded)
                return;
            RequestShellRefresh();

        });
    return true;
}

/**
 * @brief 将剪贴板内容粘贴到指定文件夹映射部件中
 * @param widgetIndex 目标部件索引
 * @return 是否成功粘贴
 */
bool DesktopApp::PasteClipboardToFolderMapping(size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return false;
    DesktopWidget& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
        return false;
    return PasteClipboardToFolderPath(
        widget.sourceFolderPath);
}

bool DesktopApp::PasteClipboardToFolderPath(
    const std::wstring& targetFolderPath)
{
    if (targetFolderPath.empty())
        return false;
    const DWORD attributes =
        GetFileAttributesW(
            targetFolderPath.c_str());
    if (attributes ==
            INVALID_FILE_ATTRIBUTES ||
        (attributes &
            FILE_ATTRIBUTE_DIRECTORY) == 0)
        return false;

    ComPtr<IDataObject> clipObj;
    if (FAILED(OleGetClipboard(&clipObj)) || !clipObj)
        return false;
    const DWORD clipboardSequence = GetClipboardSequenceNumber();
    const auto source = snowdesktop::external_drop_content::
        ProbeClipboardFileSource(clipObj.Get());
    if (source == snowdesktop::external_drop_content::ClipboardFileSource::None)
        return false;

    DropAction action = DropAction::Copy;
    CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medPref{};
    if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)))
    {
        if (medPref.tymed == TYMED_HGLOBAL && medPref.hGlobal &&
            GlobalSize(medPref.hGlobal) >= sizeof(DWORD))
        {
            const auto* effect = static_cast<const DWORD*>(GlobalLock(medPref.hGlobal));
            if (effect)
            {
                action = DropActionFromClipboardEffect(*effect);
                GlobalUnlock(medPref.hGlobal);
            }
        }
        ReleaseStgMedium(&medPref);
    }

    const bool asynchronous = snowdesktop::virtual_file_drop::UsesAsyncMode(clipObj.Get());
    const ULONGLONG pasteStarted = GetTickCount64();
    wchar_t pasteMessage[224]{};
    swprintf_s(pasteMessage, L"Clipboard paste begin: sequence=%lu source=%d async=%d action=%d",
        clipboardSequence, static_cast<int>(source), asynchronous, static_cast<int>(action));
    WriteDiagnosticLogEntry(pasteMessage);
    auto operationCompletion = [this, action, clipboardSequence, pasteStarted](bool succeeded) {
        wchar_t completionMessage[224]{};
        swprintf_s(completionMessage, L"Clipboard paste completed: sequence=%lu success=%d elapsed_ms=%llu",
            clipboardSequence, succeeded, GetTickCount64() - pasteStarted);
        WriteDiagnosticLogEntry(completionMessage);
        if (!succeeded)
            return;
        if (action == DropAction::Move &&
            GetClipboardSequenceNumber() == clipboardSequence &&
            OpenClipboard(hwnd_))
        {
            // A later copy must survive completion of an earlier paste.
            if (GetClipboardSequenceNumber() == clipboardSequence)
            {
                EmptyClipboard();
                cutPaths_.clear();
            }
            CloseClipboard();
        }
        RequestShellRefresh();
    };

    if (source == snowdesktop::external_drop_content::ClipboardFileSource::ShellObjects ||
        asynchronous)
    {
        // Keep namespace IDs and FileContents intact; a phone item cannot be
        // reduced to a local path. Shell also owns virtual folder recursion.
        const bool move = action == DropAction::Move;
        const bool link = action == DropAction::Link;
        const DWORD effect = move ? DROPEFFECT_MOVE :
            link ? DROPEFFECT_LINK : DROPEFFECT_COPY;
        return QueueAsyncShellDrop(clipObj.Get(), targetFolderPath,
            snowdesktop::ClipboardShellDropKeyState(effect), {}, effect,
            std::move(operationCompletion), {}, true);
    }
    const auto paths = snowdesktop::external_drop_content::ReadFilePaths(clipObj.Get());
    if (paths.empty()) return false;

    DragSourceList sourceList;
    sourceList.hasExternalFiles = true;
    sourceList.entries.reserve(paths.size());
    for (const auto& path : paths)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = sourceList.entries.size();
        entry.filePath = path;
        entry.displayName = FileNameFromPath(path);
        sourceList.entries.push_back(std::move(entry));
    }

    return MaterializeFilesToFolder(
        sourceList, targetFolderPath,
        action, std::move(operationCompletion));
}

bool DesktopApp::PasteClipboardToDesktop()
{
    bool fromDesktop = false;
    std::unordered_set<std::wstring> clipPaths;

    ComPtr<IDataObject> clipObj;
    if (SUCCEEDED(OleGetClipboard(&clipObj)) && clipObj &&
        snowdesktop::external_drop_content::ProbeClipboardFileSource(clipObj.Get()) ==
            snowdesktop::external_drop_content::ClipboardFileSource::FilePaths &&
        !snowdesktop::virtual_file_drop::UsesAsyncMode(clipObj.Get()))
    {
        CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fmtPref{ cfPreferred, nullptr,
            DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medPref{};
        if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) &&
            medPref.hGlobal)
        {
            DWORD* pEffect = static_cast<DWORD*>(
                GlobalLock(medPref.hGlobal));
            const bool isMove =
                pEffect && (*pEffect & DROPEFFECT_MOVE);
            if (pEffect) GlobalUnlock(medPref.hGlobal);
            if (isMove)
            {
                FORMATETC fmtDrop{ CF_HDROP, nullptr,
                    DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                STGMEDIUM medDrop{};
                if (SUCCEEDED(clipObj->GetData(
                        &fmtDrop, &medDrop)) &&
                    medDrop.hGlobal)
                {
                    HDROP hDrop = static_cast<HDROP>(
                        medDrop.hGlobal);
                    const UINT count = DragQueryFileW(
                        hDrop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < count; ++i)
                    {
                        wchar_t path[MAX_PATH]{};
                        if (DragQueryFileW(
                                hDrop, i, path, MAX_PATH) > 0)
                            clipPaths.insert(path);
                    }
                    ReleaseStgMedium(&medDrop);
                }
            }
            ReleaseStgMedium(&medPref);
        }
    }

    if (!clipPaths.empty())
    {
        for (const auto& item : items_)
        {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(
                    item.absolutePidl.get(), path) &&
                clipPaths.contains(path))
            {
                fromDesktop = true;
                break;
            }
        }
    }

    if (fromDesktop)
    {
        cutPaths_.clear();
        if (OpenClipboard(hwnd_))
        {
            EmptyClipboard();
            CloseClipboard();
        }
        UpdateCutState();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    wchar_t desktopPath[MAX_PATH]{};
    if (snowdesktop::desktop_source::CopyDirectory(desktopPath) &&
        PasteClipboardToFolderPath(desktopPath))
    {
        return true;
    }

    ComPtr<IContextMenu> bgMenu;
    if (SUCCEEDED(desktopFolder_->CreateViewObject(
            hwnd_, IID_IContextMenu,
            reinterpret_cast<void**>(
                bgMenu.GetAddressOf()))) &&
        bgMenu)
    {
        CMINVOKECOMMANDINFO info{};
        info.cbSize = sizeof(info);
        info.hwnd = ShellDialogOwnerHwnd();
        info.lpVerb = "paste";
        info.nShow = SW_SHOWNORMAL;
        SafeInvokeCommand(bgMenu.Get(), &info);
        RequestShellRefresh();
    }
    return true;
}

/**
 * @brief 处理键盘按键按下事件
 * @param key 虚拟键码
 */
