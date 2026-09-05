#include "app.h"
#include "../shortcut_application_rules.h"
#include "quick_navigation_helpers.h"
#include "quick_navigation_rules.h"

// Quick-navigation rename, click handling, shortcuts and context menus.

void DesktopApp::BeginQuickNavigationItemRename(
    const std::wstring& name, bool isDirectory)
{
    if (renameEdit_ || name.empty() ||
        !quickNavigationOpen_ ||
        !quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_) ||
        IsRectEmptyRect(
            quickNavigationRenameItemRect_))
        return;

    const RECT itemRect =
        quickNavigationRenameItemRect_;
    const RECT iconRect =
        GetQuickNavItemIconRect(itemRect);
    const int horizontalPad = QuickNavScale(3);
    const int textTop = std::max<LONG>(
        itemRect.top,
        iconRect.bottom +
            std::max(1, QuickNavScale(2)));
    RECT editRect = MakeRect(
        itemRect.left + horizontalPad,
        textTop,
        itemRect.right - horizontalPad,
        std::min<LONG>(
            itemRect.bottom,
            textTop + QuickNavScale(32)));
    if (IsRectEmptyRect(editRect))
        return;

    renameCommitPending_ = false;
    renameController_.
        SetQuickNavigationPresentation(true);
    // The no-redirection DComp host cannot reliably display GDI child
    // controls. Match the search box: use an owned popup positioned over the
    // item's name area so the editor remains visually inside the panel.
    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE |
            WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST,
        L"EDIT", name.c_str(),
        snowdesktop::rename_edit_layout::EditStyle(),
        editRect.left + virtualLeft_,
        editRect.top + virtualTop_,
        editRect.right - editRect.left,
        editRect.bottom - editRect.top,
        quickNavigationHwnd_, nullptr,
        instance_, nullptr);
    if (!renameEdit_)
    {
        renameController_.Reset();
        return;
    }

    if (renameFont_)
        DeleteObject(renameFont_);
    renameFont_ = CreateFontW(
        -std::max(1, QuickNavScale(13)),
        0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    SendMessageW(
        renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_
                ? renameFont_
                : GetStockObject(
                    DEFAULT_GUI_FONT)),
        TRUE);
    SendMessageW(
        renameEdit_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(
            std::max(1, QuickNavScale(4)),
            std::max(1, QuickNavScale(4))));
    SetWindowSubclass(
        renameEdit_,
        &DesktopApp::RenameEditSubclassProc,
        1,
        reinterpret_cast<DWORD_PTR>(this));
    renameEditLayout_.Begin(renameEdit_);
    SetWindowPos(renameEdit_, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    int selectionEnd = -1;
    if (!isDirectory)
    {
        const size_t dot =
            name.find_last_of(L'.');
        if (dot != std::wstring::npos &&
            dot > 0 && dot + 1 < name.size())
            selectionEnd =
                static_cast<int>(dot);
    }
    SendMessageW(
        renameEdit_, EM_SETSEL,
        0, selectionEnd);
    SetFocus(renameEdit_);
}

void DesktopApp::
BeginQuickNavigationDesktopItemRename(
    size_t itemIndex)
{
    if (itemIndex >= items_.size() ||
        !items_[itemIndex].
            desktopIconClsid.empty())
        return;

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(
            items_[itemIndex].
                absolutePidl.get(),
            path))
        return;
    const DWORD attributes =
        GetFileAttributesW(path);
    const bool isDirectory =
        attributes !=
            INVALID_FILE_ATTRIBUTES &&
        (attributes &
            FILE_ATTRIBUTE_DIRECTORY) != 0;

    renameController_.BeginDesktopItem(itemIndex);
    BeginQuickNavigationItemRename(
        items_[itemIndex].name,
        isDirectory);
    if (!renameController_.
            IsQuickNavigationPresentation())
        renameController_.Reset();
}

void DesktopApp::
BeginQuickNavigationFolderEntryRename(
    size_t widgetIndex, size_t entryIndex)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type !=
            DesktopWidgetType::FolderMapping ||
        entryIndex >=
            widgets_[widgetIndex].
                folderEntries.size())
        return;

    const FolderEntry& entry =
        widgets_[widgetIndex].
            folderEntries[entryIndex];
    renameController_.BeginFolderEntry(
        widgetIndex, entryIndex);
    BeginQuickNavigationItemRename(
        entry.name, entry.isDirectory);
    if (!renameController_.
            IsQuickNavigationPresentation())
    {
        renameController_.Reset();
    }
}

