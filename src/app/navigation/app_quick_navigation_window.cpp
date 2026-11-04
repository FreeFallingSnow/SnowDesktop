#include "ui/input/private_pointer.h"
#include "ui/render/app_font.h"
#include "app/app.h"
#include "navigation/quick_navigation_animation_rules.h"
#include "app/navigation/quick_navigation_state.h"
#include "common/l10n.h"
#include "diagnostics/diagnostic_log.h"
#include "settings/animation_settings.h"
#include "shell/native_start_menu.h"
#include "common/utils.h"
#include "ui/input/text_input_window.h"
#include "shell/shell_item_visibility.h"
#include "navigation/everything_search.h"
#include "app/dock/persistent_dock_host.h"
#include "quick_navigation_helpers.h"
#include "navigation/quick_navigation_rules.h"
#include "platform/shell_overlay_window.h"

namespace
{
constexpr wchar_t kQuickNavigationOutsidePress[] = L"SnowDesktop.Navigation.OutsidePress";
unsigned NavigationPointerButtons()
{
    return ((snowdesktop::private_pointer::AsyncKeyState(VK_LBUTTON) & 0x8000) ? 1u : 0u) |
        ((snowdesktop::private_pointer::AsyncKeyState(VK_RBUTTON) & 0x8000) ? 2u : 0u) |
        ((snowdesktop::private_pointer::AsyncKeyState(VK_MBUTTON) & 0x8000) ? 4u : 0u);
}
}

void DesktopApp::StopQuickNavigationAnimationTimeline()
{
    if (quickNavigation_->animationCompletionToken)
        uiAnimationScheduler_.Cancel(quickNavigation_->animationCompletionToken);
    if (quickNavigation_->animationFrameToken)
        uiAnimationScheduler_.Cancel(quickNavigation_->animationFrameToken);
    quickNavigation_->animationCompletionToken = 0;
    quickNavigation_->animationFrameToken = 0;
    quickNavigation_->animationCompositorDriven = false;
}

void DesktopApp::ConfigureQuickNavigationAnimation()
{
    namespace rules = snowdesktop::quick_navigation_animation_rules;
    namespace motion = snowdesktop::animation;
    const bool dockSource = generalSettings_.dockEnabled &&
        quickNavigation_->invocationSource == QuickNavigationInvocationSource::DockSearch &&
        quickNavigationDockHost_ && quickNavigationDockHost_->active &&
        quickNavigationDockHost_->container &&
        !IsRectEmptyRect(quickNavigationDockHost_->container->GetSearchRect()) &&
        !IsRectEmpty(&quickNavigation_->animationDockRect);
    auto effect = rules::ResolveEffect(motion::RuntimeAnimationsEnabled(), dockSource,
        motion::RuntimeWindowEffect(), motion::RuntimePopupEffect());
    if (rules::IsDeformationEffect(effect) && quickNavigation_->deformationFailed)
        effect = rules::Effect::Scale;
    quickNavigation_->animation.Configure(effect, motion::RuntimeDurationScale(),
        dockSource && motion::RuntimeWindowEffect() != 0, !dockSource);
}

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
    if (quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd))
        return true;

    quickNavigation_->hwnd = snowdesktop::CreateShellOverlayWindowEx(
        WS_EX_TOOLWINDOW |
            (quickNavigation_->topmost ? WS_EX_TOPMOST : 0) |
            WS_EX_NOREDIRECTIONBITMAP,
        kQuickNavigationWindowClassName,
_LW("app.interact.snow_nav_title"),
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!quickNavigation_->hwnd)
        return false;

    return true;
}

/**
 * @brief 销毁快捷导航窗口及其子控件
 */
void DesktopApp::DestroyQuickNavigationWindow()
{
    quickNavigation_->externalDialogActive = false;
    if (quickNavigation_->hwnd) RemovePropW(quickNavigation_->hwnd, kQuickNavigationOutsidePress);
    CancelQuickNavigationTabHover();
    quickNavigation_->hasLastEditAnimationFrame = false;
    quickNavigation_->backdropCompositor.Reset();
    if (quickNavigation_->searchEdit && IsWindow(quickNavigation_->searchEdit))
    {
        RemoveWindowSubclass(quickNavigation_->searchEdit, &DesktopApp::QuickNavigationSearchSubclassProc, 1);
        DestroyWindow(quickNavigation_->searchEdit);
    }
    quickNavigation_->searchEdit = nullptr;
    if (quickNavigation_->searchFont)
    {
        DeleteObject(quickNavigation_->searchFont);
        quickNavigation_->searchFont = nullptr;
    }
    ResetQuickNavCompositionResources();
    quickNavigation_->dcompScaleTransform.Reset();
    quickNavigation_->dcompEffect = nullptr;
    if (quickNavigation_->dcompVisual)
        quickNavigation_->dcompVisual = nullptr;
    if (quickNavigation_->dcompTarget)
        quickNavigation_->dcompTarget = nullptr;
    quickNavigation_->dcompDevice.Reset();
    quickNavigation_->compositionCommitPending = false;
    quickNavigation_->tabTextFormat.Reset();
    quickNavigation_->itemTextFormat.Reset();
    quickNavigation_->searchTextFormat.Reset();
    quickNavigation_->pathTextFormat.Reset();
    quickNavigation_->fluentTextFormat.Reset();
    if (quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd))
        DestroyWindow(quickNavigation_->hwnd);
    quickNavigation_->hwnd = nullptr;
    quickNavigation_->windowRegionExpanded = false;
    quickNavigation_->hoverRegions.clear();
    quickNavigation_->pointerTarget = {};
    quickNavigation_->animation.ResetHidden();
    quickNavigation_->topmost = true;
    ApplyFloatingDockLayerPolicy();
}

void DesktopApp::SetQuickNavigationTopmost(
    bool topmost)
{
    quickNavigation_->topmost = topmost;
    const HWND insertAfter =
        topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    constexpr UINT flags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;

    if (quickNavigation_->hwnd &&
        IsWindow(quickNavigation_->hwnd))
    {
        SetWindowPos(
            quickNavigation_->hwnd, insertAfter,
            0, 0, 0, 0, flags);
    }
    quickNavigation_->backdropCompositor.SetPopupTopmost(
        topmost);
    if (quickNavigation_->searchEdit &&
        IsWindow(quickNavigation_->searchEdit))
    {
        SetWindowPos(
            quickNavigation_->searchEdit, insertAfter,
            0, 0, 0, 0, flags);
    }
}

/**
 * @brief 创建、同步或移除快捷导航窗口下方的原生毛玻璃层
 */
