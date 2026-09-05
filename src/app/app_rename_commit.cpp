#include "app.h"
#include "rename_model_update.h"
#include "../drag_input_rules.h"

#include <new>

// Shell work runs on the existing serial STA; completions own copied data
// and return through the existing UI-thread file-operation message.
bool DesktopApp::QueueRename(snowdesktop::ShellRenameRequest request)
{
    // A desktop file can also be reached through a mapped-folder popup. Keep
    // its desktop namespace identity so that surface uses the same fast path.
    snowdesktop::rename_model_update::AttachDesktopIdentity(request, items_);
    const HWND completionWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_ : hwnd_;
    if (!completionWindow || !IsWindow(completionWindow) ||
        !renameNotifications_.Begin(request.sourcePath, GetTickCount64()))
        return false;

    const std::wstring source = request.sourcePath;
    auto result = std::make_shared<snowdesktop::ShellRenameResult>();
    auto* completion = new (std::nothrow) ShellFileOperationUiCompletion{
        false, [this, result](bool) {
            pendingRenames_.push_back(result);
            ApplyPendingRenames();
        } };
    if (!completion)
    {
        renameNotifications_.Finish(source, {}, false, GetTickCount64());
        return false;
    }
    const bool queued = shellFileOperationWorker_.Enqueue(
        std::move(request),
        [completionWindow, completion, result](snowdesktop::ShellRenameResult value) {
            *result = std::move(value);
            completion->succeeded = SUCCEEDED(result->status);
            if (!PostMessageW(completionWindow, kShellFileOperationCompletedMessage,
                    0, reinterpret_cast<LPARAM>(completion)))
                delete completion;
        });
    if (!queued)
    {
        delete completion;
        renameNotifications_.Finish(source, {}, false, GetTickCount64());
        return false;
    }
    ++shellFileOperationInFlight_;
    return true;
}

