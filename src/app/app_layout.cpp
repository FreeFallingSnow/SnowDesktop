#include "app.h"
#include "../widgets/collection_group_rules.h"

#include "../layout_storage.h"

// ── 布局持久化 ──────────────────────────────────────────────

/**
 * @brief 获取布局文件的完整路径（exe\data 下的 SnowDesktop.layout.json）。
 * @return 布局文件路径。
 */
std::wstring DesktopApp::GetLayoutPath() const
{
    return GetDataFilePath(L"SnowDesktop.layout.json");
}

/**
 * @brief 记录页面 ID 到已保存页面列表（去重）。
 * @param pageId 页面 ID。
 */
void DesktopApp::RememberSavedPageId(const std::wstring& pageId)
{
    if (pageId.empty() || pageId == kDockPageId) return;
    if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
        savedPageIds_.push_back(pageId);
}

/**
 * @brief 从布局 JSON 文件加载所有页面、组件和项目的网格位置信息。
 *
 * 解析内容包括：首选监视器、页面 ID/行列数、每个项目的网格位置及组件定义。
 */
void DesktopApp::LoadLayoutSlots()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    snowdesktop::layout_storage::Document document;
    const auto loadResult = snowdesktop::layout_storage::LoadDocument(
        GetLayoutPath(), document);
    if (loadResult.status ==
        snowdesktop::layout_storage::LoadStatus::Missing)
    {
        return;
    }
    if (loadResult.status ==
        snowdesktop::layout_storage::LoadStatus::Invalid)
    {
        const std::wstring message = L"Layout load rejected: " +
            Utf8ToWide(loadResult.error);
        WriteDiagnosticLogEntry(
            message.c_str(), DiagnosticLogLevel::Error);
        return;
    }
    if (loadResult.status ==
        snowdesktop::layout_storage::LoadStatus::RecoveredBackup)
    {
        const std::wstring message = L"Layout recovered from last-good backup: " +
            Utf8ToWide(loadResult.error);
        WriteDiagnosticLogEntry(
            message.c_str(), DiagnosticLogLevel::Warning);
    }

    dockFolderTargetCache_.clear();
    dockFolderIconIndexCache_.clear();
    struct PreservedFolderEntries
    {
        std::wstring sourceFolderPath;
        std::vector<FolderEntry> entries;
    };
    std::unordered_map<std::wstring, PreservedFolderEntries> preservedFolderEntries;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping || widget.id.empty())
            continue;
        PreservedFolderEntries preserved;
        preserved.sourceFolderPath = widget.sourceFolderPath;
        preserved.entries = std::move(widget.folderEntries);
        preservedFolderEntries.emplace(ToUpperInvariant(widget.id), std::move(preserved));
    }
    auto releasePreservedEntries = [this, &preservedFolderEntries]()
    {
        for (auto& [id, preserved] : preservedFolderEntries)
        {
            for (auto& entry : preserved.entries)
            {
                if (entry.iconBitmap)
                    EraseD2DIconCacheForBitmap(entry.iconBitmap);
            }
        }
        preservedFolderEntries.clear();
    };

    layoutRecords_.clear();
    widgets_.clear();
    dockEntries_.clear();
    savedPageIds_.clear();
    savedPageColumns_.clear();
    savedPageRows_.clear();

    const int widgetTitleSchemaVersion =
        document.widgetTitleSchemaVersion.value_or(0);
    const bool hasTrustedWidgetTitleMode = widgetTitleSchemaVersion >= 1;
    const bool hasTrustedWidgetContentOptions =
        document.widgetContentOptionsSchemaVersion.value_or(0) >= 1;
    const bool hasTrustedDetailColumns =
        document.widgetContentOptionsSchemaVersion.value_or(0) >= 3;

    if (document.firstPageMonitor)
        firstPageMonitorId_ = Utf8ToWide(*document.firstPageMonitor);

    if (document.lastPageMonitor)
        lastPageMonitorId_ = Utf8ToWide(*document.lastPageMonitor);

    if (document.dockEnabled)
        generalSettings_.dockEnabled = *document.dockEnabled;

    if (document.itemFontSize &&
        *document.itemFontSize >= 10.0f &&
        *document.itemFontSize <= 24.0f)
        itemFontSize_ = *document.itemFontSize;

    listItemFontSize_ = snowdesktop::list_detail_rules::ResolveFontSize(
        document.listItemFontSize, itemFontSize_);

    if (document.itemFontWeight &&
        *document.itemFontWeight >= 100 &&
        *document.itemFontWeight <= 950)
        itemFontWeight_ = static_cast<DWRITE_FONT_WEIGHT>(
            static_cast<int>(*document.itemFontWeight));

    if (document.iconSpacing &&
        *document.iconSpacing >= 0.5f &&
        *document.iconSpacing <= 2.0f)
        iconSpacingScale_ = *document.iconSpacing;

    componentSpacingScale_ = snowdesktop::widget_spacing_rules::
        ClampComponentScale(
            document.componentSpacing.value_or(1.0f),
            snowdesktop::widget_spacing_rules::kMaximumComponentScale);

    if (document.shortcutArrowMode)
        shortcutArrowMode_ = std::clamp(
            *document.shortcutArrowMode, 0, 2);

    // Missing beautification fields are the compatibility path for old layouts.
    iconBeautifySettings_ = snowdesktop::IconBeautifySettings{};
    if (document.iconBeautifyEnabled)
        iconBeautifySettings_.enabled = *document.iconBeautifyEnabled;
    if (document.iconBeautifyPreset)
        iconBeautifySettings_.preset = static_cast<snowdesktop::IconBeautifyPreset>(
            *document.iconBeautifyPreset);

    if (document.iconBeautifyMode)
        iconBeautifySettings_.mode = *document.iconBeautifyMode;

    if (document.iconBeautifyBgOpacity)
        iconBeautifySettings_.backgroundOpacity = *document.iconBeautifyBgOpacity;
    if (document.iconBeautifyGradientEnabled)
        iconBeautifySettings_.gradientEnabled =
            *document.iconBeautifyGradientEnabled;
    if (document.iconBeautifyGradientDirection)
        iconBeautifySettings_.gradientDirection =
            *document.iconBeautifyGradientDirection;
    if (document.iconBeautifyBgStartR)
        iconBeautifySettings_.backgroundStartR = *document.iconBeautifyBgStartR;
    if (document.iconBeautifyBgStartG)
        iconBeautifySettings_.backgroundStartG = *document.iconBeautifyBgStartG;
    if (document.iconBeautifyBgStartB)
        iconBeautifySettings_.backgroundStartB = *document.iconBeautifyBgStartB;
    if (document.iconBeautifyBgEndR)
        iconBeautifySettings_.backgroundEndR = *document.iconBeautifyBgEndR;
    if (document.iconBeautifyBgEndG)
        iconBeautifySettings_.backgroundEndG = *document.iconBeautifyBgEndG;
    if (document.iconBeautifyBgEndB)
        iconBeautifySettings_.backgroundEndB = *document.iconBeautifyBgEndB;
    if (document.iconBeautifyShape)
        iconBeautifySettings_.shape = static_cast<snowdesktop::IconBeautifyShape>(
            *document.iconBeautifyShape);
    if (document.iconBeautifyContentScale)
        iconBeautifySettings_.contentScale = *document.iconBeautifyContentScale;
    if (document.iconBeautifyFinish)
        snowdesktop::icon_beautify::ApplyLegacyFinish(
            iconBeautifySettings_, static_cast<snowdesktop::IconBeautifyFinish>(
                *document.iconBeautifyFinish));
    if (document.iconBeautifyTextureHighlightStrength)
        iconBeautifySettings_.textureHighlightStrength =
            *document.iconBeautifyTextureHighlightStrength;
    if (document.iconBeautifyTextureHighlightSize)
        iconBeautifySettings_.textureHighlightSize =
            *document.iconBeautifyTextureHighlightSize;
    if (document.iconBeautifyTextureHighlightAngle)
        iconBeautifySettings_.textureHighlightAngle =
            *document.iconBeautifyTextureHighlightAngle;
    if (document.iconBeautifyTextureShadeStrength)
        iconBeautifySettings_.textureShadeStrength =
            *document.iconBeautifyTextureShadeStrength;
    if (document.iconBeautifyTextureEdgeHighlight)
        iconBeautifySettings_.textureEdgeHighlight =
            *document.iconBeautifyTextureEdgeHighlight;
    if (document.iconBeautifyFilterEnabled)
        iconBeautifySettings_.filterEnabled =
            *document.iconBeautifyFilterEnabled;
    if (document.iconBeautifyFilterStrength)
        iconBeautifySettings_.filterStrength =
            *document.iconBeautifyFilterStrength;
    if (document.iconBeautifyFilterTintR)
        iconBeautifySettings_.filterTintR = *document.iconBeautifyFilterTintR;
    if (document.iconBeautifyFilterTintG)
        iconBeautifySettings_.filterTintG = *document.iconBeautifyFilterTintG;
    if (document.iconBeautifyFilterTintB)
        iconBeautifySettings_.filterTintB = *document.iconBeautifyFilterTintB;
    if (document.iconBeautifyOutlineEnabled)
        iconBeautifySettings_.outlineEnabled =
            *document.iconBeautifyOutlineEnabled;
    else if (document.iconBeautifyOutlineMode)
    {
        // Only the former custom mode represented an explicit user outline.
        // The removed automatic mode migrates to the new disabled state.
        iconBeautifySettings_.outlineEnabled =
            *document.iconBeautifyOutlineMode == 2;
    }
    if (document.iconBeautifyOutlineWidth)
        iconBeautifySettings_.outlineWidth = *document.iconBeautifyOutlineWidth;
    if (document.iconBeautifyOutlineOpacity)
        iconBeautifySettings_.outlineOpacity = *document.iconBeautifyOutlineOpacity;
    if (document.iconBeautifyOutlineR)
        iconBeautifySettings_.outlineR = *document.iconBeautifyOutlineR;
    if (document.iconBeautifyOutlineG)
        iconBeautifySettings_.outlineG = *document.iconBeautifyOutlineG;
    if (document.iconBeautifyOutlineB)
        iconBeautifySettings_.outlineB = *document.iconBeautifyOutlineB;
    if (document.iconBeautifyShadowStrength)
        iconBeautifySettings_.shadowStrength = *document.iconBeautifyShadowStrength;
    iconBeautifySettings_ = snowdesktop::icon_beautify::Normalize(
        iconBeautifySettings_);
    if (document.iconBeautifyPreset &&
        iconBeautifySettings_.preset ==
            snowdesktop::IconBeautifyPreset::DefaultBeautify)
        iconBeautifySettings_ = snowdesktop::icon_beautify::MakePreset(
            snowdesktop::IconBeautifyPreset::DefaultBeautify);
    else if (!document.iconBeautifyPreset)
        iconBeautifySettings_.preset = snowdesktop::icon_beautify::IdentifyPreset(
            iconBeautifySettings_);

    for (const auto& page : document.pages)
    {
        const std::wstring pageId = Utf8ToWide(page.id);
        if (pageId == kDockPageId) continue;
        RememberSavedPageId(pageId);
        if (page.columns && *page.columns > 0)
            savedPageColumns_[pageId] = *page.columns;
        if (page.rows && *page.rows > 0)
            savedPageRows_[pageId] = *page.rows;
    }

    for (const auto& item : document.items)
    {
        LayoutRecord record;
        if (item.page && item.column && item.row)
        {
            record.cell.pageId = Utf8ToWide(*item.page);
            record.cell.column = *item.column;
            record.cell.row = *item.row;
            RememberSavedPageId(record.cell.pageId);
            record.span.columns = std::max(1, item.width);
            record.span.rows = std::max(1, item.height);
            record.hasGrid = true;
            record.legacySlot = SlotFromCell(gridPages_, record.cell);
        }
        layoutRecords_[ToUpperInvariant(Utf8ToWide(item.key))] = record;
    }

    // Load widgets
    for (const auto& saved : document.widgets)
    {
        const std::string titleUtf8 = saved.title.value_or("");
        const bool hasCustomTitle = saved.customTitle.has_value();
        const std::string customTitleUtf8 =
            saved.customTitle.value_or("");
        const bool hasTitleMode = saved.titleMode.has_value();
        const std::string titleModeUtf8 = saved.titleMode.value_or("");
        const bool hasUserRenamed = saved.userRenamed.has_value();
        const bool userRenamed = saved.userRenamed.value_or(false);
        const std::string scriptUtf8 = !saved.scriptPath.empty()
            ? saved.scriptPath : saved.legacyScriptPath;

        DesktopWidget widget;
        widget.id = Utf8ToWide(saved.id);
        widget.type = WidgetTypeFromJson(Utf8ToWide(saved.type));
        widget.demoIconCategory = Utf8ToWide(
            saved.demoIconCategory);
        widget.sourceFolderPath = Utf8ToWide(saved.sourceFolderPath);
        widget.packageId = Utf8ToWide(saved.packageId);
        widget.packageSourceProvider = Utf8ToWide(
            saved.packageSourceProvider);
        widget.packageSourceExternalItemId = Utf8ToWide(
            saved.packageSourceExternalItemId);
        widget.packageSourceUrl = Utf8ToWide(saved.packageSourceUrl);
        if (widget.packageId.empty())
            widget.legacyScriptPath = Utf8ToWide(scriptUtf8);
        if (widget.packageId.empty() &&
            !widget.legacyScriptPath.empty())
        {
            if (const auto migrated =
                WidgetEngine::ResolveLegacyWidgetPackage(
                    widget.legacyScriptPath))
            {
                widget.packageId = *migrated;
                widget.legacyScriptPath.clear();
                legacyWidgetLayoutMigrationPending_ = true;
            }
        }
        CaptureWidgetPackageSource(widget);

        if (titleUtf8.empty())
        {
            if (widget.type == DesktopWidgetType::LuaScript)
            {
                widget.title =
                    WidgetEngine::GetWidgetDisplayName(widget.packageId);
                if (widget.title.empty())
                    widget.title = !widget.legacyScriptPath.empty()
                        ? widget.legacyScriptPath : widget.packageId;
            }
            else if (widget.type == DesktopWidgetType::Guide)
                widget.title = _LW("app.guide.title");
            else if (widget.type == DesktopWidgetType::CollectionGroup)
                widget.title = _LW("widget.collection_group");
            else if (widget.type == DesktopWidgetType::FileGroup)
                widget.title = _LW("widget.file_group");
            else
                widget.title =
                    widget.type == DesktopWidgetType::FileCategories
                        ? _LW("widget.desktop_files")
                    : widget.type == DesktopWidgetType::FolderMapping
                        ? _LW("widget.folder_mapping")
                        : _LW("widget.collection");
        }
        else
        {
            widget.title = Utf8ToWide(titleUtf8);
        }

        widget.gridCell.pageId = Utf8ToWide(saved.page);
        widget.gridCell.column = saved.column;
        widget.gridCell.row = saved.row;
        widget.gridSpan.columns = std::max(1, saved.width);
        widget.gridSpan.rows = std::max(1, saved.height);
        widget.autoCollect = saved.autoCollect;
        widget.listMode = saved.listMode;
        if (hasTrustedDetailColumns)
        {
            widget.detailShowModified = saved.detailShowModified;
            widget.detailShowType = saved.detailShowType;
            widget.detailShowSize = saved.detailShowSize;
        }
        else if (saved.showDetails)
        {
            widget.detailShowModified = true;
            widget.detailShowType = true;
            widget.detailShowSize = true;
        }
        widget.detailModifiedWidth =
            snowdesktop::list_detail_rules::ClampPreferredWidth(
                saved.detailModifiedWidth.value_or(160.0f));
        widget.detailTypeWidth =
            snowdesktop::list_detail_rules::ClampPreferredWidth(
                saved.detailTypeWidth.value_or(120.0f));
        widget.detailSizeWidth =
            snowdesktop::list_detail_rules::ClampPreferredWidth(
                saved.detailSizeWidth.value_or(90.0f));
        widget.showDetails = snowdesktop::list_detail_rules::
            HasMetadataColumns(
                widget.detailShowModified,
                widget.detailShowType,
                widget.detailShowSize);
        widget.dateHeaders =
            widget.type == DesktopWidgetType::CollectionGroup
                ? false : saved.dateHeaders;
        if (widget.type == DesktopWidgetType::FileCategories &&
            !hasTrustedWidgetContentOptions)
        {
            // These fields existed in legacy files but were ignored by the
            // standalone desktop-files component and were therefore always
            // saved as false.  Preserve the old visible UI on first upgrade.
            widget.showFileCategories = true;
            widget.showSearchBox = true;
        }
        else
        {
            widget.showFileCategories = saved.showFileCategories;
            widget.showSearchBox = saved.showSearchBox;
        }
        widget.showOnHoverOnly = saved.showOnHoverOnly;
        widget.privacyMode = saved.privacyMode;
        widget.scrollContainerMode = saved.scrollContainerMode;
        widget.keepWhenDesktopHidden = saved.keepWhenDesktopHidden;
        widget.showTitle = saved.showTitle.value_or(
            widget.type != DesktopWidgetType::LuaScript);
        widget.bottomBarHover = saved.bottomBarHover.value_or(
            widget.type == DesktopWidgetType::Collection ||
            widget.type == DesktopWidgetType::LuaScript ||
            widget.type == DesktopWidgetType::Guide);

        if (hasTrustedWidgetTitleMode && hasTitleMode)
        {
            if (titleModeUtf8 == "custom")
            {
                widget.customTitle = Utf8ToWide(
                    hasCustomTitle ? customTitleUtf8 : titleUtf8);
                widget.title = widget.customTitle;
            }
            else
            {
                widget.customTitle.clear();
            }
        }
        else if (hasUserRenamed && userRenamed)
        {
            // Legacy layouts only set this flag reliably when it is true.
            widget.customTitle = Utf8ToWide(
                hasCustomTitle ? customTitleUtf8 : titleUtf8);
            widget.title = widget.customTitle;
        }
        else if (!widget.title.empty())
        {
            bool usesDefaultTitle = false;
            switch (widget.type)
            {
            case DesktopWidgetType::Collection:
                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                    L10N_KEY("widget.collection"), widget.title);
                break;
            case DesktopWidgetType::CollectionGroup:
                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                    L10N_KEY("widget.collection_group"), widget.title);
                break;
            case DesktopWidgetType::FileGroup:
                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                    L10N_KEY("widget.file_group"), widget.title);
                break;
            case DesktopWidgetType::FileCategories:
                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                    L10N_KEY("widget.desktop_files"), widget.title);
                break;
            case DesktopWidgetType::Guide:
                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                    L10N_KEY("app.guide.title"), widget.title);
                break;
            case DesktopWidgetType::LuaScript:
                usesDefaultTitle = WidgetEngine::IsWidgetDefaultName(
                    widget.packageId, widget.title);
                break;
            case DesktopWidgetType::FolderMapping:
            default:
                break;
            }
            if (!usesDefaultTitle)
                widget.customTitle = widget.title;
            else if (widget.type == DesktopWidgetType::LuaScript &&
                widget.title != WidgetEngine::GetWidgetDisplayName(
                    widget.packageId))
                widget.scriptTitle = widget.title;
        }
        widget.userRenamed = !widget.customTitle.empty();
        if (widget.customTitle.empty() &&
            widget.type == DesktopWidgetType::LuaScript &&
            widget.scriptTitle.empty() && !widget.title.empty() &&
            !WidgetEngine::IsWidgetDefaultName(
                widget.packageId, widget.title))
        {
            widget.scriptTitle = widget.title;
        }
        widget.scrollOffset = std::max(0, saved.scrollOffset);
        widget.tabScrollOffset = std::max(0, saved.tabScrollOffset);
        widget.folderSortMode = snowdesktop::folder_sort_rules::NormalizeMode(
            saved.folderSortMode);
        widget.folderSortAscending = saved.folderSortAscending;
        widget.contentSortColumn =
            snowdesktop::list_detail_rules::FromString(
                saved.contentSortColumn);
        widget.contentSortAscending = saved.contentSortAscending;
        if (widget.contentSortColumn ==
                snowdesktop::list_detail_rules::Column::None &&
            widget.type == DesktopWidgetType::FolderMapping &&
            widget.folderSortMode >=
                snowdesktop::folder_sort_rules::kName)
        {
            widget.contentSortColumn = snowdesktop::list_detail_rules::
                FromLegacyFolderSortMode(widget.folderSortMode);
            widget.contentSortAscending = widget.folderSortAscending;
        }
        widget.activeCategoryId = Utf8ToWide(saved.activeCategory);
        widget.itemKeys.reserve(saved.items.size());
        for (const auto& key : saved.items)
            widget.itemKeys.push_back(Utf8ToWide(key));
        widget.childWidgetIds.reserve(saved.childWidgets.size());
        for (const auto& child : saved.childWidgets)
            widget.childWidgetIds.push_back(Utf8ToWide(child));
        ConfigureWidgetGridLimits(widget);
        {
            std::unordered_set<std::wstring> seen;
            std::vector<std::wstring> unique;
            for (auto& key : widget.itemKeys)
            {
                key = ToUpperInvariant(key);
                if (!key.empty() && seen.insert(key).second)
                    unique.push_back(key);
            }
            widget.itemKeys = std::move(unique);
        }

        widgets_.push_back(std::move(widget));
        if (widgets_.back().type == DesktopWidgetType::FolderMapping &&
            !widgets_.back().sourceFolderPath.empty())
        {
            auto preservedIt = preservedFolderEntries.find(
                ToUpperInvariant(widgets_.back().id));
            if (preservedIt != preservedFolderEntries.end() &&
                _wcsicmp(preservedIt->second.sourceFolderPath.c_str(),
                    widgets_.back().sourceFolderPath.c_str()) == 0)
            {
                widgets_.back().folderEntries =
                    std::move(preservedIt->second.entries);
                preservedFolderEntries.erase(preservedIt);
            }
            EnumerateFolderMappingEntries(widgets_.back());
        }
    }

    // Normalize grouped-widget membership after every referenced widget is loaded.
    {
        std::unordered_set<std::wstring> claimedCollections;
        std::unordered_set<std::wstring> claimedFileSources;
        for (auto& group : widgets_)
        {
            if (group.type == DesktopWidgetType::CollectionGroup)
            {
                std::vector<std::wstring> validChildren;
                for (const auto& childId : group.childWidgetIds)
                {
                    const size_t childIndex = FindWidgetIndexById(childId);
                    if (childIndex >= widgets_.size() ||
                        widgets_[childIndex].type != DesktopWidgetType::Collection ||
                        !claimedCollections.insert(childId).second)
                        continue;
                    validChildren.push_back(childId);
                }
                group.childWidgetIds = std::move(validChildren);
                group.activeCategoryId =
                    snowdesktop::collection_group_rules::ResolveActiveItem(
                        group.childWidgetIds, group.activeCategoryId);
                continue;
            }
            if (group.type == DesktopWidgetType::FileGroup)
            {
                std::vector<std::wstring> validChildren;
                for (const auto& childId : group.childWidgetIds)
                {
                    const size_t childIndex = FindWidgetIndexById(childId);
                    if (childIndex >= widgets_.size())
                        continue;
                    const DesktopWidgetType type =
                        widgets_[childIndex].type;
                    if ((type != DesktopWidgetType::FileCategories &&
                         type != DesktopWidgetType::FolderMapping) ||
                        !claimedFileSources.insert(childId).second)
                        continue;
                    validChildren.push_back(childId);
                }
                group.childWidgetIds = std::move(validChildren);
                group.activeCategoryId =
                    snowdesktop::collection_group_rules::ResolveActiveItem(
                        group.childWidgetIds, group.activeCategoryId);
                continue;
            }
            group.childWidgetIds.clear();
        }
    }

    // Ensure widget-owned items have layout records (they're not in the JSON items array)
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    for (auto& w : widgets_)
    {
        for (auto& key : w.itemKeys)
        {
            auto upper = ToUpperInvariant(key);
            if (layoutRecords_.count(upper) == 0)
            {
                LayoutRecord rec;
                rec.cell = w.gridCell;
                rec.span = {1, 1};
                rec.hasGrid = true;
                rec.legacySlot = SlotFromCell(gridPages_, w.gridCell);
                layoutRecords_[upper] = rec;
            }
        }
    }

    // Load Dock references. "ref" intentionally differs from desktop item
    // "key" in the serialized document.
    for (const auto& saved : document.dockEntries)
    {
        DockEntry entry;
        if (saved.type == "collection")
            entry.type = DockEntryType::Collection;
        else if (saved.type == "folderMapping")
            entry.type = DockEntryType::FolderMapping;
        else
            entry.type = DockEntryType::DesktopItem;
        entry.reference = Utf8ToWide(saved.reference);
        if (entry.type == DockEntryType::DesktopItem)
            entry.reference = ToUpperInvariant(entry.reference);
        entry.keepOnDesktop = saved.keepOnDesktop;
        entry.folderSortMode =
            snowdesktop::folder_sort_rules::NormalizeMode(
                saved.folderSortMode);
        entry.folderSortAscending = saved.folderSortAscending;
        entry.folderItemKeys.reserve(saved.folderItems.size());
        for (const auto& key : saved.folderItems)
            entry.folderItemKeys.push_back(Utf8ToWide(key));
        if (!entry.reference.empty() &&
            !(entry.type == DockEntryType::DesktopItem &&
                snowdesktop::shell_item_visibility::IsAlwaysHidden(
                    entry.reference)))
        {
            dockEntries_.push_back(std::move(entry));
        }
    }

    std::erase_if(dockEntries_, [&](const DockEntry& entry) {
        if (entry.type != DockEntryType::Collection &&
            entry.type != DockEntryType::FolderMapping)
            return false;
        const size_t widgetIndex =
            FindWidgetIndexById(entry.reference);
        if (widgetIndex >= widgets_.size())
            return true;
        if (entry.type ==
                DockEntryType::FolderMapping)
        {
            for (auto& group : widgets_)
            {
                if (group.type !=
                        DesktopWidgetType::FileGroup)
                    continue;
                std::erase(
                    group.childWidgetIds,
                    entry.reference);
                group.activeCategoryId =
                    snowdesktop::
                        collection_group_rules::
                            ResolveActiveItem(
                                group.childWidgetIds,
                                group.activeCategoryId);
            }
            return false;
        }
        return IsGroupedWidget(
            widgets_[widgetIndex]);
    });
    NormalizeDockRecycleBinPosition();

    // Dock coordinates are not desktop pages. Migrate both current Dock
    // entries and layouts previously polluted by a normalized Dock pseudo-page.
    std::unordered_set<std::wstring> legacyDockPageCandidates;
    for (auto& entry : dockEntries_)
    {
        if (entry.type == DockEntryType::Collection ||
            entry.type == DockEntryType::FolderMapping)
        {
            entry.keepOnDesktop = false;
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex >= widgets_.size()) continue;
            DesktopWidget& widget = widgets_[widgetIndex];
            if (!widget.gridCell.pageId.empty() && widget.gridCell.pageId != kDockPageId)
                legacyDockPageCandidates.insert(widget.gridCell.pageId);
            widget.gridCell = { kDockPageId, 0, 0 };
            for (const auto& key : widget.itemKeys)
            {
                auto record = layoutRecords_.find(ToUpperInvariant(key));
                if (record == layoutRecords_.end()) continue;
                if (!record->second.cell.pageId.empty() &&
                    record->second.cell.pageId != kDockPageId)
                    legacyDockPageCandidates.insert(record->second.cell.pageId);
                record->second.cell = { kDockPageId, 0, 0 };
                record->second.span = { 1, 1 };
                record->second.hasGrid = true;
            }
            continue;
        }

        if (entry.keepOnDesktop) continue;
        auto record = layoutRecords_.find(ToUpperInvariant(entry.reference));
        if (record == layoutRecords_.end()) continue;
        if (!record->second.cell.pageId.empty() &&
            record->second.cell.pageId != kDockPageId)
            legacyDockPageCandidates.insert(record->second.cell.pageId);
        record->second.cell = { kDockPageId, 0, 0 };
        record->second.span = { 1, 1 };
        record->second.hasGrid = true;
    }

    std::unordered_set<std::wstring> widgetOwnedKeys;
    for (const auto& widget : widgets_)
        for (const auto& key : widget.itemKeys)
            widgetOwnedKeys.insert(ToUpperInvariant(key));

    for (const auto& candidate : legacyDockPageCandidates)
    {
        if (candidate.empty() || candidate == kDockPageId) continue;
        bool hasDesktopContent = std::any_of(widgets_.begin(), widgets_.end(),
            [&](const DesktopWidget& widget) {
                return widget.gridCell.pageId == candidate;
            });
        if (!hasDesktopContent)
        {
            hasDesktopContent = std::any_of(layoutRecords_.begin(), layoutRecords_.end(),
                [&](const auto& pair) {
                    return !widgetOwnedKeys.contains(pair.first) &&
                        pair.second.hasGrid && pair.second.cell.pageId == candidate;
                });
        }
        if (hasDesktopContent) continue;
        std::erase(savedPageIds_, candidate);
        savedPageColumns_.erase(candidate);
        savedPageRows_.erase(candidate);
    }

    navTabOrder_.clear();
    navTabOrder_.reserve(document.navTabOrder.size());
    for (const auto& id : document.navTabOrder)
        navTabOrder_.push_back(Utf8ToWide(id));
    EnsureNavTabOrder();
    NormalizePageIds();
    releasePreservedEntries();
}