/**
 * @brief 切换快捷导航面板的打开/关闭状态
 */
void DesktopApp::ToggleQuickNavigation()
{
    if (quickNavigationOpen_)
        CloseQuickNavigation();
    else
        OpenQuickNavigation(
            QuickNavigationInvocationSource::Hotkey);
}

/**
 * @brief 处理快捷导航面板内的点击事件
 * @param point 点击坐标（客户端坐标）
 * @return 是否已处理
 */
bool DesktopApp::HandleQuickNavigationClick(POINT point)
{
    if (!quickNavigationOpen_)
        return false;
    ResetQuickNavigationKeyboardTarget();

    RECT overlay = quickNavigationRect_;
    if (!PtInRect(&overlay, point))
    {
        if (DockContainer* dock =
                GetDockContainerAtPoint(point);
            dock && dock->IsSearchPoint(point))
        {
            PersistentDockHost* requestedDockHost =
                FindPersistentDockHost(dock);
            if (requestedDockHost &&
                requestedDockHost != quickNavigationDockHost_)
            {
                OpenQuickNavigation(
                    QuickNavigationInvocationSource::DockSearch);
                return true;
            }
        }
        CloseQuickNavigation();
        // Outside dismissal is a notification, not ownership of the press.
        // The same click must still be routed to the Dock/desktop target.
        return false;
    }

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    const bool searching = !GetQuickNavigationEffectiveSearchText().empty();
    if (!searching)
    {
        if (TrySetQuickNavigationDesktopViewModeAtPoint(
                point))
            return true;

        RECT tab0Rect = GetQuickNavigationTabRect(overlay, 0);
        if (PtInRect(&tab0Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
            quickNavigationScrollOffset_ = 0;
            quickNavigationInitialJumpOpen_ = false;
            InvalidateQuickNavigationWindow();
            return true;
        }
        RECT tab1Rect = GetQuickNavigationTabRect(overlay, 1);
        if (PtInRect(&tab1Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-2);
            quickNavigationScrollOffset_ = 0;
            quickNavigationInitialJumpOpen_ = false;
            InvalidateQuickNavigationWindow();
            return true;
        }
    }

    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return true;

    if (HandleQuickNavigationInitialJumpClick(
            point))
        return true;

    if (!searching &&
        quickNavigationActiveWidgetIndex_ ==
            static_cast<size_t>(-1) &&
        navigationSettings_.desktopViewMode ==
            QuickNavigationDesktopViewMode::Initial)
    {
        const QuickNavigationContentModel model =
            BuildQuickNavigationContentModel();
        for (size_t sectionIndex = 0;
            sectionIndex < model.sections.size();
            ++sectionIndex)
        {
            const RECT header =
                GetQuickNavigationSectionHeaderRect(
                    overlay, sectionIndex,
                    model);
            if (!PtInRect(&header, point))
                continue;
            const std::wstring& label =
                model.sections[sectionIndex].label;
            if (label.size() == 1)
            {
                quickNavigationInitialJumpSelection_ =
                    snowdesktop::
                        quick_navigation_rules::
                            InitialJumpBucketIndex(
                                label.front());
                quickNavigationInitialJumpOpen_ =
                    true;
                ResetQuickNavigationKeyboardTarget();
                InvalidateQuickNavigationWindow();
            }
            return true;
        }
    }

    if (!everythingSearchAvailable_ && searching)
    {
        std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
        bool onNotice = false;
        if (entries.empty() && quickNavigationEverythingResults_.empty())
        {
            onNotice = true;
        }
        else
        {
            const int columns = GetQuickNavigationColumnCount(overlay);
            const int desktopRows = entries.empty() ? 0 :
                (static_cast<int>(entries.size()) + columns - 1) / columns;
            const int headerH = QuickNavScale(28);
            const int gap = QuickNavScale(8);
            const int rowH = QuickNavScale(46);
            const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
            const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
                QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
            const int appSectionHeight = quickNavigationAppResultIndices_.empty()
                ? 0
                : headerH + gap + static_cast<int>(visibleAppCount) * rowH +
                    (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
            const int listHeaderTop = content.top + headerH
                + desktopGridH
                + gap + appSectionHeight - quickNavigationScrollOffset_;
            RECT noticeHeader = MakeRect(
                content.left + QuickNavScale(8),
                listHeaderTop,
                content.right - QuickNavScale(12),
                listHeaderTop + headerH);
            onNotice = PtInRect(&noticeHeader, point);
        }
        if (onNotice)
        {
            if (!quickNavigationAppsIndexed_)
            {
                StartQuickNavigationAppIndexing();
                InvalidateQuickNavigationWindow();
                return true;
            }

            const QuickNavigationAppEntry* everythingApp =
                FindQuickNavigationEverythingAppEntry();
            if (everythingApp)
            {
                CloseQuickNavigationThenLaunchApp(
                    *everythingApp);
            }
            else
            {
                CloseQuickNavigationThen(
                    []() {
                        ShellExecuteW(nullptr, L"open",
                            L"https://www.voidtools.com/zh-cn/downloads/",
                            nullptr, nullptr, SW_SHOWNORMAL);
                    });
            }
            return true;
        }
    }

    if (TryExpandQuickNavigationAppsAtPoint(point))
        return true;

    if (TryLoadMoreQuickNavigationEverythingResultsAtPoint(point))
        return true;

    const QuickNavigationAppEntry* appEntry = nullptr;
    if (TryGetQuickNavigationAppEntryAtPoint(point, appEntry) &&
        appEntry && appEntry->absolutePidl.get())
    {
        if (IsLuaLogicalSlotPickerOpen())
        {
            const auto candidate =
                BuildLuaLogicalSlotPickerCandidate(*appEntry);
            if (candidate)
                CommitLuaLogicalSlotPickerCandidate(*candidate);
            else
                MessageBeep(MB_ICONWARNING);
            return true;
        }
        CloseQuickNavigationThenLaunchApp(*appEntry);
        return true;
    }

    QuickNavigationEverythingEntry everythingEntry;
    if (TryGetQuickNavigationEverythingEntryAtPoint(point, everythingEntry) &&
        !everythingEntry.path.empty())
    {
        if (IsLuaLogicalSlotPickerOpen())
        {
            const auto candidate =
                BuildLuaLogicalSlotPickerCandidate(everythingEntry);
            if (candidate)
                CommitLuaLogicalSlotPickerCandidate(*candidate);
            else
                MessageBeep(MB_ICONWARNING);
            return true;
        }
        const std::wstring launchPath =
            everythingEntry.path;
        CloseQuickNavigationThen(
            [this, launchPath]() {
                shellLaunchWorker_.Enqueue(
                    nullptr, launchPath);
            });
        return true;
    }

    std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        RECT itemRect = GetQuickNavigationItemRect(overlay, i);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point)) continue;

        const QuickNavigationEntry entry = std::move(entries[i]);
        if (IsLuaLogicalSlotPickerOpen())
        {
            const auto candidate =
                BuildLuaLogicalSlotPickerCandidate(entry);
            if (candidate)
                CommitLuaLogicalSlotPickerCandidate(*candidate);
            else
                MessageBeep(MB_ICONWARNING);
            return true;
        }
        if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
            entry.itemIndex != static_cast<size_t>(-1) && entry.itemIndex < items_.size())
        {
            const size_t itemIndex = entry.itemIndex;
            CloseQuickNavigationThen(
                [this, itemIndex]() {
                    LaunchDesktopItem(itemIndex, true);
                });
        }
        else if (entry.kind == QuickNavigationEntry::Kind::FolderEntry && !entry.path.empty())
        {
            const std::wstring launchPath = entry.path;
            CloseQuickNavigationThen(
                [this, launchPath]() {
                    shellLaunchWorker_.Enqueue(
                        nullptr, launchPath);
                });
        }
        return true;
    }

    return true;
}

