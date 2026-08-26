#include "app.h"
#include "quick_navigation_helpers.h"

// Quick-navigation hotkey, window, search edit and positioning lifecycle.

void DesktopApp::UnregisterNavigationHotkey()
{
    if (navigationHotkeyRegistered_ && navigationHotkeyHwnd_)
    {
        UnregisterHotKey(navigationHotkeyHwnd_, kQuickNavigationHotkeyId);
        navigationHotkeyRegistered_ = false;
    }
    navigationHotkeyHwnd_ = nullptr;
}

/**
 * @brief 创建快捷导航窗口（若已存在则直接返回）
 * @return 窗口创建是否成功
 */
bool DesktopApp::CreateQuickNavigationWindow()
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        return true;

    quickNavigationHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW |
            (quickNavigationTopmost_ ? WS_EX_TOPMOST : 0) |
            WS_EX_NOREDIRECTIONBITMAP,
        kQuickNavigationWindowClassName,
_LW("app.interact.snow_nav_title"),
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!quickNavigationHwnd_)
        return false;

    return true;
}

/**
 * @brief 销毁快捷导航窗口及其子控件
 */
void DesktopApp::DestroyQuickNavigationWindow()
{
    quickNavigationHasLastEditAnimationFrame_ = false;
    quickNavBackdropCompositor_.Reset();
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        RemoveWindowSubclass(quickNavigationSearchEdit_, &DesktopApp::QuickNavigationSearchSubclassProc, 1);
        DestroyWindow(quickNavigationSearchEdit_);
    }
    quickNavigationSearchEdit_ = nullptr;
    if (quickNavigationSearchFont_)
    {
        DeleteObject(quickNavigationSearchFont_);
        quickNavigationSearchFont_ = nullptr;
    }
    ResetQuickNavCompositionResources();
    quickNavDcompScaleTransform_.Reset();
    quickNavDcompEffect_ = nullptr;
    if (quickNavDcompVisual_)
        quickNavDcompVisual_ = nullptr;
    if (quickNavDcompTarget_)
        quickNavDcompTarget_ = nullptr;
    quickNavDcompDevice_.Reset();
    quickNavCompositionCommitPending_ = false;
    quickNavTabTextFormat_.Reset();
    quickNavItemTextFormat_.Reset();
    quickNavPathTextFormat_.Reset();
    quickNavFluentTextFormat_.Reset();
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        DestroyWindow(quickNavigationHwnd_);
    quickNavigationHwnd_ = nullptr;
    quickNavigationWindowRegionExpanded_ = false;
    quickNavigationHoverRegions_.clear();
    quickNavigationPointerTarget_ = {};
    quickNavigationAnimation_.ResetHidden();
    quickNavigationTopmost_ = true;
}

void DesktopApp::SetQuickNavigationTopmost(
    bool topmost)
{
    quickNavigationTopmost_ = topmost;
    const HWND insertAfter =
        topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    constexpr UINT flags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        SetWindowPos(
            quickNavigationHwnd_, insertAfter,
            0, 0, 0, 0, flags);
    }
    quickNavBackdropCompositor_.SetPopupTopmost(
        topmost);
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        SetWindowPos(
            quickNavigationSearchEdit_, insertAfter,
            0, 0, 0, 0, flags);
    }
}

/**
 * @brief 创建、同步或移除快捷导航窗口下方的原生毛玻璃层
 */
void DesktopApp::UpdateQuickNavigationBackdrop()
{
    if (!quickNavGlassTheme_ || !quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_))
    {
        quickNavBackdropCompositor_.Reset();
        return;
    }

    if (!quickNavBackdropCompositor_.IsAvailable())
    {
        const bool initiallyVisible =
            quickNavigationAnimation_.GetVisual().visible;
        if (!quickNavBackdropCompositor_.InitializePopup(
                quickNavigationHwnd_, quickNavigationTopmost_,
                initiallyVisible))
        {
            std::wstring message = L"Quick navigation native backdrop unavailable: ";
            message += quickNavBackdropCompositor_.LastError();
            WriteDiagnosticLogEntry(message.c_str());
            return;
        }
        WriteDiagnosticLogEntry(L"Quick navigation native CompositionBackdropBrush initialized");
    }
    else
    {
        quickNavBackdropCompositor_.Reattach(quickNavigationHwnd_);
    }

    RECT clientRect = {
        quickNavigationRect_.left -
            quickNavigationHostRect_.left,
        quickNavigationRect_.top -
            quickNavigationHostRect_.top,
        quickNavigationRect_.right -
            quickNavigationHostRect_.left,
        quickNavigationRect_.bottom -
            quickNavigationHostRect_.top
    };
    if (IsRectEmptyRect(clientRect))
        return;
    const float cornerRadius = static_cast<float>(QuickNavScale(16)) / 2.0f;
    quickNavBackdropCompositor_.BeginFrame(true);
    quickNavBackdropCompositor_.AddPanel(clientRect, cornerRadius,
        quickNavBlurRadius_);
    quickNavBackdropCompositor_.EndFrame();
    const auto visual = quickNavigationAnimation_.GetVisual();
    const float anchorX = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.x -
        quickNavigationHostRect_.left);
    const float anchorY = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.y -
        quickNavigationHostRect_.top);
    quickNavBackdropCompositor_.SetVisualTransform(
        visual.scale, visual.opacity,
        anchorX, anchorY);
    quickNavBackdropCompositor_.SetVisible(visual.visible);
}

