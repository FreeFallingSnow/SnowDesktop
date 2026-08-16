#include "app.h"
#include "../logical_slot_picker_rules.h"
#include "name_pinyin.h"
#include "search_match.h"

// Lua widget data conversion and application-service bridge.

namespace
{
std::wstring NormalizeLuaApplicationLaunchTarget(
    const std::wstring& parsingName)
{
    if (parsingName.empty()) return {};
    std::wstring launchTarget = parsingName;
    const bool hasShellPrefix =
        launchTarget.size() >= 6 &&
        _wcsnicmp(launchTarget.c_str(), L"shell:", 6) == 0;
    const bool hasNamespacePrefix = launchTarget.starts_with(L"::");
    const bool hasDrivePrefix =
        launchTarget.size() >= 2 && launchTarget[1] == L':';
    const bool hasUncPrefix = launchTarget.starts_with(L"\\\\");
    if (!hasShellPrefix && !hasNamespacePrefix &&
        !hasDrivePrefix && !hasUncPrefix)
    {
        launchTarget = L"shell:AppsFolder\\" + launchTarget;
    }
    return launchTarget;
}
}

std::string LuaWidgetWideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), len, nullptr, nullptr);
    return result;
}

std::wstring LuaWidgetUtf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), len);
    return result;
}

/**
 * @brief 构建 Lua 桌面快照：收集所有桌面项和文件夹条目的信息
 * @param selectedOnly 是否仅包含选中的项
 * @return LuaDesktopItemInfo 向量，供 Lua 脚本使用
 */
std::vector<LuaDesktopItemInfo> DesktopApp::BuildLuaDesktopSnapshot(bool selectedOnly) const
{
    std::vector<LuaDesktopItemInfo> result;
    auto appendDesktopItem = [&](const DesktopItem& item, const std::wstring& source) {
        if (selectedOnly && !item.selected) return;
        LuaDesktopItemInfo info;
        info.id = LuaWidgetWideToUtf8(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        info.title = LuaWidgetWideToUtf8(item.name);
        info.path = LuaWidgetWideToUtf8(item.parsingName);
        info.source = LuaWidgetWideToUtf8(source);
        info.type = LuaWidgetWideToUtf8(item.typeName.empty() ? L"desktopItem" : item.typeName);
        info.selected = item.selected;
        result.push_back(std::move(info));
    };

    for (const auto& item : items_)
    {
        if (!IsItemInAnyWidget(item))
            appendDesktopItem(item, L"desktop");
    }

    for (const auto& widget : widgets_)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            for (const auto& entry : widget.folderEntries)
            {
                if (selectedOnly && !entry.selected) continue;
                LuaDesktopItemInfo info;
                info.id = LuaWidgetWideToUtf8(entry.fullPath);
                info.title = LuaWidgetWideToUtf8(entry.name);
                info.path = LuaWidgetWideToUtf8(entry.fullPath);
                info.source = LuaWidgetWideToUtf8(widget.title.empty() ? L"folderMapping" : widget.title);
                info.type = entry.isDirectory ? "folder" : "file";
                info.selected = entry.selected;
                result.push_back(std::move(info));
            }
            continue;
        }

        for (const auto& key : widget.itemKeys)
        {
            size_t idx = FindItemIndexByKey(key);
            if (idx != static_cast<size_t>(-1))
                appendDesktopItem(items_[idx], widget.title.empty() ? L"widget" : widget.title);
        }
    }
    return result;
}