bool DesktopApp::HandleQuickNavigationRightClick(POINT point, POINT screenPoint)
{
    if (!quickNavigationOpen_)
        return false;
    if (IsLuaLogicalSlotPickerOpen())
        return true;

    const QuickNavigationAppEntry* appEntry = nullptr;
    if (TryGetQuickNavigationAppEntryAtPoint(point, appEntry) && appEntry)
    {
        ShowQuickNavigationAppContextMenu(*appEntry, screenPoint);
        return true;
    }

    QuickNavigationEverythingEntry entry;
    if (TryGetQuickNavigationEverythingEntryAtPoint(point, entry))
    {
        ShowQuickNavigationEverythingContextMenu(entry, screenPoint);
        return true;
    }

    const RECT overlay = quickNavigationRect_;
    const RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    std::vector<QuickNavigationEntry> entries =
        GetQuickNavigationEntries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        RECT itemRect =
            GetQuickNavigationItemRect(overlay, i);
        RECT clipped = itemRect;
        clipped.top =
            std::max(clipped.top, content.top);
        clipped.bottom =
            std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top ||
            !PtInRect(&clipped, point))
            continue;

        const QuickNavigationEntry selectedEntry =
            std::move(entries[i]);
        quickNavigationRenameItemRect_ =
            itemRect;

        if (selectedEntry.kind ==
                QuickNavigationEntry::Kind::DesktopItem &&
            selectedEntry.itemIndex < items_.size())
        {
            SelectOnly(static_cast<int>(
                selectedEntry.itemIndex));
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (IsProtectedDesktopIcon(
                    items_[selectedEntry.itemIndex]))
                ShowShellContextMenu(
                    screenPoint,
                    static_cast<int>(
                        selectedEntry.itemIndex),
                    true);
            else
                ShowItemContextMenu(
                    screenPoint,
                    static_cast<int>(
                        selectedEntry.itemIndex),
                    false, true);
            return true;
        }

        if (selectedEntry.kind ==
                QuickNavigationEntry::Kind::FolderEntry &&
            selectedEntry.widgetIndex < widgets_.size() &&
            selectedEntry.folderEntryIndex <
                widgets_[selectedEntry.widgetIndex].
                    folderEntries.size())
        {
            ClearSelection();
            widgets_[selectedEntry.widgetIndex].
                folderEntries[
                    selectedEntry.folderEntryIndex].
                selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            ShowFolderEntryContextMenu(
                screenPoint,
                selectedEntry.widgetIndex,
                selectedEntry.folderEntryIndex,
                true);
            return true;
        }
        return true;
    }

    return false;
}

