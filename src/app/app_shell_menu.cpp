#include "app.h"
#include "../shell_context_menu_invoke.h"
#include "../shell_context_menu_site.h"

// Shell New menu, desktop host restoration and protected-icon handling.

DesktopApp::ShellPopupMenuLayerGuard::
ShellPopupMenuLayerGuard(DesktopApp& app)
    : app_(app)
{
    app_.BeginShellPopupMenuLayer();
}

DesktopApp::ShellPopupMenuLayerGuard::
~ShellPopupMenuLayerGuard()
{
    app_.EndShellPopupMenuLayer();
}

UINT DesktopApp::TrackShellPopupMenuWithDesktopPump(
    HMENU menu,
    UINT flags,
    POINT screenPoint,
    HWND owner)
{
    if (!menu || !owner || !IsWindow(owner))
        return 0;

    HANDLE completedEvent = CreateEventW(
        nullptr, TRUE, FALSE, nullptr);
    if (!completedEvent)
    {
        WriteDiagnosticLogEntry(
            L"Native menu tracker event unavailable; using synchronous fallback");
        return TrackPopupMenuEx(
            menu, flags, screenPoint.x, screenPoint.y,
            owner, nullptr);
    }

    std::atomic<UINT> selectedCommand{ 0 };
    std::thread tracker;
    try
    {
        tracker = std::thread([
            menu, flags, screenPoint, owner,
            completedEvent, &selectedCommand]() {
            const HRESULT comResult = CoInitializeEx(
                nullptr, COINIT_APARTMENTTHREADED);
            const UINT command = TrackPopupMenuEx(
                menu, flags,
                screenPoint.x, screenPoint.y,
                owner, nullptr);
            selectedCommand.store(
                command, std::memory_order_release);
            SetEvent(completedEvent);
            if (SUCCEEDED(comResult))
                CoUninitialize();
        });
    }
    catch (...)
    {
        CloseHandle(completedEvent);
        WriteDiagnosticLogEntry(
            L"Native menu tracker thread unavailable; using synchronous fallback");
        return TrackPopupMenuEx(
            menu, flags, screenPoint.x, screenPoint.y,
            owner, nullptr);
    }

    bool quitPending = false;
    WPARAM quitCode = 0;
    bool waitFailed = false;
    while (WaitForSingleObject(completedEvent, 0) !=
        WAIT_OBJECT_0)
    {
        HANDLE waitHandles[2]{ completedEvent, nullptr };
        DWORD handleCount = 1;
        HANDLE animationWait =
            uiAnimationScheduler_.WaitHandle();
        if (animationWait)
            waitHandles[handleCount++] = animationWait;

        const DWORD waitResult = MsgWaitForMultipleObjectsEx(
            handleCount,
            waitHandles,
            INFINITE,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_FAILED)
        {
            waitFailed = true;
            SendMessageW(owner, WM_CANCELMODE, 0, 0);
            break;
        }
        if (waitResult == WAIT_OBJECT_0)
            break;

        const bool animationWasReady =
            animationWait &&
            waitResult == WAIT_OBJECT_0 + 1;
        MSG message{};
        unsigned processedMessages = 0;
        while (processedMessages < 64 &&
            PeekMessageW(
                &message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                quitPending = true;
                quitCode = message.wParam;
                SendMessageW(owner, WM_CANCELMODE, 0, 0);
                ++processedMessages;
                continue;
            }
            const bool settingsMessageHandled =
                settingsWindow_ &&
                (settingsWindow_->PreTranslateMessage(&message) ||
                    settingsWindow_->ProcessTabNavigation(&message));
            if (!settingsMessageHandled)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            FlushPendingCompositionCommit();
            FlushPendingQuickNavigationCompositionCommit();
            ++processedMessages;
        }

        if (animationWait &&
            (animationWasReady ||
                WaitForSingleObject(animationWait, 0) ==
                    WAIT_OBJECT_0))
        {
            uiAnimationScheduler_.DispatchDue();
            FlushPendingCompositionCommit();
            FlushPendingQuickNavigationCompositionCommit();
        }
    }

    if (waitFailed &&
        WaitForSingleObject(completedEvent, 1000) !=
            WAIT_OBJECT_0)
    {
        WriteDiagnosticLogEntry(
            L"Native menu tracker did not stop after wait failure");
    }
    tracker.join();
    const UINT command = selectedCommand.load(
        std::memory_order_acquire);
    CloseHandle(completedEvent);
    PostMessageW(owner, WM_NULL, 0, 0);
    if (quitPending)
        PostQuitMessage(static_cast<int>(quitCode));
    return command;
}