std::vector<LuaDesktopItemInfo>
DesktopApp::BuildLuaApplicationSearch(
    const std::string& query, int maxResults)
{
    std::vector<LuaDesktopItemInfo> result;
    const std::wstring queryWide =
        LuaWidgetUtf8ToWide(query);
    if (queryWide.empty() || maxResults <= 0)
        return result;

    StartQuickNavigationAppIndexing();
    if (!quickNavigationAppsIndexed_)
        return result;

    const size_t resultLimit = static_cast<size_t>(
        std::clamp(maxResults, 1, 200));
    std::array<std::vector<size_t>,
        kNameSearchNoMatchRank> buckets;
    for (size_t index = 0;
        index < quickNavigationAppEntries_.size();
        ++index)
    {
        const int rank = NameSearchMatchRank(
            quickNavigationAppEntries_[index].name,
            queryWide);
        if (rank >= 0 &&
            rank < kNameSearchNoMatchRank)
        {
            buckets[static_cast<size_t>(rank)]
                .push_back(index);
        }
    }

    result.reserve(std::min(
        resultLimit,
        quickNavigationAppEntries_.size()));
    for (const auto& bucket : buckets)
    {
        for (const size_t index : bucket)
        {
            const QuickNavigationAppEntry& entry =
                quickNavigationAppEntries_[index];
            if (entry.parsingName.empty())
                continue;
            const std::wstring launchPath =
                NormalizeLuaApplicationLaunchTarget(entry.parsingName);
            LuaDesktopItemInfo info;
            info.id = LuaWidgetWideToUtf8(
                entry.parsingName);
            info.title = LuaWidgetWideToUtf8(
                entry.name);
            info.path = LuaWidgetWideToUtf8(
                launchPath);
            info.source = "Applications";
            info.type = "application";
            result.push_back(std::move(info));
            if (result.size() >= resultLimit)
                return result;
        }
    }
    return result;
}

LuaApplicationCatalogSnapshot DesktopApp::BuildLuaApplicationCatalog()
{
    LuaApplicationCatalogSnapshot snapshot;
    StartQuickNavigationAppIndexing();
    if (!quickNavigationAppsIndexed_)
    {
        snapshot.state = quickNavigationAppIndexing_.load()
            ? "indexing" : "unavailable";
        return snapshot;
    }

    snapshot.state = "ready";
    constexpr std::size_t MaximumEntries = 20000;
    snapshot.entries.reserve(std::min(
        quickNavigationAppEntries_.size(), MaximumEntries));
    for (const auto& entry : quickNavigationAppEntries_)
    {
        if (snapshot.entries.size() >= MaximumEntries)
            break;
        const std::wstring launchTarget =
            NormalizeLuaApplicationLaunchTarget(entry.parsingName);
        if (launchTarget.empty()) continue;

        snowdesktop::widget_runtime::WidgetAppCatalogEntry item;
        item.id = LuaWidgetWideToUtf8(entry.parsingName);
        item.title = LuaWidgetWideToUtf8(entry.name);
        item.launchTarget = LuaWidgetWideToUtf8(launchTarget);
        item.foldedTitle = LuaWidgetWideToUtf8(
            ToUpperInvariant(entry.name));
        item.pinyinFull = BuildNamePinyinFullKey(entry.name);
        item.pinyinInitials = BuildNamePinyinInitialKey(entry.name);
        item.source = "Applications";
        item.type = "application";
        if (!item.id.empty() && !item.title.empty())
            snapshot.entries.push_back(std::move(item));
    }
    return snapshot;
}

std::string DesktopApp::BuildLuaApplicationIndexStatus()
{
    StartQuickNavigationAppIndexing();
    if (quickNavigationAppsIndexed_) return "ready";
    return quickNavigationAppIndexing_.load()
        ? "indexing" : "unavailable";
}

std::vector<LuaDesktopItemInfo> DesktopApp::BuildLuaEverythingSearch(const std::string& query, int maxResults) const
{
    std::vector<LuaDesktopItemInfo> result;
    std::wstring queryWide = LuaWidgetUtf8ToWide(query);
    if (queryWide.empty() || maxResults <= 0)
        return result;
    std::unordered_set<std::wstring> seenPaths;
    DWORD limit = static_cast<DWORD>(std::clamp(maxResults, 1, 200));
    EverythingSearchClient search;
    std::vector<EverythingSearchResult> entries =
        search.Search(queryWide, limit);
    const std::wstring normalizedQuery = ToUpperInvariant(queryWide);
    std::stable_sort(entries.begin(), entries.end(),
        [&normalizedQuery](const EverythingSearchResult& left,
            const EverythingSearchResult& right) {
            const int leftRank = NameSearchMatchRank(
                left.name, normalizedQuery);
            const int rightRank = NameSearchMatchRank(
                right.name, normalizedQuery);
            if (leftRank != rightRank) return leftRank < rightRank;
            return ToUpperInvariant(left.name) <
                ToUpperInvariant(right.name);
        });
    for (const auto& entry : entries)
    {
        std::wstring normalizedPath = ToUpperInvariant(entry.path);
        if (normalizedPath.empty() || seenPaths.contains(normalizedPath))
            continue;
        seenPaths.insert(std::move(normalizedPath));

        LuaDesktopItemInfo info;
        info.id = LuaWidgetWideToUtf8(entry.path);
        info.title = LuaWidgetWideToUtf8(entry.name);
        info.path = LuaWidgetWideToUtf8(entry.path);
        info.source = "Everything";
        info.type = entry.isDirectory ? "folder" : "file";
        info.selected = false;
        result.push_back(std::move(info));
    }
    return result;
}

