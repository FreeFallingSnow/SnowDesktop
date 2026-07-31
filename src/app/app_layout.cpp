#include "app.h"

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
 * @brief 从 JSON 文本中解析保存的页面信息（ID、行数、列数）。
 * @param text JSON 格式的布局文本。
 */
void DesktopApp::LoadSavedPagesFromJson(const std::string& text)
{
    size_t pagesName = text.find("\"pages\"");
    if (pagesName == std::string::npos) return;

    size_t arrayStart = text.find('[', pagesName);
    size_t arrayEnd = text.find(']', arrayStart == std::string::npos ? pagesName : arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart) return;

    size_t pos = arrayStart + 1;
    while ((pos = text.find('{', pos)) != std::string::npos && pos < arrayEnd)
    {
        size_t objectEnd = text.find('}', pos);
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;

        std::string objectText = text.substr(pos, objectEnd - pos + 1);
        std::string pageUtf8;
        if (ReadJsonStringField(objectText, "id", pageUtf8))
        {
            std::wstring pageId = Utf8ToWide(pageUtf8);
            if (pageId == kDockPageId)
            {
                pos = objectEnd + 1;
                continue;
            }
            if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
                savedPageIds_.push_back(pageId);
            int columns = 0, rows = 0;
            if (ReadJsonIntField(objectText, "columns", columns) && columns > 0)
                savedPageColumns_[pageId] = columns;
            if (ReadJsonIntField(objectText, "rows", rows) && rows > 0)
                savedPageRows_[pageId] = rows;
        }
        pos = objectEnd + 1;
    }
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
    std::string text;
    const auto loadResult = snowdesktop::layout_storage::LoadDocument(
        GetLayoutPath(), text);
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

    int widgetTitleSchemaVersion = 0;
    ReadJsonIntField(text, "widgetTitleSchemaVersion", widgetTitleSchemaVersion);
    const bool hasTrustedWidgetTitleMode = widgetTitleSchemaVersion >= 1;

    std::string firstPageMonitorUtf8;
    if (ReadJsonStringField(text, "firstPageMonitor", firstPageMonitorUtf8))
        firstPageMonitorId_ = Utf8ToWide(firstPageMonitorUtf8);

    std::string lastPageMonitorUtf8;
    if (ReadJsonStringField(text, "lastPageMonitor", lastPageMonitorUtf8))
        lastPageMonitorId_ = Utf8ToWide(lastPageMonitorUtf8);

    bool loadedDockEnabled = false;
    if (ReadJsonBoolField(text, "dockEnabled", loadedDockEnabled))
        generalSettings_.dockEnabled = loadedDockEnabled;

    float loadedFontSize = 0;
    if (ReadJsonFloatField(text, "itemFontSize", loadedFontSize) &&
        loadedFontSize >= 10.0f && loadedFontSize <= 24.0f)
        itemFontSize_ = loadedFontSize;

    float loadedFontWeight = 0;
    if (ReadJsonFloatField(text, "itemFontWeight", loadedFontWeight) &&
        loadedFontWeight >= 100 && loadedFontWeight <= 950)
        itemFontWeight_ = static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(loadedFontWeight));

    float loadedIconSpacing = 0;
    if (ReadJsonFloatField(text, "iconSpacing", loadedIconSpacing) &&
        loadedIconSpacing >= 0.5f && loadedIconSpacing <= 2.0f)
        iconSpacingScale_ = loadedIconSpacing;

    int loadedShortcutArrowMode = 0;
    if (ReadJsonIntField(text, "shortcutArrowMode", loadedShortcutArrowMode))
        shortcutArrowMode_ = std::clamp(loadedShortcutArrowMode, 0, 2);

    bool loadedIconBeautify = false;
    if (ReadJsonBoolField(text, "iconBeautifyEnabled", loadedIconBeautify))
        iconBeautifyEnabled_ = loadedIconBeautify;

    int loadedIconBeautifyMode = 0;
    if (ReadJsonIntField(text, "iconBeautifyMode", loadedIconBeautifyMode))
        iconBeautifyMode_ = std::clamp(loadedIconBeautifyMode, 0, 1);

    float loadedIconBeautifyFloat = 0.0f;
    if (ReadJsonFloatField(text, "iconBeautifyBgOpacity", loadedIconBeautifyFloat))
        iconBeautifyBgOpacity_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    bool loadedIconBeautifyGradient = false;
    if (ReadJsonBoolField(text, "iconBeautifyGradientEnabled", loadedIconBeautifyGradient))
        iconBeautifyGradientEnabled_ = loadedIconBeautifyGradient;
    int loadedIconBeautifyDirection = 0;
    if (ReadJsonIntField(text, "iconBeautifyGradientDirection", loadedIconBeautifyDirection))
        iconBeautifyGradientDirection_ = std::clamp(loadedIconBeautifyDirection, 0, 3);
    if (ReadJsonFloatField(text, "iconBeautifyBgStartR", loadedIconBeautifyFloat))
        iconBeautifyBgStartR_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgStartG", loadedIconBeautifyFloat))
        iconBeautifyBgStartG_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgStartB", loadedIconBeautifyFloat))
        iconBeautifyBgStartB_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgEndR", loadedIconBeautifyFloat))
        iconBeautifyBgEndR_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgEndG", loadedIconBeautifyFloat))
        iconBeautifyBgEndG_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgEndB", loadedIconBeautifyFloat))
        iconBeautifyBgEndB_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);

    LoadSavedPagesFromJson(text);

    size_t pos = 0;
    while ((pos = text.find("\"key\"", pos)) != std::string::npos)
    {
        size_t objStart = text.rfind('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = objStart + 1;
        int depth = 1;
        for (size_t i = objStart + 1; i < text.size() && depth > 0; ++i)
        {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}') --depth;
            objEnd = i;
        }
        if (depth != 0) break;

        std::string objText = text.substr(objStart, objEnd - objStart + 1);
        std::string keyUtf8;
        if (!ReadJsonStringField(objText, "key", keyUtf8)) { pos = objEnd + 1; continue; }

        LayoutRecord record;
        std::string pageUtf8;
        int x = 0, y = 0, w = 1, h = 1;
        if (ReadJsonStringField(objText, "page", pageUtf8) &&
            ReadJsonIntField(objText, "x", x) && ReadJsonIntField(objText, "y", y))
        {
            record.cell.pageId = Utf8ToWide(pageUtf8);
            record.cell.column = x;
            record.cell.row = y;
            RememberSavedPageId(record.cell.pageId);
            ReadJsonIntField(objText, "w", w);
            ReadJsonIntField(objText, "h", h);
            record.span.columns = std::max(1, w);
            record.span.rows = std::max(1, h);
            record.hasGrid = true;
            record.legacySlot = SlotFromCell(gridPages_, record.cell);
        }
        layoutRecords_[ToUpperInvariant(Utf8ToWide(keyUtf8))] = record;
        pos = objEnd + 1;
    }

    // Load widgets
    {
        size_t widgetsName = text.find("\"widgets\"");
        if (widgetsName != std::string::npos)
        {
            size_t arrayStart = text.find('[', widgetsName);
            if (arrayStart != std::string::npos)
            {
                size_t arrayEnd = FindJsonArrayEnd(text, arrayStart);
                if (arrayEnd != std::string::npos && arrayEnd > arrayStart)
                {
                    size_t wp = arrayStart + 1;
                    while ((wp = text.find('{', wp)) != std::string::npos && wp < arrayEnd)
                    {
                        size_t objectEnd = FindJsonObjectEnd(text, wp);
                        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
                        std::string obj = text.substr(wp, objectEnd - wp + 1);
                        std::string idUtf8, typeUtf8, titleUtf8, customTitleUtf8,
                            titleModeUtf8, sourceUtf8, packageIdUtf8, scriptUtf8,
                            activeCategoryUtf8, pageUtf8;
                        int x = 0, y = 0, w = 1, h = 1, scrollOffset = 0,
                            tabScrollOffset = 0,
                            folderSortMode =
                                snowdesktop::folder_sort_rules::kManual;
                        bool autoCollect = false, listMode = false, dateHeaders = false,
                            showFileCategories = false, showSearchBox = false,
                            showOnHoverOnly = false, privacyMode = false,
                            scrollContainerMode = false, showTitle = false,
                            bottomBarHover = false, userRenamed = false,
                            folderSortAscending = true;
                        bool keepWhenDesktopHidden = false;
                        if (!ReadJsonStringField(obj, "id", idUtf8) ||
                            !ReadJsonStringField(obj, "page", pageUtf8) ||
                            !ReadJsonIntField(obj, "x", x) ||
                            !ReadJsonIntField(obj, "y", y))
                        {
                            wp = objectEnd + 1;
                            continue;
                        }
                        ReadJsonStringField(obj, "type", typeUtf8);
                        ReadJsonStringField(obj, "title", titleUtf8);
                        const bool hasCustomTitle =
                            ReadJsonStringField(obj, "customTitle", customTitleUtf8);
                        const bool hasTitleMode =
                            ReadJsonStringField(obj, "titleMode", titleModeUtf8);
                        ReadJsonStringField(obj, "sourceFolderPath", sourceUtf8);
                        ReadJsonStringField(obj, "packageId", packageIdUtf8);
                        ReadJsonStringField(obj, "scriptPath", scriptUtf8);
                        if (scriptUtf8.empty())
                            ReadJsonStringField(obj, "legacyScriptPath", scriptUtf8);
                        ReadJsonStringField(obj, "activeCategory", activeCategoryUtf8);
                        ReadJsonIntField(obj, "w", w);
                        ReadJsonIntField(obj, "h", h);
                        ReadJsonIntField(obj, "scrollOffset", scrollOffset);
ReadJsonIntField(obj, "tabScrollOffset", tabScrollOffset);
                        ReadJsonIntField(
                            obj, "folderSortMode",
                            folderSortMode);
                        ReadJsonBoolField(
                            obj, "folderSortAscending",
                            folderSortAscending);
                        ReadJsonBoolField(obj, "autoCollect", autoCollect);
                        ReadJsonBoolField(obj, "listMode", listMode);
                        ReadJsonBoolField(obj, "dateHeaders", dateHeaders);
                        ReadJsonBoolField(obj, "showFileCategories", showFileCategories);
                        ReadJsonBoolField(obj, "showSearchBox", showSearchBox);
                        ReadJsonBoolField(obj, "showOnHoverOnly", showOnHoverOnly);
                        ReadJsonBoolField(obj, "privacyMode", privacyMode);
                        ReadJsonBoolField(obj, "scrollContainerMode", scrollContainerMode);
                        ReadJsonBoolField(obj, "keepWhenDesktopHidden",
                            keepWhenDesktopHidden);

                        DesktopWidget widget;
                        widget.id = Utf8ToWide(idUtf8);
                        widget.type = WidgetTypeFromJson(Utf8ToWide(typeUtf8));
                        widget.sourceFolderPath = Utf8ToWide(sourceUtf8);
                        widget.packageId = Utf8ToWide(packageIdUtf8);
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
                        if (titleUtf8.empty())
                        {
                            if (widget.type == DesktopWidgetType::LuaScript)
                            {
                                widget.title = WidgetEngine::GetWidgetDisplayName(widget.packageId);
                                if (widget.title.empty())
                                    widget.title = !widget.legacyScriptPath.empty()
                                        ? widget.legacyScriptPath : widget.packageId;
                            }
                            else if (widget.type == DesktopWidgetType::Guide)
                            {
                                widget.title = _LW("app.guide.title");
                            }
                            else if (widget.type == DesktopWidgetType::CollectionGroup)
                            {
                                widget.title = _LW("widget.collection_group");
                            }
                            else if (widget.type == DesktopWidgetType::FileGroup)
                            {
                                widget.title = _LW("widget.file_group");
                            }
                            else
                            {
                                widget.title = widget.type == DesktopWidgetType::FileCategories ? _LW("widget.desktop_files")
                                    : widget.type == DesktopWidgetType::FolderMapping ? _LW("widget.folder_mapping")
                                    : _LW("widget.collection");
                            }
                        }
                        else
                        {
                            widget.title = Utf8ToWide(titleUtf8);
                        }
                        widget.gridCell.pageId = Utf8ToWide(pageUtf8);
                        widget.gridCell.column = x;
                        widget.gridCell.row = y;
                        widget.gridSpan.columns = std::max(1, w);
                        widget.gridSpan.rows = std::max(1, h);
                        widget.autoCollect = autoCollect;
                        widget.listMode = listMode;
                        widget.dateHeaders =
                            widget.type == DesktopWidgetType::CollectionGroup
                                ? false
                                : dateHeaders;
                        widget.showFileCategories = showFileCategories;
                        widget.showSearchBox = showSearchBox;
                        widget.showOnHoverOnly = showOnHoverOnly;
                        widget.privacyMode = privacyMode;
                        widget.scrollContainerMode = scrollContainerMode;
                        widget.keepWhenDesktopHidden = keepWhenDesktopHidden;
                        showTitle = widget.type != DesktopWidgetType::LuaScript;
                        bottomBarHover = (widget.type == DesktopWidgetType::Collection ||
                            widget.type == DesktopWidgetType::LuaScript ||
                            widget.type == DesktopWidgetType::Guide);
                        ReadJsonBoolField(obj, "showTitle", showTitle);
                        ReadJsonBoolField(obj, "bottomBarHover", bottomBarHover);
                        const bool hasUserRenamed =
                            ReadJsonBoolField(obj, "userRenamed", userRenamed);
                        widget.showTitle = showTitle;
                        widget.bottomBarHover = bottomBarHover;
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
                            // Legacy layouts only set this flag reliably when it
                            // is true. Older versions wrote false even for
                            // user-named widgets, so false must still go through
                            // title-content inference below.
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
                        widget.scrollOffset = std::max(0, scrollOffset);
                        widget.tabScrollOffset =
                            std::max(0, tabScrollOffset);
                        widget.folderSortMode =
                            snowdesktop::folder_sort_rules::
                                NormalizeMode(
                                    folderSortMode);
                        widget.folderSortAscending =
                            folderSortAscending;
                        widget.activeCategoryId = Utf8ToWide(activeCategoryUtf8);
                        ReadJsonStringArrayField(obj, "items", widget.itemKeys);
                        ReadJsonStringArrayField(obj, "childWidgets", widget.childWidgetIds);
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
                        wp = objectEnd + 1;
                    }
                }
            }
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
    // "key" so the legacy item scanner cannot mistake Dock entries for layout records.
    {
        size_t dockName = text.find("\"dockEntries\"");
        if (dockName != std::string::npos)
        {
            size_t arrayStart = text.find('[', dockName);
            size_t arrayEnd = arrayStart == std::string::npos
                ? std::string::npos : FindJsonArrayEnd(text, arrayStart);
            size_t dp = arrayStart == std::string::npos ? 0 : arrayStart + 1;
            while (arrayEnd != std::string::npos &&
                (dp = text.find('{', dp)) != std::string::npos && dp < arrayEnd)
            {
                size_t objectEnd = FindJsonObjectEnd(text, dp);
                if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
                std::string object = text.substr(dp, objectEnd - dp + 1);
                std::string typeUtf8, referenceUtf8;
                bool keepOnDesktop = false;
                bool folderSortAscending = true;
                int folderSortMode =
                    snowdesktop::folder_sort_rules::kName;
                std::vector<std::wstring> folderItemKeys;
                if (ReadJsonStringField(object, "type", typeUtf8) &&
                    ReadJsonStringField(object, "ref", referenceUtf8))
                {
                    ReadJsonBoolField(object, "keepOnDesktop", keepOnDesktop);
                    ReadJsonIntField(
                        object, "folderSortMode",
                        folderSortMode);
                    ReadJsonBoolField(
                        object, "folderSortAscending",
                        folderSortAscending);
                    ReadJsonStringArrayField(
                        object, "folderItems",
                        folderItemKeys);
                    DockEntry entry;
                    if (typeUtf8 == "collection")
                        entry.type = DockEntryType::Collection;
                    else if (typeUtf8 == "folderMapping")
                        entry.type = DockEntryType::FolderMapping;
                    else
                        entry.type = DockEntryType::DesktopItem;
                    entry.reference = Utf8ToWide(referenceUtf8);
                    if (entry.type == DockEntryType::DesktopItem)
                        entry.reference = ToUpperInvariant(entry.reference);
                    entry.keepOnDesktop = keepOnDesktop;
                    entry.folderSortMode =
                        snowdesktop::folder_sort_rules::
                            NormalizeMode(
                                folderSortMode);
                    entry.folderSortAscending =
                        folderSortAscending;
                    entry.folderItemKeys =
                        std::move(folderItemKeys);
                    if (!entry.reference.empty() &&
                        !(entry.type ==
                                DockEntryType::
                                    DesktopItem &&
                            snowdesktop::
                                shell_item_visibility::
                                    IsAlwaysHidden(
                                        entry.reference)))
                        dockEntries_.push_back(
                            std::move(entry));
                }
                dp = objectEnd + 1;
            }
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

    {
        std::vector<std::wstring> savedOrder;
        ReadJsonStringArrayField(text, "navTabOrder", savedOrder);
        navTabOrder_ = std::move(savedOrder);
    }
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
         << ",\n  \"firstPageMonitor\": \"" << JsonEscapeUtf8(firstPageMonitorId_)
         << "\",\n  \"lastPageMonitor\": \""  << JsonEscapeUtf8(lastPageMonitorId_)
         << "\",\n  \"dockEnabled\": " << (generalSettings_.dockEnabled ? "true" : "false")
         << ",\n  \"itemFontSize\": " << itemFontSize_
         << ",\n  \"itemFontWeight\": " << static_cast<int>(itemFontWeight_)
         << ",\n  \"iconSpacing\": " << iconSpacingScale_
         << ",\n  \"shortcutArrowMode\": " << shortcutArrowMode_
         << ",\n  \"iconBeautifyEnabled\": " << (iconBeautifyEnabled_ ? "true" : "false")
         << ",\n  \"iconBeautifyMode\": " << iconBeautifyMode_
         << ",\n  \"iconBeautifyBgOpacity\": " << iconBeautifyBgOpacity_
         << ",\n  \"iconBeautifyGradientEnabled\": " << (iconBeautifyGradientEnabled_ ? "true" : "false")
         << ",\n  \"iconBeautifyGradientDirection\": " << iconBeautifyGradientDirection_
         << ",\n  \"iconBeautifyBgStartR\": " << iconBeautifyBgStartR_
         << ",\n  \"iconBeautifyBgStartG\": " << iconBeautifyBgStartG_
         << ",\n  \"iconBeautifyBgStartB\": " << iconBeautifyBgStartB_
         << ",\n  \"iconBeautifyBgEndR\": " << iconBeautifyBgEndR_
         << ",\n  \"iconBeautifyBgEndG\": " << iconBeautifyBgEndG_
         << ",\n  \"iconBeautifyBgEndB\": " << iconBeautifyBgEndB_
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
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& w = widgets_[i];
        const bool hasCustomTitle = !w.customTitle.empty();
        file << "    { \"id\": \"" << JsonEscapeUtf8(w.id)
             << "\", \"type\": \"" << JsonEscapeUtf8(WidgetTypeToJson(w.type))
             << "\", \"title\": \"" << JsonEscapeUtf8(w.title)
             << "\", \"titleMode\": \"" << (hasCustomTitle ? "custom" : "auto")
             << "\", \"customTitle\": \"" << JsonEscapeUtf8(w.customTitle)
             << "\", \"sourceFolderPath\": \"" << JsonEscapeUtf8(w.sourceFolderPath)
             << "\", \"packageId\": \"" << JsonEscapeUtf8(w.packageId)
             << "\", \"legacyScriptPath\": \"" << JsonEscapeUtf8(w.legacyScriptPath)
             << "\", \"activeCategory\": \"" << JsonEscapeUtf8(w.activeCategoryId)
             << "\", \"page\": \"" << JsonEscapeUtf8(w.gridCell.pageId)
             << "\", \"x\": " << w.gridCell.column
             << ", \"y\": " << w.gridCell.row
             << ", \"w\": " << std::max(1, w.gridSpan.columns)
             << ", \"h\": " << std::max(1, w.gridSpan.rows)
             << ", \"autoCollect\": " << (w.autoCollect ? "true" : "false")
             << ", \"listMode\": " << (w.listMode ? "true" : "false")
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
 * @brief 从 JSON 对象文本中读取字符串字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，UTF-8 编码的值。
 * @return 读取成功返回 true。
 */
bool DesktopApp::ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t quote = objectText.find('"', colon == std::string::npos ? name + marker.size() : colon + 1);
    size_t end = 0;
    return quote != std::string::npos && ParseJsonStringAt(objectText, quote, value, end);
}

/**
 * @brief 从 JSON 对象文本中读取整数字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，整数值。
 * @return 读取成功返回 true。
 */
bool DesktopApp::ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t numberStart = objectText.find_first_of("-0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (numberStart == std::string::npos) return false;
    try { value = std::stoi(objectText.substr(numberStart)); return true; }
    catch (...) { return false; }
}

/**
 * @brief 从 JSON 对象文本中读取布尔字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，布尔值。
 * @return 读取成功返回 true。
 */
bool DesktopApp::ReadJsonBoolField(const std::string& objectText, const char* fieldName, bool& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t valueStart = objectText.find_first_not_of(" \t\r\n", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (valueStart == std::string::npos) return false;
    if (objectText.compare(valueStart, 4, "true") == 0) { value = true; return true; }
    if (objectText.compare(valueStart, 5, "false") == 0) { value = false; return true; }
    return false;
}

/**
 * @brief 从 JSON 对象文本中读取浮点字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，浮点值。
 * @return 读取成功返回 true。
 */
bool DesktopApp::ReadJsonFloatField(const std::string& objectText, const char* fieldName, float& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t numberStart = objectText.find_first_of("-.0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (numberStart == std::string::npos) return false;
    try { value = std::stof(objectText.substr(numberStart)); return true; }
    catch (...) { return false; }
}

/**
 * @brief 在 JSON 文本中查找匹配的闭合括号位置（支持字符串内转义）。
 * @param text JSON 文本。
 * @param start 起始位置（应为 '{' 或 '['）。
 * @param open 起始括号字符。
 * @param close 闭合括号字符。
 * @return 闭合位置，未找到返回 npos。
 */
size_t DesktopApp::FindJsonContainerEnd(const std::string& text, size_t start, char open, char close) const
{
    if (start >= text.size() || text[start] != open) return std::string::npos;
    int depth = 1;
    bool inString = false;
    for (size_t i = start + 1; i < text.size(); ++i)
    {
        char ch = text[i];
        if (ch == '"' && (i == 0 || text[i - 1] != '\\')) inString = !inString;
        else if (!inString)
        {
            if (ch == open) ++depth;
            else if (ch == close) { --depth; if (depth == 0) return i; }
        }
    }
    return std::string::npos;
}

/**
 * @brief 在 JSON 文本中查找对象结束位置。
 */
size_t DesktopApp::FindJsonObjectEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '{', '}'); }

/**
 * @brief 在 JSON 文本中查找数组结束位置。
 */
size_t DesktopApp::FindJsonArrayEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '[', ']'); }

/**
 * @brief 从 JSON 对象文本中读取字符串数组字段。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param values 输出参数，宽字符串数组。
 * @return 读取成功返回 true。
 */
bool DesktopApp::ReadJsonStringArrayField(const std::string& objectText, const char* fieldName, std::vector<std::wstring>& values) const
{
    values.clear();
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t arrayStart = objectText.find('[', colon == std::string::npos ? name + marker.size() : colon + 1);
    if (arrayStart == std::string::npos) return false;
    size_t arrayEnd = FindJsonArrayEnd(objectText, arrayStart);
    if (arrayEnd == std::string::npos) return false;
    size_t pos = arrayStart + 1;
    while (pos < arrayEnd)
    {
        size_t quote = objectText.find('"', pos);
        if (quote == std::string::npos || quote >= arrayEnd) break;
        std::string utf8;
        size_t end = 0;
        if (!ParseJsonStringAt(objectText, quote, utf8, end)) break;
        values.push_back(Utf8ToWide(utf8));
        pos = end;
    }
    return true;
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