void DesktopApp::ShowNewMenuAndInvoke(POINT screenPoint, const std::wstring& targetDir)
{
    ComPtr<IContextMenu> ctxMenu;
    if (FAILED(CoCreateInstance(CLSID_NewMenu, nullptr, CLSCTX_INPROC_SERVER,
        IID_IContextMenu, reinterpret_cast<void**>(ctxMenu.GetAddressOf()))) || !ctxMenu)
        return;

    ComPtr<IShellExtInit> shellExtInit;
    if (FAILED(ctxMenu.As(&shellExtInit)) || !shellExtInit)
        return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(targetDir.c_str(), nullptr, &pidl, 0, nullptr)) || pidl == nullptr)
        return;

    HRESULT hr = shellExtInit->Initialize(pidl, nullptr, 0);
    ILFree(pidl);
    if (FAILED(hr)) return;

    HMENU tmpMenu = CreatePopupMenu();
    if (!tmpMenu) return;
    hr = ctxMenu->QueryContextMenu(tmpMenu, 0, 1, 0x7FFF, CMF_NORMAL);
    if (FAILED(hr)) { DestroyMenu(tmpMenu); return; }

    HMENU newSub = GetSubMenu(tmpMenu, 0);
    if (!newSub) { DestroyMenu(tmpMenu); return; }

    ctxMenu.As(&newMenuContextMenu_);
    const HWND menuOwner = ShellDialogOwnerHwnd();
    SetForegroundWindow(menuOwner);
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    UINT cmd = TrackShellPopupMenuWithDesktopPump(
        newSub,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_LEFTBUTTON,
        screenPoint, menuOwner);
    newMenuContextMenu_.Reset();

    if (cmd != 0 && cmd >= 1)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - 1);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - 1);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, targetDir, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        SafeInvokeCommand(ctxMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
    }

    for (int i = GetMenuItemCount(tmpMenu) - 1; i >= 0; --i)
    {
        if (GetSubMenu(tmpMenu, i) == nullptr)
            RemoveMenu(tmpMenu, i, MF_BYPOSITION);
    }
    DestroyMenu(tmpMenu);
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
}

/**
 * @brief 通过 Shell IContextMenu 显示桌面的系统背景右键菜单。
 *        使用 desktopFolder_->CreateViewObject 获取桌面文件夹的
 *        IContextMenu 接口，显示系统提供的背景菜单（如显示设置、个性化等）。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
void DesktopApp::ShowDesktopBackgroundContextMenu(POINT screenPoint)
{
    const HWND menuOwner = ShellDialogOwnerHwnd();
    snowdesktop::ShellContextMenuSite menuSite;
    menuSite.Initialize(desktopFolder_.Get(), menuOwner);
    HWND shellOwner = menuSite.HostWindow()
        ? menuSite.HostWindow() : menuOwner;
    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = desktopFolder_->CreateViewObject(shellOwner, IID_IContextMenu,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(hr) || !contextMenu)
        return;
    menuSite.Attach(contextMenu.Get());

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    hr = contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd,
        CMF_NORMAL | CMF_SYNCCASCADEMENU);
    if (FAILED(hr)) { DestroyMenu(menu); RestoreDesktopWindowLayer(); return; }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(menuOwner);
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    UINT cmd = TrackShellPopupMenuWithDesktopPump(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint, menuOwner);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (cmd != 0)
    {
        const std::wstring invocationDirectory =
            snowdesktop::DesktopShellInvocationDirectory();
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(cmd - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(cmd - kFirstCmd);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, invocationDirectory, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(contextMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }
    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
}

/**
 * @brief 恢复桌面窗口的 Z 序层次位置。
 *        菜单弹出后可能改变窗口 Z 序，此方法将窗口恢复到正确的位置。
 *        有父窗口时置顶（HWND_TOP），无父窗口时置底（HWND_BOTTOM）。
 */