/**
 * @brief 将所有项目、组件和页面的网格布局信息持久化到 JSON 文件。
 *
 * 写入内容包括：首选监视器、页面列表、桌面项（排除组件所属项）以及所有组件的完整定义。
 */
void DesktopApp::SaveLayoutSlots()
{
    demoCollectionIdentityCache_.clear();
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    layoutRecords_.clear();
    for (const auto& item : items_)
    {
        if (!item.parsingName.empty())
        {
            RememberSavedPageId(item.gridCell.pageId);
            LayoutRecord record;
            record.cell = item.gridCell;
            record.span = item.gridSpan;
            record.hasGrid = true;
            record.legacySlot = item.slot;
            layoutRecords_[item.layoutKey] = record;
        }
    }

    std::vector<const DesktopItem*> sorted;
    for (const auto& item : items_) sorted.push_back(&item);
    std::sort(sorted.begin(), sorted.end(), [](const DesktopItem* a, const DesktopItem* b) {
        if (a->gridCell.pageId != b->gridCell.pageId) return a->gridCell.pageId < b->gridCell.pageId;
        if (a->gridCell.column != b->gridCell.column) return a->gridCell.column < b->gridCell.column;
        return a->gridCell.row < b->gridCell.row;
    });

    for (const auto& page : gridPages_)
    {
        savedPageColumns_[page.id] = page.columns;
        savedPageRows_[page.id] = page.rows;
    }

    std::vector<std::wstring> pagesToWrite;
    pagesToWrite.reserve(savedPageIds_.size());
    for (const auto& pageId : savedPageIds_)
        if (!pageId.empty() && pageId != kDockPageId)
            pagesToWrite.push_back(pageId);
    if (pagesToWrite.empty() && !gridPages_.empty())
    {
        const GridPage* firstPage = GetFirstPageGridPage();
        if (firstPage) pagesToWrite.push_back(firstPage->id);
    }

    std::ostringstream file;

    file << "{\n  \"layoutSchemaVersion\": 1"
         << ",\n  \"widgetTitleSchemaVersion\": 1"
         << ",\n  \"widgetContentOptionsSchemaVersion\": 3"
         << ",\n  \"firstPageMonitor\": \"" << JsonEscapeUtf8(firstPageMonitorId_)
         << "\",\n  \"lastPageMonitor\": \""  << JsonEscapeUtf8(lastPageMonitorId_)
         << "\",\n  \"dockEnabled\": " << (generalSettings_.dockEnabled ? "true" : "false")
         << ",\n  \"itemFontSize\": " << itemFontSize_
         << ",\n  \"listItemFontSize\": " << listItemFontSize_
         << ",\n  \"itemFontWeight\": " << static_cast<int>(itemFontWeight_)
         << ",\n  \"iconSpacing\": " << iconSpacingScale_
         << ",\n  \"componentSpacing\": " << componentSpacingScale_
         << ",\n  \"shortcutArrowMode\": " << shortcutArrowMode_
         << ",\n  \"iconBeautifyEnabled\": " << (iconBeautifySettings_.enabled ? "true" : "false")
         << ",\n  \"iconBeautifyPreset\": " << static_cast<int>(iconBeautifySettings_.preset)
         << ",\n  \"iconBeautifyMode\": " << iconBeautifySettings_.mode
         << ",\n  \"iconBeautifyBgOpacity\": " << iconBeautifySettings_.backgroundOpacity
         << ",\n  \"iconBeautifyGradientEnabled\": " << (iconBeautifySettings_.gradientEnabled ? "true" : "false")
         << ",\n  \"iconBeautifyGradientDirection\": " << iconBeautifySettings_.gradientDirection
         << ",\n  \"iconBeautifyBgStartR\": " << iconBeautifySettings_.backgroundStartR
         << ",\n  \"iconBeautifyBgStartG\": " << iconBeautifySettings_.backgroundStartG
         << ",\n  \"iconBeautifyBgStartB\": " << iconBeautifySettings_.backgroundStartB
         << ",\n  \"iconBeautifyBgEndR\": " << iconBeautifySettings_.backgroundEndR
         << ",\n  \"iconBeautifyBgEndG\": " << iconBeautifySettings_.backgroundEndG
         << ",\n  \"iconBeautifyBgEndB\": " << iconBeautifySettings_.backgroundEndB
         << ",\n  \"iconBeautifyShape\": " << static_cast<int>(iconBeautifySettings_.shape)
         << ",\n  \"iconBeautifyContentScale\": " << iconBeautifySettings_.contentScale
         << ",\n  \"iconBeautifyTextureHighlightStrength\": " << iconBeautifySettings_.textureHighlightStrength
         << ",\n  \"iconBeautifyTextureHighlightSize\": " << iconBeautifySettings_.textureHighlightSize
         << ",\n  \"iconBeautifyTextureHighlightAngle\": " << iconBeautifySettings_.textureHighlightAngle
         << ",\n  \"iconBeautifyTextureShadeStrength\": " << iconBeautifySettings_.textureShadeStrength
         << ",\n  \"iconBeautifyTextureEdgeHighlight\": " << iconBeautifySettings_.textureEdgeHighlight
         << ",\n  \"iconBeautifyFilterEnabled\": " << (iconBeautifySettings_.filterEnabled ? "true" : "false")
         << ",\n  \"iconBeautifyFilterStrength\": " << iconBeautifySettings_.filterStrength
         << ",\n  \"iconBeautifyFilterTintR\": " << iconBeautifySettings_.filterTintR
         << ",\n  \"iconBeautifyFilterTintG\": " << iconBeautifySettings_.filterTintG
         << ",\n  \"iconBeautifyFilterTintB\": " << iconBeautifySettings_.filterTintB
         << ",\n  \"iconBeautifyOutlineEnabled\": " << (iconBeautifySettings_.outlineEnabled ? "true" : "false")
         << ",\n  \"iconBeautifyOutlineWidth\": " << iconBeautifySettings_.outlineWidth
         << ",\n  \"iconBeautifyOutlineOpacity\": " << iconBeautifySettings_.outlineOpacity
         << ",\n  \"iconBeautifyOutlineR\": " << iconBeautifySettings_.outlineR
         << ",\n  \"iconBeautifyOutlineG\": " << iconBeautifySettings_.outlineG
         << ",\n  \"iconBeautifyOutlineB\": " << iconBeautifySettings_.outlineB
         << ",\n  \"iconBeautifyShadowStrength\": " << iconBeautifySettings_.shadowStrength
         << ",\n  \"pages\": [\n";
    for (size_t i = 0; i < pagesToWrite.size(); ++i)
    {
        const GridPage* page = FindGridPage(gridPages_, pagesToWrite[i]);
        file << "    { \"id\": \"" << JsonEscapeUtf8(pagesToWrite[i]) << "\", \"monitor\": \"";
        file << JsonEscapeUtf8(page != nullptr ? page->monitorId : L"");
        int columns = page != nullptr ? page->columns : 0;
        int rows = page != nullptr ? page->rows : 0;
        if (page == nullptr)
        {
            auto colIt = savedPageColumns_.find(pagesToWrite[i]);
            auto rowIt = savedPageRows_.find(pagesToWrite[i]);
            if (colIt != savedPageColumns_.end()) columns = colIt->second;
            if (rowIt != savedPageRows_.end()) rows = rowIt->second;
        }
        file << "\", \"columns\": " << std::max(1, columns) <<
            ", \"rows\": " << std::max(1, rows) << " }";
        file << (i + 1 == pagesToWrite.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"items\": [\n";
    // Collect widget-owned keys — items in widgets should not be saved
    // to the desktop items array (they belong to their widget's items list)
    std::unordered_set<std::wstring> widgetOwnedKeys;
    for (auto& w : widgets_)
        for (auto& k : w.itemKeys)
            if (!k.empty())
                widgetOwnedKeys.insert(ToUpperInvariant(k));

    bool firstItem = true;
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto* it = sorted[i];
        if (widgetOwnedKeys.count(ToUpperInvariant(it->layoutKey))) continue;
        if (!firstItem) file << ",\n";
        firstItem = false;
        file << "    { \"key\": \"" << JsonEscapeUtf8(it->layoutKey)
             << "\", \"page\": \"" << JsonEscapeUtf8(it->gridCell.pageId)
             << "\", \"x\": " << it->gridCell.column
             << ", \"y\": " << it->gridCell.row
             << ", \"w\": " << std::max(1, it->gridSpan.columns)
             << ", \"h\": " << std::max(1, it->gridSpan.rows)
             << ", \"slot\": " << it->slot << " }";
    }
    if (!firstItem) file << "\n";
    file << "  ],\n  \"widgets\": [\n";
    for (auto& widget : widgets_)
        CaptureWidgetPackageSource(widget);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& w = widgets_[i];
        const bool hasCustomTitle = !w.customTitle.empty();
        file << "    { \"id\": \"" << JsonEscapeUtf8(w.id)
             << "\", \"type\": \"" << JsonEscapeUtf8(WidgetTypeToJson(w.type))
             << "\", \"title\": \"" << JsonEscapeUtf8(w.title)
             << "\", \"titleMode\": \"" << (hasCustomTitle ? "custom" : "auto")
             << "\", \"customTitle\": \"" << JsonEscapeUtf8(w.customTitle)
             << "\", \"demoIconCategory\": \""
             << JsonEscapeUtf8(w.demoIconCategory)
             << "\", \"sourceFolderPath\": \"" << JsonEscapeUtf8(w.sourceFolderPath)
             << "\", \"packageId\": \"" << JsonEscapeUtf8(w.packageId)
             << "\", \"packageSourceProvider\": \""
             << JsonEscapeUtf8(w.packageSourceProvider)
             << "\", \"packageSourceExternalItemId\": \""
             << JsonEscapeUtf8(w.packageSourceExternalItemId)
             << "\", \"packageSourceUrl\": \""
             << JsonEscapeUtf8(w.packageSourceUrl)
             << "\", \"legacyScriptPath\": \"" << JsonEscapeUtf8(w.legacyScriptPath)
             << "\", \"activeCategory\": \"" << JsonEscapeUtf8(w.activeCategoryId)
             << "\", \"page\": \"" << JsonEscapeUtf8(w.gridCell.pageId)
             << "\", \"x\": " << w.gridCell.column
             << ", \"y\": " << w.gridCell.row
             << ", \"w\": " << std::max(1, w.gridSpan.columns)
             << ", \"h\": " << std::max(1, w.gridSpan.rows)
             << ", \"autoCollect\": " << (w.autoCollect ? "true" : "false")
             << ", \"listMode\": " << (w.listMode ? "true" : "false")
             << ", \"showDetails\": "
             << (snowdesktop::list_detail_rules::HasMetadataColumns(
                    w.detailShowModified,
                    w.detailShowType,
                    w.detailShowSize)
                    ? "true" : "false")
             << ", \"detailShowModified\": "
             << (w.detailShowModified ? "true" : "false")
             << ", \"detailShowType\": "
             << (w.detailShowType ? "true" : "false")
             << ", \"detailShowSize\": "
             << (w.detailShowSize ? "true" : "false")
             << ", \"detailModifiedWidth\": "
             << w.detailModifiedWidth
             << ", \"detailTypeWidth\": "
             << w.detailTypeWidth
             << ", \"detailSizeWidth\": "
             << w.detailSizeWidth
             << ", \"contentSortColumn\": \""
             << snowdesktop::list_detail_rules::ToString(
                    w.contentSortColumn)
             << "\", \"contentSortAscending\": "
             << (w.contentSortAscending ? "true" : "false")
             << ", \"dateHeaders\": " << (w.dateHeaders ? "true" : "false")
             << ", \"showFileCategories\": " << (w.showFileCategories ? "true" : "false")
             << ", \"showSearchBox\": " << (w.showSearchBox ? "true" : "false")
             << ", \"showOnHoverOnly\": " << (w.showOnHoverOnly ? "true" : "false")
             << ", \"privacyMode\": " << (w.privacyMode ? "true" : "false")
             << ", \"scrollContainerMode\": " << (w.scrollContainerMode ? "true" : "false")
             << ", \"keepWhenDesktopHidden\": "
             << (w.keepWhenDesktopHidden ? "true" : "false")
             << ", \"showTitle\": " << (w.showTitle ? "true" : "false")
             << ", \"bottomBarHover\": " << (w.bottomBarHover ? "true" : "false")
             << ", \"userRenamed\": " << (hasCustomTitle ? "true" : "false")
             << ", \"scrollOffset\": " << std::max(0, w.scrollOffset)
             << ", \"tabScrollOffset\": " << std::max(0, w.tabScrollOffset)
             << ", \"folderSortMode\": "
             << snowdesktop::folder_sort_rules::
                    NormalizeMode(w.folderSortMode)
             << ", \"folderSortAscending\": "
             << (w.folderSortAscending ? "true" : "false")
             << ", \"items\": [";
        for (size_t j = 0; j < w.itemKeys.size(); ++j)
        {
            file << "\"" << JsonEscapeUtf8(w.itemKeys[j]) << "\"";
            if (j + 1 != w.itemKeys.size()) file << ", ";
        }
        file << "], \"childWidgets\": [";
        for (size_t j = 0; j < w.childWidgetIds.size(); ++j)
        {
            file << "\"" << JsonEscapeUtf8(w.childWidgetIds[j]) << "\"";
            if (j + 1 != w.childWidgetIds.size()) file << ", ";
        }
        file << "] }";
        file << (i + 1 == widgets_.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"dockEntries\": [\n";
    for (size_t i = 0; i < dockEntries_.size(); ++i)
    {
        const DockEntry& entry = dockEntries_[i];
        file << "    { \"type\": \""
             << (entry.type == DockEntryType::Collection
                    ? "collection"
                    : (entry.type == DockEntryType::FolderMapping
                        ? "folderMapping" : "item"))
             << "\", \"ref\": \"" << JsonEscapeUtf8(entry.reference)
             << "\", \"keepOnDesktop\": " << (entry.keepOnDesktop ? "true" : "false")
             << ", \"folderSortMode\": "
             << snowdesktop::folder_sort_rules::
                    NormalizeMode(
                        entry.folderSortMode)
             << ", \"folderSortAscending\": "
             << (entry.folderSortAscending
                    ? "true" : "false")
             << ", \"folderItems\": [";
        for (size_t j = 0;
            j < entry.folderItemKeys.size(); ++j)
        {
            file << "\""
                 << JsonEscapeUtf8(
                        entry.folderItemKeys[j])
                 << "\"";
            if (j + 1 !=
                entry.folderItemKeys.size())
                file << ", ";
        }
        file << "] }"
             << (i + 1 == dockEntries_.size()
                    ? "\n" : ",\n");
    }
    file << "  ],\n  \"navTabOrder\": [";
    for (size_t i = 0; i < navTabOrder_.size(); ++i)
    {
        file << "\"" << JsonEscapeUtf8(navTabOrder_[i]) << "\"";
        if (i + 1 != navTabOrder_.size()) file << ", ";
    }
    file << "]\n}\n";
    std::string saveError;
    if (!snowdesktop::layout_storage::SaveDocument(
            GetLayoutPath(), file.str(), &saveError))
    {
        const std::wstring message = L"Layout save failed: " +
            Utf8ToWide(saveError);
        WriteDiagnosticLogEntry(
            message.c_str(), DiagnosticLogLevel::Error);
    }
}

/**
 * @brief 将 JSON 字符串转换为组件类型枚举。
 * @param type 类型字符串（不区分大小写）。
 * @return 对应的 DesktopWidgetType 枚举值。
 */
DesktopWidgetType DesktopApp::WidgetTypeFromJson(const std::wstring& type) const
{
    std::wstring n = ToUpperInvariant(type);
    if (n == L"FILECATEGORIES" || n == L"FILE_CATEGORIES") return DesktopWidgetType::FileCategories;
    if (n == L"FOLDERMAPPING" || n == L"FOLDER_MAPPING") return DesktopWidgetType::FolderMapping;
    if (n == L"COLLECTIONGROUP" || n == L"COLLECTION_GROUP") return DesktopWidgetType::CollectionGroup;
    if (n == L"FILEGROUP" || n == L"FILE_GROUP") return DesktopWidgetType::FileGroup;
    if (n == L"LUA" || n == L"LUASCRIPT" || n == L"LUA_SCRIPT") return DesktopWidgetType::LuaScript;
    if (n == L"GUIDE") return DesktopWidgetType::Guide;
    if (n == L"COLLECTION") return DesktopWidgetType::Collection;
    return DesktopWidgetType::Collection;
}

/**
 * @brief 将组件类型枚举转换为 JSON 字符串。
 * @param type 组件类型。
 * @return 对应的字符串表示。
 */
std::wstring DesktopApp::WidgetTypeToJson(DesktopWidgetType type) const
{
    switch (type)
    {
    case DesktopWidgetType::CollectionGroup: return L"collectionGroup";
    case DesktopWidgetType::FileGroup:       return L"fileGroup";
    case DesktopWidgetType::FileCategories: return L"fileCategories";
    case DesktopWidgetType::FolderMapping:  return L"folderMapping";
    case DesktopWidgetType::LuaScript:      return L"lua";
    case DesktopWidgetType::Guide:          return L"guide";
    case DesktopWidgetType::Collection:
    default:                                return L"collection";
    }
}