/**
 * @brief 确保快捷导航的搜索编辑框已创建
 */
void DesktopApp::EnsureQuickNavigationSearchEdit()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
        return;

    // 无重定向的 DComp 主窗口不能可靠承载 GDI 子控件，因此搜索框使用
    // 由快捷导航拥有的独立 popup HWND；它仍与主窗口位于同一 UI 线程。
    quickNavigationSearchEdit_ = CreateWindowExW(
        WS_EX_TOOLWINDOW |
            (quickNavigationTopmost_ ? WS_EX_TOPMOST : 0) |
            WS_EX_LAYERED,
        L"EDIT", L"", WS_POPUP | ES_AUTOHSCROLL,
        0, 0, 1, 1, quickNavigationHwnd_, nullptr,
        instance_, nullptr);
    if (!quickNavigationSearchEdit_)
        return;
    SetWindowLongPtrW(quickNavigationSearchEdit_, GWLP_ID, 1002);

    quickNavigationSearchFont_ = CreateFontW(-QuickNavScale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(quickNavigationSearchEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(quickNavigationSearchFont_ ? quickNavigationSearchFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(quickNavigationSearchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(QuickNavScale(10), QuickNavScale(10)));
    const wchar_t* searchHint = IsLuaLogicalSlotPickerOpen()
        ? _LW("app.nav.slot_picker_search_hint")
        : _LW("app.nav.search_hint");
    SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(searchHint));
    SetWindowSubclass(quickNavigationSearchEdit_, &DesktopApp::QuickNavigationSearchSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
}

/**
 * @brief 更新快捷导航搜索编辑框的位置和大小
 */
void DesktopApp::UpdateQuickNavigationSearchEditRect()
{
    if (!quickNavigationSearchEdit_ || !IsWindow(quickNavigationSearchEdit_))
        return;
    RECT search = GetQuickNavigationSearchRect(quickNavigationRect_);
    const int width = std::max<LONG>(
        1, search.right - search.left - QuickNavScale(8));
    const int height = std::max<LONG>(
        1, search.bottom - search.top - QuickNavScale(10));
    // The edit occupies only the solid center of the Direct2D search field.
    // Leave its HWND rectangular: a Win32 rounded region is a binary mask and
    // would add a non-antialiased edge inside the smooth Direct2D outline.
    SetWindowPos(
        quickNavigationSearchEdit_,
        quickNavigationTopmost_
            ? HWND_TOPMOST
            : HWND_NOTOPMOST,
        search.left + virtualLeft_ + QuickNavScale(4),
        search.top + virtualTop_ + QuickNavScale(6),
        width, height,
        SWP_NOACTIVATE);
}

std::wstring DesktopApp::GetQuickNavigationEffectiveSearchText() const
{
    if (quickNavigationSearchCompositionText_.empty())
        return quickNavigationSearchText_;

    std::wstring result = quickNavigationSearchText_;
    result += quickNavigationSearchCompositionText_;
    return result;
}

void DesktopApp::RefreshQuickNavigationSearchCompositionText(HWND editHwnd, LPARAM compositionFlags)
{
    if ((compositionFlags & GCS_COMPSTR) == 0)
        return;

    const std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigationSearchCompositionText_ = QuickNavigationReadImeCompositionString(editHwnd);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigationInitialJumpOpen_ = false;
        ResetQuickNavigationKeyboardTarget();
        quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
        quickNavigationScrollOffset_ = 0;
        InvalidateQuickNavigationWindow();
    }
}

void DesktopApp::ClearQuickNavigationSearchCompositionText()
{
    if (quickNavigationSearchCompositionText_.empty())
        return;

    const std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigationSearchCompositionText_.clear();
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigationInitialJumpOpen_ = false;
        ResetQuickNavigationKeyboardTarget();
        quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
        quickNavigationScrollOffset_ = 0;
        InvalidateQuickNavigationWindow();
    }
}

/**
 * @brief 刷新快捷导航搜索文本内容（从编辑框读取）
 */
void DesktopApp::RefreshQuickNavigationSearchText()
{
    ResetQuickNavigationKeyboardTarget();
    quickNavigationInitialJumpOpen_ = false;
    std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigationSearchText_.clear();
    if (!quickNavigationSearchEdit_ || !IsWindow(quickNavigationSearchEdit_))
    {
        if (!previousQuery.empty())
            ClearQuickNavigationEverythingResults();
        return;
    }
    int len = GetWindowTextLengthW(quickNavigationSearchEdit_);
    if (len <= 0)
    {
        if (GetQuickNavigationEffectiveSearchText() != previousQuery)
        {
            quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
            RefreshQuickNavigationEverythingResults();
        }
        return;
    }
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(quickNavigationSearchEdit_, buffer.data(), len + 1);
    buffer.resize(static_cast<size_t>(len));
    quickNavigationSearchText_ = std::move(buffer);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
    }
}

