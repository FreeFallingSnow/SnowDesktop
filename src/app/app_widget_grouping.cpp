#include "app.h"
#include "../widgets/collection_group_rules.h"
#include "../steam_app_identity.h"

#include <commctrl.h>
#include <objbase.h>

#include <set>
#include <thread>

// Widget creation, collection/file-group membership and release operations.

namespace
{
const wchar_t* WidgetPermissionLabel(std::string_view permission)
{
    if (const char* key = snowdesktop::widget::
            WidgetPermissionLabelLocalizationKey(permission))
        return _LW(key);
    static thread_local std::wstring fallback;
    fallback = Utf8ToWide(std::string(permission));
    return fallback.c_str();
}

enum class WidgetConsentChoice
{
    AllowAll,
    AllowRequired,
    Deny,
    Cancel,
};

struct WidgetConsentDialogPresentation
{
    std::wstring title;
    std::wstring instruction;
    std::wstring content;
    std::wstring allowAllLabel;
    std::wstring allowRequiredLabel;
    std::wstring denyLabel;
    bool showRequiredOnly = false;
};

WidgetConsentDialogPresentation BuildWidgetConsentDialogPresentation(
    const snowdesktop::widget::InstalledPackage& package,
    const std::wstring& displayName,
    std::span<const std::string> requiredPermissions,
    std::span<const std::string> optionalPermissions)
{
    WidgetConsentDialogPresentation presentation;
    presentation.title = _LW("app.widget_permission.title");
    presentation.instruction = _LFW(
        "app.widget_permission.instruction", displayName);
    presentation.allowAllLabel = _LW("app.widget_permission.allow");
    presentation.allowRequiredLabel =
        _LW("app.widget_permission.allow_required");
    presentation.denyLabel = _LW("app.widget_permission.deny");
    presentation.showRequiredOnly = !optionalPermissions.empty();
    std::wstring content;
    if (package.builtin)
        content = _LW("app.widget_permission.source_builtin");
    else if (package.development)
        content = _LW("app.widget_permission.source_development");
    else
        content = _LFW("app.widget_permission.source_external",
            Utf8ToWide(package.source.providerId));
    content += L"\n\n";
    if (!requiredPermissions.empty())
    {
        content += _LW("app.widget_permission.permissions_required_heading");
        for (const auto& permission : requiredPermissions)
        {
            content += L"\n  • ";
            content += WidgetPermissionLabel(permission);
        }
    }
    if (!optionalPermissions.empty())
    {
        if (!requiredPermissions.empty()) content += L"\n\n";
        content += _LW("app.widget_permission.permissions_optional_heading");
        for (const auto& permission : optionalPermissions)
        {
            content += L"\n  • ";
            content += WidgetPermissionLabel(permission);
        }
    }
    if (!package.manifest.networkDomains.empty())
    {
        content += L"\n\n";
        content += _LW("app.widget_permission.domains_heading");
        for (const auto& domain : package.manifest.networkDomains)
        {
            content += L"\n  • ";
            content += Utf8ToWide(domain);
        }
    }
    content += L"\n\n";
    content += _LW("app.widget_permission.runtime_notice");
    presentation.content = std::move(content);
    return presentation;
}

WidgetConsentChoice ShowWidgetConsentDialog(
    const WidgetConsentDialogPresentation& presentation,
    HWND resultWindow, std::uint64_t sessionId,
    POINT anchorScreenPoint)
{
    struct CallbackContext
    {
        HWND resultWindow = nullptr;
        std::uint64_t sessionId = 0;
        POINT anchorScreenPoint{};
    } callbackContext{ resultWindow, sessionId, anchorScreenPoint };
    const auto callback = +[](HWND dialogWindow, UINT notification,
        WPARAM, LPARAM, LONG_PTR rawContext) -> HRESULT {
        if (notification != TDN_CREATED) return S_OK;
        const auto* context = reinterpret_cast<const CallbackContext*>(
            rawContext);
        if (context && context->resultWindow)
        {
            PostMessageW(context->resultWindow,
                kWidgetConsentOpenedMessage,
                static_cast<WPARAM>(context->sessionId),
                reinterpret_cast<LPARAM>(dialogWindow));
        }

        // The consent dialog intentionally has no owner so its modal loop
        // cannot disable the desktop UI thread. An ownerless dialog otherwise
        // defaults to the primary monitor and SetForegroundWindow may be
        // rejected for this worker thread. Place it on the monitor where the
        // user clicked and retain topmost until the user closes it.
        int x = 0;
        int y = 0;
        UINT positionFlags = SWP_NOSIZE | SWP_SHOWWINDOW;
        RECT dialogRect{};
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        const HMONITOR monitor = MonitorFromPoint(
            context ? context->anchorScreenPoint : POINT{},
            MONITOR_DEFAULTTONEAREST);
        if (GetWindowRect(dialogWindow, &dialogRect) && monitor &&
            GetMonitorInfoW(monitor, &monitorInfo))
        {
            const auto position = snowdesktop::widget_runtime::
                CenterConsentDialogInWorkArea(
                    monitorInfo.rcWork.left, monitorInfo.rcWork.top,
                    monitorInfo.rcWork.right, monitorInfo.rcWork.bottom,
                    dialogRect.right - dialogRect.left,
                    dialogRect.bottom - dialogRect.top);
            x = position.x;
            y = position.y;
        }
        else
        {
            positionFlags |= SWP_NOMOVE;
        }
        ShowWindow(dialogWindow, SW_SHOWNORMAL);
        SetWindowPos(dialogWindow, HWND_TOPMOST, x, y, 0, 0,
            positionFlags);
        SetForegroundWindow(dialogWindow);
        return S_OK;
    };
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.push_back({ 100, presentation.allowAllLabel.c_str() });
    if (presentation.showRequiredOnly)
        buttons.push_back({ 102,
            presentation.allowRequiredLabel.c_str() });
    buttons.push_back({ 101, presentation.denyLabel.c_str() });
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
        TDF_SIZE_TO_CONTENT | TDF_USE_COMMAND_LINKS;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = presentation.title.c_str();
    dialog.pszMainIcon = TD_SHIELD_ICON;
    dialog.pszMainInstruction = presentation.instruction.c_str();
    dialog.pszContent = presentation.content.c_str();
    dialog.cButtons = static_cast<UINT>(buttons.size());
    dialog.pButtons = buttons.data();
    dialog.nDefaultButton = 101;
    dialog.pfCallback = callback;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(
        &callbackContext);

    int selected = IDCANCEL;
    if (SUCCEEDED(TaskDialogIndirect(
            &dialog, &selected, nullptr, nullptr)))
    {
        if (selected == 100) return WidgetConsentChoice::AllowAll;
        if (selected == 102) return WidgetConsentChoice::AllowRequired;
        if (selected == 101) return WidgetConsentChoice::Deny;
        return WidgetConsentChoice::Cancel;
    }
    const int fallback = MessageBoxW(nullptr,
        presentation.content.c_str(), presentation.instruction.c_str(),
        MB_ICONWARNING | MB_YESNOCANCEL |
            MB_DEFBUTTON2);
    if (fallback == IDYES) return WidgetConsentChoice::AllowAll;
    if (fallback == IDNO) return WidgetConsentChoice::Deny;
    return WidgetConsentChoice::Cancel;
}

void StartWidgetConsentDialog(HWND resultWindow,
    std::uint64_t sessionId,
    WidgetConsentDialogPresentation presentation,
    POINT anchorScreenPoint)
{
    std::thread([resultWindow, sessionId,
                    presentation = std::move(presentation),
                    anchorScreenPoint]() {
        const HRESULT initialized = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED);
        const WidgetConsentChoice choice =
            ShowWidgetConsentDialog(
                presentation, resultWindow, sessionId,
                anchorScreenPoint);
        PostMessageW(resultWindow, kWidgetConsentResolvedMessage,
            static_cast<WPARAM>(choice),
            static_cast<LPARAM>(sessionId));
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).detach();
}

void ShowWidgetPermissionErrorAsync(
    std::wstring title, std::wstring message)
{
    std::thread([title = std::move(title),
                    message = std::move(message)]() {
        MessageBoxW(nullptr, message.c_str(), title.c_str(),
            MB_OK | MB_ICONERROR);
    }).detach();
}

bool SameStringSet(const std::vector<std::string>& left,
    const std::vector<std::string>& right)
{
    return std::set<std::string>(left.begin(), left.end()) ==
        std::set<std::string>(right.begin(), right.end());
}
}