bool DesktopApp::CopyTextToClipboard(const std::wstring& text)
{
    if (text.empty())
        return false;

    HWND owner = quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    if (!OpenClipboard(owner))
        return false;

    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle)
    {
        CloseClipboard();
        return false;
    }

    void* data = GlobalLock(handle);
    if (!data)
    {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }

    std::memcpy(data, text.c_str(), bytes);
    GlobalUnlock(handle);

    if (!SetClipboardData(CF_UNICODETEXT, handle))
    {
        GlobalFree(handle);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

std::wstring DesktopApp::SanitizeShortcutFileStem(const std::wstring& name)
{
    std::wstring stem = name;
    for (auto& ch : stem)
    {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch))
            ch = L'_';
    }
    while (!stem.empty() && (stem.back() == L'.' || stem.back() == L' '))
        stem.pop_back();
    while (!stem.empty() && stem.front() == L' ')
        stem.erase(stem.begin());
    if (stem.empty())
        stem = _LW("widget.shortcut");
    if (stem.size() > 80)
        stem.resize(80);
    return stem;
}

bool DesktopApp::IsApplicationsShellLinkTarget(
    IShellLinkW* shellLink,
    const std::wstring& shortcutPath)
{
    if (!shellLink)
        return false;

    namespace shortcutRules =
        snowdesktop::shortcut_application_rules;

    auto readProperty = [](
        IPropertyStore* propertyStore,
        REFPROPERTYKEY propertyKey) -> std::wstring
    {
        if (!propertyStore)
            return {};
        PROPVARIANT value{};
        PropVariantInit(&value);
        std::wstring result;
        if (SUCCEEDED(propertyStore->GetValue(propertyKey, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal)
            result = value.pwszVal;
        PropVariantClear(&value);
        return result;
    };

    std::wstring appUserModelId;
    std::wstring targetParsingPath;
    ComPtr<IPropertyStore> propertyStore;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&propertyStore))) &&
        propertyStore)
    {
        appUserModelId = readProperty(
            propertyStore.Get(), PKEY_AppUserModel_ID);
        targetParsingPath = readProperty(
            propertyStore.Get(), PKEY_Link_TargetParsingPath);
    }

    if (!shortcutPath.empty() &&
        (appUserModelId.empty() || targetParsingPath.empty()))
    {
        ComPtr<IShellItem2> shortcutItem;
        if (SUCCEEDED(SHCreateItemFromParsingName(
                shortcutPath.c_str(), nullptr,
                IID_PPV_ARGS(&shortcutItem))) && shortcutItem)
        {
            PWSTR value = nullptr;
            if (appUserModelId.empty() &&
                SUCCEEDED(shortcutItem->GetString(
                    PKEY_AppUserModel_ID, &value)) && value)
                appUserModelId = value;
            if (value)
            {
                CoTaskMemFree(value);
                value = nullptr;
            }
            if (targetParsingPath.empty() &&
                SUCCEEDED(shortcutItem->GetString(
                    PKEY_Link_TargetParsingPath, &value)) && value)
                targetParsingPath = value;
            if (value)
                CoTaskMemFree(value);
        }
    }

    wchar_t resolvedPath[32768]{};
    shellLink->GetPath(
        resolvedPath, static_cast<int>(std::size(resolvedPath)),
        nullptr, 0);
    wchar_t arguments[32768]{};
    shellLink->GetArguments(
        arguments, static_cast<int>(std::size(arguments)));

    PIDLIST_ABSOLUTE rawPidl = nullptr;
    Pidl targetPidl;
    bool applicationsPidlTarget = false;
    if (SUCCEEDED(shellLink->GetIDList(&rawPidl)) && rawPidl)
    {
        targetPidl.reset(rawPidl);
        const SIGDN names[] = {
            SIGDN_DESKTOPABSOLUTEPARSING,
            SIGDN_PARENTRELATIVEPARSING,
            SIGDN_NORMALDISPLAY,
        };
        for (SIGDN nameKind : names)
        {
            PWSTR parsingName = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(
                    targetPidl.get(), nameKind, &parsingName)) &&
                parsingName)
            {
                applicationsPidlTarget =
                    shortcutRules::LooksLikeApplicationsParsingName(
                        parsingName);
            }
            if (parsingName)
                CoTaskMemFree(parsingName);
            if (applicationsPidlTarget)
                break;
        }

        if (!applicationsPidlTarget)
        {
            SHFILEINFOW info{};
            if (SHGetFileInfoW(
                    reinterpret_cast<LPCWSTR>(targetPidl.get()), 0,
                    &info, sizeof(info),
                    SHGFI_PIDL | SHGFI_TYPENAME) && info.szTypeName[0])
            {
                const std::wstring typeName =
                    ToUpperInvariant(info.szTypeName);
                applicationsPidlTarget =
                    typeName == L"APPLICATION" ||
                    typeName == L"APPLICATIONS" ||
                    typeName == ToUpperInvariant(
                        _LW("app.nav.app_label")) ||
                    typeName == ToUpperInvariant(
                        _LW("app.interact.app_title"));
            }
        }
    }

    return shortcutRules::IsApplicationsShellLinkTarget(
        resolvedPath, arguments, appUserModelId,
        targetParsingPath, applicationsPidlTarget);
}