void DesktopApp::UpdateQuickNavigationBackdrop()
{
    if (!quickNavigation_->glassTheme || !quickNavigation_->hwnd ||
        !IsWindow(quickNavigation_->hwnd))
    {
        quickNavigation_->backdropCompositor.Reset();
        return;
    }

    if (!quickNavigation_->backdropCompositor.IsAvailable())
    {
        const bool initiallyVisible =
            quickNavigation_->animation.GetVisual().visible;
        if (!quickNavigation_->backdropCompositor.InitializePopup(
                quickNavigation_->hwnd, quickNavigation_->topmost,
                initiallyVisible))
        {
            std::wstring message = L"Quick navigation native backdrop unavailable: ";
            message += quickNavigation_->backdropCompositor.LastError();
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, message.c_str(), DiagnosticLogLevel::Warning);
            return;
        }
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, L"Quick navigation native CompositionBackdropBrush initialized");
    }
    else
    {
        quickNavigation_->backdropCompositor.Reattach(quickNavigation_->hwnd);
    }

    RECT clientRect = {
        quickNavigation_->rect.left -
            quickNavigation_->hostRect.left,
        quickNavigation_->rect.top -
            quickNavigation_->hostRect.top,
        quickNavigation_->rect.right -
            quickNavigation_->hostRect.left,
        quickNavigation_->rect.bottom -
            quickNavigation_->hostRect.top
    };
    if (IsRectEmptyRect(clientRect))
        return;
    const float cornerRadius = static_cast<float>(QuickNavScale(navigationSettings_.layout.cornerRadius));
    quickNavigation_->backdropCompositor.BeginFrame(true);
    quickNavigation_->backdropCompositor.AddPanel(clientRect, cornerRadius,
        quickNavigation_->blurRadius, kQuickNavigationBackdropPanelOwner);
    quickNavigation_->backdropCompositor.EndFrame(false);
    const auto visual = quickNavigation_->animation.GetVisual();
    const float anchorX = static_cast<float>(
        quickNavigation_->animationAnchorPoint.x -
        quickNavigation_->hostRect.left);
    const float anchorY = static_cast<float>(
        quickNavigation_->animationAnchorPoint.y -
        quickNavigation_->hostRect.top);
    quickNavigation_->backdropCompositor.SetVisualTransform(
        visual.scale, visual.opacity,
        anchorX, anchorY);
    quickNavigation_->backdropCompositor.SetVisible(visual.visible);
    if (!quickNavigation_->compositionPaintInProgress)
        quickNavigation_->backdropCompositor.CommitVisualChanges();
}

/**
 * @brief 确保快捷导航的搜索编辑框已创建
 */