void DesktopApp::ClearQuickNavigationEverythingResults()
{
    ++quickNavigationEverythingSearchGeneration_;
    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        KillTimer(
            quickNavigationHwnd_,
            kQuickNavigationEverythingSearchTimerId);
    }
    quickNavigationAppResultIndices_.clear();
    quickNavigationAppsExpanded_ = false;
    quickNavigationEverythingResults_.clear();
    quickNavigationEverythingHasMore_ = false;
    quickNavigationEverythingSearchPending_ = false;
    quickNavigationEverythingResultsQuery_.clear();
    quickNavigationEverythingResultLimit_ = kQuickNavigationEverythingResultBatchSize;
}

int DesktopApp::GetQuickNavigationEverythingIconIndex(
    const std::wstring& path, bool isDirectory)
{
    if (isDirectory)
    {
        auto cached = quickNavigationEverythingIconCache_.find(L"<DIR>");
        if (cached != quickNavigationEverythingIconCache_.end())
            return cached->second;

        SHFILEINFOW info{};
        DWORD_PTR imageList = SHGetFileInfoW(
            path.empty() ? L"<DIR>" : path.c_str(),
            FILE_ATTRIBUTE_DIRECTORY,
            &info,
            sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        int iconIndex = imageList ? info.iIcon : -1;
        if (imageList)
            quickNavigationSystemImageListSmall_ = reinterpret_cast<HIMAGELIST>(imageList);
        quickNavigationEverythingIconCache_[L"<DIR>"] = iconIndex;
        return iconIndex;
    }

    std::wstring ext = ToUpperInvariant(PathFindExtensionW(path.c_str()));
    if (ext.empty())
        ext = L"<FILE>";

    bool perFileIcon = !path.empty() && (ext == L".EXE" || ext == L".LNK" || ext == L".DLL" ||
        ext == L".ICO" || ext == L".SCR" || ext == L".MSI" || ext == L".CPL");

    std::wstring cacheKey = perFileIcon ? ToUpperInvariant(path) : ext;

    auto cached = quickNavigationEverythingIconCache_.find(cacheKey);
    if (cached != quickNavigationEverythingIconCache_.end())
        return cached->second;

    SHFILEINFOW info{};
    DWORD_PTR imageList = 0;

    if (perFileIcon)
    {
        imageList = SHGetFileInfoW(
            path.c_str(), 0, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (!imageList)
        {
            imageList = SHGetFileInfoW(
                ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        }
    }
    else
    {
        imageList = SHGetFileInfoW(
            ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    }

    int iconIndex = imageList ? info.iIcon : -1;
    if (imageList)
        quickNavigationSystemImageListSmall_ = reinterpret_cast<HIMAGELIST>(imageList);

    quickNavigationEverythingIconCache_[cacheKey] = iconIndex;
    return iconIndex;
}

void DesktopApp::RefreshQuickNavigationEverythingResults()
{
    quickNavigationAppResultIndices_.clear();
    quickNavigationAppsExpanded_ = false;
    const std::wstring query = GetQuickNavigationEffectiveSearchText();
    if (query.empty())
    {
        ClearQuickNavigationEverythingResults();
        return;
    }

    RefreshQuickNavigationAppResults();

    if (IsLuaLogicalSlotPickerOpen() &&
        !LuaLogicalSlotPickerAccepts("filesystem.reference"))
    {
        ++quickNavigationEverythingSearchGeneration_;
        quickNavigationEverythingResults_.clear();
        quickNavigationEverythingHasMore_ = false;
        quickNavigationEverythingSearchPending_ = false;
        quickNavigationEverythingResultsQuery_.clear();
        return;
    }

    const DWORD requestLimit = std::max<DWORD>(
        quickNavigationEverythingResultLimit_,
        kQuickNavigationEverythingResultBatchSize);
    quickNavigationEverythingResultLimit_ = requestLimit;
    const bool preserveLoadedOrder =
        query == quickNavigationEverythingResultsQuery_ &&
        requestLimit > kQuickNavigationEverythingResultBatchSize &&
        !quickNavigationEverythingResults_.empty();
    if (!preserveLoadedOrder)
        quickNavigationEverythingResults_.clear();
    quickNavigationEverythingHasMore_ = false;
    quickNavigationEverythingSearchPending_ = true;
    everythingSearchAvailable_ = true;
    quickNavigationEverythingResultsQuery_ = query;
    ++quickNavigationEverythingSearchGeneration_;

    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_) &&
        SetTimer(
            quickNavigationHwnd_,
            kQuickNavigationEverythingSearchTimerId,
            kQuickNavigationEverythingSearchDebounceMs,
            nullptr))
    {
        return;
    }
    StartQueuedQuickNavigationEverythingSearch();
}

void DesktopApp::StartQueuedQuickNavigationEverythingSearch()
{
    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        KillTimer(
            quickNavigationHwnd_,
            kQuickNavigationEverythingSearchTimerId);
    }

    const std::wstring query =
        GetQuickNavigationEffectiveSearchText();
    if (!quickNavigationOpen_ || query.empty() ||
        !quickNavigationEverythingSearchPending_)
    {
        return;
    }

    snowdesktop::QuickNavigationEverythingSearchRequest
        request;
    request.generation =
        quickNavigationEverythingSearchGeneration_;
    request.query = query;
    request.maxResults =
        quickNavigationEverythingResultLimit_;
    if (!quickNavigationEverythingSearch_.Submit(
            hwnd_,
            kQuickNavigationEverythingSearchMessage,
            std::move(request)))
    {
        quickNavigationEverythingSearchPending_ = false;
        everythingSearchAvailable_ = false;
        InvalidateQuickNavigationWindow();
    }
}

void DesktopApp::OnQuickNavigationEverythingSearchCompleted(
    WPARAM cookie)
{
    if (cookie !=
        quickNavigationEverythingSearch_.MessageCookie())
    {
        return;
    }

    auto result =
        quickNavigationEverythingSearch_.TakeCompleted();
    if (!result || !quickNavigationOpen_ ||
        result->generation !=
            quickNavigationEverythingSearchGeneration_ ||
        result->query !=
            GetQuickNavigationEffectiveSearchText())
    {
        return;
    }

    ApplyQuickNavigationEverythingSearchResult(
        std::move(*result));
}

void DesktopApp::ApplyQuickNavigationEverythingSearchResult(
    snowdesktop::QuickNavigationEverythingSearchResult result)
{
    const bool preserveLoadedOrder =
        result.query ==
            quickNavigationEverythingResultsQuery_ &&
        result.maxResults >
            kQuickNavigationEverythingResultBatchSize &&
        !quickNavigationEverythingResults_.empty();
    std::vector<QuickNavigationEverythingEntry>
        previousResults;
    if (preserveLoadedOrder)
        previousResults = quickNavigationEverythingResults_;

    quickNavigationEverythingResults_.clear();
    quickNavigationEverythingSearchPending_ = false;
    quickNavigationEverythingResultsQuery_ = result.query;
    everythingSearchAvailable_ = result.error != 2;
    quickNavigationEverythingHasMore_ =
        everythingSearchAvailable_ &&
        result.results.size() >=
            static_cast<size_t>(result.maxResults);

    std::unordered_set<std::wstring> seenPaths;
    auto appendResult = [&](const EverythingSearchResult& result) {
        if (result.path.empty())
            return;
        if (IsLuaLogicalSlotPickerOpen() &&
            !LuaLogicalSlotPickerAcceptsType(
                result.isDirectory ? "folder" : "file"))
            return;
        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(
                    result.name.empty()
                        ? result.path
                        : result.name))
            return;
        std::wstring normalizedPath = ToUpperInvariant(result.path);
        if (seenPaths.contains(normalizedPath))
            return;
        seenPaths.insert(std::move(normalizedPath));

        QuickNavigationEverythingEntry entry;
        entry.name = result.name.empty() ? FileNameFromPath(result.path) : result.name;
        entry.path = result.path;
        entry.dateModified = result.dateModified;
        entry.modifiedText = QuickNavigationFormatModifiedTime(result.dateModified);
        entry.isDirectory = result.isDirectory;
        entry.systemIconIndex = GetQuickNavigationEverythingIconIndex(entry.path, entry.isDirectory);
        quickNavigationEverythingResults_.push_back(std::move(entry));
    };

    if (preserveLoadedOrder)
    {
        std::unordered_map<std::wstring, size_t> resultIndicesByPath;
        resultIndicesByPath.reserve(result.results.size());
        for (size_t i = 0; i < result.results.size(); ++i)
        {
            if (result.results[i].path.empty())
                continue;
            resultIndicesByPath.emplace(
                ToUpperInvariant(result.results[i].path),
                i);
        }

        for (const auto& previous : previousResults)
        {
            auto found = resultIndicesByPath.find(ToUpperInvariant(previous.path));
            if (found == resultIndicesByPath.end())
                continue;
            appendResult(result.results[found->second]);
        }
    }

    for (const auto& entry : result.results)
        appendResult(entry);

    quickNavigationScrollOffset_ = std::clamp(
        quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(
            quickNavigationRect_));
    ResetQuickNavigationKeyboardTarget();
    InvalidateQuickNavigationWindow();
}

