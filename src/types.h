/**
 * @file types.h
 * @brief 桌面网格系统核心类型定义
 * @details 定义桌面网格布局、桌面项、组件、文件夹条目等核心数据结构，
 *          以及 PIDL 智能包装器和枚举上下文等辅助类型
 */

#pragma once
#include <shlobj.h>

#include "constants.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

enum class IconState : uint8_t {
    Loading,
    IconReady,
    FullQuality
};

// ── Grid Layout Types ──────────────────────

/**
 * @brief 网格单元格
 * @details 表示桌面网格中的一个单元格位置，包含页面ID、列号和行号
 */
struct GridCell
{
    std::wstring pageId;
    int column = 0;
    int row = 0;
};

/**
 * @brief 网格跨度
 * @details 表示单元格在网格中占据的列数和行数
 */
struct GridSpan
{
    int columns = 1;
    int rows = 1;
};

/**
 * @brief 布局记录
 * @details 记录桌面项在网格中的完整布局信息，包含单元格位置、跨度和传统布局槽位
 */
struct LayoutRecord
{
    GridCell cell;
    GridSpan span;
    bool hasGrid = false;
    int legacySlot = -1;
};

/**
 * @brief 网格页面
 * @details 表示一个显示器对应的网格页面，包含显示器边界、工作区、网格行列数及边距等布局参数
 */
struct GridPage
{
    std::wstring id;
    std::wstring monitorId;
    RECT bounds{};
    RECT workArea{};
    bool isPrimary = false;
    UINT dpiX = 96;
    UINT dpiY = 96;
    int columns = 1;
    int rows = 1;
    int cellWidth = kCellWidth;
    int cellHeight = kMinCellHeight;
    int gapX = 0;
    int gapY = 0;
    int marginX = kGridMarginX;
    int marginY = kGridMarginY;
};

/**
 * @brief 待处理的网格移动
 * @details 记录网格项待执行的目标单元格移动操作
 */
struct PendingGridMove
{
    size_t index = 0;
    GridCell cell;
};

/**
 * @brief 待处理的组件移动
 * @details 记录组件待执行的目标单元格移动操作
 */
struct PendingWidgetMove
{
    size_t index = 0;
    GridCell cell;
};

/** 桌面组件类型 */
enum class DesktopWidgetType
{
    Collection,      /**< 集合组件 */
    FileCategories,  /**< 文件分类组件 */
    FolderMapping,   /**< 文件夹映射组件 */
    LuaScript,       /**< Lua 脚本组件 */
    Guide,           /**< 分页指南组件 */
};

// ── PIDL Wrapper ───────────────────────────

/**
 * @brief PIDL 智能包装器
 * @details 封装 PIDLIST_ABSOLUTE 的生命周期管理，支持移动语义，禁止拷贝。自动调用 ILFree 释放
 */
struct Pidl
{
    PIDLIST_ABSOLUTE value = nullptr;

    Pidl() = default;
    explicit Pidl(PIDLIST_ABSOLUTE pidl) : value(pidl) {}
    ~Pidl() { reset(); }

    Pidl(const Pidl&) = delete;
    Pidl& operator=(const Pidl&) = delete;

    Pidl(Pidl&& other) noexcept : value(other.value) { other.value = nullptr; }

    Pidl& operator=(Pidl&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }

    void reset(PIDLIST_ABSOLUTE pidl = nullptr)
    {
        if (value != nullptr)
        {
            ILFree(value);
        }
        value = pidl;
    }

    [[nodiscard]] PIDLIST_ABSOLUTE get() const { return value; }
};

// ── DesktopItem ────────────────────────────

/**
 * @brief 桌面项
 * @details 表示桌面上的一个图标项，包含名称、PIDL、图标、布局信息、选择状态等属性。
 *          支持移动语义，管理图标位图的生命周期
 */
struct DesktopItem
{
    std::wstring name;
    std::wstring parsingName;
    std::wstring layoutKey;
    std::wstring desktopIconClsid;
    std::wstring typeName;
    Pidl absolutePidl;
    Pidl childPidl;
    HBITMAP iconBitmap = nullptr;
    SIZE iconBitmapSize{};
    int sysIconIndex = -1;
    RECT bounds{};
    int slot = 0;
    GridCell gridCell;
    GridSpan gridSpan;
    bool selected = false;
    bool shortcutArrow = false;
    bool isCut = false;
    IconState iconState = IconState::Loading;

    DesktopItem() = default;

