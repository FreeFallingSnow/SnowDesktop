#pragma once
#include <shlobj.h>

#include "constants.h"

#include <string>
#include <vector>

struct GridCell
{
    std::wstring pageId;
    int column = 0;
    int row = 0;
};

struct GridSpan
{
    int columns = 1;
    int rows = 1;
};

struct LayoutRecord
{
    GridCell cell;
    GridSpan span;
    bool hasGrid = false;
    int legacySlot = -1;
};

struct GridPage
{
    std::wstring id;
    std::wstring monitorId;
    RECT bounds{};
    RECT workArea{};
    bool isPrimary = false;
    int columns = 1;
    int rows = 1;
    int cellWidth = kCellWidth;
    int cellHeight = kMinCellHeight;
    int gapX = 0;
    int gapY = 0;
    int marginX = kGridMarginX;
    int marginY = kGridMarginY;
};

struct PendingGridMove
{
    size_t index = 0;
    GridCell cell;
};

struct PendingWidgetMove
{
    size_t index = 0;
    GridCell cell;
};

enum class DesktopWidgetType
{
    Collection,
    FileCategories,
    FolderMapping,
};

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
          shortcutArrow(other.shortcutArrow)
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

struct FolderEntry
{
    std::wstring name;
    std::wstring fullPath;
    bool isDirectory = false;
    int sysIconIndex = -1;
    HBITMAP iconBitmap = nullptr;
    SIZE iconBitmapSize{};
    bool selected = false;

    FolderEntry() = default;

    FolderEntry(const FolderEntry& other)
        : name(other.name),
          fullPath(other.fullPath),
          isDirectory(other.isDirectory),
          sysIconIndex(other.sysIconIndex),
          iconBitmap(nullptr),
          iconBitmapSize(other.iconBitmapSize),
          selected(other.selected)
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
          selected(other.selected)
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
            other.iconBitmap = nullptr;
            other.iconBitmapSize = {};
        }
        return *this;
    }
};

struct DesktopWidget
{
    std::wstring id;
    DesktopWidgetType type = DesktopWidgetType::Collection;
    std::wstring title;
    std::wstring sourceFolderPath;
    GridCell gridCell;
    GridSpan gridSpan;
    RECT bounds{};
    bool selected = false;
    bool autoCollect = false;
    bool listMode = false;
    int scrollOffset = 0;
    int tabScrollOffset = 0;
    std::wstring activeCategoryId;
    std::vector<std::wstring> itemKeys;
    std::vector<FolderEntry> folderEntries;
};

struct DesktopWindows
{
    HWND progman = nullptr;
    HWND defView = nullptr;
    HWND listView = nullptr;
    HWND host = nullptr;
    bool listViewWasVisible = false;
};

struct DefViewSearch
{
    HWND defView = nullptr;
    HWND parent = nullptr;
};

struct MonitorEnumContext
{
    int virtualLeft = 0;
    int virtualTop = 0;
    std::vector<GridPage>* pages = nullptr;
};