/**
 * @brief 定位并显示快捷导航窗口
 */
void DesktopApp::PositionQuickNavigationWindow()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;

    quickNavigationRect_ = GetQuickNavigationRect();
    const RECT anchorRect = MakeRect(
        quickNavigationAnimationAnchorPoint_.x - 1,
        quickNavigationAnimationAnchorPoint_.y - 1,
        quickNavigationAnimationAnchorPoint_.x + 2,
        quickNavigationAnimationAnchorPoint_.y + 2);
    UnionRect(
        &quickNavigationHostRect_,
        &quickNavigationRect_,
        &anchorRect);
    InflateRect(
        &quickNavigationHostRect_, 2, 2);
    const int width = std::max<LONG>(
        1,
        quickNavigationHostRect_.right -
            quickNavigationHostRect_.left);
    const int height = std::max<LONG>(
        1,
        quickNavigationHostRect_.bottom -
            quickNavigationHostRect_.top);

    // Reserve the complete animation path once. ApplyQuickNavigationAnimationFrame
    // shrinks the region to the final panel once the transform reaches rest.
    UpdateQuickNavigationWindowRegion(true);
    const DWM_WINDOW_CORNER_PREFERENCE
        cornerPreference =
            DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(
        quickNavigationHwnd_,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &cornerPreference,
        sizeof(cornerPreference));

    SetWindowPos(
        quickNavigationHwnd_,
        quickNavigationTopmost_
            ? HWND_TOPMOST
            : HWND_NOTOPMOST,
        quickNavigationHostRect_.left + virtualLeft_,
        quickNavigationHostRect_.top + virtualTop_,
        width, height,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    UpdateQuickNavigationBackdrop();
    EnsureQuickNavigationSearchEdit();
    UpdateQuickNavigationSearchEditRect();
    ApplyQuickNavigationAnimationFrame();
}