    DesktopItem(DesktopItem&& other) noexcept
        : name(std::move(other.name)),
          parsingName(std::move(other.parsingName)),
          layoutKey(std::move(other.layoutKey)),
          desktopIconClsid(std::move(other.desktopIconClsid)),
          typeName(std::move(other.typeName)),
          absolutePidl(std::move(other.absolutePidl)),
          childPidl(std::move(other.childPidl)),
          iconBitmap(other.iconBitmap),
          iconBitmapSize(other.iconBitmapSize),
          sysIconIndex(other.sysIconIndex),
          bounds(other.bounds),
          slot(other.slot),
          gridCell(std::move(other.gridCell)),
          gridSpan(other.gridSpan),
          selected(other.selected),
          shortcutArrow(other.shortcutArrow),
          isCut(other.isCut),
          iconState(other.iconState)
    {
        other.iconBitmap = nullptr;
        other.iconBitmapSize = {};
    }

    DesktopItem& operator=(DesktopItem&& other) noexcept
    {
        if (this != &other)
        {
            if (iconBitmap != nullptr)
            {
                DeleteObject(iconBitmap);
            }

            name = std::move(other.name);
            parsingName = std::move(other.parsingName);
            layoutKey = std::move(other.layoutKey);
            desktopIconClsid = std::move(other.desktopIconClsid);
            typeName = std::move(other.typeName);
            absolutePidl = std::move(other.absolutePidl);
            childPidl = std::move(other.childPidl);
            iconBitmap = other.iconBitmap;
            iconBitmapSize = other.iconBitmapSize;
            sysIconIndex = other.sysIconIndex;
            bounds = other.bounds;
            slot = other.slot;
            gridCell = std::move(other.gridCell);
            gridSpan = other.gridSpan;
            selected = other.selected;
            shortcutArrow = other.shortcutArrow;
            isCut = other.isCut;
            iconState = other.iconState;
            other.iconBitmap = nullptr;
            other.iconBitmapSize = {};
        }
        return *this;
    }

    ~DesktopItem()
    {
        if (iconBitmap != nullptr)
        {
            DeleteObject(iconBitmap);
        }
    }
};

// ── FolderEntry ────────────────────────────

/**
 * @brief 文件夹条目
 * @details 表示文件夹映射组件中的一个文件或子目录条目，包含路径、图标、选择状态等信息
 */
struct FolderEntry
{
    std::wstring name;
    std::wstring fullPath;
    bool isDirectory = false;
    int sysIconIndex = -1;
    HBITMAP iconBitmap = nullptr;
    SIZE iconBitmapSize{};
    bool selected = false;
    bool isCut = false;
    bool shortcutArrow = false;
    IconState iconState = IconState::Loading;

    FolderEntry() = default;

    FolderEntry(const FolderEntry& other)
        : name(other.name),
          fullPath(other.fullPath),
          isDirectory(other.isDirectory),
          sysIconIndex(other.sysIconIndex),
          iconBitmap(nullptr),
          iconBitmapSize(other.iconBitmapSize),
          selected(other.selected),
          isCut(other.isCut),
          shortcutArrow(other.shortcutArrow),
          iconState(other.iconState)
    {
        if (other.iconBitmap != nullptr)
        {
            iconBitmap = CopyFolderEntryBitmap(other.iconBitmap, iconBitmapSize);
        }
    }

    FolderEntry& operator=(const FolderEntry& other)
    {
        if (this != &other)
        {
            if (iconBitmap != nullptr)
            {
                DeleteObject(iconBitmap);
                iconBitmap = nullptr;
                iconBitmapSize = {};
            }
            name = other.name;
            fullPath = other.fullPath;
            isDirectory = other.isDirectory;
            sysIconIndex = other.sysIconIndex;
            iconBitmapSize = other.iconBitmapSize;
            selected = other.selected;
            isCut = other.isCut;
            shortcutArrow = other.shortcutArrow;
            iconState = other.iconState;
            if (other.iconBitmap != nullptr)
            {
                iconBitmap = CopyFolderEntryBitmap(other.iconBitmap, iconBitmapSize);
            }
        }
        return *this;
    }

private:
    static HBITMAP CopyFolderEntryBitmap(HBITMAP source, SIZE& outSize)
    {
        if (source == nullptr) return nullptr;
        BITMAP bm{};
        if (GetObjectW(source, sizeof(bm), &bm) == 0) return nullptr;
        HDC screenDc = GetDC(nullptr);
        HDC srcDc = CreateCompatibleDC(screenDc);
        HDC dstDc = CreateCompatibleDC(screenDc);
        HBITMAP result = CreateCompatibleBitmap(screenDc, bm.bmWidth, bm.bmHeight);
        HBITMAP oldSrc = static_cast<HBITMAP>(SelectObject(srcDc, source));
        HBITMAP oldDst = static_cast<HBITMAP>(SelectObject(dstDc, result));
        BitBlt(dstDc, 0, 0, bm.bmWidth, bm.bmHeight, srcDc, 0, 0, SRCCOPY);
        SelectObject(srcDc, oldSrc);
        SelectObject(dstDc, oldDst);
        DeleteDC(srcDc);
        DeleteDC(dstDc);
        ReleaseDC(nullptr, screenDc);
        outSize = { bm.bmWidth, bm.bmHeight };
        return result;
    }

public:

