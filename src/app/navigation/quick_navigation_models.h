#pragma once

#include "common/types.h"
#include <windows.h>
#include <commctrl.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

enum class QuickNavigationInvocationSource
{
    Pointer,
    DockSearch,
    Hotkey,
    StatusBar,
};

namespace snowdesktop::quick_navigation
{
/**
 * @brief 快速导航面板中的条目结构体。
 *
 * 表示快速导航面板中的一个可操作项，可以是桌面图标项（DesktopItem）
 * 或文件夹部件中的条目（FolderEntry），包含图标、名称、路径等信息，
 * 供导航面板渲染和交互使用。
 */
struct Entry
{
    /**
     * @brief 条目来源类型。
     * @var DesktopItem  来自桌面图标
     * @var FolderEntry  来自文件夹映射部件的条目
     */
    enum class Kind
    {
        DesktopItem,
        FolderEntry,
        PathFavorite,
        ApplicationFavorite,
    };

    Kind kind = Kind::DesktopItem;            /**< 条目来源类型 */
    size_t itemIndex = static_cast<size_t>(-1);   /**< 在桌面项列表中的索引（DesktopItem 类型时有效） */
    size_t widgetIndex = static_cast<size_t>(-1); /**< 所在部件的索引 */
    size_t demoCollectionIndex = static_cast<size_t>(-1); /**< 演示模式下提供虚拟身份的集合 */
    size_t folderEntryIndex = static_cast<size_t>(-1); /**< 在文件夹条目列表中的索引（FolderEntry 类型时有效） */
    std::wstring name;           /**< 显示名称 */
    std::wstring path;           /**< 完整路径 */
    std::wstring source;         /**< 来源标识 */
    bool isDirectory = false;    /**< 路径收藏是否为目录 */
};

struct Section
{
    std::wstring label;
    size_t firstEntryIndex = 0;
    size_t entryCount = 0;
    bool showWhenEmpty = false;
    bool favoriteSection = false;
};

struct ContentModel
{
    std::vector<Entry> entries;
    std::vector<Section> sections;

    bool IsSectioned() const
    {
        return !sections.empty();
    }
};

struct EverythingEntry
{
    std::wstring name;
    std::wstring path;
    FILETIME dateModified{};
    std::wstring modifiedText;
    bool isDirectory = false;
    int systemIconIndex = -1;
    std::wstring iconCacheKey;
};

struct AppEntry
{
    std::wstring name;
    std::wstring parsingName;
    std::wstring iconCacheIdentity;
    Pidl absolutePidl;
    HBITMAP iconBitmap = nullptr;
    SIZE iconBitmapSize{};
    int systemIconIndex = -1;

    AppEntry() = default;
    AppEntry(const AppEntry&) = delete;
    AppEntry& operator=(
        const AppEntry&) = delete;

    AppEntry(
        AppEntry&& other) noexcept
        : name(std::move(other.name)),
          parsingName(std::move(other.parsingName)),
          iconCacheIdentity(
              std::move(other.iconCacheIdentity)),
          absolutePidl(std::move(other.absolutePidl)),
          iconBitmap(other.iconBitmap),
          iconBitmapSize(other.iconBitmapSize),
          systemIconIndex(other.systemIconIndex)
    {
        other.iconBitmap = nullptr;
        other.iconBitmapSize = {};
    }

    AppEntry& operator=(
        AppEntry&& other) noexcept
    {
        if (this == &other)
            return *this;
        if (iconBitmap)
            DeleteObject(iconBitmap);
        name = std::move(other.name);
        parsingName = std::move(other.parsingName);
        iconCacheIdentity =
            std::move(other.iconCacheIdentity);
        absolutePidl = std::move(other.absolutePidl);
        iconBitmap = other.iconBitmap;
        iconBitmapSize = other.iconBitmapSize;
        systemIconIndex = other.systemIconIndex;
        other.iconBitmap = nullptr;
        other.iconBitmapSize = {};
        return *this;
    }

    ~AppEntry()
    {
        if (iconBitmap)
            DeleteObject(iconBitmap);
    }
};

struct AppIndexResult
{
    uint64_t serial = 0;
    std::vector<AppEntry> entries;
    HIMAGELIST systemImageListSmall = nullptr;
};

enum class KeyboardTargetKind
{
    None,
    Item,
    App,
    Everything,
    ExpandApps,
    LoadMoreEverything,
};

struct KeyboardTarget
{
    KeyboardTargetKind kind = KeyboardTargetKind::None;
    size_t index = 0;
    RECT rect{};
};

enum class PointerTargetKind
{
    None,
    Tab,
    ViewMode,
    InitialBack,
    InitialBucket,
    SectionHeader,
    SectionAdd,
    Item,
    App,
    ExpandApps,
    Everything,
    LoadMoreEverything,
    Scrollbar,
};

struct PointerTarget
{
    PointerTargetKind kind =
        PointerTargetKind::None;
    size_t index = 0;

    bool operator==(
        const PointerTarget&) const = default;
};

struct HoverRegion
{
    RECT bounds{};
    PointerTarget target{};
};

enum class Menu { None, Types, Views };
}