void DesktopApp::RestoreDesktopWindowLayer()
{
    ApplyFloatingDockLayerPolicy();
    ApplyFloatingPopupLayerPolicy();
    if (!hwnd_ || !IsWindow(hwnd_))
        return;
    POINT origin{ virtualLeft_, virtualTop_ };
    HWND parent = GetParent(hwnd_);
    if (parent)
    {
        ScreenToClient(parent, &origin);
        SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
    }
    else
    {
        SetWindowPos(hwnd_, HWND_BOTTOM, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
    }
}

void DesktopApp::ApplyFloatingDockLayerPolicy()
{
    for (const auto& host : persistentDockHosts_)
        if (host)
            ApplyFloatingDockLayerPolicy(*host);
}

void DesktopApp::ApplyFloatingDockLayerPolicy(
    PersistentDockHost& host)
{
    if (!host.active || !host.hwnd ||
        !IsWindow(host.hwnd))
        return;
    const bool promoted =
        IsPersistentDockHostEffectivelyFloating(host);

    if (!promoted)
    {
        // A desktop-band Dock is still a top-level no-activate window. Place
        // it immediately above Explorer's desktop host and therefore below
        // every ordinary application, while preserving the same HWND and
        // compositor resources used by floating mode.
        HWND insertAfter = nullptr;
        const HWND desktopHost =
            desktopWindows_.host && IsWindow(desktopWindows_.host)
                ? desktopWindows_.host : nullptr;
        if (desktopHost)
        {
            insertAfter = GetWindow(desktopHost, GW_HWNDPREV);
            while (insertAfter)
            {
                wchar_t className[96]{};
                GetClassNameW(
                    insertAfter, className,
                    static_cast<int>(std::size(className)));
                if (!IsPersistentDockHostWindow(insertAfter) &&
                    _wcsicmp(
                        className,
                        L"SnowDesktopBackdropWindow") != 0)
                {
                    break;
                }
                insertAfter = GetWindow(
                    insertAfter, GW_HWNDPREV);
            }
        }
        host.backdrop.SetPopupWindowPairZOrder(
            host.hwnd,
            insertAfter ? insertAfter : HWND_TOP,
            false);
        return;
    }

    const bool shouldBeTopmost =
        snowdesktop::floating_dock_rules::
            ShouldFloatingDockBeTopmost(
                true,
                shellPopupMenuLayerDepth_);
    host.backdrop.SetPopupWindowPairZOrder(
        host.hwnd,
        shouldBeTopmost
            ? HWND_TOPMOST : HWND_NOTOPMOST,
        shouldBeTopmost);
    ApplyDragPreviewLayerPolicy();
}

void DesktopApp::BeginShellPopupMenuLayer()
{
    // Flush the frame that opened the native menu before its tracking thread
    // takes pointer capture. The desktop pump remains live for the session.
    FlushPendingCompositionCommit();
    FlushPendingQuickNavigationCompositionCommit();
    ++shellPopupMenuLayerDepth_;
    if (shellPopupMenuLayerDepth_ == 1)
    {
        if (shellHoverTraceActive_)
            FlushShellHoverTrace();
        BeginShellHoverTrace();
    }
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MenuBegin);
    ApplyFloatingDockLayerPolicy();
    ApplyFloatingPopupLayerPolicy();
}