void DesktopApp::UpdateQuickNavigationWindowRegion(
    bool expanded)
{
    if (!quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_) ||
        quickNavigationWindowRegionExpanded_ == expanded)
        return;
    if (expanded)
    {
        if (SetWindowRgn(
                quickNavigationHwnd_, nullptr, FALSE))
            quickNavigationWindowRegionExpanded_ = true;
        return;
    }

    const RECT panelRegionRect{
        quickNavigationRect_.left - quickNavigationHostRect_.left,
        quickNavigationRect_.top - quickNavigationHostRect_.top,
        quickNavigationRect_.right - quickNavigationHostRect_.left,
        quickNavigationRect_.bottom - quickNavigationHostRect_.top
    };
    // Keep the native region rectangular. The DComp surface owns the rounded
    // alpha edge; applying a binary Win32 rounded region here clips away its
    // partial-coverage pixels and leaves visibly jagged corners at rest.
    if (HRGN panelRegion = CreateRectRgn(
            panelRegionRect.left,
            panelRegionRect.top,
            panelRegionRect.right + 1,
            panelRegionRect.bottom + 1))
    {
        if (!SetWindowRgn(
                quickNavigationHwnd_,
                panelRegion, FALSE))
            DeleteObject(panelRegion);
        else
            quickNavigationWindowRegionExpanded_ = false;
    }
}

/**
 * @brief 使快捷导航窗口失效并触发重绘
 */
void DesktopApp::InvalidateQuickNavigationWindow(
    bool immediate)
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
    {
        InvalidateRect(quickNavigationHwnd_, nullptr, FALSE);
        // WM_MOUSEMOVE has higher dispatch priority than WM_PAINT. During a
        // fast sweep, a plain invalidation can therefore leave hover one or
        // more items behind the pointer. Hover callers synchronously present
        // each target transition; movement inside the same target is filtered
        // before reaching this function.
        if (immediate &&
            !quickNavCompositionPaintInProgress_)
            UpdateWindow(quickNavigationHwnd_);
    }
}

/**
 * @brief 应用并注册导航热键
 */
void DesktopApp::ApplyNavigationHotkey()
{
    UnregisterNavigationHotkey();
    if (!navigationSettings_.enabled || navigationSettings_.virtualKey == 0)
        return;

    HWND hotkeyWindow = inputHwnd_ && IsWindow(inputHwnd_)
        ? inputHwnd_
        : (controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_);
    if (!hotkeyWindow)
        return;

    UINT modifiers = navigationSettings_.modifiers | MOD_NOREPEAT;
    navigationHotkeyRegistered_ =
        RegisterHotKey(hotkeyWindow, kQuickNavigationHotkeyId,
            modifiers, navigationSettings_.virtualKey) != FALSE;
    if (navigationHotkeyRegistered_)
        navigationHotkeyHwnd_ = hotkeyWindow;
}

/**
 * @brief 获取指定标签页的显示名称
 */
std::wstring DesktopApp::GetQuickNavTabLabel(size_t tab) const
{
    if (tab == 0) return _LW("app.nav.tab_desktop");
    if (tab == 1) return _LW("app.nav.tab_mapping");
    std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
    if (tab - 2 >= ci.size()) return L"";
    const DesktopWidget& widget = widgets_[ci[tab - 2]];
    if (ShouldUseDemoCollectionIdentity(&widget))
        return GetDemoCollectionCategoryTitle(widget);
    if (!widget.title.empty())
        return widget.title;
    if (widget.type == DesktopWidgetType::FileCategories)
        return _LW("widget.desktop_files");
    if (widget.type == DesktopWidgetType::FolderMapping)
        return _LW("widget.folder_mapping");
    return _LW("widget.collection") + std::to_wstring(tab - 1);
}

/**
 * @brief 根据文字测量宽度，更新 quickNavTabWidths_
 */