bool DesktopApp::IsLuaLogicalSlotPickerOpen() const
{
    return !logicalSlotPickerRequest_.widgetId.empty();
}

bool DesktopApp::LuaLogicalSlotPickerAccepts(std::string_view kind) const
{
    return IsLuaLogicalSlotPickerOpen() &&
        snowdesktop::logical_slot_picker_rules::Accepts(
            logicalSlotPickerRequest_.accepts, kind);
}

bool DesktopApp::LuaLogicalSlotPickerAcceptsType(
    std::string_view type) const
{
    return IsLuaLogicalSlotPickerOpen() &&
        snowdesktop::logical_slot_picker_rules::MatchesType(
            logicalSlotPickerRequest_.referenceType, type);
}

bool DesktopApp::OpenLuaLogicalSlotPicker(
    const LogicalSlotPickerRequest& request)
{
    if (request.widgetId.empty() || request.slotId.empty() ||
        request.accepts.empty() || quickNavigationOpen_ ||
        IsLuaLogicalSlotPickerOpen() ||
        (!request.referenceType.empty() &&
            request.referenceType != "file" &&
            request.referenceType != "folder"))
        return false;
    const size_t widgetIndex = FindWidgetIndexById(request.widgetId);
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::LuaScript)
        return false;

    logicalSlotPickerRequest_ = request;
    quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
    OpenQuickNavigation(QuickNavigationInvocationSource::Pointer);
    if (!quickNavigationOpen_)
    {
        logicalSlotPickerRequest_ = {};
        return false;
    }
    return true;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotItem>