void DesktopApp::EndShellPopupMenuLayer()
{
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MenuEnd);
    if (shellPopupMenuLayerDepth_ > 0)
        --shellPopupMenuLayerDepth_;
    else
        shellPopupMenuLayerDepth_ = 0;
    if (shellPopupMenuLayerDepth_ == 0)
        shellHoverTraceMenuEndTick_ = GetTickCount64();
    ApplyFloatingDockLayerPolicy();
    ApplyFloatingPopupLayerPolicy();
    RefocusFloatingDockKeyboardSession();
    if (shellPopupMenuLayerDepth_ == 0)
    {
        ReconcileDesktopHoverState(
            snowdesktop::desktop_hover_rules::
                ShellPopupCloseReconcileMode());
    }
    FlushPendingCompositionCommit();
    FlushPendingQuickNavigationCompositionCommit();
}

/**
 * @brief 判断指定桌面项是否为受保护的系统图标。
 *        通过比较 CLSID 判断是否为此电脑、用户文件、网络、
 *        控制面板或回收站等系统图标。
 * @param item 要检查的桌面项。
 * @return 如果是受保护的系统图标返回 true，否则返回 false。
 */
bool DesktopApp::IsProtectedDesktopIcon(const DesktopItem& item) const
{
    std::wstring clsid = !item.desktopIconClsid.empty()
        ? item.desktopIconClsid
        : ExtractClsidText(item.parsingName);
    return clsid == kDesktopIconClsidThisPC ||
        clsid == kDesktopIconClsidUserFiles ||
        clsid == kDesktopIconClsidNetwork ||
        clsid == kDesktopIconClsidControlPanel ||
        clsid == kDesktopIconClsidRecycleBin;
}

/**
 * @brief 对指定的文件夹路径显示 Shell 右键菜单。
 *        解析路径的 PIDL，绑定到 IShellFolder，通过 CreateViewObject
 *        获取 IContextMenu 接口后弹出系统右键菜单。
 * @param folderPath  目标文件夹的路径。
 * @param screenPoint 菜单弹出的屏幕坐标。
 */
void DesktopApp::ShowShellContextMenuForPath(const std::wstring& folderPath, POINT screenPoint)
{
    const HWND menuOwner = ShellDialogOwnerHwnd();
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(folderPath.c_str(), nullptr, &pidl, 0, nullptr)))
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child)))
    {
        ILFree(pidl);
        return;
    }

    IShellFolder* folder = nullptr;
    HRESULT bindHr = parentFolder->BindToObject(child, nullptr, IID_IShellFolder, reinterpret_cast<void**>(&folder));
    parentFolder->Release();
    if (FAILED(bindHr) || folder == nullptr)
    {
        ILFree(pidl);
        return;
    }

    snowdesktop::ShellContextMenuSite menuSite;
    menuSite.Initialize(folder, menuOwner);
    HWND shellOwner = menuSite.HostWindow()
        ? menuSite.HostWindow() : menuOwner;
    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = folder->CreateViewObject(shellOwner, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (SUCCEEDED(hr) && contextMenu)
        menuSite.Attach(contextMenu.Get());
    folder->Release();
    if (FAILED(hr) || !contextMenu)
    {
        ILFree(pidl);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu) { ILFree(pidl); return; }

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(contextMenu->QueryContextMenu(menu, 0,
            kFirstCmd, kLastCmd,
            CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME |
                CMF_SYNCCASCADEMENU)))
    {
        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);
        return;
    }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(menuOwner);
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    UINT command = TrackShellPopupMenuWithDesktopPump(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint, menuOwner);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command != 0)
    {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCmd);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCmd);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, folderPath, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(contextMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        ReloadItems();
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
    ILFree(pidl);
}