bool DesktopApp::CreateDesktopShortcutForShellLink(const std::wstring& displayName,
    PIDLIST_ABSOLUTE targetPidl, const std::wstring& targetPath, const std::wstring& workingDirectory)
{
    if (!targetPidl && targetPath.empty())
        return false;

    wchar_t desktopPath[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE))
        return false;

    std::wstring stem = SanitizeShortcutFileStem(displayName);
    if (stem.empty() && !targetPath.empty())
    {
        wchar_t nameBuf[MAX_PATH]{};
        wcscpy_s(nameBuf, PathFindFileNameW(targetPath.c_str()));
        PathRemoveExtensionW(nameBuf);
        stem = SanitizeShortcutFileStem(nameBuf);
    }

    std::wstring shortcutPath;
    for (int i = 1; i < 1000; ++i)
    {
        std::wstring fileName = i == 1
            ? stem + L".lnk"
            : stem + L" (" + std::to_wstring(i) + L").lnk";
        wchar_t candidate[MAX_PATH]{};
        PathCombineW(candidate, desktopPath, fileName.c_str());
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES)
        {
            shortcutPath = candidate;
            break;
        }
    }
    if (shortcutPath.empty())
    {
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath, (stem + L" (1000).lnk").c_str());
        shortcutPath = fallback;
    }

    ComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))) || !shellLink)
        return false;

    HRESULT setTargetHr = targetPidl
        ? shellLink->SetIDList(targetPidl)
        : shellLink->SetPath(targetPath.c_str());
    if (FAILED(setTargetHr))
        return false;

    if (!workingDirectory.empty())
        shellLink->SetWorkingDirectory(workingDirectory.c_str());

    ComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink.As(&persistFile)) ||
        FAILED(persistFile->Save(shortcutPath.c_str(), TRUE)))
        return false;

    ReloadItems();
    return true;
}

bool DesktopApp::CreateDesktopShortcutForApp(const QuickNavigationAppEntry& entry)
{
    if (!entry.absolutePidl.get())
        return false;
    return CreateDesktopShortcutForShellLink(entry.name, entry.absolutePidl.get(), L"", L"");
}

