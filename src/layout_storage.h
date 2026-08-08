#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::layout_storage
{
inline constexpr int kCurrentSchemaVersion = 1;

struct PageRecord
{
    std::string id;
    std::optional<int> columns;
    std::optional<int> rows;
};

struct ItemRecord
{
    std::string key;
    std::optional<std::string> page;
    std::optional<int> column;
    std::optional<int> row;
    int width = 1;
    int height = 1;
};

struct WidgetRecord
{
    std::string id;
    std::string page;
    int column = 0;
    int row = 0;
    int width = 1;
    int height = 1;
    std::string type;
    std::optional<std::string> title;
    std::optional<std::string> customTitle;
    std::optional<std::string> titleMode;
    std::string sourceFolderPath;
    std::string packageId;
    std::string scriptPath;
    std::string legacyScriptPath;
    std::string activeCategory;
    int scrollOffset = 0;
    int tabScrollOffset = 0;
    int folderSortMode = -1;
    bool folderSortAscending = true;
    bool autoCollect = false;
    bool listMode = false;
    bool dateHeaders = false;
    bool showFileCategories = false;
    bool showSearchBox = false;
    bool showOnHoverOnly = false;
    bool privacyMode = false;
    bool scrollContainerMode = false;
    bool keepWhenDesktopHidden = false;
    std::optional<bool> showTitle;
    std::optional<bool> bottomBarHover;
    std::optional<bool> userRenamed;
    std::vector<std::string> items;
    std::vector<std::string> childWidgets;
};

struct DockRecord
{
    std::string type;
    std::string reference;
    bool keepOnDesktop = false;
    int folderSortMode = 0;
    bool folderSortAscending = true;
    std::vector<std::string> folderItems;
};

struct Document
{
    int sourceSchemaVersion = 0;
    int schemaVersion = kCurrentSchemaVersion;
    std::optional<int> widgetTitleSchemaVersion;
    std::optional<int> widgetContentOptionsSchemaVersion;
    std::optional<std::string> firstPageMonitor;
    std::optional<std::string> lastPageMonitor;
    std::optional<bool> dockEnabled;
    std::optional<float> itemFontSize;
    std::optional<float> itemFontWeight;
    std::optional<float> iconSpacing;
    std::optional<float> componentSpacing;
    std::optional<int> shortcutArrowMode;
    std::optional<bool> iconBeautifyEnabled;
    std::optional<int> iconBeautifyMode;
    std::optional<float> iconBeautifyBgOpacity;
    std::optional<bool> iconBeautifyGradientEnabled;
    std::optional<int> iconBeautifyGradientDirection;
    std::optional<float> iconBeautifyBgStartR;
    std::optional<float> iconBeautifyBgStartG;
    std::optional<float> iconBeautifyBgStartB;
    std::optional<float> iconBeautifyBgEndR;
    std::optional<float> iconBeautifyBgEndG;
    std::optional<float> iconBeautifyBgEndB;
    std::vector<PageRecord> pages;
    std::vector<ItemRecord> items;
    std::vector<WidgetRecord> widgets;
    std::vector<DockRecord> dockEntries;
    std::vector<std::string> navTabOrder;
};

enum class LoadStatus
{
    Missing,
    LoadedPrimary,
    RecoveredBackup,
    Invalid,
};

struct LoadResult
{
    LoadStatus status = LoadStatus::Missing;
    std::string error;
};

std::filesystem::path BackupPath(const std::filesystem::path& layoutPath);
bool ParseDocument(std::string_view contents, Document& document,
    std::string* error = nullptr);
bool ValidateDocument(std::string_view contents, std::string* error = nullptr);
LoadResult LoadDocument(const std::filesystem::path& layoutPath,
    Document& document);
bool SaveDocument(const std::filesystem::path& layoutPath,
    std::string_view contents, std::string* error = nullptr);
}