void DesktopApp::
ShowShellItemContextMenuForPath(
    const std::wstring& itemPath,
    POINT screenPoint)
{
    const HWND menuOwner = ShellDialogOwnerHwnd();
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(
            itemPath.c_str(), nullptr,
            &pidl, 0, nullptr)) ||
        !pidl)
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(
            pidl, IID_IShellFolder,
            reinterpret_cast<void**>(
                &parentFolder),
            &child)) ||
        !parentFolder)
    {
        ILFree(pidl);
        return;
    }

    snowdesktop::ShellContextMenuSite menuSite;
    menuSite.Initialize(parentFolder, menuOwner);
    HWND shellOwner = menuSite.HostWindow()
        ? menuSite.HostWindow() : menuOwner;
    ComPtr<IContextMenu> contextMenu;
    const HRESULT hr =
        parentFolder->GetUIObjectOf(
            shellOwner, 1, &child,
            IID_IContextMenu, nullptr,
            reinterpret_cast<void**>(
                contextMenu.
                    GetAddressOf()));
    if (SUCCEEDED(hr) && contextMenu)
        menuSite.Attach(contextMenu.Get());
    parentFolder->Release();
    if (FAILED(hr) || !contextMenu)
    {
        ILFree(pidl);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        ILFree(pidl);
        return;
    }
    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    if (FAILED(
            contextMenu->QueryContextMenu(
                menu, 0, kFirstCmd,
                kLastCmd,
                CMF_NORMAL |
                    CMF_EXPLORE |
                    CMF_CANRENAME |
                    CMF_SYNCCASCADEMENU)))
    {
        DestroyMenu(menu);
        ILFree(pidl);
        return;
    }

    contextMenu.As(
        &activeContextMenu2_);
    contextMenu.As(
        &activeContextMenu3_);
    SetForegroundWindow(menuOwner);
    ShellPopupMenuLayerGuard shellMenuLayer(*this);
    const UINT command =
        TrackShellPopupMenuWithDesktopPump(
            menu,
            TPM_RETURNCMD |
                TPM_RIGHTBUTTON,
            screenPoint,
            menuOwner);
    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command >= kFirstCmd &&
        command <= kLastCmd)
    {
        const UINT commandOffset =
            command - kFirstCmd;
        if (IsShellDeleteCommand(
                contextMenu.Get(), commandOffset))
        {
            DestroyMenu(menu);
            RestoreDesktopWindowLayer();
            ILFree(pidl);
            std::vector<snowdesktop::ShellFileOperationStep> steps;
            steps.push_back({
                FO_DELETE,
                { itemPath },
                {},
                static_cast<FILEOP_FLAGS>(
                    FOF_ALLOWUNDO |
                    FOF_NOCONFIRMATION) });
            QueueShellFileOperation(
                std::move(steps),
                [this](bool succeeded) {
                    if (!succeeded)
                        return;
                    ReloadItems(false);
                    if (dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                });
            return;
        }
        const std::wstring invocationDirectory =
            snowdesktop::ShellInvocationDirectoryForItem(itemPath);
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask =
            CMIC_MASK_UNICODE |
            CMIC_MASK_PTINVOKE;
        invoke.hwnd =
            ShellDialogOwnerHwnd();
        invoke.lpVerb =
            MAKEINTRESOURCEA(
                commandOffset);
        invoke.lpVerbW =
            MAKEINTRESOURCEW(
                commandOffset);
        std::string invocationDirectoryA;
        snowdesktop::SetShellInvocationDirectory(
            invoke, invocationDirectory, invocationDirectoryA);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(
            contextMenu.Get(),
            reinterpret_cast<
                LPCMINVOKECOMMANDINFO>(
                    &invoke));
        for (size_t i = 0;
            i < widgets_.size(); ++i)
        {
            if (widgets_[i].type ==
                DesktopWidgetType::
                    FolderMapping)
                RefreshFolderMappingWidget(
                    i);
        }
        ReloadItems(false);
        if (dockFolderPopupOpen_)
            RefreshDockFolderPopup();
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    RestoreInteractionInputFocus();
    ILFree(pidl);
}