void DesktopApp::UpdateQuickNavTabWidths()
{
    std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
    const size_t tabCount = ci.size() + 2;
    quickNavTabWidths_.resize(tabCount, QuickNavScale(72));

    if (!dwriteFactory_ || !quickNavTabTextFormat_)
        return;

    for (size_t i = 0; i < tabCount; ++i)
    {
        std::wstring label = GetQuickNavTabLabel(i);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(label.c_str(),
            static_cast<UINT32>(label.size()), quickNavTabTextFormat_.Get(),
            10000.0f, 10000.0f, &layout)) || !layout)
            continue;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        quickNavTabWidths_[i] = static_cast<int>(std::clamp(
            static_cast<LONG>(static_cast<long>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + QuickNavScale(20)),
            static_cast<LONG>(QuickNavScale(72)), static_cast<LONG>(QuickNavScale(200))));
    }
}

/**
 * @brief 根据拖拽位移计算目标标签索引
 * @param dragTab 被拖拽的标签索引
 * @param deltaX 拖拽水平位移
 * @return 目标标签索引（≥2）
 */
int DesktopApp::GetQuickNavTabDragTarget(size_t dragTab, int deltaX) const
{
    const auto& tw = quickNavTabWidths_;
    if (tw.empty() || dragTab >= tw.size()) return 2;
    RECT overlay = quickNavigationRect_;
    int srcCenter = GetQuickNavigationTabRect(overlay, dragTab).left
        + tw[dragTab] / 2 + deltaX;
    int target = 2;
    for (size_t i = 2; i < tw.size(); ++i)
    {
        RECT r = GetQuickNavigationTabRect(overlay, i);
        if (srcCenter < r.left + tw[i] / 2) { target = static_cast<int>(i); break; }
    }
    if (target > static_cast<int>(tw.size()) - 1)
        target = static_cast<int>(tw.size()) - 1;
    return target;
}

/**
 * @brief 打开快捷导航面板
 */

void DesktopApp::OpenQuickNavigation(
    QuickNavigationInvocationSource source)
{
    if (quickNavigationOpen_)
        return;
    quickNavigationPostCloseAction_ = {};
    quickNavigationHasLastEditAnimationFrame_ = false;
    if (dragSession_.IsActive() ||
        dragDropController_.IsExternalDragActive())
        return;
    quickNavigationInvocationSource_ = source;
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        const wchar_t* searchHint = IsLuaLogicalSlotPickerOpen()
            ? _LW("app.nav.slot_picker_search_hint")
            : _LW("app.nav.search_hint");
        SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(searchHint));
    }

    const bool reversingClose =
        quickNavigationAnimation_.IsClosing() &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_);
    if (reversingClose)
    {
        if (source == QuickNavigationInvocationSource::DockSearch)
        {
            POINT cursor{};
            if (GetCursorPos(&cursor))
            {
                const POINT appPoint{
                    cursor.x - virtualLeft_,
                    cursor.y - virtualTop_,
                };
                if (DockContainer* dock =
                        GetDockContainerAtPoint(appPoint);
                    dock && dock->IsSearchPoint(appPoint))
                {
                    const RECT searchRect = dock->GetSearchRect();
                    if (!IsRectEmptyRect(searchRect))
                    {
                        quickNavigationAnimationAnchorPoint_ = {
                            (searchRect.left + searchRect.right) / 2,
                            (searchRect.top + searchRect.bottom) / 2,
                        };
                        PositionQuickNavigationWindow();
                    }
                }
            }
        }
        quickNavigationOpen_ = true;
        if (quickNavigationSearchEdit_ &&
            IsWindow(quickNavigationSearchEdit_))
        {
            EnableWindow(quickNavigationSearchEdit_, TRUE);
            ShowWindow(
                quickNavigationSearchEdit_,
                SW_SHOWNOACTIVATE);
        }
        if (snowdesktop::dock_launch_animation::
                SystemAnimationsEnabled())
        {
            quickNavigationAnimation_.Open(
                static_cast<std::uint64_t>(
                    snowdesktop::UiAnimationScheduler::
                        MonotonicMilliseconds()));
            if (!StartQuickNavigationCompositionAnimation())
            {
                ApplyQuickNavigationAnimationFrame();
                EnsureUiAnimationFrame();
            }
        }
        else
        {
            quickNavigationAnimation_.
                ShowImmediately();
        }
        if (!quickNavigationAnimationCompositorDriven_)
            ApplyQuickNavigationAnimationFrame();
        if (quickNavigationSearchEdit_ &&
            IsWindow(quickNavigationSearchEdit_))
        {
            SetForegroundWindow(
                quickNavigationSearchEdit_);
            SetFocus(quickNavigationSearchEdit_);
            SendMessageW(
                quickNavigationSearchEdit_,
                EM_SETSEL, 0, -1);
        }
        else
        {
            SetForegroundWindow(
                quickNavigationHwnd_);
            SetFocus(quickNavigationHwnd_);
        }
        return;
    }

    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        quickNavigationOpenPoint_ = { cursor.x - virtualLeft_, cursor.y - virtualTop_ };
        quickNavigationLastMousePoint_ =
            quickNavigationOpenPoint_;
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        UINT dpiX = 96, dpiY = 96;
        if (monitor)
            GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        quickNavDpiScale_ = static_cast<float>(dpiX) / 96.0f;
    }
    else
    {
        quickNavigationOpenPoint_ = lastMousePoint_;
        quickNavigationLastMousePoint_ =
            quickNavigationOpenPoint_;
    }
    quickNavigationAnimationAnchorPoint_ =
        quickNavigationOpenPoint_;
    if (source == QuickNavigationInvocationSource::DockSearch)
    {
        if (DockContainer* dock =
                GetDockContainerAtPoint(
                    quickNavigationOpenPoint_);
            dock && dock->IsSearchPoint(
                quickNavigationOpenPoint_))
        {
            const RECT searchRect = dock->GetSearchRect();
            if (!IsRectEmptyRect(searchRect))
            {
                quickNavigationAnimationAnchorPoint_ = {
                    (searchRect.left + searchRect.right) / 2,
                    (searchRect.top + searchRect.bottom) / 2,
                };
            }
        }
    }

    EnsureQuickNavTextFormats();
    UpdateQuickNavTabWidths();
    quickNavigationOpen_ = true;
    EnsureNavTabOrder();
    if (quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-2))
    {
        // keep "映射文件夹全部" tab selection
    }
    else if (quickNavigationActiveWidgetIndex_ >= widgets_.size() ||
        (widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::Collection &&
         widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::FileCategories &&
         widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::FolderMapping))
    {
        quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
    }
    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationInitialJumpOpen_ = false;
    ResetQuickNavigationKeyboardTarget();
    quickNavigationSearchText_.clear();
    quickNavigationSearchCompositionText_.clear();
    ClearQuickNavigationEverythingResults();
    StartQuickNavigationAppIndexing();
    quickNavigationAnimation_.ResetHidden();
    if (!CreateQuickNavigationWindow())
    {
        quickNavigationOpen_ = false;
        MessageBeep(MB_ICONWARNING);
        return;
    }
    PositionQuickNavigationWindow();
    if (quickNavigationSearchEdit_)
        SetWindowTextW(quickNavigationSearchEdit_, L"");
    // Positioning deliberately leaves every quick-navigation visual at zero
    // opacity. Build and present the first content surface while it is still
    // hidden; attaching an empty DComp surface after the open animation has
    // started otherwise exposes a transparent frame when opened from the
    // floating Dock.
    InvalidateQuickNavigationWindow(true);
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        EnableWindow(quickNavigationSearchEdit_, TRUE);
        SetLayeredWindowAttributes(
            quickNavigationSearchEdit_,
            0, 0, LWA_ALPHA);
        ShowWindow(
            quickNavigationSearchEdit_,
            SW_SHOWNOACTIVATE);
    }
    if (snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
    {
        quickNavigationAnimation_.Open(
            static_cast<std::uint64_t>(
                snowdesktop::UiAnimationScheduler::
                    MonotonicMilliseconds()));
        if (!StartQuickNavigationCompositionAnimation())
        {
            ApplyQuickNavigationAnimationFrame();
            EnsureUiAnimationFrame();
        }
    }
    else
    {
        quickNavigationAnimation_.
            ShowImmediately();
    }
    if (!quickNavigationAnimationCompositorDriven_)
        ApplyQuickNavigationAnimationFrame();
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SetForegroundWindow(quickNavigationSearchEdit_);
        SetFocus(quickNavigationSearchEdit_);
        SendMessageW(quickNavigationSearchEdit_, EM_SETSEL, 0, -1);
    }
    else
    {
        SetForegroundWindow(quickNavigationHwnd_);
        SetFocus(quickNavigationHwnd_);
    }
}