std::wstring DesktopApp::MakeNewWidgetId() const
{
    return L"widget-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
}

void DesktopApp::CaptureWidgetPackageSource(DesktopWidget& widget) const
{
    if (widget.type != DesktopWidgetType::LuaScript ||
        widget.packageId.empty())
        return;
    const auto source = WidgetEngine::GetWidgetPackageSource(
        widget.packageId);
    if (!source)
    {
        if (widget.packageSourceProvider != L"steam-workshop")
        {
            widget.packageSourceProvider.clear();
            widget.packageSourceExternalItemId.clear();
            widget.packageSourceUrl.clear();
        }
        return;
    }

    const bool hasRememberedWorkshopSource =
        widget.packageSourceProvider == L"steam-workshop" &&
        !snowdesktop::widget::SteamPublishedFileId(
            WideToUtf8(widget.packageSourceExternalItemId)).empty();
    if (hasRememberedWorkshopSource &&
        source->providerId != "steam-workshop")
    {
        // A development override may remain after the subscribed managed copy
        // disappears. Keep the durable Workshop recovery address instead of
        // replacing it with the temporary local source.
        return;
    }

    if (source->providerId != "steam-workshop")
    {
        // Built-in and local packages have no recoverable subscription page;
        // keep their layout records free of provider-specific noise.
        widget.packageSourceProvider.clear();
        widget.packageSourceExternalItemId.clear();
        widget.packageSourceUrl.clear();
        return;
    }

    widget.packageSourceProvider = Utf8ToWide(source->providerId);
    widget.packageSourceExternalItemId = Utf8ToWide(
        source->externalItemId);
    widget.packageSourceUrl.clear();
    if (source->providerId == "steam-workshop")
    {
        const std::string publishedFileId = snowdesktop::widget::
            SteamPublishedFileId(source->externalItemId);
        if (!publishedFileId.empty())
        {
            widget.packageSourceUrl = snowdesktop::
                SnowDesktopSteamCommunityItemUrl(publishedFileId);
        }
    }
}

void DesktopApp::ConfigureWidgetGridLimits(DesktopWidget& widget) const
{
    widget.minGridSpan = { 1, 1 };
    widget.maxGridSpan = { 0, 0 };

    if (widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup ||
        widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping)
    {
        widget.minGridSpan = { 2, 2 };
    }
    else if (widget.type == DesktopWidgetType::LuaScript && !widget.packageId.empty())
    {
        LuaWidgetManifest manifest = WidgetEngine::GetWidgetManifest(widget.packageId);
        widget.minGridSpan = {
            std::max(1, manifest.minColumns),
            std::max(1, manifest.minRows)
        };
        widget.maxGridSpan = {
            std::max(0, manifest.maxColumns),
            std::max(0, manifest.maxRows)
        };
    }
}

GridSpan DesktopApp::ClampWidgetGridSpan(const DesktopWidget& widget, GridSpan span,
    int availableColumns, int availableRows) const
{
    const int pageMaxColumns = std::max(1, availableColumns);
    const int pageMaxRows = std::max(1, availableRows);
    const int maxColumns = widget.maxGridSpan.columns > 0
        ? std::min(pageMaxColumns, widget.maxGridSpan.columns)
        : pageMaxColumns;
    const int maxRows = widget.maxGridSpan.rows > 0
        ? std::min(pageMaxRows, widget.maxGridSpan.rows)
        : pageMaxRows;
    const int minColumns = std::min(
        maxColumns, std::max(1, widget.minGridSpan.columns));
    const int minRows = std::min(
        maxRows, std::max(1, widget.minGridSpan.rows));

    span.columns = std::clamp(span.columns, minColumns, maxColumns);
    span.rows = std::clamp(span.rows, minRows, maxRows);
    return span;
}

/**
 * @brief 将组件添加到网格中，自动查找空闲位置。
 * @param widget 组件对象（移动语义）。
 * @param span 组件跨度。
 */