void DesktopApp::ApplyPendingRenames()
{
    if (pendingRenames_.empty())
        return;
    // Sorting entries/rebuilding adapters must not invalidate a live editor,
    // pointer press, menu or retained native/OLE drag source.
    if (shellFileOperationInFlight_ > 0 || mouseDown_ || reloading_ ||
        renameEdit_ || HasActiveContextMenuSession() ||
        snowdesktop::drag_input_rules::ShouldDeferModelReload(
            dragSession_.HasContext(), dragDropController_.IsTransportActive()))
    {
        SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
        return;
    }

    const ULONGLONG started = GetTickCount64();
    const auto pending = std::exchange(pendingRenames_, {});
    bool changed = false;
    bool popupChanged = false;
    bool usageChanged = false;
    bool categoryChanged = false;
    ULONGLONG shellMs = 0;
    for (const auto& result : pending)
    {
        shellMs += result->elapsedMs;
        shellReloadPending_ |= renameNotifications_.Finish(
            result->sourcePath, result->path, SUCCEEDED(result->status), GetTickCount64());
        if (FAILED(result->status))
        {
            MessageBeep(MB_ICONWARNING);
            continue;
        }

        const auto changes = snowdesktop::rename_model_update::Apply(
            *result, ToUpperInvariant(result->path), items_, widgets_, dockEntries_,
            dockFolderPopupOpen_ ? &dockFolderPopupWidget_ : nullptr);
        shellReloadPending_ |= changes.needsReload;
        changed = changed || !result->path.empty();
        categoryChanged = categoryChanged ||
            (!changes.desktopItems.empty() &&
                snowdesktop::folder_sort_rules::CompareInsensitive(
                    snowdesktop::folder_sort_rules::ExtensionOf(result->sourcePath),
                    snowdesktop::folder_sort_rules::ExtensionOf(result->path)) != 0);
        popupChanged = popupChanged || changes.popup;
        const auto oldKey = ToUpperInvariant(result->sourcePath);
        const auto newKey = ToUpperInvariant(result->path);
        if (!newKey.empty() && oldKey != newKey)
        {
            if (const auto oldUsage = dockUsageStats_.find(oldKey);
                oldUsage != dockUsageStats_.end())
            {
                const DockUsageRecord usage = oldUsage->second;
                dockUsageStats_.erase(oldUsage);
                auto& destination = dockUsageStats_[newKey];
                destination.launchCount = std::max(destination.launchCount, usage.launchCount);
                destination.lastUsed = std::max(destination.lastUsed, usage.lastUsed);
                usageChanged = true;
            }
            if (cutPaths_.erase(oldKey))
                cutPaths_.insert(newKey);
        }
        if (!result->metadataComplete || result->absoluteId.empty())
            continue;

        // Keep all existing bitmaps visible. Only the renamed paths get new
        // asynchronous icon tasks; unrelated requests/generations stay alive.
        const auto queueIcon = [&](bool desktop, const std::wstring& widgetId) {
            IconLoadTask task;
            task.serial = iconLoadSerial_;
            task.layoutKey = newKey;
            task.absolutePidl.reset(ILCloneFull(
                reinterpret_cast<PCIDLIST_ABSOLUTE>(result->absoluteId.data())));
            task.sysIconIndex = result->sysIconIndex;
            task.parsingName = result->path;
            task.isDesktopItem = desktop;
            task.widgetId = widgetId;
            task.folderPath = desktop ? L"" : result->path;
            task.phase = IconLoadPhase::Phase1;
            if (task.absolutePidl.get())
                EnqueueIconLoad(std::move(task));
        };
        if (!changes.desktopItems.empty())
            queueIcon(true, {});
        for (const auto& widgetId : changes.folders)
            queueIcon(false, widgetId);
        if (changes.popup)
            queueIcon(false, kDockFolderPopupWidgetId);
    }
    if (changed)
    {
        RefreshDesktopItemIndexCache();
        dockAppIdentityCache_.clear();
        dockFolderTargetCache_.clear();
        dockFolderIconIndexCache_.clear();
        if (categoryChanged)
        {
            ApplyAutoCollectFileCategoryWidgets();
            LayoutItems();
        }
        SaveLayoutSlots();
        // Rebind the lightweight views without enumerating files or reloading
        // Lua storage. Their underlying item vectors and bitmaps are retained.
        RebuildContainersAndItems();
        if (popupChanged)
        {
            dockFolderPopupContainer_ =
                std::make_unique<FolderMapping>(&dockFolderPopupWidget_, this);
            dockFolderPopupContainer_->InvalidateFilterCache();
            RefreshDockFolderPopupGeometry();
        }
        if (usageChanged)
            SaveDockUsageStats();
        InvalidateDragStaticScene();
        InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateFloatingPopupWindow(false);
        InvalidateQuickNavigationWindow();
        if (widgetEngine_)
            widgetEngine_->NotifyDesktopChanged("reload");
    }
    if (shellReloadPending_)
        SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);

    wchar_t timing[256]{};
    swprintf_s(timing,
        L"Rename async complete: count=%zu workerMs=%llu applyMs=%llu fallbackReload=%d",
        pending.size(), shellMs, GetTickCount64() - started, shellReloadPending_ ? 1 : 0);
    WriteDiagnosticLogEntry(timing);
}

void DesktopApp::CommitFolderEntryRename(const std::wstring& newName, bool cancel)
{
    const size_t widgetIndex = renameController_.OwnerIndex();
    const size_t memberIndex = renameController_.Index();
    const bool dockPopupEntry = renameController_.IsDockFolderEntry();
    renameController_.Reset();

    const FolderEntry* entry = nullptr;
    if (dockPopupEntry)
    {
        if (dockFolderPopupOpen_ &&
            memberIndex < dockFolderPopupWidget_.folderEntries.size())
            entry = &dockFolderPopupWidget_.folderEntries[memberIndex];
    }
    else if (widgetIndex < widgets_.size() &&
        widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
        memberIndex < widgets_[widgetIndex].folderEntries.size())
        entry = &widgets_[widgetIndex].folderEntries[memberIndex];
    if (cancel || !entry || newName.empty() || newName == entry->name)
        return;
    if (!QueueRename({ entry->fullPath, newName, {} }))
        MessageBeep(MB_ICONWARNING);
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

    snowdesktop::ShellRenameRequest request;
    request.sourcePath = items_[renameIndex].parsingName;
    request.newName = newName;
    const auto* child = items_[renameIndex].childPidl.get();
    if (child)
    {
        const auto* bytes = reinterpret_cast<const BYTE*>(child);
        request.desktopChildId.assign(bytes, bytes + ILGetSize(child));
    }
    renameController_.Reset();
    if (!QueueRename(std::move(request)))
        MessageBeep(MB_ICONWARNING);
    if (quickNavigationRename)
        InvalidateQuickNavigationWindow();
}