void DesktopApp::EnsureQuickNavigationSearchEdit()
{
    if (!quickNavigation_->hwnd || !IsWindow(quickNavigation_->hwnd))
        return;
    if (quickNavigation_->searchEdit && IsWindow(quickNavigation_->searchEdit))
        return;

    // Only focus and IME use this child. Its visible contents are part of the
    // quick-navigation surface, including opening and closing transforms.
    quickNavigation_->searchEdit = CreateWindowExW(
        0, snowdesktop::text_input::WindowClass(), L"", WS_CHILD | ES_AUTOHSCROLL,
        -32000, -32000, 1, 1, quickNavigation_->hwnd, nullptr,
        instance_, nullptr);
    if (!quickNavigation_->searchEdit)
        return;
    SetWindowLongPtrW(quickNavigation_->searchEdit, GWLP_ID, 1002);

    quickNavigation_->searchFont = CreateFontW(-QuickNavScale(navigationSettings_.layout.searchFontSize), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, snowdesktop::app_fonts::GdiFamily().c_str());
    SendMessageW(quickNavigation_->searchEdit, WM_SETFONT,
        reinterpret_cast<WPARAM>(quickNavigation_->searchFont ? quickNavigation_->searchFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    snowdesktop::text_input::SetPadding(quickNavigation_->searchEdit, 0.f, 0.f);
    snowdesktop::text_input::SetCaretHeight(quickNavigation_->searchEdit, static_cast<float>(QuickNavScale(navigationSettings_.layout.searchFontSize + 2)));
    const wchar_t* searchHint = IsLuaLogicalSlotPickerOpen()
        ? _LW("app.nav.slot_picker_search_hint")
        : _LW("app.nav.search_hint");
    SendMessageW(quickNavigation_->searchEdit, EM_SETCUEBANNER, TRUE,
        reinterpret_cast<LPARAM>(searchHint));
    snowdesktop::text_input::SetAccessibleName(quickNavigation_->searchEdit,searchHint);
    SetWindowSubclass(quickNavigation_->searchEdit, &DesktopApp::QuickNavigationSearchSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
}

/**
 * @brief 更新快捷导航搜索编辑框的位置和大小
 */
void DesktopApp::UpdateQuickNavigationSearchEditRect()
{
    if (!quickNavigation_->searchEdit || !IsWindow(quickNavigation_->searchEdit))
        return;
    const wchar_t* hint = IsLuaLogicalSlotPickerOpen() ? _LW("app.nav.slot_picker_search_hint") :
        _LW(quickNavigation_->searchType == QuickNavigationSearchType::All ? "app.nav.search_hint" : "quickNav.search.scopedPlaceholder");
    SendMessageW(quickNavigation_->searchEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(hint));
    const auto accessibleName = quickNavigation_->searchType == QuickNavigationSearchType::All ? std::wstring(hint) : QuickNavigationTypeLabel() + L" · " + hint;
    snowdesktop::text_input::SetAccessibleName(quickNavigation_->searchEdit, accessibleName);
    LOGFONTW font{};
    if (!quickNavigation_->searchFont || GetObjectW(quickNavigation_->searchFont,sizeof(font),&font) != sizeof(font) || font.lfHeight != -QuickNavScale(navigationSettings_.layout.searchFontSize))
    {
        HFONT replacement = CreateFontW(-QuickNavScale(navigationSettings_.layout.searchFontSize),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH | FF_SWISS,snowdesktop::app_fonts::GdiFamily().c_str());
        if (replacement)
        {
            SendMessageW(quickNavigation_->searchEdit,WM_SETFONT,reinterpret_cast<WPARAM>(replacement),FALSE);
            if (quickNavigation_->searchFont) DeleteObject(quickNavigation_->searchFont);
            quickNavigation_->searchFont = replacement;
        }
    }
    snowdesktop::text_input::SetCaretHeight(quickNavigation_->searchEdit, static_cast<float>(QuickNavScale(navigationSettings_.layout.searchFontSize + 2)));
    RECT frame = GetQuickNavigationInputRect(quickNavigation_->rect);
    OffsetRect(&frame, -quickNavigation_->hostRect.left, -quickNavigation_->hostRect.top);
    snowdesktop::text_input::SetEmbeddedPose(quickNavigation_->searchEdit, frame, frame,
        true, true);
}

std::wstring DesktopApp::GetQuickNavigationEffectiveSearchText() const
{
    return quickNavigation_->effectiveSearchText;
}

void DesktopApp::RefreshQuickNavigationSearchCompositionText(HWND editHwnd, LPARAM compositionFlags)
{
    if ((compositionFlags & GCS_COMPSTR) == 0)
        return;

    const std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigation_->searchCompositionText = QuickNavigationReadImeCompositionString(editHwnd);
    quickNavigation_->effectiveSearchText = snowdesktop::text_input::DisplayText(editHwnd);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigation_->initialJumpOpen = false;
        ResetQuickNavigationKeyboardTarget();
        quickNavigation_->everythingResultLimit = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
        quickNavigation_->scrollOffset = 0;
        PositionQuickNavigationWindow();
        InvalidateQuickNavigationWindow();
    }
}

void DesktopApp::ClearQuickNavigationSearchCompositionText()
{
    if (quickNavigation_->searchCompositionText.empty())
        return;

    const std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigation_->searchCompositionText.clear();
    quickNavigation_->effectiveSearchText = snowdesktop::text_input::DisplayText(quickNavigation_->searchEdit);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigation_->initialJumpOpen = false;
        ResetQuickNavigationKeyboardTarget();
        quickNavigation_->everythingResultLimit = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
        quickNavigation_->scrollOffset = 0;
        PositionQuickNavigationWindow();
        InvalidateQuickNavigationWindow();
    }
}

/**
 * @brief 刷新快捷导航搜索文本内容（从编辑框读取）
 */
void DesktopApp::RefreshQuickNavigationSearchText()
{
    CancelQuickNavigationTabHover();
    ResetQuickNavigationKeyboardTarget();
    quickNavigation_->initialJumpOpen = false;
    std::wstring previousQuery = GetQuickNavigationEffectiveSearchText();
    quickNavigation_->searchText.clear();
    quickNavigation_->effectiveSearchText = snowdesktop::text_input::DisplayText(quickNavigation_->searchEdit);
    if (!quickNavigation_->searchEdit || !IsWindow(quickNavigation_->searchEdit))
    {
        if (!previousQuery.empty())
            ClearQuickNavigationEverythingResults();
        return;
    }
    int len = GetWindowTextLengthW(quickNavigation_->searchEdit);
    if (len <= 0)
    {
        if (GetQuickNavigationEffectiveSearchText() != previousQuery)
        {
            quickNavigation_->everythingResultLimit = kQuickNavigationEverythingResultBatchSize;
            RefreshQuickNavigationEverythingResults();
        }
        PositionQuickNavigationWindow();
        return;
    }
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(quickNavigation_->searchEdit, buffer.data(), len + 1);
    buffer.resize(static_cast<size_t>(len));
    quickNavigation_->searchText = std::move(buffer);
    if (GetQuickNavigationEffectiveSearchText() != previousQuery)
    {
        quickNavigation_->everythingResultLimit = kQuickNavigationEverythingResultBatchSize;
        RefreshQuickNavigationEverythingResults();
    }
    PositionQuickNavigationWindow();
}

void DesktopApp::ClearQuickNavigationEverythingResults()
{
    ++quickNavigation_->everythingSearchGeneration;
    if (quickNavigation_->hwnd &&
        IsWindow(quickNavigation_->hwnd))
    {
        KillTimer(
            quickNavigation_->hwnd,
            kQuickNavigationEverythingSearchTimerId);
    }
    quickNavigation_->appResultIndices.clear();
    quickNavigation_->appsExpanded = false;
    quickNavigation_->everythingResults.clear();
    quickNavigation_->everythingIconRows.Clear();
    quickNavigation_->everythingHasMore = false;
    quickNavigation_->everythingSearchPending = false;
    quickNavigation_->everythingResultsQuery.clear();
    quickNavigation_->everythingResultLimit = kQuickNavigationEverythingResultBatchSize;
}

int DesktopApp::GetQuickNavigationEverythingIconIndex(
    const std::wstring& path, bool isDirectory, std::wstring* outKey)
{
    auto extension = ToUpperInvariant(PathFindExtensionW(path.c_str()));
    if (extension.empty()) extension = L"<FILE>";
    const bool perFile = !isDirectory && (extension == L".EXE" || extension == L".LNK" ||
        extension == L".DLL" || extension == L".ICO" || extension == L".SCR" ||
        extension == L".MSI" || extension == L".CPL");
    const auto key = isDirectory ? std::wstring(L"<DIR>") : perFile ? ToUpperInvariant(path) : extension;
    if (outKey) *outKey = key;
    if (const auto* cached = quickNavigation_->everythingIconCache.Find(key)) return *cached;
    shellVisualWork_.Submit(L"everything:" + key, [path, extension, perFile, isDirectory] {
        SHFILEINFOW info{};
        DWORD_PTR loaded = 0;
        if (perFile) loaded = SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (!loaded) loaded = SHGetFileInfoW(isDirectory ? L"folder" : extension.c_str(),
            isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        return loaded ? info.iIcon : -1;
    }, [this, key](int index) {
        quickNavigation_->everythingIconCache.Insert(key, index);
        // Metadata can serve a later query, but row numbers belong only to the
        // current result vector. No callback re-queries unrelated icons.
        bool changed = false;
        quickNavigation_->everythingIconRows.Visit(key, quickNavigation_->everythingResults.size(),
            [&](std::size_t row) {
                auto& entry = quickNavigation_->everythingResults[row];
                if (entry.iconCacheKey != key || entry.systemIconIndex == index) return;
                entry.systemIconIndex = index;
                changed = true;
            });
        if (changed ||
            (navigationSettings_.favoritesEnabled &&
                quickNavigation_->open))
            InvalidateQuickNavigationWindow();
    }, hwnd_, kBackgroundShellReadyMessage);
    return -1;
}

void DesktopApp::RefreshQuickNavigationEverythingResults()
{
    quickNavigation_->appResultIndices.clear();
    quickNavigation_->appsExpanded = false;
    RefreshQuickNavigationTypedResults();
    if (quickNavigation_->searchType != QuickNavigationSearchType::All && quickNavigation_->searchType != QuickNavigationSearchType::File)
    {
        ClearQuickNavigationEverythingResults();
        RefreshQuickNavigationAppResults();
        return;
    }
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
        ++quickNavigation_->everythingSearchGeneration;
        quickNavigation_->everythingResults.clear();
        quickNavigation_->everythingIconRows.Clear();
        quickNavigation_->everythingHasMore = false;
        quickNavigation_->everythingSearchPending = false;
        quickNavigation_->everythingResultsQuery.clear();
        return;
    }

    const DWORD requestLimit = std::max<DWORD>(
        quickNavigation_->everythingResultLimit,
        kQuickNavigationEverythingResultBatchSize);
    quickNavigation_->everythingResultLimit = requestLimit;
    const bool preserveLoadedOrder =
        query == quickNavigation_->everythingResultsQuery &&
        requestLimit > kQuickNavigationEverythingResultBatchSize &&
        !quickNavigation_->everythingResults.empty();
    if (!preserveLoadedOrder)
    {
        quickNavigation_->everythingResults.clear();
        quickNavigation_->everythingIconRows.Clear();
    }
    quickNavigation_->everythingHasMore = false;
    quickNavigation_->everythingSearchPending = true;
    quickNavigation_->everythingSearchAvailable = true;
    quickNavigation_->everythingResultsQuery = query;
    ++quickNavigation_->everythingSearchGeneration;

    if (quickNavigation_->hwnd &&
        IsWindow(quickNavigation_->hwnd) &&
        SetTimer(
            quickNavigation_->hwnd,
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
    if (quickNavigation_->hwnd &&
        IsWindow(quickNavigation_->hwnd))
    {
        KillTimer(
            quickNavigation_->hwnd,
            kQuickNavigationEverythingSearchTimerId);
    }

    const std::wstring query =
        GetQuickNavigationEffectiveSearchText();
    if (!quickNavigation_->open || query.empty() ||
        !quickNavigation_->everythingSearchPending)
    {
        return;
    }

    snowdesktop::QuickNavigationEverythingSearchRequest
        request;
    request.generation =
        quickNavigation_->everythingSearchGeneration;
    request.query = query;
    request.maxResults =
        quickNavigation_->everythingResultLimit;
    if (!quickNavigation_->everythingSearch.Submit(
            hwnd_,
            kQuickNavigationEverythingSearchMessage,
            std::move(request)))
    {
        quickNavigation_->everythingSearchPending = false;
        quickNavigation_->everythingSearchAvailable = false;
        if (quickNavigation_->collapsed) PositionQuickNavigationWindow();
        InvalidateQuickNavigationWindow();
    }
}

void DesktopApp::OnQuickNavigationEverythingSearchCompleted(
    WPARAM cookie)
{
    if (cookie !=
        quickNavigation_->everythingSearch.MessageCookie())
    {
        return;
    }

    auto result =
        quickNavigation_->everythingSearch.TakeCompleted();
    if (!result || !quickNavigation_->open ||
        result->generation !=
            quickNavigation_->everythingSearchGeneration ||
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
            quickNavigation_->everythingResultsQuery &&
        result.maxResults >
            kQuickNavigationEverythingResultBatchSize &&
        !quickNavigation_->everythingResults.empty();
    std::vector<QuickNavigationEverythingEntry>
        previousResults;
    if (preserveLoadedOrder)
        previousResults = quickNavigation_->everythingResults;

    quickNavigation_->everythingResults.clear();
    quickNavigation_->everythingIconRows.Clear();
    quickNavigation_->everythingSearchPending = false;
    quickNavigation_->everythingResultsQuery = result.query;
    quickNavigation_->everythingSearchAvailable = result.error == ERROR_SUCCESS;
    quickNavigation_->everythingHasMore =
        quickNavigation_->everythingSearchAvailable &&
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
        entry.systemIconIndex = GetQuickNavigationEverythingIconIndex(
            entry.path, entry.isDirectory, &entry.iconCacheKey);
        quickNavigation_->everythingIconRows.Add(entry.iconCacheKey, quickNavigation_->everythingResults.size());
        quickNavigation_->everythingResults.push_back(std::move(entry));
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

    quickNavigation_->scrollOffset = std::clamp(
        quickNavigation_->scrollOffset, 0,
        GetQuickNavigationMaxScrollOffset(
            quickNavigation_->rect));
    ResetQuickNavigationKeyboardTarget();
    if (quickNavigation_->collapsed) PositionQuickNavigationWindow();
    InvalidateQuickNavigationWindow();
}

/**
 * @brief 定位并显示快捷导航窗口
 */
void DesktopApp::PositionQuickNavigationWindow()
{
    if (!quickNavigation_->hwnd || !IsWindow(quickNavigation_->hwnd))
        return;

    const RECT previousHost = quickNavigation_->hostRect;
    RECT placementBounds{};
    const POINT invocationPoint{
        quickNavigation_->openPoint.x + virtualLeft_,
        quickNavigation_->openPoint.y + virtualTop_};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (GetMonitorInfoW(MonitorFromPoint(invocationPoint, MONITOR_DEFAULTTONEAREST), &monitorInfo))
    {
        placementBounds = monitorInfo.rcMonitor;
        OffsetRect(&placementBounds, -virtualLeft_, -virtualTop_);
    }
    RECT nextRect = GetQuickNavigationRect();
    auto& resize = quickNavigation_->animation.Resize();
    const auto now = static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
    if (!resize.IsAnimating() && quickNavigation_->open && quickNavigation_->collapsed &&
        quickNavigation_->fixedTop && quickNavigation_->animation.IsInteractive() &&
        !snowdesktop::quick_navigation_animation_rules::IsDeformationEffect(
            quickNavigation_->animation.GetEffect()) &&
        !IsRectEmpty(&quickNavigation_->rect) &&
        quickNavigation_->rect.left == nextRect.left && quickNavigation_->rect.top == nextRect.top &&
        quickNavigation_->rect.right == nextRect.right && quickNavigation_->rect.bottom != nextRect.bottom)
    {
        resize.StartResultFollow(
            {quickNavigation_->rect.left, quickNavigation_->rect.top, quickNavigation_->rect.right, quickNavigation_->rect.bottom},
            {nextRect.left, nextRect.top, nextRect.right, nextRect.bottom}, now,
            snowdesktop::animation::RuntimeAnimationsEnabled(), snowdesktop::animation::RuntimeDurationScale());
    }
    if (resize.IsAnimating())
    {
        resize.Retarget({nextRect.left, nextRect.top, nextRect.right, nextRect.bottom}, now);
        const auto frame = resize.Advance(now, snowdesktop::animation::RuntimeAnimationsEnabled());
        nextRect = {frame.left, frame.top, frame.right, frame.bottom};
    }
    const bool geometryChanged = !EqualRect(&quickNavigation_->rect, &nextRect);
    if (geometryChanged && quickNavigation_->animationCompositorDriven)
    {
        // A running native timeline refers to the previous host geometry.
        // Continue from its current progress with the updated content bounds.
        quickNavigation_->animation.Advance(static_cast<std::uint64_t>(
            snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
        StopQuickNavigationAnimationTimeline();
    }
    quickNavigation_->rect = nextRect;
    // The destination header anchors result-height updates during a transition;
    // feeding an intermediate top back into layout would keep moving its target.
    if (!resize.IsAnimating())
        quickNavigation_->anchorTop = quickNavigation_->rect.top;
    quickNavigation_->fixedTop = true;
    RECT anchorRect = MakeRect(
        quickNavigation_->animationAnchorPoint.x - 1,
        quickNavigation_->animationAnchorPoint.y - 1,
        quickNavigation_->animationAnchorPoint.x + 2,
        quickNavigation_->animationAnchorPoint.y + 2);
    if (!IsRectEmpty(&quickNavigation_->animationDockRect))
        UnionRect(&anchorRect, &anchorRect, &quickNavigation_->animationDockRect);
    UnionRect(
        &quickNavigation_->hostRect,
        &quickNavigation_->rect,
        &anchorRect);
    if (resize.IsAnimating())
    {
        // Keep the native host stable while the panel, input and backdrop resize.
        const auto bounds = resize.Envelope();
        const RECT envelope{bounds.left, bounds.top, bounds.right, bounds.bottom};
        UnionRect(&quickNavigation_->hostRect, &quickNavigation_->hostRect, &envelope);
    }
    if (quickNavigation_->animation.GetEffect() ==
        snowdesktop::quick_navigation_animation_rules::Effect::Attraction)
    {
        // Reserve from the same monitor-sized source for every panel size, so
        // a later resize cannot move the native origin for a future Dock warp.
        const auto rect = [](const RECT& r) {
            return snowdesktop::dock_attraction::Rect{static_cast<double>(r.left),
                static_cast<double>(r.top), static_cast<double>(r.right), static_cast<double>(r.bottom)};
        };
        const auto bounds = snowdesktop::dock_attraction::Bounds(
            rect(IsRectEmpty(&placementBounds) ? quickNavigation_->rect : placementBounds),
            rect(quickNavigation_->animationDockRect));
        const RECT envelope{static_cast<LONG>(std::floor(bounds.left)),
            static_cast<LONG>(std::floor(bounds.top)), static_cast<LONG>(std::ceil(bounds.right)),
            static_cast<LONG>(std::ceil(bounds.bottom))};
        UnionRect(&quickNavigation_->hostRect, &quickNavigation_->hostRect, &envelope);
    }
    quickNavigation_->hostRect = QuickNavigationHostAllocation(
        previousHost, quickNavigation_->hostRect, placementBounds);
    const bool hostChanged = !EqualRect(&previousHost, &quickNavigation_->hostRect);
    const int width = std::max<LONG>(
        1,
        quickNavigation_->hostRect.right -
            quickNavigation_->hostRect.left);
    const int height = std::max<LONG>(
        1,
        quickNavigation_->hostRect.bottom -
            quickNavigation_->hostRect.top);

    if (hostChanged || !IsWindowVisible(quickNavigation_->hwnd))
    {
        UpdateQuickNavigationWindowRegion(quickNavigation_->animation.IsAnimating() ||
            !quickNavigation_->animation.GetVisual().visible);
        const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(quickNavigation_->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
            &cornerPreference, sizeof(cornerPreference));
        // Layer policy and SetQuickNavigationTopmost own Z order. Do not
        // reinsert an unchanged popup/backdrop pair on every resize frame.
        SetWindowPos(quickNavigation_->hwnd, nullptr,
            quickNavigation_->hostRect.left + virtualLeft_,
            quickNavigation_->hostRect.top + virtualTop_, width, height,
            SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                SWP_NOREDRAW | SWP_NOCOPYBITS);
    }
    EnsureQuickNavigationSearchEdit();
    UpdateQuickNavigationSearchEditRect();
    if (geometryChanged || hostChanged)
    {
        // Paint the pixels before publishing the new pose and backdrop.
        InvalidateQuickNavigationWindow(true);
    }
    else
        ApplyQuickNavigationAnimationFrame();
    if (quickNavigation_->animation.IsAnimating() || resize.IsAnimating())
        EnsureUiAnimationFrame();
}

void DesktopApp::UpdateQuickNavigationWindowRegion(
    bool expanded)
{
    if (!quickNavigation_->hwnd ||
        !IsWindow(quickNavigation_->hwnd))
        return;
    RECT animationPath{};
    if (expanded)
    {
        animationPath = MakeRect(quickNavigation_->animationAnchorPoint.x - 1,
            quickNavigation_->animationAnchorPoint.y - 1,
            quickNavigation_->animationAnchorPoint.x + 2,
            quickNavigation_->animationAnchorPoint.y + 2);
        animationPath = QuickNavigationRetainedAllocation(animationPath, quickNavigation_->animationDockRect);
        if (quickNavigation_->animation.GetEffect() ==
            snowdesktop::quick_navigation_animation_rules::Effect::Attraction)
        {
            const auto rect = [](const RECT& r) {
                return snowdesktop::dock_attraction::Rect{static_cast<double>(r.left),
                    static_cast<double>(r.top), static_cast<double>(r.right), static_cast<double>(r.bottom)};
            };
            const auto bounds = snowdesktop::dock_attraction::Bounds(
                rect(quickNavigation_->rect), rect(quickNavigation_->animationDockRect));
            const RECT envelope{static_cast<LONG>(std::floor(bounds.left)),
                static_cast<LONG>(std::floor(bounds.top)), static_cast<LONG>(std::ceil(bounds.right)),
                static_cast<LONG>(std::ceil(bounds.bottom))};
            animationPath = QuickNavigationRetainedAllocation(animationPath, envelope);
        }
    }

    // Even while opening/closing, the input fence covers only the visible
    // animation path, not the monitor-sized native coordinate reservation.
    const RECT panelRegionRect = QuickNavigationWindowFence(quickNavigation_->rect,
        animationPath, quickNavigation_->hostRect, expanded);
    // Keep the native region rectangular. The DComp surface owns the rounded
    // alpha edge; applying a binary Win32 rounded region here clips away its
    // partial-coverage pixels and leaves visibly jagged corners at rest.
    if (HRGN panelRegion = CreateRectRgn(
            panelRegionRect.left,
            panelRegionRect.top,
            panelRegionRect.right + 1,
            panelRegionRect.bottom + 1))
    {
        HRGN previousRegion = CreateRectRgn(0, 0, 0, 0);
        const bool unchanged = previousRegion &&
            GetWindowRgn(quickNavigation_->hwnd, previousRegion) != ERROR &&
            EqualRgn(panelRegion, previousRegion);
        if (previousRegion) DeleteObject(previousRegion);
        if (unchanged)
        {
            DeleteObject(panelRegion);
            quickNavigation_->windowRegionExpanded = expanded;
            return;
        }
        if (!SetWindowRgn(
                quickNavigation_->hwnd,
                panelRegion, FALSE))
            DeleteObject(panelRegion);
        else
            quickNavigation_->windowRegionExpanded = expanded;
    }
}

/**
 * @brief 使快捷导航窗口失效并触发重绘
 */
void DesktopApp::InvalidateQuickNavigationWindow(
    bool immediate)
{
    if (quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd))
    {
        InvalidateRect(quickNavigation_->hwnd, nullptr, FALSE);
        // WM_MOUSEMOVE has higher dispatch priority than WM_PAINT. During a
        // fast sweep, a plain invalidation can therefore leave hover one or
        // more items behind the pointer. Hover callers synchronously present
        // each target transition; movement inside the same target is filtered
        // before reaching this function.
        if (immediate &&
            !quickNavigation_->compositionPaintInProgress)
            UpdateWindow(quickNavigation_->hwnd);
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
 * @brief 根据文字测量宽度，更新 quickNavigation_->tabWidths
 */
void DesktopApp::UpdateQuickNavTabWidths()
{
    std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
    const size_t tabCount = ci.size() + 2;
    quickNavigation_->tabWidths.resize(tabCount, QuickNavScale(72));
    quickNavigation_->viewModeSlotWidth = QuickNavScale(60);

    if (!dwriteFactory_ || !quickNavigation_->tabTextFormat)
        return;

    // Rendering and hit testing share the width of the current mode label.
    const std::wstring viewLabel = QuickNavigationViewLabel();
    ComPtr<IDWriteTextLayout> viewLayout;
    if (SUCCEEDED(dwriteFactory_->CreateTextLayout(viewLabel.c_str(),
            static_cast<UINT32>(viewLabel.size()), quickNavigation_->tabTextFormat.Get(),
            10000.f, 10000.f, &viewLayout)) && viewLayout)
    {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(viewLayout->GetMetrics(&metrics)))
            quickNavigation_->viewModeSlotWidth = std::clamp(
                static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + QuickNavScale(30),
                QuickNavScale(60), QuickNavScale(164));
    }

    for (size_t i = 0; i < tabCount; ++i)
    {
        std::wstring label = GetQuickNavTabLabel(i);
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(label.c_str(),
            static_cast<UINT32>(label.size()), quickNavigation_->tabTextFormat.Get(),
            10000.0f, 10000.0f, &layout)) || !layout)
            continue;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        quickNavigation_->tabWidths[i] = static_cast<int>(std::clamp(
            static_cast<LONG>(static_cast<long>(std::ceil(metrics.widthIncludingTrailingWhitespace)) + QuickNavScale(20)),
            static_cast<LONG>(QuickNavScale(56)), static_cast<LONG>(QuickNavScale(200))));
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
    const auto& tw = quickNavigation_->tabWidths;
    if (tw.empty() || dragTab >= tw.size()) return 2;
    RECT overlay = quickNavigation_->rect;
    int srcCenter = GetQuickNavigationTabRect(overlay, dragTab).left
        + tw[dragTab] / 2 + deltaX;
    std::vector<int> tabCenters(tw.size());
    for (size_t i = 2; i < tw.size(); ++i)
    {
        RECT r = GetQuickNavigationTabRect(overlay, i);
        tabCenters[i] = r.left + tw[i] / 2;
    }
    return snowdesktop::quick_navigation_rules::
        TabDragTarget(dragTab, srcCenter, tabCenters);
}

/**
 * @brief 打开快捷导航面板
 */

void DesktopApp::OpenQuickNavigation(
    QuickNavigationInvocationSource source,
    std::optional<POINT> invocationScreenPoint)
{
    // Hotkeys and Dock search share the same boundary as the status bar.
    if (statusBarTaskViewTransition_.Busy(snowdesktop::UiAnimationScheduler::MonotonicMilliseconds())) return;
    if (dragSession_.IsActive() ||
        dragDropController_.IsExternalDragActive())
        return;

    if (!invocationScreenPoint)
    {
        POINT point{lastMousePoint_.x + virtualLeft_, lastMousePoint_.y + virtualTop_};
        snowdesktop::private_pointer::CursorPosition(&point);
        invocationScreenPoint = point;
    }
    if (FullscreenBlocksDockReveal(DockRevealSource::AssociatedSurface,
        MonitorFromPoint(*invocationScreenPoint, MONITOR_DEFAULTTONEAREST))) return;
    if (dockStartMenu_ && dockStartMenu_->DismissThen([this, source, invocationScreenPoint] {
            OpenQuickNavigation(source, invocationScreenPoint);
        })) return;

    const auto watchOutside = [this] {
        const HWND target = quickNavigation_->hwnd;
        if (!target || !IsWindow(target)) return;
        auto press = std::make_shared<snowdesktop::quick_navigation_rules::OutsidePress>(NavigationPointerButtons());
        if (!SetPropW(target, kQuickNavigationOutsidePress, press.get())) return;
        const auto watch = uiAnimationScheduler_.ScheduleInterval(16, [this, target, press](auto token) {
            if (!quickNavigation_->open || target != quickNavigation_->hwnd || !IsWindow(target) ||
                GetPropW(target, kQuickNavigationOutsidePress) != press.get())
            {
                uiAnimationScheduler_.Cancel(token);
                return;
            }
            POINT screen{};
            if (!snowdesktop::private_pointer::CursorPosition(&screen)) return;
            const POINT point{screen.x - virtualLeft_, screen.y - virtualTop_};
            const auto* dock = GetDockContainerAtPoint(point);
            const HWND foreground = snowdesktop::private_pointer::ForegroundWindow();
            // Activated search keeps its normal deactivation/IME/menu route.
            // This supplement is for a popup whose activation request failed.
            const bool searchForeground = foreground && (foreground == target ||
                foreground == quickNavigation_->searchEdit || IsChild(target, foreground));
            const bool retained = searchForeground || (dock && dock->ContainsInteractivePoint(point)) ||
                (statusBar_ && statusBar_->ContainsPoint(screen)) || HasActiveContextMenuSession() ||
                quickNavigation_->externalDialogActive ||
                dragSession_.IsActive() || dragDropController_.IsTransportActive() ||
                snowdesktop::text_input::HasEditingMenu(quickNavigation_->searchEdit) ||
                (renameController_.IsQuickNavigationPresentation() &&
                    snowdesktop::text_input::HasEditingMenu(renameInputWindow_));
            if (!press->Observe(NavigationPointerButtons(), PtInRect(&quickNavigation_->rect, point) != FALSE, retained)) return;
            if (statusBarActivationMonitor_ && quickNavigation_->postCloseAction)
                CancelStatusBarActivation(statusBarActivationMonitor_);
            CloseQuickNavigation(false);
            uiAnimationScheduler_.Cancel(token);
        });
        if (!watch) RemovePropW(target, kQuickNavigationOutsidePress);
    };

    POINT requestedOpenPoint = lastMousePoint_;
    POINT requestedAnchorPoint = requestedOpenPoint;
    RECT requestedDockRect{};
    float requestedDpiScale = quickNavigation_->dpiScale;
    PersistentDockHost* requestedDockHost = nullptr;
    if (invocationScreenPoint)
    {
        const POINT cursor = *invocationScreenPoint;
        requestedOpenPoint = {
            cursor.x - virtualLeft_,
            cursor.y - virtualTop_
        };
        requestedAnchorPoint = requestedOpenPoint;
        HMONITOR monitor = MonitorFromPoint(
            cursor, MONITOR_DEFAULTTONEAREST);
        UINT dpiX = 96, dpiY = 96;
        if (monitor)
        {
            GetDpiForMonitor(
                monitor, MDT_EFFECTIVE_DPI,
                &dpiX, &dpiY);
        }
        requestedDpiScale =
            static_cast<float>(dpiX) / 96.0f;
    }
    if (source == QuickNavigationInvocationSource::DockSearch)
    {
        if (DockContainer* dock =
                GetDockContainerAtPoint(requestedOpenPoint);
            dock && dock->IsSearchPoint(requestedOpenPoint))
        {
            requestedDockHost =
                FindPersistentDockHost(dock);
            const RECT searchRect = dock->GetElementVisualRect(
                dock->GetSearchRect(), requestedOpenPoint);
            if (!IsRectEmptyRect(searchRect))
            {
                requestedDockRect = searchRect;
                requestedAnchorPoint = {
                    (searchRect.left + searchRect.right) / 2,
                    (searchRect.top + searchRect.bottom) / 2,
                };
            }
        }
    }

    if (source == QuickNavigationInvocationSource::DockSearch && requestedDockHost)
    {
        RECT dockScreen = requestedDockHost->dockRect;
        OffsetRect(&dockScreen, virtualLeft_, virtualTop_);
        KeepDockTopmostForAction(dockScreen);
    }

    if (quickNavigation_->open)
    {
        if (source != QuickNavigationInvocationSource::DockSearch) return;
        RECT screenPanel = quickNavigation_->rect;
        OffsetRect(&screenPanel, virtualLeft_, virtualTop_);
        const POINT screenPoint{requestedOpenPoint.x + virtualLeft_, requestedOpenPoint.y + virtualTop_};
        const bool sameScreen = MonitorFromRect(&screenPanel, MONITOR_DEFAULTTONEAREST) ==
            MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        if (snowdesktop::quick_navigation_rules::ResolveDockSearchPressAction(true, sameScreen, requestedDockHost != nullptr) ==
            snowdesktop::quick_navigation_rules::DockSearchPressAction::Close)
        {
            CloseQuickNavigation();
            return;
        }

        quickNavigation_->animation.Advance(static_cast<std::uint64_t>(
            snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
        StopQuickNavigationAnimationTimeline();
        quickNavigation_->invocationSource = source;
        quickNavigationDockHost_ = requestedDockHost;
        quickNavigation_->openPoint = requestedOpenPoint;
        quickNavigation_->fixedTop = false;
        quickNavigation_->lastMousePoint = requestedOpenPoint;
        quickNavigation_->animationAnchorPoint =
            requestedAnchorPoint;
        quickNavigation_->animationDockRect = requestedDockRect;
        quickNavigation_->animationDockEdge = static_cast<int>(dockSettings_.position);
        ConfigureQuickNavigationAnimation();
        quickNavigation_->dpiScale = requestedDpiScale;
        EnsureQuickNavTextFormats();
        UpdateQuickNavTabWidths();
        PositionQuickNavigationWindow();
        InvalidateQuickNavigationWindow(true);
        if (quickNavigation_->animation.IsAnimating() &&
            !StartQuickNavigationCompositionAnimation())
            EnsureUiAnimationFrame();
        if (quickNavigation_->searchEdit &&
            IsWindow(quickNavigation_->searchEdit))
        {
            (void)FocusKeyboardWindow(
                quickNavigation_->searchEdit, true,
                L"Quick navigation search edit", true);
        }
        ApplyFloatingDockLayerPolicy();
        return;
    }

    quickNavigation_->animation.Advance(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
    const bool reversingClose = quickNavigation_->animation.IsClosing() &&
        quickNavigation_->hwnd && IsWindow(quickNavigation_->hwnd);
    StopQuickNavigationAnimationTimeline();
    quickNavigation_->postCloseAction = {};
    quickNavigation_->hasLastEditAnimationFrame = false;
    quickNavigation_->invocationSource = source;
    quickNavigationDockHost_ =
        source == QuickNavigationInvocationSource::DockSearch
            ? requestedDockHost : nullptr;
    quickNavigation_->openPoint = requestedOpenPoint;
    quickNavigation_->lastMousePoint = requestedOpenPoint;
    quickNavigation_->animationAnchorPoint =
        requestedAnchorPoint;
    quickNavigation_->animationDockRect = requestedDockRect;
    quickNavigation_->animationDockEdge = static_cast<int>(dockSettings_.position);
    if (!reversingClose)
        quickNavigation_->deformationFailed = false;
    ConfigureQuickNavigationAnimation();
    quickNavigation_->dpiScale = requestedDpiScale;
    if (quickNavigation_->searchEdit &&
        IsWindow(quickNavigation_->searchEdit))
    {
        const wchar_t* searchHint = IsLuaLogicalSlotPickerOpen()
            ? _LW("app.nav.slot_picker_search_hint")
            : _LW("app.nav.search_hint");
        SendMessageW(quickNavigation_->searchEdit, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(searchHint));
        snowdesktop::text_input::SetAccessibleName(quickNavigation_->searchEdit,searchHint);
    }
    EnsureQuickNavTextFormats();
    UpdateQuickNavTabWidths();

    if (reversingClose)
    {
        PositionQuickNavigationWindow();
        quickNavigation_->open = true;
        if (quickNavigation_->searchEdit &&
            IsWindow(quickNavigation_->searchEdit))
        {
            EnableWindow(quickNavigation_->searchEdit, TRUE);
            ShowWindow(
                quickNavigation_->searchEdit,
                SW_SHOWNOACTIVATE);
        }
        if (quickNavigation_->animation.GetEffect() !=
            snowdesktop::quick_navigation_animation_rules::Effect::None)
        {
            quickNavigation_->animation.Open(
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
            quickNavigation_->animation.
                ShowImmediately();
        }
        if (!quickNavigation_->animationCompositorDriven)
            ApplyQuickNavigationAnimationFrame();
        if (quickNavigation_->searchEdit &&
            IsWindow(quickNavigation_->searchEdit))
        {
            (void)FocusKeyboardWindow(
                quickNavigation_->searchEdit, true,
                L"Quick navigation search edit", true);
            SendMessageW(
                quickNavigation_->searchEdit,
                EM_SETSEL, 0, -1);
        }
        else
        {
            (void)FocusKeyboardWindow(
                quickNavigation_->hwnd, true,
                L"Quick navigation window", true);
        }
        watchOutside();
        ApplyFloatingDockLayerPolicy();
        return;
    }

    quickNavigation_->open = true;
    CancelQuickNavigationTabHover();
    quickNavigation_->collapsed = navigationSettings_.lastCollapsed && !IsLuaLogicalSlotPickerOpen();
    quickNavigation_->fixedTop = false;
    quickNavigation_->searchType = QuickNavigationSearchType::All;
    quickNavigation_->searchEngine.clear();
    quickNavigation_->menu = QuickNavigationMenu::None;
    quickNavigation_->listSelection = -1;
    quickNavigation_->actionNotice.clear();
    EnsureNavTabOrder();
    if (quickNavigation_->activeWidgetIndex == static_cast<size_t>(-2))
    {
        // keep "映射文件夹全部" tab selection
    }
    else if (quickNavigation_->activeWidgetIndex >= widgets_.size() ||
        (widgets_[quickNavigation_->activeWidgetIndex].type != DesktopWidgetType::Collection &&
         widgets_[quickNavigation_->activeWidgetIndex].type != DesktopWidgetType::FileCategories &&
         widgets_[quickNavigation_->activeWidgetIndex].type != DesktopWidgetType::FolderMapping))
    {
        quickNavigation_->activeWidgetIndex = static_cast<size_t>(-1);
    }
    quickNavigation_->scrollOffset = 0;
    quickNavigation_->tabScrollOffset = 0;
    quickNavigation_->initialJumpOpen = false;
    ResetQuickNavigationKeyboardTarget();
    quickNavigation_->searchText.clear();
    quickNavigation_->effectiveSearchText.clear();
    quickNavigation_->searchCompositionText.clear();
    ClearQuickNavigationEverythingResults();
    StartQuickNavigationAppIndexing();
    quickNavigation_->animation.ResetHidden();
    if (!CreateQuickNavigationWindow())
    {
        quickNavigation_->open = false;
        quickNavigationDockHost_ = nullptr;
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const RECT contentRectBeforePosition = quickNavigation_->rect;
    PositionQuickNavigationWindow();
    const bool searchNeedsClearing = quickNavigation_->searchEdit &&
        GetWindowTextLengthW(quickNavigation_->searchEdit) != 0;
    if (searchNeedsClearing)
        SetWindowTextW(quickNavigation_->searchEdit, L"");
    // Positioning deliberately leaves every quick-navigation visual at zero
    // opacity. Build and present the first content surface while it is still
    // hidden; attaching an empty DComp surface after the open animation has
    // started otherwise exposes a transparent frame when opened from the
    // floating Dock.
    // Positioning already paints synchronously when the content bounds change.
    // Reuse that first surface unless the edit changed its contents, positioning
    // did not paint, or the first draw requested graphics recovery.
    if (searchNeedsClearing ||
        EqualRect(&contentRectBeforePosition, &quickNavigation_->rect) ||
        quickNavigation_->compositionRenderRecoveryPending)
        InvalidateQuickNavigationWindow(true);
    if (quickNavigation_->searchEdit &&
        IsWindow(quickNavigation_->searchEdit))
    {
        EnableWindow(quickNavigation_->searchEdit, TRUE);
        SetLayeredWindowAttributes(
            quickNavigation_->searchEdit,
            0, 0, LWA_ALPHA);
        ShowWindow(
            quickNavigation_->searchEdit,
            SW_SHOWNOACTIVATE);
    }
    if (quickNavigation_->animation.GetEffect() !=
        snowdesktop::quick_navigation_animation_rules::Effect::None)
    {
        quickNavigation_->animation.Open(
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
        quickNavigation_->animation.
            ShowImmediately();
    }
    if (!quickNavigation_->animationCompositorDriven)
        ApplyQuickNavigationAnimationFrame();
    if (quickNavigation_->searchEdit && IsWindow(quickNavigation_->searchEdit))
    {
        (void)FocusKeyboardWindow(
            quickNavigation_->searchEdit, true,
            L"Quick navigation search edit", true);
        SendMessageW(quickNavigation_->searchEdit, EM_SETSEL, 0, -1);
    }
    else
    {
        (void)FocusKeyboardWindow(
            quickNavigation_->hwnd, true,
            L"Quick navigation window", true);
    }
    watchOutside();
    ApplyFloatingDockLayerPolicy();
}

/**
 * @brief 关闭快捷导航面板
 */
void DesktopApp::CloseQuickNavigation(bool restoreDesktopFocus)
{
    quickNavigation_->externalDialogActive = false;
    if (quickNavigation_->hwnd) RemovePropW(quickNavigation_->hwnd, kQuickNavigationOutsidePress);
    const HWND captured = snowdesktop::private_pointer::Capture();
    if (captured && (captured == quickNavigation_->hwnd || captured == quickNavigation_->searchEdit ||
        (renameController_.IsQuickNavigationPresentation() && captured == renameInputWindow_)))
        ReleaseCapturePreservingPointerState();
    CancelQuickNavigationTabHover();
    if (!quickNavigation_->open) return;
    if (!quickNavigation_->postCloseAction)
        quickNavigation_->slotPickerRequest = {};
    if (renameController_.
            IsQuickNavigationPresentation() &&
        renameInputWindow_ && IsWindow(renameInputWindow_))
        CommitRename(false);
    quickNavigation_->animation.Advance(static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()));
    StopQuickNavigationAnimationTimeline();
    ConfigureQuickNavigationAnimation();
    bool animationAnchorChanged = false;
    const bool dockAnchorValid = quickNavigationDockHost_ &&
        quickNavigationDockHost_->active && quickNavigationDockHost_->container &&
        !IsRectEmptyRect(quickNavigationDockHost_->container->GetSearchRect()) &&
        generalSettings_.dockEnabled;
    if (dockAnchorValid)
    {
        const auto* dock = quickNavigationDockHost_->container;
        const RECT anchor = dock->GetElementVisualRect(dock->GetSearchRect(), lastMousePoint_);
        if (!IsRectEmpty(&anchor) && !EqualRect(&anchor, &quickNavigation_->animationDockRect))
        {
            quickNavigation_->animationDockRect = anchor;
            quickNavigation_->animationAnchorPoint = {
                (anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
            quickNavigation_->animationDockEdge = static_cast<int>(dockSettings_.position);
            animationAnchorChanged = true;
        }
    }
    POINT cursor{};
    if (!dockAnchorValid &&
        snowdesktop::private_pointer::CursorPosition(&cursor))
    {
        quickNavigation_->animationDockRect = {};
        const POINT currentPointer = {
            cursor.x - virtualLeft_,
            cursor.y - virtualTop_
        };
        if (currentPointer.x !=
                quickNavigation_->animationAnchorPoint.x ||
            currentPointer.y !=
                quickNavigation_->animationAnchorPoint.y)
        {
            quickNavigation_->animationAnchorPoint =
                currentPointer;
            animationAnchorChanged = true;
        }
    }
    if (animationAnchorChanged &&
        quickNavigation_->hwnd &&
        IsWindow(quickNavigation_->hwnd))
    {
        // Pointer-mode close animations collapse toward the current pointer.
        // Recompute the compact host so that new anchor remains inside it.
        PositionQuickNavigationWindow();
    }
    quickNavigation_->open = false;
    ResetQuickNavigationKeyboardTarget();
    quickNavigation_->tabDragIndex = static_cast<size_t>(-1);
    quickNavigation_->tabDragDeltaX = 0;
    quickNavigation_->tabDragging = false;
    if (quickNavigation_->searchEdit &&
        IsWindow(quickNavigation_->searchEdit))
        EnableWindow(quickNavigation_->searchEdit, FALSE);
    // Deactivation may run while Windows is still switching to Task View or
    // Notifications. Never activate the desktop from that callback, or during
    // a queued handoff to another surface. Explicit dismissal may restore it
    // only while this search surface still owns the foreground.
    const HWND foreground = snowdesktop::private_pointer::ForegroundWindow();
    const bool searchHasForeground = foreground &&
        (foreground == quickNavigation_->hwnd ||
            foreground == quickNavigation_->searchEdit ||
            (quickNavigation_->hwnd && IsChild(quickNavigation_->hwnd, foreground)));
    if (snowdesktop::quick_navigation_rules::ShouldRestoreDesktopFocusOnClose(
            restoreDesktopFocus, customDesktopVisible_,
            static_cast<bool>(quickNavigation_->postCloseAction), searchHasForeground))
        FocusDesktopInputWindow();

    if (!quickNavigation_->hwnd ||
        !IsWindow(quickNavigation_->hwnd))
    {
        FinalizeCloseQuickNavigation();
        return;
    }

    if (quickNavigation_->animation.GetEffect() !=
        snowdesktop::quick_navigation_animation_rules::Effect::None)
    {
        quickNavigation_->animation.Close(
            static_cast<std::uint64_t>(
                snowdesktop::UiAnimationScheduler::
                    MonotonicMilliseconds()));
    }
    else
    {
        quickNavigation_->animation.ResetHidden();
    }
    if (quickNavigation_->animation.IsHidden())
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
    if (!quickNavigation_->open)
    {
        if (action)
            action();
        return;
    }
    quickNavigation_->postCloseAction =
        std::move(action);
    CloseQuickNavigation();
}

void DesktopApp::ApplyQuickNavigationAnimationFrame()
{
    if (quickNavigation_->animationCompositorDriven)
        return;

    auto visual =
        quickNavigation_->animation.GetVisual();
    const float anchorX = static_cast<float>(
        quickNavigation_->animationAnchorPoint.x -
        quickNavigation_->hostRect.left);
    const float anchorY = static_cast<float>(
        quickNavigation_->animationAnchorPoint.y -
        quickNavigation_->hostRect.top);

    namespace rules = snowdesktop::quick_navigation_animation_rules;
    const auto effect = quickNavigation_->animation.GetEffect();
    bool deformationFrame = quickNavigation_->animation.IsAnimating() &&
        rules::IsDeformationEffect(effect);
    const float collapsed = 1.0f - rules::EaseInOutSmooth(visual.progress);
    if (deformationFrame && !(effect == rules::Effect::Attraction
            ? ApplyQuickNavigationAttractionFrame(collapsed, visual.opacity)
            : ApplyQuickNavigationGenieFrame(collapsed, visual.opacity)))
    {
        quickNavigation_->deformationFailed = true;
        ConfigureQuickNavigationAnimation();
        visual = quickNavigation_->animation.GetVisual();
        deformationFrame = false;
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Navigation, L"QuickNav deformation unavailable; using scale for this session", DiagnosticLogLevel::Warning);
    }
    if (!deformationFrame)
    {
        ClearQuickNavigationDeformation();
        quickNavigation_->backdropCompositor.SetVisualTransform(
            visual.scale, visual.opacity, anchorX, anchorY);
    }
    quickNavigation_->backdropCompositor.SetVisible(
        visual.visible);
    UpdateQuickNavigationWindowRegion(
        quickNavigation_->animation.IsAnimating() ||
            !visual.visible);

    if (quickNavigation_->dcompVisual && !deformationFrame)
    {
        if (quickNavigation_->hwnd &&
            IsWindow(quickNavigation_->hwnd))
        {
            quickNavigation_->dcompVisual->SetOffsetX(
                static_cast<float>(
                    quickNavigation_->rect.left -
                    quickNavigation_->hostRect.left));
            quickNavigation_->dcompVisual->SetOffsetY(
                static_cast<float>(
                    quickNavigation_->rect.top -
                    quickNavigation_->hostRect.top));
            if (quickNavigation_->dcompScaleTransform)
            {
                quickNavigation_->dcompScaleTransform->SetCenterX(
                    static_cast<float>(
                        quickNavigation_->animationAnchorPoint.x -
                        quickNavigation_->rect.left));
                quickNavigation_->dcompScaleTransform->SetCenterY(
                    static_cast<float>(
                        quickNavigation_->animationAnchorPoint.y -
                        quickNavigation_->rect.top));
                quickNavigation_->dcompScaleTransform->SetScaleX(
                    visual.scale);
                quickNavigation_->dcompScaleTransform->SetScaleY(
                    visual.scale);
            }
            if (quickNavigation_->dcompEffect)
            {
                quickNavigation_->dcompEffect->SetOpacity(
                    visual.opacity);
            }
            if (quickNavigation_->dcompDevice)
                CommitQuickNavigationCompositionFrame();
        }
    }

    // Paint publishes content first, then the matching backdrop in one place.
    // Visibility-only frames have no pending pixel update.
    if (!quickNavigation_->compositionPaintInProgress)
        quickNavigation_->backdropCompositor.CommitVisualChanges();

    if (quickNavigation_->searchEdit && IsWindow(quickNavigation_->searchEdit))
    {
        UpdateQuickNavigationSearchEditRect();
        if (!visual.visible) ShowWindow(quickNavigation_->searchEdit, SW_HIDE);
    }
}

void DesktopApp::FinalizeCloseQuickNavigation()
{
    std::function<void()> postCloseAction =
        std::move(quickNavigation_->postCloseAction);
    quickNavigation_->postCloseAction = {};
    quickNavigation_->lastMousePoint = {
        LONG_MIN, LONG_MIN };
    quickNavigation_->hasLastEditAnimationFrame = false;
    if (quickNavigation_->searchEdit &&
        IsWindow(quickNavigation_->searchEdit))
    {
        ShowWindow(
            quickNavigation_->searchEdit,
            SW_HIDE);
    }
    if (quickNavigation_->hwnd &&
        IsWindow(quickNavigation_->hwnd))
    {
        ShowWindow(
            quickNavigation_->hwnd,
            SW_HIDE);
    }

    quickNavigation_->scrollOffset = 0;
    quickNavigation_->tabScrollOffset = 0;
    quickNavigation_->initialJumpOpen = false;
    quickNavigation_->searchText.clear();
    quickNavigation_->effectiveSearchText.clear();
    quickNavigation_->searchCompositionText.clear();
    ClearQuickNavigationEverythingResults();
    quickNavigation_->rect = {};
    quickNavigation_->hostRect = {};
    quickNavigation_->animation.ResetHidden();
    DestroyQuickNavigationWindow();
    quickNavigation_->invocationSource =
        QuickNavigationInvocationSource::Pointer;
    quickNavigationDockHost_ = nullptr;
    quickNavigation_->slotPickerRequest = {};
    if (postCloseAction)
        postCloseAction();
}
