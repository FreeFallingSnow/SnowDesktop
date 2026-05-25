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

    DesktopItem() = default;

    DesktopItem(DesktopItem&& other) noexcept
        : name(std::move(other.name)),
          parsingName(std::move(other.parsingName)),
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
          selected(other.selected)
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
