#include "app.h"

// Rename commit operations.

namespace
{
struct RenameCommitTiming
{
    RenameTargetKind target;
    ULONGLONG started = GetTickCount64();
    ULONGLONG shellMs = 0;
    ULONGLONG refreshMs = 0;

    ~RenameCommitTiming()
    {
        const ULONGLONG totalMs = GetTickCount64() - started;
        if (totalMs < 50)
            return;
        wchar_t message[256]{};
        swprintf_s(message,
            L"Rename commit slow: target=%d totalMs=%llu shellMs=%llu refreshMs=%llu",
            static_cast<int>(target), totalMs, shellMs, refreshMs);
        WriteDiagnosticLogEntry(message);
    }
};
}

static std::wstring MakeUniqueFileName(const std::wstring& folderPath, const std::wstring& desiredName)
{
    wchar_t fullPath[MAX_PATH]{};
    PathCombineW(fullPath, folderPath.c_str(), desiredName.c_str());
    if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
        return desiredName;

    DWORD attrs = GetFileAttributesW(fullPath);
    bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);

    std::wstring stem = desiredName;
    std::wstring ext;
    if (!isDir)
    {
        wchar_t stemBuf[MAX_PATH]{};
        wcscpy_s(stemBuf, stem.c_str());
        PathRemoveExtensionW(stemBuf);
        stem = stemBuf;
        const wchar_t* extPtr = PathFindExtensionW(desiredName.c_str());
        ext = extPtr ? extPtr : L"";
    }

    for (int i = 2; i < 1000; ++i)
    {
        std::wstring candidate = stem + L" (" + std::to_wstring(i) + L")" + ext;
        PathCombineW(fullPath, folderPath.c_str(), candidate.c_str());
        if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return stem + L" (1000)" + ext;
}

/**
 * @brief 提交或取消文件夹条目的重命名
 * @param newName 新名称
 * @param cancel 是否取消重命名
 */
void DesktopApp::CommitFolderEntryRename(const std::wstring& newName, bool cancel)
{
    const size_t widgetIndex =
        renameController_.OwnerIndex();
    const size_t memberIndex =
        renameController_.Index();
    const bool dockPopupEntry =
        renameController_.IsDockFolderEntry();
    renameController_.Reset();

    const FolderEntry* sourceEntry = nullptr;
    if (dockPopupEntry)
    {
        if (dockFolderPopupOpen_ &&
            memberIndex <
                dockFolderPopupWidget_.
                    folderEntries.size())
            sourceEntry =
                &dockFolderPopupWidget_.
                    folderEntries[memberIndex];
    }
    else if (widgetIndex < widgets_.size() &&
        widgets_[widgetIndex].type ==
            DesktopWidgetType::FolderMapping &&
        memberIndex <
            widgets_[widgetIndex].
                folderEntries.size())
    {
        sourceEntry =
            &widgets_[widgetIndex].
                folderEntries[memberIndex];
    }

    if (cancel || !sourceEntry ||
        newName.empty() ||
        newName == sourceEntry->name)
        return;

    RenameCommitTiming timing{
        dockPopupEntry ? RenameTargetKind::DockFolderEntry
                       : RenameTargetKind::FolderEntry };
    const ULONGLONG shellStarted = GetTickCount64();
    PIDLIST_ABSOLUTE pidl = nullptr;
    const std::wstring oldPath =
        sourceEntry->fullPath;
    if (FAILED(SHParseDisplayName(oldPath.c_str(), nullptr, &pidl, 0, nullptr)))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child);
    std::wstring renamedPath;
    if (SUCCEEDED(hr) && parentFolder)
    {
        wchar_t dirBuf[MAX_PATH]{};
        wcscpy_s(dirBuf, oldPath.c_str());
        PathRemoveFileSpecW(dirBuf);
        std::wstring uniqueName = MakeUniqueFileName(dirBuf, newName);
        wchar_t renamedPathBuffer[MAX_PATH]{};
        if (PathCombineW(
                renamedPathBuffer,
                dirBuf,
                uniqueName.c_str()))
            renamedPath = renamedPathBuffer;
        PITEMID_CHILD newChild = nullptr;
        hr = parentFolder->SetNameOf(ShellDialogOwnerHwnd(), child, uniqueName.c_str(), SHGDN_NORMAL, &newChild);
        if (newChild) ILFree(newChild);
        parentFolder->Release();
    }
    ILFree(pidl);
    timing.shellMs = GetTickCount64() - shellStarted;

    if (FAILED(hr))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    const ULONGLONG refreshStarted = GetTickCount64();
    if (!renamedPath.empty())
    {
        auto replaceOrderKey =
            [&](std::vector<std::wstring>& keys) {
                for (auto& key : keys)
                {
                    if (PathsEqualInsensitive(
                            key, oldPath))
                        key = renamedPath;
                }
            };
        for (auto& widget : widgets_)
        {
            if (widget.type ==
                DesktopWidgetType::
                    FolderMapping)
                replaceOrderKey(
                    widget.itemKeys);
        }
        replaceOrderKey(
            dockFolderPopupWidget_.
                itemKeys);
    }
    // ReloadItems(false) already enumerates every mapping, rebuilds the
    // containers and saves the migrated order. Do that work only once.
    ReloadItems(false);
    if (dockFolderPopupOpen_)
        RefreshDockFolderPopup();
    timing.refreshMs = GetTickCount64() - refreshStarted;
}