/**
 * @brief 关闭快捷导航面板
 */
void DesktopApp::CloseQuickNavigation()
{
    if (!quickNavigationOpen_) return;
    if (!quickNavigationPostCloseAction_)
        logicalSlotPickerRequest_ = {};
    if (renameController_.
            IsQuickNavigationPresentation() &&
        renameEdit_ && IsWindow(renameEdit_))
        CommitRename(false);
    bool animationAnchorChanged = false;
    POINT cursor{};
    if (quickNavigationInvocationSource_ !=
            QuickNavigationInvocationSource::DockSearch &&
        GetCursorPos(&cursor))
    {
        const POINT currentPointer = {
            cursor.x - virtualLeft_,
            cursor.y - virtualTop_
        };
        if (currentPointer.x !=
                quickNavigationAnimationAnchorPoint_.x ||
            currentPointer.y !=
                quickNavigationAnimationAnchorPoint_.y)
        {
            quickNavigationAnimationAnchorPoint_ =
                currentPointer;
            animationAnchorChanged = true;
        }
    }
    if (animationAnchorChanged &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        // Pointer-mode close animations collapse toward the current pointer.
        // Recompute the compact host so that new anchor remains inside it.
        PositionQuickNavigationWindow();
    }
    quickNavigationOpen_ = false;
    ResetQuickNavigationKeyboardTarget();
    quickNavTabDragIndex_ = static_cast<size_t>(-1);
    quickNavTabDragDeltaX_ = 0;
    quickNavTabDragging_ = false;
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
        EnableWindow(quickNavigationSearchEdit_, FALSE);
    if (customDesktopVisible_)
        FocusDesktopInputWindow();

    if (!quickNavigationHwnd_ ||
        !IsWindow(quickNavigationHwnd_))
    {
        FinalizeCloseQuickNavigation();
        return;
    }

    if (snowdesktop::dock_launch_animation::
            SystemAnimationsEnabled())
    {
        quickNavigationAnimation_.Close(
            static_cast<std::uint64_t>(
                snowdesktop::UiAnimationScheduler::
                    MonotonicMilliseconds()));
    }
    else
    {
        quickNavigationAnimation_.ResetHidden();
    }
    if (quickNavigationAnimation_.IsHidden())
    {
        FinalizeCloseQuickNavigation();
        return;
    }
    if (!StartQuickNavigationCompositionAnimation())
    {
        ApplyQuickNavigationAnimationFrame();
        EnsureUiAnimationFrame();
        InvalidateQuickNavigationWindow();
    }
}