void DesktopApp::AddWidgetToGrid(DesktopWidget&& widget, GridSpan span)
{
    ConfigureWidgetGridLimits(widget);
    const GridPage* page = GridPageFromScreenPoint(lastContextMenuScreenPoint_);
    GridCell cell;
    if (page)
    {
        cell.pageId = page->id;
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        cell.column = GetGridAxisIndexFromPoint(*page, clientPoint.x, true);
        cell.row = GetGridAxisIndexFromPoint(*page, clientPoint.y, false);
    }
    if (cell.pageId.empty())
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            cell = { firstPage->id, 0, 0 };
        if (cell.pageId.empty()) return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w) &&
            !(w.type == DesktopWidgetType::Guide &&
                w.gridCell.pageId == cell.pageId))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell freeCell;
    const GridPage* cellPage = FindGridPage(gridPages_, cell.pageId);
    if (cellPage)
        span = ClampWidgetGridSpan(widget, span, cellPage->columns, cellPage->rows);
    bool needSearch = AreGridSlotsMarked(usedSlots, cell, span) ||
        !IsGridAreaValid(cell, span);
    if (!needSearch && cellPage)
    {
        if (cell.column + span.columns > cellPage->columns ||
            cell.row + span.rows > cellPage->rows)
            needSearch = true;
    }
    if (needSearch)
    {
        int startSlot = 0;
        const GridPage* searchPage = FindGridPage(gridPages_, cell.pageId);
        if (searchPage)
            startSlot = cell.column * std::max(1, searchPage->rows) + cell.row;
        if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, startSlot))
        {
            if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, 0))
                TryFindFreeCell(span, usedSlots, freeCell, L"", 0);
        }
        if (freeCell.pageId.empty()) return;
        cell = freeCell;
    }

    widget.gridCell = cell;
    widget.gridSpan = span;
    widgets_.push_back(std::move(widget));
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 按组件预览页所表达的尺寸与显示模式创建组件。
 */