    ~FolderEntry()
    {
        if (iconBitmap != nullptr)
        {
            DeleteObject(iconBitmap);
        }
    }

    FolderEntry(FolderEntry&& other) noexcept
        : name(std::move(other.name)),
          fullPath(std::move(other.fullPath)),
          isDirectory(other.isDirectory),
          sysIconIndex(other.sysIconIndex),
          iconBitmap(other.iconBitmap),
          iconBitmapSize(other.iconBitmapSize),
          selected(other.selected),
          isCut(other.isCut),
          shortcutArrow(other.shortcutArrow),
          iconState(other.iconState)
    {
        other.iconBitmap = nullptr;
        other.iconBitmapSize = {};
    }

    FolderEntry& operator=(FolderEntry&& other) noexcept
    {
        if (this != &other)
        {
            if (iconBitmap != nullptr)
            {
                DeleteObject(iconBitmap);
            }
            name = std::move(other.name);
            fullPath = std::move(other.fullPath);
            isDirectory = other.isDirectory;
            sysIconIndex = other.sysIconIndex;
            iconBitmap = other.iconBitmap;
            iconBitmapSize = other.iconBitmapSize;
            selected = other.selected;
            isCut = other.isCut;
            shortcutArrow = other.shortcutArrow;
            iconState = other.iconState;
            other.iconBitmap = nullptr;
            other.iconBitmapSize = {};
        }
        return *this;
    }
};

/**
 * @brief 桌面组件
 * @details 表示桌面上的一个组件实例，可以是集合、文件分类、文件夹映射或 Lua 脚本组件。
 *          包含位置、尺寸、滚动状态及内部条目列表等完整状态信息
 */
struct DesktopWidget
{
    std::wstring id;
    DesktopWidgetType type = DesktopWidgetType::Collection;
    std::wstring title;
    std::wstring sourceFolderPath;
    GridCell gridCell;
    GridSpan gridSpan;
    GridSpan minGridSpan{ 1, 1 };
    GridSpan maxGridSpan{ 0, 0 }; // 0 means unrestricted (up to the current grid page)
    RECT bounds{};
    float cellScale = 1.0f; // Runtime layout cache; recalculated by LayoutItems().
    bool selected = false;
    bool autoCollect = false;
    bool listMode = false;
    bool showTitle = false;
    bool bottomBarHover = true;
    int scrollOffset = 0;
    int tabScrollOffset = 0;
    std::wstring activeCategoryId;
    std::wstring scriptPath;
    bool showOnHoverOnly = false;
    bool scrollContainerMode = false;
    bool userRenamed = false;
    std::vector<std::wstring> itemKeys;
    std::vector<FolderEntry> folderEntries;
};

/**
 * @brief 桌面窗口句柄集合
 * @details 缓存桌面相关的重要窗口句柄，包括 Progman、DefView、ListView 等
 */
struct DesktopWindows
{
    HWND progman = nullptr;
    HWND defView = nullptr;
    HWND listView = nullptr;
    HWND host = nullptr;
    bool listViewWasVisible = false;
};

/**
 * @brief DefView 搜索结果
 * @details 存储通过 FindWindow 搜索 DefView 窗口的结果
 */
struct DefViewSearch
{
    HWND defView = nullptr;
    HWND parent = nullptr;
};

/**
 * @brief 显示器枚举上下文
 * @details 在枚举显示器时传递的上下文参数，用于收集所有显示器的虚拟坐标和网格页面信息
 */
struct MonitorEnumContext
{
    int virtualLeft = 0;
    int virtualTop = 0;
    std::vector<GridPage>* pages = nullptr;
};