void DesktopApp::CloseQuickNavigationThen(
    std::function<void()> action)
{
    if (!quickNavigationOpen_)
    {
        if (action)
            action();
        return;
    }
    quickNavigationPostCloseAction_ =
        std::move(action);
    CloseQuickNavigation();
}

void DesktopApp::ApplyQuickNavigationAnimationFrame()
{
    if (quickNavigationAnimationCompositorDriven_)
        return;

    const auto visual =
        quickNavigationAnimation_.GetVisual();
    const float anchorX = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.x -
        quickNavigationHostRect_.left);
    const float anchorY = static_cast<float>(
        quickNavigationAnimationAnchorPoint_.y -
        quickNavigationHostRect_.top);

    quickNavBackdropCompositor_.SetVisualTransform(
        visual.scale, visual.opacity,
        anchorX, anchorY);
    quickNavBackdropCompositor_.SetVisible(
        visual.visible);
    UpdateQuickNavigationWindowRegion(
        quickNavigationAnimation_.IsAnimating() ||
            !visual.visible);

    if (quickNavDcompVisual_)
    {
        if (quickNavigationHwnd_ &&
            IsWindow(quickNavigationHwnd_))
        {
            quickNavDcompVisual_->SetOffsetX(
                static_cast<float>(
                    quickNavigationRect_.left -
                    quickNavigationHostRect_.left));
            quickNavDcompVisual_->SetOffsetY(
                static_cast<float>(
                    quickNavigationRect_.top -
                    quickNavigationHostRect_.top));
            if (quickNavDcompScaleTransform_)
            {
                quickNavDcompScaleTransform_->SetCenterX(
                    static_cast<float>(
                        quickNavigationAnimationAnchorPoint_.x -
                        quickNavigationRect_.left));
                quickNavDcompScaleTransform_->SetCenterY(
                    static_cast<float>(
                        quickNavigationAnimationAnchorPoint_.y -
                        quickNavigationRect_.top));
                quickNavDcompScaleTransform_->SetScaleX(
                    visual.scale);
                quickNavDcompScaleTransform_->SetScaleY(
                    visual.scale);
            }
            if (quickNavDcompEffect_)
            {
                quickNavDcompEffect_->SetOpacity(
                    visual.opacity);
            }
            if (quickNavDcompDevice_)
                CommitQuickNavigationCompositionFrame();
        }
    }

    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        if (!visual.visible)
        {
            ShowWindow(
                quickNavigationSearchEdit_,
                SW_HIDE);
            quickNavigationLastEditAnimationOpacity_ = 0;
            quickNavigationHasLastEditAnimationFrame_ = true;
            return;
        }
        const BYTE opacity =
            quickNavigationAnimation_.IsAnimating()
                ? 0 : 255;
        if (!quickNavigationHasLastEditAnimationFrame_ ||
            opacity != quickNavigationLastEditAnimationOpacity_)
        {
            SetLayeredWindowAttributes(
                quickNavigationSearchEdit_,
                0, opacity, LWA_ALPHA);
        }
        quickNavigationLastEditAnimationOpacity_ = opacity;
        quickNavigationHasLastEditAnimationFrame_ = true;
    }
}

void DesktopApp::FinalizeCloseQuickNavigation()
{
    std::function<void()> postCloseAction =
        std::move(quickNavigationPostCloseAction_);
    quickNavigationPostCloseAction_ = {};
    quickNavigationLastMousePoint_ = {
        LONG_MIN, LONG_MIN };
    quickNavigationHasLastEditAnimationFrame_ = false;
    if (quickNavigationSearchEdit_ &&
        IsWindow(quickNavigationSearchEdit_))
    {
        ShowWindow(
            quickNavigationSearchEdit_,
            SW_HIDE);
    }
    if (quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_))
    {
        ShowWindow(
            quickNavigationHwnd_,
            SW_HIDE);
    }

    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationInitialJumpOpen_ = false;
    quickNavigationSearchText_.clear();
    quickNavigationSearchCompositionText_.clear();
    ClearQuickNavigationEverythingResults();
    quickNavigationRect_ = {};
    quickNavigationHostRect_ = {};
    quickNavigationAnimation_.ResetHidden();
    DestroyQuickNavigationWindow();
    quickNavigationInvocationSource_ =
        QuickNavigationInvocationSource::Pointer;
    logicalSlotPickerRequest_ = {};
    if (postCloseAction)
        postCloseAction();
}