bool DesktopApp::CreateDesktopShortcutForPath(
    const std::wstring& path, bool isDirectory, const std::wstring& displayName)
{
    if (path.empty())
        return false;

    std::wstring workingDirectory;
    if (isDirectory)
    {
        workingDirectory = path;
    }
    else
    {
        wchar_t dir[MAX_PATH]{};
        wcscpy_s(dir, path.c_str());
        if (PathRemoveFileSpecW(dir))
            workingDirectory = dir;
    }

    std::wstring stem = displayName;
    if (stem.empty())
    {
        wchar_t nameBuf[MAX_PATH]{};
        wcscpy_s(nameBuf, PathFindFileNameW(path.c_str()));
        PathRemoveExtensionW(nameBuf);
        stem = nameBuf;
    }
    return CreateDesktopShortcutForShellLink(stem, nullptr, path, workingDirectory);
}

void DesktopApp::ShowQuickNavigationAppContextMenu(
    const QuickNavigationAppEntry& entry, POINT screenPoint)
{
    if (!entry.absolutePidl.get())
        return;
    PrepareMenuIconsForPoint(screenPoint);

    enum : UINT
    {
        kAppOpen = 1,
        kAppCreateShortcut = 2,
        kAppReveal = 3,
    };

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kAppOpen, _LW("app.nav.open"));
    AppendMenuW(menu,
        snowdesktop::item_location::CanReveal(entry.parsingName)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kAppReveal, _LW("app.menu.open_file_location"));
    AppendMenuW(menu, MF_STRING, kAppCreateShortcut, _LW("app.nav.send_to_desktop"));

    SetMenuItemIcon(menu, kAppOpen, L"");
    SetMenuItemIcon(menu, kAppReveal, L"");
    SetMenuItemIcon(menu, kAppCreateShortcut, L"");
    HWND owner = quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(owner);
    const UINT command = ShowModernMenu(menu, screenPoint, owner);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kAppOpen:
    {
        CloseQuickNavigationThenLaunchApp(entry);
        break;
    }
    case kAppCreateShortcut:
        CreateDesktopShortcutForApp(entry);
        break;
    case kAppReveal:
        snowdesktop::item_location::Reveal(
            hwnd_, entry.parsingName);
        break;
    default:
        break;
    }
}

void DesktopApp::ShowQuickNavigationEverythingContextMenu(
    const QuickNavigationEverythingEntry& entry, POINT screenPoint)
{
    if (entry.path.empty())
        return;
    PrepareMenuIconsForPoint(screenPoint);

    enum : UINT
    {
        kEverythingOpen = 1,
        kEverythingReveal = 2,
        kEverythingCopyPath = 3,
        kEverythingCreateShortcut = 4,
    };

    HMENU menu = CreatePopupMenu();
    if (!menu)
        return;

    AppendMenuW(menu, MF_STRING, kEverythingOpen, _LW("app.nav.open"));
    AppendMenuW(menu,
        snowdesktop::item_location::CanReveal(entry.path)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kEverythingReveal,
        _LW("app.menu.open_file_location"));
    AppendMenuW(menu, MF_STRING, kEverythingCreateShortcut, _LW("app.nav.send_to_desktop"));
    AppendMenuW(menu, MF_STRING, kEverythingCopyPath, _LW("app.nav.copy_path"));

    SetMenuItemIcon(menu, kEverythingOpen, L"");
    SetMenuItemIcon(menu, kEverythingReveal, L"");
    SetMenuItemIcon(menu, kEverythingCreateShortcut, L"");
    SetMenuItemIcon(menu, kEverythingCopyPath, L"");
    HWND owner = quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(owner);
    const UINT command = ShowModernMenu(menu, screenPoint, owner);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kEverythingOpen:
    {
        const std::wstring launchPath = entry.path;
        CloseQuickNavigationThen(
            [this, launchPath]() {
                shellLaunchWorker_.Enqueue(
                    nullptr, launchPath);
            });
        break;
    }
    case kEverythingReveal:
        snowdesktop::item_location::Reveal(
            hwnd_, entry.path);
        break;
    case kEverythingCopyPath:
        CopyTextToClipboard(entry.path);
        break;
    case kEverythingCreateShortcut:
        CreateDesktopShortcutForPath(entry.path, entry.isDirectory,
            entry.name.empty() ? FileNameFromPath(entry.path) : entry.name);
        break;
    default:
        break;
    }
}

/**
 * @brief 创建/重建快捷导航 DirectWrite 文本格式（DPI 变化时调用）。
 */