void DesktopApp::ApplyWidgetPreviewSettings(POINT screenPoint,
    const snowdesktop::component_preview::ApplySettings& settings)
{
    using snowdesktop::component_preview::ApplyKind;
    if (settings.kind == ApplyKind::None) return;

    lastContextMenuScreenPoint_ = screenPoint;
    const GridSpan span{
        std::clamp(settings.columns, 1, 8),
        std::clamp(settings.rows, 1, 8),
    };
    DesktopWidget widget;
    widget.id = MakeNewWidgetId();
    widget.listMode = settings.listMode;
    widget.scrollContainerMode = settings.scrollContainerMode;
    widget.dateHeaders = settings.dateHeaders;
    widget.showFileCategories = settings.showFileCategories;
    widget.showSearchBox = settings.showSearchBox;

    switch (settings.kind)
    {
    case ApplyKind::Collection:
        widget.type = DesktopWidgetType::Collection;
        widget.title = _LW("widget.collection");
        widget.showTitle = true;
        widget.bottomBarHover = true;
        break;
    case ApplyKind::CollectionGroup:
        widget.type = DesktopWidgetType::CollectionGroup;
        widget.title = _LW("widget.collection_group");
        widget.showTitle = true;
        break;
    case ApplyKind::FileGroup:
        widget.type = DesktopWidgetType::FileGroup;
        widget.title = _LW("widget.file_group");
        widget.showTitle = true;
        break;
    case ApplyKind::FileCategories:
        widget.type = DesktopWidgetType::FileCategories;
        widget.title = _LW("widget.desktop_files");
        widget.showTitle = true;
        break;
    case ApplyKind::FolderMapping:
    {
        std::wstring folderPath;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            dialog->SetTitle(_LW("app.interact.select_folder"));
            if (SUCCEEDED(dialog->Show(hwnd_)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(
                            SIGDN_FILESYSPATH, &path)))
                    {
                        folderPath = path;
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        CoUninitialize();
        if (folderPath.empty()) return;

        std::wstring title = folderPath;
        if (!title.empty() && title.back() == L'\\') title.pop_back();
        const size_t lastSeparator = title.find_last_of(L"\\/");
        if (lastSeparator != std::wstring::npos)
            title = title.substr(lastSeparator + 1);
        widget.type = DesktopWidgetType::FolderMapping;
        widget.title = title;
        widget.showTitle = true;
        widget.sourceFolderPath = folderPath;
        break;
    }
    case ApplyKind::LuaScript:
        if (settings.packageId.empty()) return;
        widget.type = DesktopWidgetType::LuaScript;
        widget.packageId = settings.packageId;
        CaptureWidgetPackageSource(widget);
        widget.title = WidgetEngine::GetWidgetDisplayName(settings.packageId);
        if (widget.title.empty()) widget.title = settings.packageId;
        widget.bottomBarHover = true;
        if (widgetEngine_)
        {
            widgetEngine_->EnsureWidgetLoaded(widget.id, settings.packageId);
            widget.showTitle = widgetEngine_->ReadBoolFlag(
                settings.packageId, "showTitle", false);
            widget.bottomBarHover = widgetEngine_->ReadBoolFlag(
                settings.packageId, "bottomBarHover", true);
        }
        break;
    default:
        return;
    }

    const bool enumerateFolder =
        widget.type == DesktopWidgetType::FolderMapping;
    const size_t oldWidgetCount = widgets_.size();
    AddWidgetToGrid(std::move(widget), span);
    if (enumerateFolder && widgets_.size() > oldWidgetCount)
    {
        EnumerateFolderMappingEntries(widgets_.back());
        RebuildContainersAndItems();
    }
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加集合组件。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddCollectionWidgetAt(POINT screenPoint)
{
    snowdesktop::component_preview::ApplySettings settings;
    settings.kind = snowdesktop::component_preview::ApplyKind::Collection;
    ApplyWidgetPreviewSettings(screenPoint, settings);
}

/**
 * @brief 在右键菜单位置添加集合组组件。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddCollectionGroupWidgetAt(POINT screenPoint)
{
    snowdesktop::component_preview::ApplySettings settings;
    settings.kind =
        snowdesktop::component_preview::ApplyKind::CollectionGroup;
    settings.columns = 3;
    settings.rows = 3;
    ApplyWidgetPreviewSettings(screenPoint, settings);
}

void DesktopApp::AddFileGroupWidgetAt(POINT screenPoint)
{
    snowdesktop::component_preview::ApplySettings settings;
    settings.kind = snowdesktop::component_preview::ApplyKind::FileGroup;
    settings.columns = 3;
    settings.rows = 3;
    settings.showFileCategories = true;
    ApplyWidgetPreviewSettings(screenPoint, settings);
}

size_t DesktopApp::HitTestCollectionGroupIndex(
    POINT point, size_t excludeWidgetIndex) const
{
    for (size_t i = widgets_.size(); i-- > 0;)
    {
        if (i == excludeWidgetIndex ||
            widgets_[i].type != DesktopWidgetType::CollectionGroup)
            continue;
        RECT bounds = widgets_[i].bounds;
        if (!IsRectEmptyRect(bounds) && PtInRect(&bounds, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

size_t DesktopApp::HitTestFileGroupIndex(
    POINT point, size_t excludeWidgetIndex) const
{
    for (size_t i = widgets_.size(); i-- > 0;)
    {
        if (i == excludeWidgetIndex ||
            widgets_[i].type != DesktopWidgetType::FileGroup)
            continue;
        RECT bounds = widgets_[i].bounds;
        if (!IsRectEmptyRect(bounds) && PtInRect(&bounds, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

bool DesktopApp::AddWidgetToFileGroup(
    size_t childIndex, size_t groupIndex, size_t insertIndex)
{
    if (childIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        childIndex == groupIndex ||
        widgets_[groupIndex].type != DesktopWidgetType::FileGroup)
        return false;
    const DesktopWidgetType childType = widgets_[childIndex].type;
    const auto childKind =
        childType == DesktopWidgetType::FileCategories
            ? snowdesktop::collection_group_rules::
                FileGroupChildKind::DesktopFileCategories
            : (childType ==
                    DesktopWidgetType::FolderMapping
                ? snowdesktop::collection_group_rules::
                    FileGroupChildKind::FolderMapping
                : snowdesktop::collection_group_rules::
                    FileGroupChildKind::Collection);
    if (!snowdesktop::collection_group_rules::
            AcceptsFileGroupChild(childKind))
        return false;

    const std::wstring childId = widgets_[childIndex].id;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FileGroup) continue;
        std::erase(widget.childWidgetIds, childId);
        if (widget.activeCategoryId == childId)
            widget.activeCategoryId =
                widget.childWidgetIds.empty()
                    ? L""
                    : widget.childWidgetIds.front();
    }

    DesktopWidget& group = widgets_[groupIndex];
    if (insertIndex == static_cast<size_t>(-1))
        insertIndex = group.childWidgetIds.size();
    insertIndex = std::min(insertIndex, group.childWidgetIds.size());
    group.childWidgetIds.insert(
        group.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(insertIndex),
        childId);
    if (group.activeCategoryId.empty())
        group.activeCategoryId = childId;
    widgets_[childIndex].selected = false;
    group.scrollOffset = 0;
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::MoveFolderMappingsToFileGroup(
    const std::vector<Item*>& sourceItems,
    size_t groupIndex, size_t insertIndex)
{
    if (sourceItems.empty() ||
        groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::FileGroup)
        return false;

    std::vector<std::wstring> movingIds;
    for (Item* source : sourceItems)
    {
        std::wstring id;
        if (auto* dockItem =
                dynamic_cast<DockEntryItem*>(source))
        {
            if (dockItem->GetEntryType() !=
                    DockEntryType::FolderMapping)
                return false;
            id = dockItem->GetReference();
        }
        else if (auto* groupEntry =
                     dynamic_cast<
                         FileGroupEntryItem*>(
                         source))
        {
            id = groupEntry->GetChildWidgetId();
        }
        else if (auto* widget =
                     dynamic_cast<Widget*>(source))
        {
            DesktopWidget* data =
                widget->GetWidgetData();
            if (!data ||
                data->type !=
                    DesktopWidgetType::
                        FolderMapping)
                return false;
            id = data->id;
        }
        else
        {
            return false;
        }

        const size_t childIndex =
            FindWidgetIndexById(id);
        if (childIndex >= widgets_.size() ||
            widgets_[childIndex].type !=
                DesktopWidgetType::FolderMapping)
            return false;
        if (std::find(
                movingIds.begin(),
                movingIds.end(), id) ==
            movingIds.end())
            movingIds.push_back(
                std::move(id));
    }
    if (movingIds.empty())
        return false;

    DesktopWidget& target =
        widgets_[groupIndex];
    const std::wstring previousActive =
        target.activeCategoryId;
    insertIndex = std::min(
        insertIndex,
        target.childWidgetIds.size());
    size_t removedBefore = 0;
    for (const auto& id : movingIds)
    {
        const auto existing =
            std::find(
                target.childWidgetIds.begin(),
                target.childWidgetIds.end(),
                id);
        if (existing !=
                target.childWidgetIds.end() &&
            static_cast<size_t>(
                std::distance(
                    target.childWidgetIds.begin(),
                    existing)) < insertIndex)
            ++removedBefore;
    }

    for (auto& group : widgets_)
    {
        if (group.type !=
                DesktopWidgetType::FileGroup)
            continue;
        for (const auto& id : movingIds)
            std::erase(
                group.childWidgetIds, id);
        group.activeCategoryId =
            snowdesktop::
                collection_group_rules::
                    ResolveActiveItem(
                        group.childWidgetIds,
                        group.activeCategoryId);
    }

    const size_t insertAt =
        std::min(
            insertIndex > removedBefore
                ? insertIndex - removedBefore
                : 0,
            target.childWidgetIds.size());
    target.childWidgetIds.insert(
        target.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(
                insertAt),
        movingIds.begin(), movingIds.end());
    target.activeCategoryId =
        snowdesktop::
            collection_group_rules::
                ResolveActiveItem(
                    target.childWidgetIds,
                    previousActive);

    const std::unordered_set<std::wstring>
        movingSet(
            movingIds.begin(), movingIds.end());
    std::erase_if(
        dockEntries_,
        [&](const DockEntry& entry) {
            return entry.type ==
                    DockEntryType::
                        FolderMapping &&
                movingSet.contains(
                    entry.reference);
        });
    for (const auto& id : movingIds)
    {
        const size_t childIndex =
            FindWidgetIndexById(id);
        if (childIndex < widgets_.size())
            widgets_[childIndex].
                selected = false;
    }

    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::ReleaseWidgetFromFileGroup(
    const std::wstring& childId, GridCell preferredCell)
{
    const size_t childIndex = FindWidgetIndexById(childId);
    const size_t groupIndex = FindFileGroupIndexForChild(childId);
    if (childIndex >= widgets_.size() ||
        groupIndex >= widgets_.size())
        return false;
    const DesktopWidgetType childType = widgets_[childIndex].type;
    if (childType != DesktopWidgetType::FileCategories &&
        childType != DesktopWidgetType::FolderMapping)
        return false;

    DesktopWidget& group = widgets_[groupIndex];
    auto membership = std::find(
        group.childWidgetIds.begin(),
        group.childWidgetIds.end(), childId);
    const size_t membershipIndex =
        membership == group.childWidgetIds.end()
            ? group.childWidgetIds.size()
            : static_cast<size_t>(std::distance(
                group.childWidgetIds.begin(), membership));
    const std::wstring previousActive = group.activeCategoryId;
    std::erase(group.childWidgetIds, childId);
    if (group.activeCategoryId == childId)
        group.activeCategoryId =
            group.childWidgetIds.empty()
                ? L""
                : group.childWidgetIds[
                    std::min(membershipIndex,
                        group.childWidgetIds.size() - 1)];

    DesktopWidget& child = widgets_[childIndex];
    child.selected = false;
    auto restoreMembership = [&]() {
        group.childWidgetIds.insert(
            group.childWidgetIds.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(membershipIndex,
                        group.childWidgetIds.size())),
            childId);
        group.activeCategoryId = previousActive;
    };

    std::unordered_set<std::wstring> usedSlots;
    GridSpan span{};
    const auto landing = FindFileGroupReleaseLanding(
        childIndex, preferredCell, usedSlots, &span);
    if (!landing)
    {
        restoreMembership();
        return false;
    }

    child.gridCell = *landing;
    child.gridSpan = span;
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

std::optional<GridCell> DesktopApp::FindFileGroupReleaseLanding(
    size_t childIndex, GridCell preferredCell,
    std::unordered_set<std::wstring>& usedSlots,
    GridSpan* outSpan) const
{
    if (childIndex >= widgets_.size()) return std::nullopt;
    const DesktopWidget& child = widgets_[childIndex];

    const GridPage* page =
        FindGridPage(gridPages_, preferredCell.pageId);
    if (!page)
    {
        const size_t groupIndex =
            FindFileGroupIndexForChild(child.id);
        if (groupIndex >= widgets_.size())
            return std::nullopt;
        preferredCell = widgets_[groupIndex].gridCell;
        page = FindGridPage(gridPages_, preferredCell.pageId);
    }
    if (!page) return std::nullopt;

    GridSpan span = ClampWidgetGridSpan(
        child, child.gridSpan, page->columns, page->rows);
    preferredCell.column = std::clamp(
        preferredCell.column, 0,
        std::max(0, page->columns - span.columns));
    preferredCell.row = std::clamp(
        preferredCell.row, 0,
        std::max(0, page->rows - span.rows));
    if (outSpan) *outSpan = span;

    if (usedSlots.empty())
    {
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (i == childIndex || IsGroupedWidget(widgets_[i]))
                continue;
            MarkGridArea(
                usedSlots, widgets_[i].gridCell,
                widgets_[i].gridSpan);
        }
        for (const auto& item : items_)
        {
            if (item.name.empty() || IsItemInAnyWidget(item)) continue;
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
        }
    }

    GridCell landing = preferredCell;
    if (!IsGridAreaValid(landing, span) ||
        AreGridSlotsMarked(usedSlots, landing, span))
        return std::nullopt;
    MarkGridArea(usedSlots, landing, span);
    return landing;
}

bool DesktopApp::AddCollectionToGroup(
    size_t collectionIndex, size_t groupIndex, size_t insertIndex)
{
    if (collectionIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        collectionIndex == groupIndex ||
        widgets_[collectionIndex].type != DesktopWidgetType::Collection ||
        widgets_[groupIndex].type != DesktopWidgetType::CollectionGroup)
        return false;

    const std::wstring collectionId = widgets_[collectionIndex].id;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::CollectionGroup) continue;
        std::erase(widget.childWidgetIds, collectionId);
        if (widget.activeCategoryId == collectionId)
            widget.activeCategoryId =
                widget.childWidgetIds.empty()
                    ? L""
                    : widget.childWidgetIds.front();
    }

    DesktopWidget& group = widgets_[groupIndex];
    if (insertIndex == static_cast<size_t>(-1))
        insertIndex = group.childWidgetIds.size();
    insertIndex = std::min(insertIndex, group.childWidgetIds.size());
    group.childWidgetIds.insert(
        group.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(insertIndex),
        collectionId);
    if (group.activeCategoryId.empty())
        group.activeCategoryId = collectionId;

    std::erase_if(dockEntries_, [&](const DockEntry& entry) {
        return entry.type == DockEntryType::Collection &&
            entry.reference == collectionId;
    });
    if (popupWidgetIndex_ == collectionIndex)
        CloseCollectionPopup();
    widgets_[collectionIndex].selected = false;
    group.scrollOffset = std::clamp(
        group.scrollOffset, 0, INT_MAX);
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

bool DesktopApp::ReleaseCollectionFromGroup(
    const std::wstring& collectionId, GridCell preferredCell)
{
    const size_t collectionIndex = FindWidgetIndexById(collectionId);
    const size_t groupIndex =
        FindCollectionGroupIndexForChild(collectionId);
    if (collectionIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        widgets_[collectionIndex].type != DesktopWidgetType::Collection)
        return false;

    DesktopWidget& group = widgets_[groupIndex];
    auto membership = std::find(
        group.childWidgetIds.begin(),
        group.childWidgetIds.end(), collectionId);
    const size_t membershipIndex =
        membership == group.childWidgetIds.end()
            ? group.childWidgetIds.size()
            : static_cast<size_t>(
                std::distance(group.childWidgetIds.begin(), membership));
    const std::wstring previousActiveCategory =
        group.activeCategoryId;
    std::erase(group.childWidgetIds, collectionId);
    if (group.activeCategoryId == collectionId)
        group.activeCategoryId =
            group.childWidgetIds.empty()
                ? L""
                : group.childWidgetIds[
                    std::min(
                        membershipIndex,
                        group.childWidgetIds.size() - 1)];
    DesktopWidget& collection = widgets_[collectionIndex];
    collection.selected = false;

    const GridPage* page =
        FindGridPage(gridPages_, preferredCell.pageId);
    if (!page)
    {
        preferredCell = group.gridCell;
        page = FindGridPage(gridPages_, preferredCell.pageId);
    }
    if (!page)
    {
        group.childWidgetIds.insert(
            group.childWidgetIds.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(membershipIndex,
                        group.childWidgetIds.size())),
            collectionId);
        group.activeCategoryId = previousActiveCategory;
        return false;
    }

    GridSpan span = ClampWidgetGridSpan(collection, collection.gridSpan,
        page->columns, page->rows);
    preferredCell.column = std::clamp(
        preferredCell.column, 0,
        std::max(0, page->columns - span.columns));
    preferredCell.row = std::clamp(
        preferredCell.row, 0,
        std::max(0, page->rows - span.rows));

    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == collectionIndex || IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(usedSlots,
            widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell landing = preferredCell;
    if (!IsGridAreaValid(landing, span) ||
        AreGridSlotsMarked(usedSlots, landing, span))
    {
        const int startSlot =
            SlotFromCell(gridPages_, preferredCell);
        if (!TryFindFreeCell(span, usedSlots, landing,
            preferredCell.pageId, startSlot) &&
            !TryFindFreeCell(span, usedSlots, landing, L"", 0))
        {
            group.childWidgetIds.insert(
                group.childWidgetIds.begin() +
                    static_cast<std::ptrdiff_t>(
                        std::min(membershipIndex,
                            group.childWidgetIds.size())),
                collectionId);
            group.activeCategoryId =
                previousActiveCategory;
            return false;
        }
    }

    collection.gridCell = landing;
    collection.gridSpan = span;
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

void DesktopApp::ReleaseCollectionGroupChildren(size_t groupIndex)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type != DesktopWidgetType::CollectionGroup)
        return;

    DesktopWidget& group = widgets_[groupIndex];
    const std::vector<std::wstring> childIds =
        snowdesktop::collection_group_rules::
            TakeAllForRelease(
                group.childWidgetIds,
                group.activeCategoryId);

    std::unordered_set<std::wstring> childSet(
        childIds.begin(), childIds.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == groupIndex ||
            childSet.contains(widgets_[i].id) ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(usedSlots,
            widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    const std::wstring preferredPage = group.gridCell.pageId;
    int startSlot = SlotFromCell(gridPages_, group.gridCell);
    for (const auto& childId : childIds)
    {
        const size_t childIndex = FindWidgetIndexById(childId);
        if (childIndex >= widgets_.size() ||
            widgets_[childIndex].type != DesktopWidgetType::Collection)
            continue;
        DesktopWidget& child = widgets_[childIndex];
        GridCell landing;
        if (!TryFindFreeCell(child.gridSpan, usedSlots, landing,
            preferredPage, startSlot) &&
            !TryFindFreeCell(child.gridSpan, usedSlots, landing, L"", 0) &&
            !FindDockReturnCell(
                usedSlots, preferredPage, startSlot,
                child.gridSpan, landing))
            continue;
        child.gridCell = landing;
        child.selected = false;
        MarkGridArea(usedSlots, landing, child.gridSpan);
        startSlot = FindGridPage(gridPages_, landing.pageId)
            ? SlotFromCell(gridPages_, landing) + 1
            : 0;
    }
}

void DesktopApp::ReleaseFileGroupChildren(size_t groupIndex)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type != DesktopWidgetType::FileGroup)
        return;

    DesktopWidget& group = widgets_[groupIndex];
    const std::vector<std::wstring> childIds =
        snowdesktop::collection_group_rules::
            TakeAllForRelease(
                group.childWidgetIds,
                group.activeCategoryId);

    std::unordered_set<std::wstring> childSet(
        childIds.begin(), childIds.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == groupIndex ||
            childSet.contains(widgets_[i].id) ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    const std::wstring preferredPage = group.gridCell.pageId;
    int startSlot = SlotFromCell(gridPages_, group.gridCell);
    for (const auto& childId : childIds)
    {
        const size_t childIndex = FindWidgetIndexById(childId);
        if (childIndex >= widgets_.size())
            continue;
        DesktopWidget& child = widgets_[childIndex];
        if (child.type != DesktopWidgetType::FileCategories &&
            child.type != DesktopWidgetType::FolderMapping)
            continue;
        GridCell landing;
        if (!TryFindFreeCell(
                child.gridSpan, usedSlots, landing,
                preferredPage, startSlot) &&
            !TryFindFreeCell(
                child.gridSpan, usedSlots, landing, L"", 0) &&
            !FindDockReturnCell(
                usedSlots, preferredPage, startSlot,
                child.gridSpan, landing))
            continue;
        child.gridCell = landing;
        child.selected = false;
        MarkGridArea(usedSlots, landing, child.gridSpan);
        startSlot = FindGridPage(
                gridPages_, landing.pageId)
            ? SlotFromCell(gridPages_, landing) + 1
            : 0;
    }
}

void DesktopApp::ReleaseDesktopItemsFromWidget(
    size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size())
        return;

    const DesktopWidget& source = widgets_[widgetIndex];
    if (source.type != DesktopWidgetType::Collection &&
        source.type != DesktopWidgetType::FileCategories)
        return;

    std::vector<std::wstring> claimedKeys;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        for (const auto& key : widgets_[i].itemKeys)
        {
            if (!key.empty())
                claimedKeys.push_back(
                    ToUpperInvariant(key));
        }
    }
    for (const auto& entry : dockEntries_)
    {
        if (entry.type ==
                DockEntryType::DesktopItem &&
            !entry.keepOnDesktop &&
            !entry.reference.empty())
        {
            claimedKeys.push_back(
                ToUpperInvariant(entry.reference));
        }
    }

    std::vector<std::wstring> sourceKeys;
    sourceKeys.reserve(source.itemKeys.size());
    for (const auto& key : source.itemKeys)
        sourceKeys.push_back(
            ToUpperInvariant(key));
    const std::vector<std::wstring> releasedKeys =
        snowdesktop::collection_group_rules::
            ClaimUniqueAllowedItems(
                sourceKeys, claimedKeys,
                [](const std::wstring& key) {
                    return !key.empty();
                });
    if (releasedKeys.empty())
        return;

    std::unordered_set<std::wstring> releasedSet(
        releasedKeys.begin(), releasedKeys.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() ||
            releasedSet.contains(
                ToUpperInvariant(item.layoutKey)) ||
            IsItemInAnyWidget(item) ||
            item.gridCell.pageId ==
                kDockPageId)
            continue;
        MarkGridArea(
            usedSlots, item.gridCell,
            item.gridSpan);
    }

    GridCell preferredCell = source.gridCell;
    if (source.type ==
            DesktopWidgetType::Collection)
    {
        const size_t groupIndex =
            FindCollectionGroupIndexForChild(
                source.id);
        if (groupIndex < widgets_.size())
            preferredCell =
                widgets_[groupIndex].gridCell;
    }
    else
    {
        const size_t groupIndex =
            FindFileGroupIndexForChild(source.id);
        if (groupIndex < widgets_.size())
            preferredCell =
                widgets_[groupIndex].gridCell;
    }

    auto isKnownPage =
        [&](const std::wstring& pageId) {
            return !pageId.empty() &&
                pageId != kDockPageId &&
                (FindGridPage(
                     gridPages_, pageId) !=
                     nullptr ||
                 (savedPageColumns_.contains(pageId) &&
                  savedPageRows_.contains(pageId)));
        };
    if (!isKnownPage(preferredCell.pageId))
    {
        const GridPage* firstPage =
            GetFirstPageGridPage();
        if (!firstPage) return;
        preferredCell = {
            firstPage->id, 0, 0
        };
    }

    auto slotForCell =
        [&](const GridCell& cell) {
            int rows = 1;
            if (const GridPage* page =
                    FindGridPage(
                        gridPages_, cell.pageId))
                rows = std::max(1, page->rows);
            else if (const auto it =
                         savedPageRows_.find(
                             cell.pageId);
                     it != savedPageRows_.end())
                rows = std::max(1, it->second);
            return std::max(0, cell.column) *
                    rows +
                std::max(0, cell.row);
        };

    std::wstring preferredPage =
        preferredCell.pageId;
    int startSlot = slotForCell(preferredCell);
    for (const auto& key : releasedKeys)
    {
        const size_t itemIndex =
            FindItemIndexByKey(key);
        if (itemIndex >= items_.size())
            continue;

        DesktopItem& item = items_[itemIndex];
        item.gridSpan.columns =
            std::max(1, item.gridSpan.columns);
        item.gridSpan.rows =
            std::max(1, item.gridSpan.rows);

        GridCell landing;
        if (!FindDockReturnCell(
                usedSlots, preferredPage,
                startSlot, item.gridSpan,
                landing))
            continue;

        item.gridCell = landing;
        item.slot = slotForCell(landing);
        item.selected = false;
        MarkGridArea(
            usedSlots, landing, item.gridSpan);
        preferredPage = landing.pageId;
        startSlot = item.slot + 1;
    }
}

/**
 * @brief 在右键菜单位置添加桌面文件分类组件。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddFileCategoryWidgetAt(POINT screenPoint)
{
    snowdesktop::component_preview::ApplySettings settings;
    settings.kind =
        snowdesktop::component_preview::ApplyKind::FileCategories;
    settings.columns = 3;
    settings.rows = 3;
    settings.showFileCategories = true;
    settings.showSearchBox = true;
    ApplyWidgetPreviewSettings(screenPoint, settings);
}

/**
 * @brief 在右键菜单位置添加文件夹映射组件（弹出文件夹选择对话框）。
 * @param screenPoint 屏幕坐标点。
 */
void DesktopApp::AddFolderMappingWidgetAt(POINT screenPoint)
{
    snowdesktop::component_preview::ApplySettings settings;
    settings.kind =
        snowdesktop::component_preview::ApplyKind::FolderMapping;
    settings.columns = 3;
    settings.rows = 3;
    ApplyWidgetPreviewSettings(screenPoint, settings);
}

/**
 * @brief 在右键菜单位置添加 Lua 脚本组件。
 * @param screenPoint 屏幕坐标点。
 * @param scriptFilename 脚本文件名。
 */
void DesktopApp::AddLuaWidgetAt(POINT screenPoint, const std::wstring& packageId)
{
    if (packageId.empty()) return;
    const auto package = WidgetEngine::GetWidgetPackage(packageId);
    if (!package) return;
    const auto requiredConsentPermissions =
        snowdesktop::widget::PermissionsRequiringConsent(
            package->manifest.permissions);
    const auto optionalConsentPermissions =
        snowdesktop::widget::PermissionsRequiringConsent(
            package->manifest.optionalPermissions);
    const bool alreadyGranted = package->permissionState ==
        snowdesktop::widget::PermissionDecisionState::Granted;
    if (!alreadyGranted &&
        (!requiredConsentPermissions.empty() ||
            !optionalConsentPermissions.empty()))
    {
        if (pendingLuaWidgetConsent_) return;
        int defaultColumns = 1;
        int defaultRows = 1;
        WidgetEngine::GetWidgetDefaultSpan(
            packageId, defaultColumns, defaultRows);
        snowdesktop::component_preview::ApplySettings settings;
        settings.kind =
            snowdesktop::component_preview::ApplyKind::LuaScript;
        settings.packageId = packageId;
        settings.columns = defaultColumns;
        settings.rows = defaultRows;
        const size_t previousCount = widgets_.size();
        ApplyWidgetPreviewSettings(screenPoint, settings);
        if (widgets_.size() <= previousCount) return;
        BeginLuaWidgetConsent(screenPoint, packageId,
            widgets_.back().id);
        return;
    }
    if (!alreadyGranted)
    {
        std::string error;
        const bool applied = widgetEngine_
            ? widgetEngine_->ApplyWidgetPermissionDecision(
                packageId,
                snowdesktop::widget::PermissionDecisionState::Granted,
                snowdesktop::widget::DeclaredPermissions(
                    package->manifest),
                package->manifest.networkDomains,
                error)
            : WidgetEngine::SetWidgetPermissionDecision(
                packageId,
                snowdesktop::widget::PermissionDecisionState::Granted,
                snowdesktop::widget::DeclaredPermissions(
                    package->manifest),
                package->manifest.networkDomains,
                error);
        if (!applied)
        {
            ShowWidgetPermissionErrorAsync(
                _LW("app.widget_permission.title"),
                _LFW("app.widget_permission.save_failed",
                    Utf8ToWide(error)));
            return;
        }
    }
    int defaultColumns = 1;
    int defaultRows = 1;
    WidgetEngine::GetWidgetDefaultSpan(packageId, defaultColumns, defaultRows);
    snowdesktop::component_preview::ApplySettings settings;
    settings.kind = snowdesktop::component_preview::ApplyKind::LuaScript;
    settings.packageId = packageId;
    settings.columns = defaultColumns;
    settings.rows = defaultRows;
    ApplyWidgetPreviewSettings(screenPoint, settings);
}

void DesktopApp::BeginLuaWidgetConsent(POINT screenPoint,
    const std::wstring& packageId,
    const std::wstring& targetWidgetId)
{
    if (targetWidgetId.empty()) return;
    if (pendingLuaWidgetConsent_)
    {
        const HWND dialogWindow =
            pendingLuaWidgetConsent_->dialogWindow;
        constexpr ULONGLONG ConsentOpeningTimeoutMs = 3000;
        const ULONGLONG elapsed = GetTickCount64() -
            pendingLuaWidgetConsent_->startedAt;
        const auto sessionAction = snowdesktop::widget_runtime::
            ConsentSessionActionFor(true, dialogWindow != nullptr,
                dialogWindow && IsWindow(dialogWindow), elapsed,
                ConsentOpeningTimeoutMs);
        if (sessionAction == snowdesktop::widget_runtime::
                WidgetConsentSessionAction::ActivateWindow)
        {
            ShowWindow(dialogWindow, SW_SHOWNORMAL);
            SetWindowPos(dialogWindow, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(dialogWindow);
            return;
        }
        if (sessionAction == snowdesktop::widget_runtime::
                WidgetConsentSessionAction::WaitForWindow)
            return;

        // The worker failed to publish a live window or a former dialog was
        // destroyed without delivering its result. Retire that session so an
        // explicit retry click cannot be swallowed indefinitely.
        pendingLuaWidgetConsent_.reset();
    }
    const auto package = WidgetEngine::GetWidgetPackage(packageId);
    if (!package) return;
    const auto requiredConsentPermissions =
        snowdesktop::widget::PermissionsRequiringConsent(
            package->manifest.permissions);
    const auto optionalConsentPermissions =
        snowdesktop::widget::PermissionsRequiringConsent(
            package->manifest.optionalPermissions);
    if (requiredConsentPermissions.empty() &&
        optionalConsentPermissions.empty()) return;

    const std::wstring displayName =
        WidgetEngine::GetWidgetDisplayName(packageId);
    WidgetConsentDialogPresentation presentation =
        BuildWidgetConsentDialogPresentation(*package,
            displayName.empty() ? packageId : displayName,
            requiredConsentPermissions,
            optionalConsentPermissions);
    std::uint64_t sessionId = ++nextLuaWidgetConsentSessionId_;
    if (sessionId == 0)
        sessionId = ++nextLuaWidgetConsentSessionId_;
    pendingLuaWidgetConsent_ = PendingLuaWidgetConsent{
        sessionId, screenPoint, packageId, targetWidgetId,
        package->source, package->manifest.permissions,
        package->manifest.optionalPermissions,
        package->manifest.networkDomains, GetTickCount64(), nullptr };
    StartWidgetConsentDialog(
        hwnd_, sessionId, std::move(presentation), screenPoint);
}

void DesktopApp::NotifyLuaWidgetConsentDialogOpened(
    WPARAM rawSessionId, LPARAM rawDialogWindow)
{
    const std::uint64_t sessionId =
        static_cast<std::uint64_t>(rawSessionId);
    const HWND dialogWindow = reinterpret_cast<HWND>(rawDialogWindow);
    if (!pendingLuaWidgetConsent_ ||
        pendingLuaWidgetConsent_->sessionId != sessionId ||
        !dialogWindow || !IsWindow(dialogWindow))
        return;
    pendingLuaWidgetConsent_->dialogWindow = dialogWindow;
}

void DesktopApp::CompleteLuaWidgetConsent(
    WPARAM rawChoice, LPARAM rawSessionId)
{
    const std::uint64_t sessionId =
        static_cast<std::uint64_t>(rawSessionId);
    if (!pendingLuaWidgetConsent_ ||
        pendingLuaWidgetConsent_->sessionId != sessionId)
        return;
    PendingLuaWidgetConsent pending =
        std::move(*pendingLuaWidgetConsent_);
    pendingLuaWidgetConsent_.reset();

    const WidgetConsentChoice choice =
        static_cast<WidgetConsentChoice>(rawChoice);
    if (choice == WidgetConsentChoice::Cancel) return;
    const auto package = WidgetEngine::GetWidgetPackage(
        pending.packageId);
    if (!package) return;
    if (package->source != pending.source ||
        !SameStringSet(package->manifest.permissions,
            pending.requestedPermissions) ||
        !SameStringSet(package->manifest.optionalPermissions,
            pending.requestedOptionalPermissions) ||
        !SameStringSet(package->manifest.networkDomains,
            pending.requestedNetworkDomains))
    {
        BeginLuaWidgetConsent(pending.screenPoint,
            pending.packageId, pending.targetWidgetId);
        return;
    }

    const bool grant = choice == WidgetConsentChoice::AllowAll ||
        choice == WidgetConsentChoice::AllowRequired;
    const auto decision = grant
        ? snowdesktop::widget::PermissionDecisionState::Granted
        : snowdesktop::widget::PermissionDecisionState::Denied;
    std::vector<std::string> grantedPermissions;
    std::vector<std::string> grantedNetworkDomains;
    if (grant)
    {
        grantedPermissions = pending.requestedPermissions;
        for (const auto& permission :
            pending.requestedOptionalPermissions)
        {
            if (choice == WidgetConsentChoice::AllowAll ||
                !snowdesktop::widget::PermissionRequiresConsent(
                    permission))
            {
                grantedPermissions.push_back(permission);
            }
        }
        if (std::find(grantedPermissions.begin(),
                grantedPermissions.end(), "network.http") !=
                grantedPermissions.end() ||
            std::find(grantedPermissions.begin(),
                grantedPermissions.end(), "network.internet") !=
                grantedPermissions.end())
        {
            grantedNetworkDomains =
                pending.requestedNetworkDomains;
        }
    }
    std::string error;
    const bool applied = widgetEngine_
        ? widgetEngine_->ApplyWidgetPermissionDecision(
            pending.packageId, decision,
            grantedPermissions, grantedNetworkDomains, error)
        : WidgetEngine::SetWidgetPermissionDecision(
            pending.packageId, decision,
            grantedPermissions, grantedNetworkDomains, error);
    if (!applied)
    {
        ShowWidgetPermissionErrorAsync(
            _LW("app.widget_permission.title"),
            _LFW("app.widget_permission.save_failed",
                Utf8ToWide(error)));
        return;
    }
    if (grant && widgetEngine_)
    {
        (void)widgetEngine_->RetryWidget(
            pending.targetWidgetId, pending.packageId);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 将组件放置到指定网格位置，并重新安置被挤占的桌面项。
 * @param widgetIndex 组件索引。
 * @param targetCell 目标单元格。
 * @param targetSpan 目标跨度。
 * @param isMove 是否为移动操作（而非缩放）。
 */