void DesktopApp::CommitRename(bool cancel)
{
    renameCommitPending_ = false;
    if (renameEdit_ == nullptr)
    {
        renameController_.
            SetQuickNavigationPresentation(false);
        return;
    }

    const bool quickNavigationRename =
        renameController_.
            IsQuickNavigationPresentation();
    renameController_.
        SetQuickNavigationPresentation(false);
    const size_t renameIndex =
        renameController_.Index();

    HWND edit = renameEdit_;
    renameEdit_ = nullptr;
    RemoveWindowSubclass(edit, &DesktopApp::RenameEditSubclassProc, 1);

    std::wstring newName;
    if (!cancel)
    {
        int length = GetWindowTextLengthW(edit);
        if (length > 0)
        {
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
            GetWindowTextW(edit, buffer.data(), length + 1);
            newName.assign(buffer.data());
        }
    }

    DestroyWindow(edit);
    if (renameFont_) { DeleteObject(renameFont_); renameFont_ = nullptr; }
    interactionPinnedWidgetId_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (renameController_.IsFolderEntry())
    {
        CommitFolderEntryRename(newName, cancel);
        if (quickNavigationRename)
            InvalidateQuickNavigationWindow();
        return;
    }

    RenameCommitTiming timing{ renameController_.Kind() };
    if (renameController_.IsWidget())
    {
        if (!cancel && renameIndex < widgets_.size())
        {
            if (!newName.empty() &&
                newName != widgets_[renameIndex].title)
            {
                widgets_[renameIndex].title = newName;
                widgets_[renameIndex].customTitle = newName;
                widgets_[renameIndex].userRenamed = true;
                SaveLayoutSlots();
            }
            else if (newName.empty() &&
                !widgets_[renameIndex].customTitle.empty())
            {
                DesktopWidget& widget = widgets_[renameIndex];
                widget.customTitle.clear();
                widgets_[renameIndex].userRenamed = false;
                if (!widget.scriptTitle.empty())
                    widget.title = widget.scriptTitle;
                else if (widget.type == DesktopWidgetType::LuaScript)
                    widget.title = WidgetEngine::GetWidgetDisplayName(widget.packageId);
                else if (widget.type == DesktopWidgetType::FileCategories)
                    widget.title = _LW("widget.desktop_files");
                else if (widget.type == DesktopWidgetType::Guide)
                    widget.title = _LW("app.guide.title");
                else if (widget.type == DesktopWidgetType::Collection)
                    widget.title = _LW("widget.collection");
                else if (widget.type ==
                    DesktopWidgetType::CollectionGroup)
                    widget.title =
                        _LW("widget.collection_group");
                SaveLayoutSlots();
            }
        }
        renameController_.Reset();
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (quickNavigationRename)
            InvalidateQuickNavigationWindow();
        return;
    }

    // Ending an unchanged/cancelled edit must not re-enumerate the desktop,
    // reload widget storage or restart icon loading on the input thread.
    if (cancel || renameIndex >= items_.size() ||
        newName.empty() || newName == items_[renameIndex].name)
    {
        renameController_.Reset();
        if (quickNavigationRename)
            InvalidateQuickNavigationWindow();
        return;
    }

    bool renamed = false;
    bool keepLayoutSlots = false;
    bool dockUsageKeyMigrated = false;
    if (!cancel && renameIndex < items_.size() &&
        !newName.empty() &&
        newName != items_[renameIndex].name)
    {
        const ULONGLONG shellStarted = GetTickCount64();
        std::wstring oldLayoutKey = items_[renameIndex].layoutKey;
        wchar_t desktopPath[MAX_PATH]{};
        SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
        std::wstring uniqueName = MakeUniqueFileName(desktopPath, newName);
        PITEMID_CHILD newChild = nullptr;
        HRESULT hr = desktopFolder_->SetNameOf(ShellDialogOwnerHwnd(),
            reinterpret_cast<PCUITEMID_CHILD>(
                items_[renameIndex].childPidl.get()),
            uniqueName.c_str(), SHGDN_NORMAL, &newChild);
        timing.shellMs = GetTickCount64() - shellStarted;
        if (SUCCEEDED(hr))
        {
            renamed = true;
            if (newChild)
            {
                PIDLIST_ABSOLUTE newAbsolute = ILCombine(desktopPidl_.get(), newChild);
                std::wstring newParsingName = StrRetToString(desktopFolder_.Get(), newChild, SHGDN_FORPARSING);
                if (newAbsolute)
                {
                    const std::wstring oldNormalizedKey =
                        ToUpperInvariant(oldLayoutKey);
                    const std::wstring newLayoutKey =
                        ToUpperInvariant(GetStableLayoutKey(
                            newAbsolute, newParsingName));
                    LayoutRecord record;
                    record.cell = items_[renameIndex].gridCell;
                    record.span = items_[renameIndex].gridSpan;
                    record.hasGrid = true;
                    record.legacySlot = items_[renameIndex].slot;
                    if (oldNormalizedKey != newLayoutKey)
                        layoutRecords_.erase(oldNormalizedKey);
                    layoutRecords_[newLayoutKey] = record;

                    snowdesktop::
                        desktop_item_reference_migration::
                            MigrateReferences(
                                widgets_, dockEntries_,
                                oldLayoutKey, newLayoutKey);

                    if (oldNormalizedKey != newLayoutKey)
                    {
                        auto oldUsage =
                            dockUsageStats_.find(
                                oldNormalizedKey);
                        if (oldUsage !=
                            dockUsageStats_.end())
                        {
                            const DockUsageRecord migrated =
                                oldUsage->second;
                            dockUsageStats_.erase(oldUsage);
                            DockUsageRecord& destination =
                                dockUsageStats_[newLayoutKey];
                            destination.launchCount =
                                std::max(
                                    destination.launchCount,
                                    migrated.launchCount);
                            destination.lastUsed =
                                std::max(
                                    destination.lastUsed,
                                    migrated.lastUsed);
                            dockUsageKeyMigrated = true;
                        }
                    }
                    keepLayoutSlots = true;
                    ILFree(newAbsolute);
                }
            }
            ILFree(newChild);
        }
        else
        {
            MessageBeep(MB_ICONWARNING);
        }
    }

    renameController_.Reset();
    const ULONGLONG refreshStarted = GetTickCount64();
    if (renamed)
        ReloadItems(!keepLayoutSlots);
    if (dockUsageKeyMigrated)
        SaveDockUsageStats();
    if (quickNavigationRename)
        InvalidateQuickNavigationWindow();
    timing.refreshMs = GetTickCount64() - refreshStarted;
}

/**
 * @brief 重命名编辑框的子类化窗口过程
 */