DesktopApp::BuildLuaLogicalSlotPickerCandidate(
    const QuickNavigationAppEntry& entry) const
{
    if (!LuaLogicalSlotPickerAccepts("app.reference") ||
        entry.parsingName.empty())
        return std::nullopt;
    const std::wstring target =
        NormalizeLuaApplicationLaunchTarget(entry.parsingName);
    if (target.empty()) return std::nullopt;

    snowdesktop::widget_runtime::LogicalSlotItem candidate;
    candidate.kind = "app.reference";
    candidate.title = LuaWidgetWideToUtf8(entry.name);
    candidate.source = "host.picker";
    candidate.type = "application";
    candidate.target = LuaWidgetWideToUtf8(target);
    candidate.available = true;
    if (candidate.title.empty()) candidate.title = candidate.target;
    return LuaLogicalSlotPickerAcceptsType(candidate.type)
        ? std::optional<snowdesktop::widget_runtime::LogicalSlotItem>(
            std::move(candidate))
        : std::nullopt;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotItem>
DesktopApp::BuildLuaLogicalSlotPickerCandidate(
    const QuickNavigationEverythingEntry& entry) const
{
    if (!LuaLogicalSlotPickerAccepts("filesystem.reference") ||
        entry.path.empty())
        return std::nullopt;
    snowdesktop::widget_runtime::LogicalSlotItem candidate;
    candidate.kind = "filesystem.reference";
    candidate.title = LuaWidgetWideToUtf8(entry.name);
    candidate.source = "host.picker";
    candidate.type = entry.isDirectory ? "folder" : "file";
    candidate.target = LuaWidgetWideToUtf8(entry.path);
    candidate.available = true;
    if (candidate.title.empty()) candidate.title = candidate.target;
    return LuaLogicalSlotPickerAcceptsType(candidate.type)
        ? std::optional<snowdesktop::widget_runtime::LogicalSlotItem>(
            std::move(candidate))
        : std::nullopt;
}

std::optional<snowdesktop::widget_runtime::LogicalSlotItem>
DesktopApp::BuildLuaLogicalSlotPickerCandidate(
    const QuickNavigationEntry& entry) const
{
    snowdesktop::widget_runtime::LogicalSlotItem candidate;
    candidate.source = "host.picker";
    candidate.available = true;
    if (entry.kind == QuickNavigationEntry::Kind::FolderEntry)
    {
        if (!LuaLogicalSlotPickerAccepts("filesystem.reference") ||
            entry.path.empty())
            return std::nullopt;
        candidate.kind = "filesystem.reference";
        candidate.title = LuaWidgetWideToUtf8(entry.name);
        candidate.target = LuaWidgetWideToUtf8(entry.path);
        const DWORD attributes = GetFileAttributesW(entry.path.c_str());
        candidate.type = attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            ? "folder" : "file";
    }
    else
    {
        if (entry.itemIndex >= items_.size()) return std::nullopt;
        const DesktopItem& item = items_[entry.itemIndex];
        const std::wstring target = !item.parsingName.empty()
            ? item.parsingName
            : (!item.layoutKey.empty() ? item.layoutKey
                : item.desktopIconClsid);
        const std::string_view kind = snowdesktop::
            logical_slot_picker_rules::DesktopCandidateKind(
                logicalSlotPickerRequest_.accepts,
                item.isApplicationShortcut,
                !item.parsingName.empty());
        if (kind.empty() || target.empty()) return std::nullopt;
        candidate.kind = std::string(kind);
        candidate.title = LuaWidgetWideToUtf8(item.name);
        candidate.target = LuaWidgetWideToUtf8(target);
        if (candidate.kind == "filesystem.reference")
        {
            const DWORD attributes = GetFileAttributesW(target.c_str());
            candidate.type = attributes != INVALID_FILE_ATTRIBUTES &&
                    (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                ? "folder" : "file";
        }
        else
        {
            candidate.type = item.typeName.empty()
                ? (item.isApplicationShortcut
                    ? "application" : "desktop.item")
                : LuaWidgetWideToUtf8(item.typeName);
        }
    }
    if (candidate.title.empty()) candidate.title = candidate.target;
    return LuaLogicalSlotPickerAcceptsType(candidate.type)
        ? std::optional<snowdesktop::widget_runtime::LogicalSlotItem>(
            std::move(candidate))
        : std::nullopt;
}

bool DesktopApp::CanPickLuaLogicalSlotEntry(
    const QuickNavigationEntry& entry) const
{
    return !IsLuaLogicalSlotPickerOpen() ||
        BuildLuaLogicalSlotPickerCandidate(entry).has_value();
}

bool DesktopApp::CommitLuaLogicalSlotPickerCandidate(
    snowdesktop::widget_runtime::LogicalSlotItem candidate)
{
    if (!widgetEngine_ || !IsLuaLogicalSlotPickerOpen()) return false;
    const LogicalSlotPickerRequest request = logicalSlotPickerRequest_;
    logicalSlotPickerRequest_ = {};
    CloseQuickNavigationThen(
        [this, request, candidate = std::move(candidate)]() mutable {
            if (!widgetEngine_) return;
            snowdesktop::widget_runtime::LogicalSlotChange change;
            std::string error;
            if (!widgetEngine_->RuntimeBindHostLogicalSlot(
                    request.widgetId, request.slotId,
                    std::move(candidate), request.targetIndex,
                    change, error, "host.picker"))
            {
                widgetEngine_->RuntimeRecordError(request.widgetId,
                    "logical slot picker: " + error);
                MessageBeep(MB_ICONWARNING);
                return;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        });
    return true;
}

/**
 * @brief Lua 调用：将指定路径提交给 Shell 启动工作线程
 * @param path 要打开的文件或文件夹路径
 * @return 是否成功提交打开请求
 */
bool DesktopApp::LuaOpenPath(const std::wstring& path)
{
    return shellLaunchWorker_.Enqueue(
        hwnd_, path);
}

/**
 * @brief Lua 调用：在资源管理器中选中并显示指定路径
 * @param path 要揭示的文件或文件夹路径
 * @return 是否成功执行
 */
bool DesktopApp::LuaRevealPath(const std::wstring& path)
{
    return snowdesktop::item_location::Reveal(hwnd_, path);
}

/**
 * @brief Lua 调用：设置指定小部件的标题
 * @param widgetId 小部件 ID
 * @param title 新标题
 */
void DesktopApp::LuaSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (title.empty()) return;
    for (auto& widget : widgets_)
    {
        if (widget.id != widgetId) continue;
        widget.scriptTitle = title;
        if (!widget.customTitle.empty()) return;
        if (widget.title == title) return;
        widget.title = title;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

/**
 * @brief 开始 Lua 内联文本编辑（创建弹出式编辑框）
 * @param request 编辑请求参数（位置、文本、多行模式等）
 */
void DesktopApp::BeginLuaInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (renameEdit_ != nullptr || request.widgetId.empty() || request.storageKey.empty())
        return;
    if (luaInlineEdit_ != nullptr)
        CommitLuaInlineTextEdit(false);

    size_t widgetIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].id == request.widgetId && widgets_[i].type == DesktopWidgetType::LuaScript)
        {
            widgetIndex = i;
            break;
        }
    }
    if (widgetIndex == static_cast<size_t>(-1))
        return;

    RECT frame = GetStandaloneWidgetFrameRect(widgets_[widgetIndex]);
    RECT rect = {
        frame.left + request.localRect.left,
        frame.top + request.localRect.top,
        frame.left + request.localRect.right,
        frame.top + request.localRect.bottom
    };
    rect.left = std::max<LONG>(frame.left + 2, std::min<LONG>(rect.left, frame.right - 4));
    rect.top = std::max<LONG>(frame.top + 2, std::min<LONG>(rect.top, frame.bottom - 4));
    rect.right = std::min<LONG>(std::max<LONG>(rect.right, rect.left + 24), frame.right - 2);
    rect.bottom = std::min<LONG>(std::max<LONG>(rect.bottom, rect.top + 22), frame.bottom - 2);
    if (IsRectEmptyRect(rect))
        return;

    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    std::wstring initial = Utf8ToWide(request.text);
    DWORD style = WS_POPUP | WS_VISIBLE | ES_LEFT | ES_NOHIDESEL | ES_AUTOVSCROLL;
    if (request.multiline)
        style |= ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL;
    else
        style |= ES_AUTOHSCROLL;

    luaInlineEdit_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", initial.c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!luaInlineEdit_)
        return;

    luaInlineEditWidgetId_ = request.widgetId;
    interactionPinnedWidgetId_ = request.widgetId;
    luaInlineEditStorageKey_ = request.storageKey;
    luaInlineEditOriginalText_ = initial;
    luaInlineEditMultiline_ = request.multiline;
    luaInlineEditLiveUpdate_ = request.liveUpdate;
    luaInlineEditTextColor_ = RGB((request.textColor >> 16) & 0xFF,
        (request.textColor >> 8) & 0xFF, request.textColor & 0xFF);
    luaInlineEditBackgroundColor_ = RGB((request.backgroundColor >> 16) & 0xFF,
        (request.backgroundColor >> 8) & 0xFF, request.backgroundColor & 0xFF);
    if (luaInlineEditBackgroundBrush_) DeleteObject(luaInlineEditBackgroundBrush_);
    luaInlineEditBackgroundBrush_ = CreateSolidBrush(luaInlineEditBackgroundColor_);

    if (luaInlineEditFont_) DeleteObject(luaInlineEditFont_);
    const int editFontSize = std::clamp(
        static_cast<int>(std::round(request.fontSize)), 9, 96);
    luaInlineEditFont_ = CreateFontW(-editFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(luaInlineEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(luaInlineEditFont_ ? luaInlineEditFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(luaInlineEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    SetWindowSubclass(luaInlineEdit_, &DesktopApp::LuaInlineEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(luaInlineEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    if (request.selectAll)
        SendMessageW(luaInlineEdit_, EM_SETSEL, 0, -1);
    else
        SendMessageW(luaInlineEdit_, EM_SETSEL,
            static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SetFocus(luaInlineEdit_);
}

/**
 * @brief 判断鼠标点是否位于任意小部件的装饰区域（含独立小部件）
 * @param pt 客户端坐标点
 * @return 若在小部件装饰区上返回 true
 */

void DesktopApp::PreviewLuaInlineTextEdit()
{
    if (!luaInlineEditLiveUpdate_ || !luaInlineEdit_ || !widgetEngine_ ||
        luaInlineEditWidgetId_.empty() || luaInlineEditStorageKey_.empty())
        return;

    int length = GetWindowTextLengthW(luaInlineEdit_);
    std::vector<wchar_t> buffer(static_cast<size_t>(std::max(0, length)) + 1);
    GetWindowTextW(luaInlineEdit_, buffer.data(), length + 1);
    widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
        LuaWidgetWideToUtf8(std::wstring(buffer.data())));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 提交或取消 Lua 内联文本编辑
 * @param cancel 是否取消编辑
 */
void DesktopApp::CommitLuaInlineTextEdit(bool cancel)
{
    if (luaInlineEdit_ == nullptr) return;

    HWND edit = luaInlineEdit_;
    luaInlineEdit_ = nullptr;
    RemoveWindowSubclass(edit, &DesktopApp::LuaInlineEditSubclassProc, 1);

    std::wstring value;
    if (!cancel)
    {
        int length = GetWindowTextLengthW(edit);
        std::vector<wchar_t> buffer(static_cast<size_t>(std::max(0, length)) + 1);
        GetWindowTextW(edit, buffer.data(), length + 1);
        value.assign(buffer.data());
    }

    DestroyWindow(edit);
    if (luaInlineEditFont_) { DeleteObject(luaInlineEditFont_); luaInlineEditFont_ = nullptr; }
    if (luaInlineEditBackgroundBrush_)
    {
        DeleteObject(luaInlineEditBackgroundBrush_);
        luaInlineEditBackgroundBrush_ = nullptr;
    }

    if (widgetEngine_ && !luaInlineEditWidgetId_.empty() && !luaInlineEditStorageKey_.empty())
    {
        if (cancel && luaInlineEditLiveUpdate_)
            widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
                LuaWidgetWideToUtf8(luaInlineEditOriginalText_));
        else if (!cancel)
            widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
                LuaWidgetWideToUtf8(value));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    luaInlineEditWidgetId_.clear();
    luaInlineEditStorageKey_.clear();
    luaInlineEditOriginalText_.clear();
    luaInlineEditMultiline_ = false;
    luaInlineEditLiveUpdate_ = false;
    luaInlineEditTextColor_ = RGB(0, 0, 0);
    luaInlineEditBackgroundColor_ = RGB(255, 255, 255);
    interactionPinnedWidgetId_.clear();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief Lua 内联编辑框的子类化窗口过程
 */
LRESULT CALLBACK DesktopApp::LuaInlineEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { app->CommitLuaInlineTextEdit(true); return 0; }
        if (wParam == VK_RETURN)
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!app->luaInlineEditMultiline_ || ctrl)
            {
                app->CommitLuaInlineTextEdit(false);
                return 0;
            }
        }
        if (wParam == VK_DELETE && app->luaInlineEditLiveUpdate_)
        {
            LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            app->PreviewLuaInlineTextEdit();
            return result;
        }
        break;
    case WM_CHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        app->PreviewLuaInlineTextEdit();
        return result;
    }
    case WM_IME_COMPOSITION:
    {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if ((lParam & GCS_RESULTSTR) != 0)
            app->PreviewLuaInlineTextEdit();
        return result;
    }
    case WM_KILLFOCUS:
        app->CommitLuaInlineTextEdit(false);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

/**
 * @brief 判断两个窗口是否在同一窗口树中
 * @param parent 父窗口
 * @param window 待检查窗口
 * @return 若 window 是 parent 自身或子窗口则返回 true
 */
