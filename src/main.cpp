#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cwchar>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
constexpr wchar_t kWindowClassName[] = L"SnowDesktopNativeProofWindow";
constexpr wchar_t kHintWindowClassName[] = L"SnowDesktopDragHintWindow";
constexpr wchar_t kHiddenBySnowDesktopProp[] = L"SnowDesktop.HiddenExplorerIconLayer";
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);
constexpr int kIconSize = 64;
constexpr int kCellWidth = 92;
constexpr int kMinCellHeight = 136;
constexpr int kGridMarginX = 6;
constexpr int kGridMarginY = 6;
constexpr int kMarginX = kGridMarginX;
constexpr int kMarginY = 6;
constexpr int kTextTop = 70;
constexpr int kTextCollapsedHeight = 34;
constexpr int kTextExpandedHeight = 58;
constexpr int kTextHeight = kTextCollapsedHeight;
constexpr float kGapPercentX = 0.16f;
constexpr float kGapPercentY = 0.14f;
constexpr int kRenameEditId = 1001;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kTrayIconId = 1;
constexpr UINT kTrayReloadCommand = 40001;
constexpr UINT kTraySortByNameCommand = 40002;
constexpr UINT kTraySwitchNativeCommand = 40003;
constexpr UINT kTraySwitchCustomCommand = 40004;
constexpr UINT kTrayExitCommand = 40005;
constexpr UINT kTraySortByTypeCommand = 40006;
constexpr UINT kTrayDesktopIconThisPC = 40007;
constexpr UINT kTrayDesktopIconUserFiles = 40008;
constexpr UINT kTrayDesktopIconNetwork = 40009;
constexpr UINT kTrayDesktopIconControlPanel = 40010;
constexpr UINT kTrayDesktopIconRecycleBin = 40011;
constexpr wchar_t kDesktopIconClsidThisPC[] = L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
constexpr wchar_t kDesktopIconClsidUserFiles[] = L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}";
constexpr wchar_t kDesktopIconClsidNetwork[] = L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}";
constexpr wchar_t kDesktopIconClsidControlPanel[] = L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}";
constexpr wchar_t kDesktopIconClsidRecycleBin[] = L"{645FF040-5081-101B-9F08-00AA002F954E}";
constexpr UINT kContextOpenCommand = 41001;
constexpr UINT kContextRenameCommand = 41002;
constexpr UINT kContextCutCommand = 41003;
constexpr UINT kContextCopyCommand = 41004;
constexpr UINT kContextPasteCommand = 41005;
constexpr UINT kContextDeleteCommand = 41006;
constexpr UINT kContextRefreshCommand = 41007;
constexpr UINT kContextSortByNameCommand = 41008;
constexpr UINT kContextSortByTypeCommand = 41009;
constexpr UINT kContextMoreCommand = 41010;
constexpr UINT kContextThisDisplayFirstCommand = 41011;
constexpr UINT kContextGridAddRow = 41012;
constexpr UINT kContextGridRemoveRow = 41013;
constexpr UINT kContextGridAddColumn = 41014;
constexpr UINT kContextGridRemoveColumn = 41015;
constexpr UINT kContextZoomIncrease = 41016;
constexpr UINT kContextZoomDecrease = 41017;
constexpr UINT kContextZoomPresetFirst = 41150;

constexpr UINT kShellChangeMessage = WM_APP + 2;
constexpr UINT_PTR kShellChangeTimerId = 2;
constexpr UINT kShellChangeDebounceMs = 500;
constexpr UINT_PTR kRecycleBinPollTimerId = 3;
constexpr UINT kRecycleBinPollIntervalMs = 2000;

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

BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lParam)
{
    auto* search = reinterpret_cast<DefViewSearch*>(lParam);
    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView != nullptr)
    {
        search->defView = defView;
        search->parent = hwnd;
        return FALSE;
    }
    return TRUE;
}

struct MonitorEnumContext
{
    int virtualLeft = 0;
    int virtualTop = 0;
    std::vector<GridPage>* pages = nullptr;
};

BOOL CALLBACK EnumGridPageMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam)
{
    auto* context = reinterpret_cast<MonitorEnumContext*>(lParam);
    if (context == nullptr || context->pages == nullptr)
    {
        return TRUE;
    }

    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        return TRUE;
    }

    GridPage page;
    page.monitorId = monitorInfo.szDevice[0] != L'\0'
        ? monitorInfo.szDevice
        : (L"Monitor" + std::to_wstring(context->pages->size()));
    page.id = page.monitorId;
    page.isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
    page.bounds = {
        monitorInfo.rcMonitor.left - context->virtualLeft,
        monitorInfo.rcMonitor.top - context->virtualTop,
        monitorInfo.rcMonitor.right - context->virtualLeft,
        monitorInfo.rcMonitor.bottom - context->virtualTop,
    };
    page.workArea = {
        monitorInfo.rcWork.left - context->virtualLeft,
        monitorInfo.rcWork.top - context->virtualTop,
        monitorInfo.rcWork.right - context->virtualLeft,
        monitorInfo.rcWork.bottom - context->virtualTop,
    };

    context->pages->push_back(page);
    return TRUE;
}

HICON LoadAppIcon()
{
    constexpr int kSize = 32;
    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = kSize;
    bmi.bmiHeader.biHeight = -kSize;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;

    void* bits = nullptr;
    HBITMAP colorBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP maskBmp = CreateBitmap(kSize, kSize, 1, 1, nullptr);
    HGDIOBJ oldBmp = SelectObject(memDc, colorBmp);

    HBRUSH bg = CreateSolidBrush(RGB(60, 130, 220));
    RECT rc = {0, 0, kSize, kSize};
    FillRect(memDc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(memDc, TRANSPARENT);
    SetTextColor(memDc, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(memDc, GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(memDc, L"S", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(memDc, oldFont);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = colorBmp;
    ii.hbmMask = maskBmp;
    HICON icon = CreateIconIndirect(&ii);

    SelectObject(memDc, oldBmp);
    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);
    return icon;
}

DesktopWindows FindDesktopWindows()
{
    DesktopWindows result{};
    result.progman = FindWindowW(L"Progman", nullptr);

    if (result.progman != nullptr)
    {
        DWORD_PTR unused = 0;
        SendMessageTimeoutW(result.progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &unused);
    }

    DefViewSearch search{};
    if (result.progman != nullptr)
    {
        search.defView = FindWindowExW(result.progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (search.defView != nullptr)
        {
            search.parent = result.progman;
        }
    }

    if (search.defView == nullptr)
    {
        EnumWindows(FindDefViewProc, reinterpret_cast<LPARAM>(&search));
    }

    result.defView = search.defView;
    result.host = search.parent != nullptr ? search.parent : result.progman;

    if (result.defView != nullptr)
    {
        result.listView = FindWindowExW(result.defView, nullptr, L"SysListView32", nullptr);
    }

    return result;
}

void RestoreExplorerIconLayerNow()
{
    DesktopWindows windows = FindDesktopWindows();
    if (windows.listView != nullptr && IsWindow(windows.listView))
    {
        ShowWindow(windows.listView, SW_SHOW);
        RemovePropW(windows.listView, kHiddenBySnowDesktopProp);
    }
}

std::wstring StrRetToString(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags)
{
    STRRET str{};
    if (FAILED(folder->GetDisplayNameOf(child, flags, &str)))
    {
        return {};
    }

    wchar_t buffer[MAX_PATH * 4]{};
    if (FAILED(StrRetToBufW(&str, child, buffer, static_cast<UINT>(std::size(buffer)))))
    {
        return {};
    }

    return buffer;
}

std::wstring ToUpperInvariant(std::wstring value)
{
    CharUpperBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring ExtractClsidText(const std::wstring& parsingName)
{
    size_t open = parsingName.find(L'{');
    size_t close = parsingName.find(L'}', open == std::wstring::npos ? 0 : open);
    if (open == std::wstring::npos || close == std::wstring::npos || close <= open)
    {
        return {};
    }

    return ToUpperInvariant(parsingName.substr(open, close - open + 1));
}

std::wstring TrimTrailingPathSeparators(std::wstring path)
{
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/'))
    {
        path.pop_back();
    }
    return path;
}

bool PathsEqualInsensitive(std::wstring left, std::wstring right)
{
    left = TrimTrailingPathSeparators(std::move(left));
    right = TrimTrailingPathSeparators(std::move(right));
    if (left.empty() || right.empty())
    {
        return false;
    }
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring ResolveDesktopIconClsid(
    const std::wstring& parsingName,
    const std::wstring& itemPath,
    const std::wstring& userProfilePath)
{
    std::wstring clsid = ExtractClsidText(parsingName);
    if (!clsid.empty())
    {
        return clsid;
    }

    if (PathsEqualInsensitive(itemPath, userProfilePath))
    {
        return kDesktopIconClsidUserFiles;
    }

    return {};
}

bool TryReadDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD& value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS &&
        RegOpenKeyExW(root, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return false;
    }

    DWORD type = 0;
    DWORD data = 0;
    DWORD dataSize = sizeof(data);
    LONG status = RegQueryValueExW(key, clsid.c_str(), nullptr, &type, reinterpret_cast<BYTE*>(&data), &dataSize);
    RegCloseKey(key);

    if (status == ERROR_SUCCESS && type == REG_DWORD && dataSize == sizeof(data))
    {
        value = data;
        return true;
    }

    return false;
}

bool TryWriteDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS &&
        RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        return false;
    }
    LONG status = RegSetValueExW(key, clsid.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

void WriteDesktopIconRegistryValue(const std::wstring& clsid, bool visible)
{
    DWORD value = visible ? 0 : 1;
    TryWriteDesktopIconRegistryValue(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel",
        clsid, value);
}

bool TryReadDesktopIconRegistryValueAnyRoot(const std::wstring& clsid, DWORD& value)
{
    static const wchar_t* keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\ClassicStartMenu",
    };

    for (const wchar_t* key : keys)
    {
        if (TryReadDesktopIconRegistryValue(HKEY_CURRENT_USER, key, clsid, value))
        {
            return true;
        }
    }

    for (const wchar_t* key : keys)
    {
        if (TryReadDesktopIconRegistryValue(HKEY_LOCAL_MACHINE, key, clsid, value))
        {
            return true;
        }
    }

    return false;
}

bool IsVisibleByDesktopIconSettings(const std::wstring& desktopIconClsid, const std::unordered_map<std::wstring, bool>& settingsIconVisibility)
{
    std::wstring clsid = ToUpperInvariant(desktopIconClsid);
    if (clsid.empty())
    {
        return true;
    }

    auto settingsIt = settingsIconVisibility.find(clsid);
    if (settingsIt != settingsIconVisibility.end())
    {
        return settingsIt->second;
    }

    DWORD value = 0;
    if (TryReadDesktopIconRegistryValueAnyRoot(clsid, value))
    {
        return value == 0;
    }

    static const std::unordered_map<std::wstring, bool> defaultVisibility = {
        { kDesktopIconClsidThisPC, false },
        { kDesktopIconClsidUserFiles, false },
        { kDesktopIconClsidNetwork, false },
        { kDesktopIconClsidControlPanel, false },
        { kDesktopIconClsidRecycleBin, true },
    };

    auto found = defaultVisibility.find(clsid);
    if (found != defaultVisibility.end())
    {
        return found->second;
    }

    return false;
}

HBITMAP CreateTopDown32BppDib(HDC referenceDc, int width, int height, void** bits)
{
    if (width <= 0 || height <= 0)
    {
        return nullptr;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    return CreateDIBSection(referenceDc, &bitmapInfo, DIB_RGB_COLORS, bits, nullptr, 0);
}

void PremultiplyBgraPixels(std::uint32_t* pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    bool needsPremultiply = false;
    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
        if (a == 0 || a == 255)
        {
            continue;
        }

        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        if (r > a || g > a || b > a)
        {
            needsPremultiply = true;
            break;
        }
    }

    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);

        if (a == 0)
        {
            pixels[i] = 0;
            continue;
        }

        if (needsPremultiply && a < 255)
        {
            r = static_cast<std::uint8_t>((static_cast<int>(r) * a + 127) / 255);
            g = static_cast<std::uint8_t>((static_cast<int>(g) * a + 127) / 255);
            b = static_cast<std::uint8_t>((static_cast<int>(b) * a + 127) / 255);
            pixels[i] = (static_cast<std::uint32_t>(a) << 24) |
                (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b);
        }
    }
}

HBITMAP CopyBitmapToAlphaDib(HBITMAP source, SIZE& size)
{
    size = {};
    if (source == nullptr)
    {
        return nullptr;
    }

    BITMAP sourceInfo{};
    if (GetObjectW(source, sizeof(sourceInfo), &sourceInfo) == 0 ||
        sourceInfo.bmWidth <= 0 ||
        sourceInfo.bmHeight <= 0)
    {
        return nullptr;
    }

    const int width = sourceInfo.bmWidth;
    const int height = sourceInfo.bmHeight;
    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr)
    {
        return nullptr;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
    if (GetDIBits(screenDc, source, 0, static_cast<UINT>(height), pixels.data(), &bitmapInfo, DIB_RGB_COLORS) == 0)
    {
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    bool hasAlpha = false;
    bool hasVisiblePixels = false;
    for (std::uint32_t pixel : pixels)
    {
        std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
        if (a != 0)
        {
            hasAlpha = true;
        }
        if ((pixel & 0x00ffffff) != 0)
        {
            hasVisiblePixels = true;
        }
    }

    if (!hasAlpha && hasVisiblePixels)
    {
        for (std::uint32_t& pixel : pixels)
        {
            pixel |= 0xff000000;
        }
    }

    PremultiplyBgraPixels(pixels.data(), width, height);

    void* bits = nullptr;
    HBITMAP copy = CreateTopDown32BppDib(screenDc, width, height, &bits);
    if (copy != nullptr && bits != nullptr)
    {
        std::copy(pixels.begin(), pixels.end(), static_cast<std::uint32_t*>(bits));
        size = { width, height };
    }
    ReleaseDC(nullptr, screenDc);
    return copy;
}

HBITMAP CreateAlphaBitmapFromIcon(HICON icon, int width, int height, SIZE& size)
{
    size = {};
    if (icon == nullptr || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr)
    {
        return nullptr;
    }

    void* bits = nullptr;
    HBITMAP bitmap = CreateTopDown32BppDib(screenDc, width, height, &bits);
    if (bitmap == nullptr || bits == nullptr)
    {
        if (bitmap != nullptr)
        {
            DeleteObject(bitmap);
        }
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    std::fill(pixels, pixels + (static_cast<size_t>(width) * static_cast<size_t>(height)), 0);

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    DrawIconEx(memoryDc, 0, 0, icon, width, height, 0, nullptr, DI_NORMAL);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);

    bool hasAlpha = false;
    bool hasVisiblePixels = false;
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < count; ++i)
    {
        std::uint32_t pixel = pixels[i];
        if (((pixel >> 24) & 0xff) != 0)
        {
            hasAlpha = true;
        }
        if ((pixel & 0x00ffffff) != 0)
        {
            hasVisiblePixels = true;
        }
    }

    if (!hasAlpha && hasVisiblePixels)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if ((pixels[i] & 0x00ffffff) != 0)
            {
                pixels[i] |= 0xff000000;
            }
        }
    }
    PremultiplyBgraPixels(pixels, width, height);

    size = { width, height };
    ReleaseDC(nullptr, screenDc);
    return bitmap;
}

HBITMAP GetHighResolutionShellIconBitmap(PCIDLIST_ABSOLUTE pidl, int fallbackIndex, SIZE& bitmapSize)
{
    bitmapSize = {};
    ComPtr<IShellItemImageFactory> imageFactory;
    if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&imageFactory))) && imageFactory)
    {
        SIZE size{ kIconSize, kIconSize };
        HBITMAP bitmap = nullptr;
        if (SUCCEEDED(imageFactory->GetImage(size, SIIGBF_ICONONLY, &bitmap)) && bitmap != nullptr)
        {
            HBITMAP alphaBitmap = CopyBitmapToAlphaDib(bitmap, bitmapSize);
            DeleteObject(bitmap);
            if (alphaBitmap != nullptr)
            {
                return alphaBitmap;
            }
        }
    }

    ComPtr<IImageList> imageList;
    HRESULT hr = SHGetImageList(SHIL_JUMBO, __uuidof(IImageList), reinterpret_cast<void**>(imageList.GetAddressOf()));
    if (FAILED(hr) || !imageList)
    {
        imageList.Reset();
        hr = SHGetImageList(SHIL_EXTRALARGE, __uuidof(IImageList), reinterpret_cast<void**>(imageList.GetAddressOf()));
    }
    if (FAILED(hr) || !imageList)
    {
        imageList.Reset();
        hr = SHGetImageList(SHIL_LARGE, __uuidof(IImageList), reinterpret_cast<void**>(imageList.GetAddressOf()));
    }

    HICON icon = nullptr;
    if (SUCCEEDED(hr) && imageList && fallbackIndex >= 0)
    {
        imageList->GetIcon(fallbackIndex, ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon);
        if (icon != nullptr)
        {
            HBITMAP bitmap = CreateAlphaBitmapFromIcon(icon, kIconSize, kIconSize, bitmapSize);
            DestroyIcon(icon);
            if (bitmap != nullptr)
            {
                return bitmap;
            }
        }
    }

    SHFILEINFOW iconInfo{};
    SHGetFileInfoW(
        reinterpret_cast<LPCWSTR>(pidl),
        0,
        &iconInfo,
        sizeof(iconInfo),
        SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON | SHGFI_ADDOVERLAYS);
    icon = iconInfo.hIcon;
    if (icon != nullptr)
    {
        HBITMAP bitmap = CreateAlphaBitmapFromIcon(icon, kIconSize, kIconSize, bitmapSize);
        DestroyIcon(icon);
        return bitmap;
    }

    return nullptr;
}

RECT MakeRect(int left, int top, int right, int bottom)
{
    RECT r{ left, top, right, bottom };
    return r;
}

bool RectsIntersect(const RECT& a, const RECT& b)
{
    RECT tmp{};
    return IntersectRect(&tmp, &a, &b) != FALSE;
}

RECT NormalizeRect(POINT a, POINT b)
{
    return MakeRect(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
}

bool IsRectEmptyRect(const RECT& rect)
{
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

RECT InflateCopy(RECT rect, int amount)
{
    InflateRect(&rect, amount, amount);
    return rect;
}

RECT UnionCopy(const RECT& a, const RECT& b)
{
    if (IsRectEmptyRect(a))
    {
        return b;
    }
    if (IsRectEmptyRect(b))
    {
        return a;
    }

    RECT result{};
    UnionRect(&result, &a, &b);
    return result;
}

std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }

    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string JsonEscapeUtf8(const std::wstring& value)
{
    std::string input = WideToUtf8(value);
    std::string output;
    output.reserve(input.size() + 16);
    for (unsigned char ch : input)
    {
        switch (ch)
        {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output.push_back(static_cast<char>(ch));
            break;
        }
    }
    return output;
}

bool ParseJsonStringAt(const std::string& text, size_t quote, std::string& value, size_t& end)
{
    if (quote >= text.size() || text[quote] != '"')
    {
        return false;
    }

    value.clear();
    for (size_t i = quote + 1; i < text.size(); ++i)
    {
        char ch = text[i];
        if (ch == '"')
        {
            end = i + 1;
            return true;
        }

        if (ch == '\\' && i + 1 < text.size())
        {
            char escaped = text[++i];
            switch (escaped)
            {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(escaped);
                break;
            }
        }
        else
        {
            value.push_back(ch);
        }
    }

    return false;
}

class SnowDesktopApp : public IDropTarget
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }

        if (riid == IID_IUnknown || riid == IID_IDropTarget)
        {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }

        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&refCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG count = InterlockedDecrement(&refCount_);
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override
    {
        if (effect == nullptr)
        {
            return E_POINTER;
        }

        externalDragActive_ = true;
        POINT oldPoint = externalDragPoint_;
        externalDragPoint_ = ScreenPointToClient(point);
        externalDragHint_ = MakeExternalDragHint(externalDragPoint_);
        ShowDragHintWindow(externalDragPoint_, externalDragHint_);
        *effect = ChooseDropEffect(keyState, *effect);
        InvalidateFast(UnionCopy(GetExternalDragDirtyRect(oldPoint), GetExternalDragDirtyRect(externalDragPoint_)));
        UNREFERENCED_PARAMETER(dataObject);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState, POINTL point, DWORD* effect) override
    {
        if (effect == nullptr)
        {
            return E_POINTER;
        }

        RECT oldDirty = GetExternalDragDirtyRect(externalDragPoint_);
        externalDragActive_ = true;
        externalDragPoint_ = ScreenPointToClient(point);
        externalDragHint_ = MakeExternalDragHint(externalDragPoint_);
        ShowDragHintWindow(externalDragPoint_, externalDragHint_);
        *effect = ChooseDropEffect(keyState, *effect);
        InvalidateFast(UnionCopy(oldDirty, GetExternalDragDirtyRect(externalDragPoint_)));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        RECT oldDirty = GetExternalDragDirtyRect(externalDragPoint_);
        externalDragActive_ = false;
        externalDragHint_.clear();
        HideDragHintWindow();
        InvalidateFast(oldDirty);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect) override
    {
        if (effect == nullptr)
        {
            return E_POINTER;
        }

        RECT oldDirty = GetExternalDragDirtyRect(externalDragPoint_);
        externalDragActive_ = false;
        externalDragHint_.clear();
        HideDragHintWindow();
        POINT clientPoint = ScreenPointToClient(point);
        DWORD localEffect = *effect;
        HRESULT hr = DropDataObjectAt(dataObject, clientPoint, keyState, &localEffect);
        *effect = localEffect;
        InvalidateFast(oldDirty);
        ReloadItems();
        return hr;
    }

    int Run(HINSTANCE instance, int showCommand, UINT smokeTestMs = 0)
    {
        instance_ = instance;

        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        INITCOMMONCONTROLSEX icc{};
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);

        HRESULT hr = OleInitialize(nullptr);
        if (FAILED(hr))
        {
            MessageBoxW(nullptr, L"OleInitialize failed.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        if (!InitializeGraphics())
        {
            wchar_t message[256]{};
            swprintf_s(
                message,
                L"Unable to initialize Direct2D/DirectComposition.\nHRESULT=0x%08lX",
                static_cast<unsigned long>(lastGraphicsError_));
            MessageBoxW(nullptr, message, L"SnowDesktop", MB_ICONERROR);
            OleUninitialize();
            return 1;
        }

        const auto cleanup = std::unique_ptr<void, void (*)(void*)>(
            this,
            [](void* ctx)
            {
                auto* self = static_cast<SnowDesktopApp*>(ctx);
                self->UnregisterOleDropTarget();
                self->RestoreExplorerIcons();
                OleUninitialize();
            });

        if (!InitializeShell())
        {
            MessageBoxW(nullptr, L"Unable to initialize the Shell desktop folder.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }
        LoadLayoutSlots();

        desktopWindows_ = FindDesktopWindows();
        if (desktopWindows_.listView != nullptr)
        {
            bool hiddenByPreviousRun = GetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp) != nullptr;
            if (hiddenByPreviousRun && !IsWindowVisible(desktopWindows_.listView))
            {
                ShowWindow(desktopWindows_.listView, SW_SHOW);
            }

            desktopWindows_.listViewWasVisible = IsWindowVisible(desktopWindows_.listView) != FALSE || hiddenByPreviousRun;
            if (desktopWindows_.listViewWasVisible)
            {
                SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1));
                ShowWindow(desktopWindows_.listView, SW_HIDE);
            }
        }

        if (!RegisterWindowClass())
        {
            MessageBoxW(nullptr, L"Unable to register the SnowDesktop window class.", L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        if (!CreateDesktopWindow(showCommand))
        {
            wchar_t message[256]{};
            if (FAILED(lastGraphicsError_))
            {
                swprintf_s(
                    message,
                    L"Unable to create the SnowDesktop composition target.\nHRESULT=0x%08lX",
                    static_cast<unsigned long>(lastGraphicsError_));
            }
            else
            {
                swprintf_s(
                    message,
                    L"Unable to create the SnowDesktop desktop window.\nGetLastError=%lu",
                    lastCreateWindowError_);
            }
            MessageBoxW(nullptr, message, L"SnowDesktop", MB_ICONERROR);
            return 1;
        }

        if (smokeTestMs > 0)
        {
            SetTimer(hwnd_, 1, smokeTestMs, nullptr);
        }

        RegisterOleDropTarget();
        AddTrayIcon();
        ReloadItems();

        SHChangeNotifyEntry entries[2]{};
        entries[0].pidl = desktopPidl_.get();
        entries[0].fRecursive = FALSE;
        PIDLIST_ABSOLUTE rbPidl = nullptr;
        if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &rbPidl)))
        {
            recycleBinPidl_.reset(rbPidl);
            entries[1].pidl = recycleBinPidl_.get();
            entries[1].fRecursive = TRUE;
        }
        const int entryCount = recycleBinPidl_.get() != nullptr ? 2 : 1;
        shellChangeRegId_ = SHChangeNotifyRegister(
            hwnd_,
            SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
            SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
            SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
            SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_ASSOCCHANGED,
            kShellChangeMessage,
            entryCount,
            entries);

        SetTimer(hwnd_, kRecycleBinPollTimerId, kRecycleBinPollIntervalMs, nullptr);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        return static_cast<int>(msg.wParam);
    }

private:
    bool InitializeGraphics()
    {
        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
        };

        D3D_FEATURE_LEVEL actualFeatureLevel{};
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &d3dDevice_,
            &actualFeatureLevel,
            nullptr);

        if (FAILED(hr))
        {
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                &d3dDevice_,
                &actualFeatureLevel,
                nullptr);
        }
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = d3dDevice_.As(&dxgiDevice);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        D2D1_FACTORY_OPTIONS factoryOptions{};
        hr = D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &factoryOptions,
            reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &bitmapContext_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        bitmapContext_->SetDpi(96.0f, 96.0f);
        bitmapContext_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

        hr = DCompositionCreateDevice2(
            d2dDevice_.Get(),
            __uuidof(IDCompositionDesktopDevice),
            reinterpret_cast<void**>(dcompDevice_.GetAddressOf()));
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            15.0f,
            L"",
            &itemTextFormat_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        itemTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        itemTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        itemTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        itemTextFormat_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.0f, 13.0f);

        hr = dwriteFactory_->CreateTextFormat(
            L"Segoe UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            14.0f,
            L"",
            &statusTextFormat_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }
        statusTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        statusTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        statusTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        D2D1_STROKE_STYLE_PROPERTIES dottedProps{};
        dottedProps.startCap = D2D1_CAP_STYLE_FLAT;
        dottedProps.endCap = D2D1_CAP_STYLE_FLAT;
        dottedProps.dashCap = D2D1_CAP_STYLE_FLAT;
        dottedProps.lineJoin = D2D1_LINE_JOIN_MITER;
        dottedProps.miterLimit = 10.0f;
        dottedProps.dashStyle = D2D1_DASH_STYLE_DOT;
        dottedProps.dashOffset = 0.0f;
        d2dFactory_->CreateStrokeStyle(dottedProps, nullptr, 0, &dottedStrokeStyle_);

        lastGraphicsError_ = S_OK;
        return true;
    }

    bool InitializeShell()
    {
        if (FAILED(SHGetDesktopFolder(&desktopFolder_)))
        {
            return false;
        }

        PIDLIST_ABSOLUTE desktopPidl = nullptr;
        if (FAILED(SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &desktopPidl)))
        {
            return false;
        }

        desktopPidl_.reset(desktopPidl);

        SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&sysImageList_));

        return true;
    }

    bool RegisterWindowClass()
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SnowDesktopApp::WindowProcThunk;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWindowClassName;
        wc.style = CS_DBLCLKS;

        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW hint{};
        hint.cbSize = sizeof(hint);
        hint.lpfnWndProc = DefWindowProcW;
        hint.hInstance = instance_;
        hint.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hint.hbrBackground = nullptr;
        hint.lpszClassName = kHintWindowClassName;

        return RegisterClassExW(&hint) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    bool CreateDesktopWindow(int showCommand)
    {
        virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
        virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
        virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        UpdateLayoutWorkArea();

        HWND parent = desktopWindows_.host != nullptr ? desktopWindows_.host : GetDesktopWindow();
        POINT origin{ virtualLeft_, virtualTop_ };
        ScreenToClient(parent, &origin);
        bool attachedToDesktopHost = true;

        DWORD exStyle = WS_EX_TOOLWINDOW;
        hwnd_ = CreateWindowExW(
            exStyle,
            kWindowClassName,
            L"SnowDesktop",
            WS_CHILD | WS_VISIBLE,
            origin.x,
            origin.y,
            virtualWidth_,
            virtualHeight_,
            parent,
            nullptr,
            instance_,
            this);

        if (hwnd_ == nullptr)
        {
            lastCreateWindowError_ = GetLastError();

            hwnd_ = CreateWindowExW(
                exStyle,
                kWindowClassName,
                L"SnowDesktop",
                WS_POPUP | WS_VISIBLE,
                virtualLeft_,
                virtualTop_,
                virtualWidth_,
                virtualHeight_,
                nullptr,
                nullptr,
                instance_,
                this);

            if (hwnd_ == nullptr)
            {
                lastCreateWindowError_ = GetLastError();
                return false;
            }

            if (parent != nullptr && parent != GetDesktopWindow())
            {
                SetLastError(ERROR_SUCCESS);
                SetParent(hwnd_, parent);
                if (GetLastError() == ERROR_SUCCESS)
                {
                    LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
                    style &= ~WS_POPUP;
                    style |= WS_CHILD | WS_VISIBLE;
                    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
                    SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                }
                else
                {
                    attachedToDesktopHost = false;
                }
            }
            else
            {
                attachedToDesktopHost = false;
            }
        }

        if (attachedToDesktopHost)
        {
            SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y, virtualWidth_, virtualHeight_, SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowPos(hwnd_, HWND_BOTTOM, virtualLeft_, virtualTop_, virtualWidth_, virtualHeight_, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        ShowWindow(hwnd_, showCommand);
        UpdateWindow(hwnd_);

        HICON appIcon = LoadAppIcon();
        if (appIcon != nullptr)
        {
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
        }
        return CreateCompositionTarget();
    }

    bool CreateCompositionTarget()
    {
        if (!dcompDevice_)
        {
            return false;
        }

        HRESULT hr = dcompDevice_->CreateTargetForHwnd(hwnd_, FALSE, &dcompTarget_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dcompDevice_->CreateVisual(&dcompVisual_);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = CreateOrResizeCompositionSurface();
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dcompTarget_->SetRoot(dcompVisual_.Get());
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        hr = dcompDevice_->Commit();
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return false;
        }

        lastGraphicsError_ = S_OK;
        return true;
    }

    HRESULT CreateOrResizeCompositionSurface()
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const UINT width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        const UINT height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));

        if (dcompSurface_ && compositionWidth_ == width && compositionHeight_ == height)
        {
            return S_OK;
        }

        ComPtr<IDCompositionSurface> surface;
        HRESULT hr = dcompDevice_->CreateSurface(
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED,
            &surface);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = dcompVisual_->SetContent(surface.Get());
        if (FAILED(hr))
        {
            return hr;
        }

        dcompSurface_ = surface;
        compositionWidth_ = width;
        compositionHeight_ = height;
        return S_OK;
    }

    void HideExplorerIcons()
    {
        if (desktopWindows_.listView != nullptr && IsWindow(desktopWindows_.listView))
        {
            desktopWindows_.listViewWasVisible = true;
            SetPropW(desktopWindows_.listView, kHiddenBySnowDesktopProp, reinterpret_cast<HANDLE>(1));
            ShowWindow(desktopWindows_.listView, SW_HIDE);
        }
    }

    void RestoreExplorerIcons()
    {
        if (desktopWindows_.listView != nullptr && IsWindow(desktopWindows_.listView) && desktopWindows_.listViewWasVisible)
        {
            ShowWindow(desktopWindows_.listView, SW_SHOW);
            RemovePropW(desktopWindows_.listView, kHiddenBySnowDesktopProp);
        }
    }

    void SwitchToNativeDesktop()
    {
        SaveLayoutSlots();
        HideDragHintWindow();
        RestoreExplorerIcons();
        if (hwnd_ != nullptr)
        {
            ShowWindow(hwnd_, SW_HIDE);
        }
        customDesktopVisible_ = false;
    }

    void SwitchToCustomDesktop()
    {
        HideExplorerIcons();
        if (hwnd_ != nullptr)
        {
            ShowWindow(hwnd_, SW_SHOW);
        }
        customDesktopVisible_ = true;
        ReloadItems();
    }

    bool EnsureDragHintWindow()
    {
        if (hintHwnd_ != nullptr)
        {
            return true;
        }

        hintHwnd_ = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            kHintWindowClassName,
            L"",
            WS_POPUP,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            instance_,
            nullptr);
        return hintHwnd_ != nullptr;
    }

    void HideDragHintWindow()
    {
        if (hintHwnd_ != nullptr)
        {
            ShowWindow(hintHwnd_, SW_HIDE);
        }
    }

    void DestroyDragHintWindow()
    {
        if (hintHwnd_ != nullptr)
        {
            DestroyWindow(hintHwnd_);
            hintHwnd_ = nullptr;
        }
    }

    void ShowDragHintWindow(POINT clientPoint, const std::wstring& text)
    {
        if (text.empty() || !EnsureDragHintWindow())
        {
            HideDragHintWindow();
            return;
        }

        POINT screenPoint = clientPoint;
        ClientToScreen(hwnd_, &screenPoint);

        HDC screenDc = GetDC(nullptr);
        HFONT font = CreateFontW(
            -15,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        HGDIOBJ oldScreenFont = SelectObject(screenDc, font);
        SIZE textSize{};
        GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
        SelectObject(screenDc, oldScreenFont);

        int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
        int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
        POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

        HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo))
        {
            windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8, monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
            windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8, monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr)
        {
            DeleteObject(font);
            ReleaseDC(nullptr, screenDc);
            HideDragHintWindow();
            return;
        }

        auto* pixels = static_cast<std::uint32_t*>(bits);
        auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
            return (static_cast<std::uint32_t>(a) << 24) |
                (static_cast<std::uint32_t>(r) << 16) |
                (static_cast<std::uint32_t>(g) << 8) |
                static_cast<std::uint32_t>(b);
        };

        const std::uint32_t background = argb(255, 255, 255, 255);
        const std::uint32_t border = argb(255, 205, 211, 220);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                bool isBorder = x == 0 || y == 0 || x == width - 1 || y == height - 1;
                pixels[(y * width) + x] = isBorder ? border : background;
            }
        }

        HDC memoryDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
        HGDIOBJ oldFont = SelectObject(memoryDc, font);
        SetBkMode(memoryDc, TRANSPARENT);
        SetTextColor(memoryDc, RGB(25, 32, 42));

        RECT textRect{ 10, 0, width - 10, height };
        DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        for (int i = 0; i < width * height; ++i)
        {
            std::uint32_t pixel = pixels[i];
            std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
            std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
            std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
            pixels[i] = argb(255, r, g, b);
        }

        POINT sourcePoint{ 0, 0 };
        SIZE windowSize{ width, height };
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
        ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

        SelectObject(memoryDc, oldFont);
        SelectObject(memoryDc, oldBitmap);
        DeleteDC(memoryDc);
        DeleteObject(bitmap);
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
    }

    void AddTrayIcon()
    {
        if (trayIconAdded_ || hwnd_ == nullptr)
        {
            return;
        }

        trayIcon_ = LoadAppIcon();

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = trayIcon_;
        wcscpy_s(data.szTip, L"SnowDesktop 桌面验证");

        trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
        if (trayIconAdded_)
        {
            data.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &data);
        }
    }

    void RegisterOleDropTarget()
    {
        if (hwnd_ != nullptr && !dropTargetRegistered_)
        {
            dropTargetRegistered_ = SUCCEEDED(RegisterDragDrop(hwnd_, static_cast<IDropTarget*>(this)));
        }
    }

    void UnregisterOleDropTarget()
    {
        if (hwnd_ != nullptr && dropTargetRegistered_)
        {
            RevokeDragDrop(hwnd_);
            dropTargetRegistered_ = false;
        }
    }

    void RemoveTrayIcon()
    {
        if (trayIconAdded_ && hwnd_ != nullptr)
        {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = hwnd_;
            data.uID = kTrayIconId;
            Shell_NotifyIconW(NIM_DELETE, &data);
            trayIconAdded_ = false;
        }

        if (trayIcon_ != nullptr)
        {
            DestroyIcon(trayIcon_);
            trayIcon_ = nullptr;
        }
    }

    bool IsClsidCurrentlyVisible(const std::wstring& clsid)
    {
        auto it = settingsIconVisibility_.find(clsid);
        if (it != settingsIconVisibility_.end())
        {
            return it->second;
        }

        DWORD value = 0;
        if (TryReadDesktopIconRegistryValueAnyRoot(clsid, value))
        {
            return value == 0;
        }

        static const std::unordered_map<std::wstring, bool> defaultVisibility = {
            { kDesktopIconClsidThisPC, false },
            { kDesktopIconClsidUserFiles, false },
            { kDesktopIconClsidNetwork, false },
            { kDesktopIconClsidControlPanel, false },
            { kDesktopIconClsidRecycleBin, true },
        };

        auto found = defaultVisibility.find(clsid);
        if (found != defaultVisibility.end())
        {
            return found->second;
        }

        return false;
    }

    void ToggleDesktopIconVisibility(UINT command)
    {
        static const std::unordered_map<UINT, std::wstring> commandToClsid = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin },
        };

        auto it = commandToClsid.find(command);
        if (it == commandToClsid.end())
        {
            return;
        }

        bool currentVisible = IsClsidCurrentlyVisible(it->second);
        bool newVisible = !currentVisible;
        if (newVisible)
            settingsIconVisibility_[it->second] = true;
        else
            settingsIconVisibility_.erase(it->second);
        WriteDesktopIconRegistryValue(it->second, newVisible);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSH, nullptr, nullptr);
        ReloadItems();
    }

    HBITMAP CreateMenuIconBitmap(COLORREF color, wchar_t glyph)
    {
        const int cx = GetSystemMetrics(SM_CXMENUCHECK);
        const int cy = GetSystemMetrics(SM_CYMENUCHECK);
        if (cx <= 0 || cy <= 0) return nullptr;

        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        HBITMAP bmp = CreateCompatibleBitmap(screenDc, cx, cy);
        HGDIOBJ oldBmp = SelectObject(memDc, bmp);
        HGDIOBJ oldFont = SelectObject(memDc, GetStockObject(DEFAULT_GUI_FONT));

        HBRUSH fill = CreateSolidBrush(color);
        RECT rc = {0, 0, cx, cy};
        FillRect(memDc, &rc, fill);
        DeleteObject(fill);

        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, RGB(255, 255, 255));
        DrawTextW(memDc, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDc, oldFont);
        SelectObject(memDc, oldBmp);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return bmp;
    }

    void SetMenuItemIcon(HMENU menu, UINT command, COLORREF color, wchar_t glyph)
    {
        HBITMAP icon = CreateMenuIconBitmap(color, glyph);
        if (icon == nullptr) return;

        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = icon;
        SetMenuItemInfoW(menu, command, FALSE, &mii);
        menuIconPool_.push_back(icon);
    }

    void ShowTrayMenu(POINT screenPoint)
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        HMENU iconSettingsMenu = CreatePopupMenu();
        if (iconSettingsMenu != nullptr)
        {
            struct IconSetting
            {
                UINT command;
                const wchar_t* clsid;
                const wchar_t* label;
            };
            const IconSetting settings[] = {
                { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC, L"计算机" },
                { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles, L"用户的文件" },
                { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork, L"网络" },
                { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel, L"控制面板" },
                { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin, L"回收站" },
            };

            for (const auto& setting : settings)
            {
                UINT flags = MF_STRING;
                if (IsClsidCurrentlyVisible(setting.clsid))
                {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(iconSettingsMenu, flags, setting.command, setting.label);
            }

            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconSettingsMenu), L"桌面图标设置");
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, customDesktopVisible_ ? MF_STRING : (MF_STRING | MF_GRAYED), kTraySwitchNativeCommand, L"切换原生桌面");
        AppendMenuW(menu, customDesktopVisible_ ? (MF_STRING | MF_GRAYED) : MF_STRING, kTraySwitchCustomCommand, L"切换自定义桌面");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出软件");

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);

        switch (command)
        {
        case kTrayReloadCommand:
            ReloadItems();
            break;
        case kTraySortByNameCommand:
            SortIconsByName();
            break;
        case kTraySortByTypeCommand:
            SortIconsByType();
            break;
        case kTraySwitchNativeCommand:
            SwitchToNativeDesktop();
            break;
        case kTraySwitchCustomCommand:
            SwitchToCustomDesktop();
            break;
        case kTrayExitCommand:
            DestroyWindow(hwnd_);
            break;
        case kTrayDesktopIconThisPC:
        case kTrayDesktopIconUserFiles:
        case kTrayDesktopIconNetwork:
        case kTrayDesktopIconControlPanel:
        case kTrayDesktopIconRecycleBin:
            ToggleDesktopIconVisibility(command);
            break;
        default:
            break;
        }

        if (iconSettingsMenu != nullptr)
        {
            DestroyMenu(iconSettingsMenu);
        }
        DestroyMenu(menu);
    }

    void ReloadItems(bool reloadLayoutFromDisk = true)
    {
        if (reloading_)
        {
            return;
        }
        reloading_ = true;

        if (reloadLayoutFromDisk)
        {
            LoadLayoutSlots();
        }

        items_.clear();
        d2dIconCache_.clear();
        selectedCount_ = 0;

        ComPtr<IEnumIDList> enumItems;
        if (FAILED(desktopFolder_->EnumObjects(
                hwnd_,
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
                &enumItems)) ||
            !enumItems)
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
            reloading_ = false;
            return;
        }

        wchar_t userDesktopPath[MAX_PATH]{};
        wchar_t commonDesktopPath[MAX_PATH]{};
        wchar_t userProfilePath[MAX_PATH]{};
        SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
        SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
        SHGetSpecialFolderPathW(nullptr, userProfilePath, CSIDL_PROFILE, FALSE);
        size_t userDesktopLen = wcslen(userDesktopPath);
        size_t commonDesktopLen = wcslen(commonDesktopPath);

        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        std::unordered_set<std::wstring> seenKeys;
        while (enumItems->Next(1, &child, &fetched) == S_OK)
        {
            PIDLIST_ABSOLUTE absolute = ILCombine(desktopPidl_.get(), child);
            if (absolute == nullptr)
            {
                ILFree(child);
                continue;
            }

            std::wstring parsingName = StrRetToString(
                desktopFolder_.Get(),
                reinterpret_cast<PCUITEMID_CHILD>(child),
                SHGDN_FORPARSING);
            wchar_t itemPathBuffer[MAX_PATH]{};
            std::wstring itemPath;
            if (SHGetPathFromIDListW(absolute, itemPathBuffer) && itemPathBuffer[0] != L'\0')
            {
                itemPath = itemPathBuffer;
            }

            std::wstring desktopIconClsid = ResolveDesktopIconClsid(parsingName, itemPath, userProfilePath);
            bool isDesktopIcon = !desktopIconClsid.empty();

            if (!isDesktopIcon)
            {
                SFGAOF attrs = SFGAO_HIDDEN | SFGAO_NONENUMERATED;
                LPCITEMIDLIST childConst = child;
                if (SUCCEEDED(desktopFolder_->GetAttributesOf(1, &childConst, &attrs)))
                {
                    if ((attrs & SFGAO_HIDDEN) != 0 || (attrs & SFGAO_NONENUMERATED) != 0)
                    {
                        ILFree(absolute);
                        ILFree(child);
                        continue;
                    }
                }
            }

            SHFILEINFOW info{};
            SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(absolute),
                0,
                &info,
                sizeof(info),
                SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME | SHGFI_TYPENAME);

            if (!IsVisibleByDesktopIconSettings(desktopIconClsid, settingsIconVisibility_))
            {
                ILFree(absolute);
                ILFree(child);
                continue;
            }

            if (!isDesktopIcon)
            {
                if (!itemPath.empty())
                {
                    bool underUser = itemPath.size() > userDesktopLen &&
                        _wcsnicmp(itemPath.c_str(), userDesktopPath, userDesktopLen) == 0 &&
                        itemPath[userDesktopLen] == L'\\';
                    bool underCommon = itemPath.size() > commonDesktopLen &&
                        _wcsnicmp(itemPath.c_str(), commonDesktopPath, commonDesktopLen) == 0 &&
                        itemPath[commonDesktopLen] == L'\\';
                    if (!underUser && !underCommon)
                    {
                        ILFree(absolute);
                        ILFree(child);
                        continue;
                    }
                }
            }

            DesktopItem item;
            item.absolutePidl.reset(absolute);
            item.childPidl.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(child));
            item.parsingName = std::move(parsingName);
            item.desktopIconClsid = std::move(desktopIconClsid);
            item.name = info.szDisplayName[0] != L'\0'
                ? info.szDisplayName
                : StrRetToString(desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()), SHGDN_NORMAL);
            item.typeName = info.szTypeName;
            item.iconBitmap = GetHighResolutionShellIconBitmap(item.absolutePidl.get(), info.iIcon, item.iconBitmapSize);
            ClampAlphaToColorKey(item.iconBitmap, kTransparentKey);
            item.sysIconIndex = info.iIcon;
            std::wstring key = GetStableLayoutKey(item.absolutePidl.get(), item.parsingName, item.desktopIconClsid);
            if (seenKeys.contains(key))
            {
                continue;
            }
            seenKeys.insert(key);
            auto knownRecord = layoutRecords_.find(key);
            if (knownRecord != layoutRecords_.end() && knownRecord->second.hasGrid)
            {
                item.gridCell = knownRecord->second.cell;
                item.gridSpan = knownRecord->second.span;
                item.slot = SlotFromCell(item.gridCell);
            }
            else if (knownRecord != layoutRecords_.end() && knownRecord->second.legacySlot >= 0)
            {
                item.gridCell = CellFromSlot(knownRecord->second.legacySlot);
                item.gridSpan = knownRecord->second.span;
                item.slot = SlotFromCell(item.gridCell);
            }
            else
            {
                item.slot = -1;
            }
            if (!gridPages_.empty() && item.gridCell.pageId.empty())
            {
                item.gridCell.pageId = gridPages_.front().id;
            }
            items_.push_back(std::move(item));
        }

        std::stable_sort(items_.begin(), items_.end(), [](const DesktopItem& a, const DesktopItem& b) {
            bool aIsDesktopIcon = !a.desktopIconClsid.empty();
            bool bIsDesktopIcon = !b.desktopIconClsid.empty();
            if (aIsDesktopIcon != bIsDesktopIcon) return aIsDesktopIcon;
            if (aIsDesktopIcon)
            {
                int cmp = ToUpperInvariant(a.typeName).compare(ToUpperInvariant(b.typeName));
                if (cmp != 0) return cmp < 0;
            }
            return ToUpperInvariant(a.name) < ToUpperInvariant(b.name);
        });

        std::unordered_set<std::wstring> usedSlots;
        int nextTailSlot = 0;
        for (auto& item : items_)
        {
            item.gridSpan.columns = std::max(1, item.gridSpan.columns);
            item.gridSpan.rows = std::max(1, item.gridSpan.rows);
            if (item.slot < 0)
            {
                continue;
            }
            item.slot = SlotFromCell(item.gridCell);
            if (!item.gridCell.pageId.empty() && !HasGridPage(item.gridCell.pageId))
            {
                continue;
            }
            if (item.slot >= 0 && IsGridAreaValid(item.gridCell, item.gridSpan) && !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
            {
                MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                nextTailSlot = std::max(nextTailSlot, item.slot + 1);
            }
            else
            {
                item.slot = -1;
            }
        }

        std::vector<DesktopItem*> unslotted;
        for (auto& item : items_)
        {
            if (item.slot < 0)
            {
                unslotted.push_back(&item);
            }
        }

        std::sort(unslotted.begin(), unslotted.end(), [](const DesktopItem* a, const DesktopItem* b) {
            bool aIsDesktopIcon = !a->desktopIconClsid.empty();
            bool bIsDesktopIcon = !b->desktopIconClsid.empty();
            if (aIsDesktopIcon != bIsDesktopIcon)
            {
                return aIsDesktopIcon;
            }
            if (aIsDesktopIcon)
            {
                int cmp = ToUpperInvariant(a->typeName).compare(ToUpperInvariant(b->typeName));
                if (cmp != 0)
                {
                    return cmp < 0;
                }
            }
            return ToUpperInvariant(a->name) < ToUpperInvariant(b->name);
        });

        for (auto* item : unslotted)
        {
            GridCell nextCell = CellFromSequentialIndex(nextTailSlot);
            while (AreGridSlotsMarked(usedSlots, nextCell, item->gridSpan) || !IsGridAreaValid(nextCell, item->gridSpan))
            {
                ++nextTailSlot;
                nextCell = CellFromSequentialIndex(nextTailSlot);
            }
            item->gridCell = nextCell;
            item->slot = SlotFromCell(item->gridCell);
            MarkGridArea(usedSlots, item->gridCell, item->gridSpan);
            nextTailSlot = item->slot + 1;
        }

        std::sort(items_.begin(), items_.end(), [](const DesktopItem& a, const DesktopItem& b) {
            if (a.gridCell.pageId != b.gridCell.pageId)
            {
                return a.gridCell.pageId < b.gridCell.pageId;
            }
            if (a.gridCell.column != b.gridCell.column)
            {
                return a.gridCell.column < b.gridCell.column;
            }
            return a.gridCell.row < b.gridCell.row;
        });

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        reloading_ = false;
    }

    std::wstring GetLayoutPath() const
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        PathRemoveFileSpecW(modulePath);
        PathAppendW(modulePath, L"SnowDesktop.layout.json");
        return modulePath;
    }

    std::wstring NormalizeLayoutKey(const std::wstring& key) const
    {
        return ToUpperInvariant(key);
    }

    std::wstring GetStableLayoutKey(
        PCIDLIST_ABSOLUTE pidl,
        const std::wstring& parsingName,
        const std::wstring& desktopIconClsid = {}) const
    {
        if (!desktopIconClsid.empty())
        {
            return ToUpperInvariant(desktopIconClsid);
        }

        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, path) && path[0] != L'\0')
        {
            return ToUpperInvariant(path);
        }
        return ToUpperInvariant(parsingName);
    }

    bool ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const
    {
        std::string marker = std::string("\"") + fieldName + "\"";
        size_t name = objectText.find(marker);
        if (name == std::string::npos)
        {
            return false;
        }

        size_t colon = objectText.find(':', name + marker.size());
        size_t quote = objectText.find('"', colon == std::string::npos ? name + marker.size() : colon + 1);
        size_t end = 0;
        return quote != std::string::npos && ParseJsonStringAt(objectText, quote, value, end);
    }

    bool ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const
    {
        std::string marker = std::string("\"") + fieldName + "\"";
        size_t name = objectText.find(marker);
        if (name == std::string::npos)
        {
            return false;
        }

        size_t colon = objectText.find(':', name + marker.size());
        size_t numberStart = objectText.find_first_of("-0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
        if (numberStart == std::string::npos)
        {
            return false;
        }

        value = std::atoi(objectText.c_str() + numberStart);
        return true;
    }

    void LoadLayoutSlots()
    {
        layoutRecords_.clear();
        savedPageIds_.clear();

        std::ifstream file(GetLayoutPath(), std::ios::binary);
        if (!file)
        {
            ApplyPageMapping();
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string text = buffer.str();

        std::string firstPageMonitorUtf8;
        if (ReadJsonStringField(text, "firstPageMonitor", firstPageMonitorUtf8))
        {
            firstPageMonitorId_ = Utf8ToWide(firstPageMonitorUtf8);
        }

        LoadSavedPagesFromJson(text);

        auto findJsonObjectEnd = [](const std::string& t, size_t start) -> size_t {
            int depth = 1;
            bool inString = false;
            for (size_t i = start + 1; i < t.size(); ++i)
            {
                char ch = t[i];
                if (ch == '"' && (i == 0 || t[i - 1] != '\\'))
                {
                    inString = !inString;
                }
                else if (!inString)
                {
                    if (ch == '{') ++depth;
                    else if (ch == '}')
                    {
                        --depth;
                        if (depth == 0) return i;
                    }
                }
            }
            return std::string::npos;
        };

        size_t pos = 0;
        while ((pos = text.find("\"key\"", pos)) != std::string::npos)
        {
            size_t objectStart = text.rfind('{', pos);
            if (objectStart == std::string::npos)
            {
                break;
            }
            size_t objectEnd = findJsonObjectEnd(text, objectStart);
            if (objectEnd == std::string::npos || objectEnd <= objectStart)
            {
                break;
            }

            std::string objectText = text.substr(objectStart, objectEnd - objectStart + 1);
            std::string keyUtf8;
            if (!ReadJsonStringField(objectText, "key", keyUtf8))
            {
                pos = objectEnd + 1;
                continue;
            }

            LayoutRecord record;
            std::string pageUtf8;
            int x = 0;
            int y = 0;
            int w = 1;
            int h = 1;
            if (ReadJsonStringField(objectText, "page", pageUtf8) &&
                ReadJsonIntField(objectText, "x", x) &&
                ReadJsonIntField(objectText, "y", y))
            {
                record.cell.pageId = Utf8ToWide(pageUtf8);
                record.cell.column = x;
                record.cell.row = y;
                RememberSavedPageId(record.cell.pageId);
                ReadJsonIntField(objectText, "w", w);
                ReadJsonIntField(objectText, "h", h);
                record.span.columns = std::max(1, w);
                record.span.rows = std::max(1, h);
                record.hasGrid = true;
                record.legacySlot = SlotFromCell(record.cell);
            }
            else
            {
                int slot = -1;
                if (!ReadJsonIntField(objectText, "slot", slot))
                {
                    pos = objectEnd + 1;
                    continue;
                }
                record.legacySlot = slot;
                record.cell = CellFromSlot(slot);
                record.span = {};
            }

            layoutRecords_[NormalizeLayoutKey(Utf8ToWide(keyUtf8))] = record;
            pos = objectEnd + 1;
        }

        ApplyPageMapping();
        ApplySavedGridDimensions();
    }

    void SaveLayoutSlots()
    {
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
                layoutRecords_[GetStableLayoutKey(item.absolutePidl.get(), item.parsingName, item.desktopIconClsid)] = record;
            }
        }

        std::vector<const DesktopItem*> sortedItems;
        sortedItems.reserve(items_.size());
        for (const auto& item : items_)
        {
            sortedItems.push_back(&item);
        }
        std::sort(sortedItems.begin(), sortedItems.end(), [](const DesktopItem* left, const DesktopItem* right) {
            if (left->gridCell.pageId != right->gridCell.pageId)
            {
                return left->gridCell.pageId < right->gridCell.pageId;
            }
            if (left->gridCell.column != right->gridCell.column)
            {
                return left->gridCell.column < right->gridCell.column;
            }
            return left->gridCell.row < right->gridCell.row;
        });

        std::ofstream file(GetLayoutPath(), std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return;
        }

        std::vector<std::wstring> pagesToWrite = savedPageIds_;
        if (pagesToWrite.empty() && !gridPages_.empty())
        {
            pagesToWrite.push_back(gridPages_.front().id);
        }

        file << "{\n  \"firstPageMonitor\": \"" << JsonEscapeUtf8(firstPageMonitorId_) << "\",\n  \"pages\": [\n";
        for (size_t i = 0; i < pagesToWrite.size(); ++i)
        {
            const GridPage* page = FindExactGridPage(pagesToWrite[i]);
            file << "    { \"id\": \"" << JsonEscapeUtf8(pagesToWrite[i]) << "\", \"monitor\": \"";
            file << JsonEscapeUtf8(page != nullptr ? page->monitorId : L"");
            file << "\", \"columns\": " << (page != nullptr ? page->columns : 1) <<
                ", \"rows\": " << (page != nullptr ? page->rows : 1) << " }";
            file << (i + 1 == pagesToWrite.size() ? "\n" : ",\n");
        }
        file << "  ],\n  \"items\": [\n";
        for (size_t i = 0; i < sortedItems.size(); ++i)
        {
            const DesktopItem* item = sortedItems[i];
            file << "    { \"key\": \"" << JsonEscapeUtf8(GetStableLayoutKey(item->absolutePidl.get(), item->parsingName, item->desktopIconClsid)) <<
                "\", \"page\": \"" << JsonEscapeUtf8(item->gridCell.pageId) <<
                "\", \"x\": " << item->gridCell.column <<
                ", \"y\": " << item->gridCell.row <<
                ", \"w\": " << std::max(1, item->gridSpan.columns) <<
                ", \"h\": " << std::max(1, item->gridSpan.rows) <<
                ", \"slot\": " << item->slot << " }";
            file << (i + 1 == sortedItems.size() ? "\n" : ",\n");
        }
        file << "  ]\n}\n";
    }

    void SortIconsByName()
    {
        std::vector<size_t> order(items_.size());
        for (size_t i = 0; i < items_.size(); ++i)
        {
            order[i] = i;
        }

        std::sort(order.begin(), order.end(), [this](size_t left, size_t right) {
            std::wstring leftName = ToUpperInvariant(items_[left].name);
            std::wstring rightName = ToUpperInvariant(items_[right].name);
            return leftName < rightName;
        });

        for (size_t i = 0; i < order.size(); ++i)
        {
            items_[order[i]].gridCell = CellFromSequentialIndex(static_cast<int>(i));
            items_[order[i]].slot = SlotFromCell(items_[order[i]].gridCell);
        }

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void SortIconsByType()
    {
        std::vector<size_t> order(items_.size());
        for (size_t i = 0; i < items_.size(); ++i)
        {
            order[i] = i;
        }

        std::sort(order.begin(), order.end(), [this](size_t left, size_t right) {
            bool leftIsDesktopIcon = !items_[left].desktopIconClsid.empty();
            bool rightIsDesktopIcon = !items_[right].desktopIconClsid.empty();
            if (leftIsDesktopIcon != rightIsDesktopIcon)
            {
                return leftIsDesktopIcon;
            }
            int cmp = ToUpperInvariant(items_[left].typeName).compare(ToUpperInvariant(items_[right].typeName));
            if (cmp != 0)
            {
                return cmp < 0;
            }
            return ToUpperInvariant(items_[left].name) < ToUpperInvariant(items_[right].name);
        });

        for (size_t i = 0; i < order.size(); ++i)
        {
            items_[order[i]].gridCell = CellFromSequentialIndex(static_cast<int>(i));
            items_[order[i]].slot = SlotFromCell(items_[order[i]].gridCell);
        }

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void LayoutItems()
    {
        for (auto& item : items_)
        {
            if (!gridPages_.empty() && item.gridCell.pageId.empty())
            {
                item.gridCell.pageId = gridPages_.front().id;
            }
            const GridPage* page = FindExactGridPage(item.gridCell.pageId);
            if (page != nullptr)
            {
                item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
                item.gridSpan.rows = std::clamp(item.gridSpan.rows, 1, std::max(1, page->rows));
                item.gridCell.column = std::clamp(item.gridCell.column, 0, std::max(0, page->columns - item.gridSpan.columns));
                item.gridCell.row = std::clamp(item.gridCell.row, 0, std::max(0, page->rows - item.gridSpan.rows));
            }
            item.slot = SlotFromCell(item.gridCell);
            item.bounds = GetGridRect(item.gridCell, item.gridSpan);
        }
    }

    void UpdateLayoutWorkArea()
    {
        layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);
        gridPages_.clear();

        MonitorEnumContext context{};
        context.virtualLeft = virtualLeft_;
        context.virtualTop = virtualTop_;
        context.pages = &gridPages_;
        EnumDisplayMonitors(nullptr, nullptr, EnumGridPageMonitorProc, reinterpret_cast<LPARAM>(&context));

        if (gridPages_.empty())
        {
            GridPage fallback;
            fallback.id = L"Primary";
            fallback.monitorId = fallback.id;
            fallback.isPrimary = true;
            fallback.bounds = layoutWorkArea_;
            fallback.workArea = layoutWorkArea_;
            gridPages_.push_back(fallback);
        }

        std::sort(gridPages_.begin(), gridPages_.end(), [](const GridPage& left, const GridPage& right) {
            if (left.bounds.left != right.bounds.left)
            {
                return left.bounds.left < right.bounds.left;
            }
            return left.bounds.top < right.bounds.top;
        });

        for (auto& page : gridPages_)
        {
            page.workArea.left = std::clamp<LONG>(page.workArea.left, 0, static_cast<LONG>(virtualWidth_));
            page.workArea.top = std::clamp<LONG>(page.workArea.top, 0, static_cast<LONG>(virtualHeight_));
            page.workArea.right = std::clamp<LONG>(page.workArea.right, page.workArea.left, static_cast<LONG>(virtualWidth_));
            page.workArea.bottom = std::clamp<LONG>(page.workArea.bottom, page.workArea.top, static_cast<LONG>(virtualHeight_));
            ConfigureGridPage(page);
        }

        std::wstring detectedPrimaryMonitorId;
        for (const auto& page : gridPages_)
        {
            if (page.isPrimary)
            {
                detectedPrimaryMonitorId = page.monitorId;
                break;
            }
        }
        if (detectedPrimaryMonitorId.empty() && !gridPages_.empty())
        {
            detectedPrimaryMonitorId = gridPages_.front().monitorId;
        }
        const bool hadPrimaryMonitor = !primaryMonitorId_.empty();
        if (primaryMonitorId_ != detectedPrimaryMonitorId)
        {
            primaryMonitorId_ = detectedPrimaryMonitorId;
            if (hadPrimaryMonitor || firstPageMonitorId_.empty())
            {
                firstPageMonitorId_ = primaryMonitorId_;
            }
            pageOffset_ = 0;
        }
        if (firstPageMonitorId_.empty())
        {
            firstPageMonitorId_ = primaryMonitorId_;
        }
        if (!firstPageMonitorId_.empty())
        {
            bool firstMonitorStillExists = false;
            for (const auto& page : gridPages_)
            {
                if (page.monitorId == firstPageMonitorId_)
                {
                    firstMonitorStillExists = true;
                    break;
                }
            }
            if (!firstMonitorStillExists)
            {
                firstPageMonitorId_ = primaryMonitorId_;
                pageOffset_ = 0;
            }
        }
        ApplyPageMapping();
        ApplySavedGridDimensions();

        if (!gridPages_.empty())
        {
            layoutWorkArea_ = gridPages_.front().workArea;
        }
    }

    void ApplySavedGridDimensions()
    {
        for (auto& page : gridPages_)
        {
            auto colIt = savedPageColumns_.find(page.id);
            auto rowIt = savedPageRows_.find(page.id);
            if (colIt != savedPageColumns_.end() && rowIt != savedPageRows_.end() &&
                colIt->second > 0 && rowIt->second > 0)
            {
                page.columns = colIt->second;
                page.rows = rowIt->second;
                ApplyGapScaleToPage(page);
            }
        }
    }

    void ApplyGapScaleToPage(GridPage& page)
    {
        const int usableW = std::max(1, static_cast<int>(page.workArea.right - page.workArea.left) - (page.marginX * 2));
        const int usableH = std::max(1, static_cast<int>(page.workArea.bottom - page.workArea.top) - (page.marginY * 2));
        const float cellRefW = static_cast<float>(usableW) / static_cast<float>(std::max(1, page.columns));
        const float cellRefH = static_cast<float>(usableH) / static_cast<float>(std::max(1, page.rows));
        const int targetGapX = std::max(0, static_cast<int>(cellRefW * kGapPercentX / gapScale_));
        const int targetGapY = std::max(0, static_cast<int>(cellRefH * kGapPercentY / gapScale_));

        page.cellWidth = page.columns > 1
            ? std::max(kIconSize, (usableW - (page.columns - 1) * targetGapX) / page.columns)
            : usableW;
        page.cellHeight = page.rows > 1
            ? std::max(kMinCellHeight / 2, (usableH - (page.rows - 1) * targetGapY) / page.rows)
            : usableH;
        page.gapX = page.columns > 1 ? (usableW - page.columns * page.cellWidth) / (page.columns - 1) : 0;
        page.gapY = page.rows > 1 ? (usableH - page.rows * page.cellHeight) / (page.rows - 1) : 0;
    }

    void SetZoom(float value)
    {
        float clamped = std::clamp(value, 0.5f, 2.0f);
        if (clamped == gapScale_)
        {
            return;
        }
        gapScale_ = clamped;
        for (auto& page : gridPages_)
        {
            ApplyGapScaleToPage(page);
        }
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void AdjustZoom(float delta)
    {
        float newVal = std::clamp(gapScale_ + delta, 0.5f, 2.0f);
        if (newVal == gapScale_)
        {
            return;
        }
        gapScale_ = newVal;
        for (auto& page : gridPages_)
        {
            ApplyGapScaleToPage(page);
        }
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void AdjustGridRows(int delta)
    {
        if (gridPages_.empty())
        {
            return;
        }
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* found = GridPageFromPoint(clientPoint);
        if (found == nullptr)
        {
            return;
        }
        GridPage* targetPage = nullptr;
        for (auto& page : gridPages_)
        {
            if (page.id == found->id) { targetPage = &page; break; }
        }
        if (targetPage == nullptr)
        {
            return;
        }

        constexpr int kMinRows = 1;
        constexpr int kMaxRows = 50;
        const int newRows = std::clamp(targetPage->rows + delta, kMinRows, kMaxRows);
        if (newRows == targetPage->rows)
        {
            return;
        }

        targetPage->rows = newRows;
        ApplyGapScaleToPage(*targetPage);

        savedPageColumns_[targetPage->id] = targetPage->columns;
        savedPageRows_[targetPage->id] = targetPage->rows;
        SaveLayoutSlots();
        ReloadItems(false);
    }

    void AdjustGridColumns(int delta)
    {
        if (gridPages_.empty())
        {
            return;
        }
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* found = GridPageFromPoint(clientPoint);
        if (found == nullptr)
        {
            return;
        }
        GridPage* targetPage = nullptr;
        for (auto& page : gridPages_)
        {
            if (page.id == found->id) { targetPage = &page; break; }
        }
        if (targetPage == nullptr)
        {
            return;
        }

        constexpr int kMinColumns = 1;
        constexpr int kMaxColumns = 50;
        const int newColumns = std::clamp(targetPage->columns + delta, kMinColumns, kMaxColumns);
        if (newColumns == targetPage->columns)
        {
            return;
        }

        targetPage->columns = newColumns;
        ApplyGapScaleToPage(*targetPage);

        savedPageColumns_[targetPage->id] = targetPage->columns;
        savedPageRows_[targetPage->id] = targetPage->rows;
        SaveLayoutSlots();
        ReloadItems(false);
    }

    void ConfigureGridPage(GridPage& page) const
    {
        const int cellW = static_cast<int>(kCellWidth * gapScale_);
        const int cellH = static_cast<int>(kMinCellHeight * gapScale_);
        const int width = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
        const int height = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
        const int usableWidth = std::max(1, width - (page.marginX * 2));
        const int usableHeight = std::max(1, height - (page.marginY * 2));

        page.columns = std::max(1, usableWidth / cellW);
        page.rows = std::max(1, usableHeight / cellH);
        page.cellWidth = cellW;
        page.cellHeight = cellH;
        page.gapX = page.columns > 1 ? std::max(0, (usableWidth - (page.columns * page.cellWidth)) / (page.columns - 1)) : 0;
        page.gapY = page.rows > 1 ? std::max(0, (usableHeight - (page.rows * page.cellHeight)) / (page.rows - 1)) : 0;
    }

    bool IsGeneratedExtraPageId(const std::wstring& pageId) const
    {
        return pageId.rfind(L"__extra:", 0) == 0;
    }

    std::wstring MakeExtraPageId(const std::wstring& monitorId) const
    {
        return L"__extra:" + monitorId;
    }

    void RememberSavedPageId(const std::wstring& pageId)
    {
        if (pageId.empty())
        {
            return;
        }

        if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
        {
            savedPageIds_.push_back(pageId);
        }
    }

    void LoadSavedPagesFromJson(const std::string& text)
    {
        size_t pagesName = text.find("\"pages\"");
        if (pagesName == std::string::npos)
        {
            return;
        }

        size_t arrayStart = text.find('[', pagesName);
        size_t arrayEnd = text.find(']', arrayStart == std::string::npos ? pagesName : arrayStart + 1);
        if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart)
        {
            return;
        }

        size_t pos = arrayStart + 1;
        while ((pos = text.find('{', pos)) != std::string::npos && pos < arrayEnd)
        {
            size_t objectEnd = text.find('}', pos);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd)
            {
                break;
            }

            std::string objectText = text.substr(pos, objectEnd - pos + 1);
            std::string pageUtf8;
            if (ReadJsonStringField(objectText, "id", pageUtf8))
            {
                std::wstring pageId = Utf8ToWide(pageUtf8);
                RememberSavedPageId(pageId);
                int columns = 0;
                int rows = 0;
                if (ReadJsonIntField(objectText, "columns", columns) && columns > 0)
                {
                    savedPageColumns_[pageId] = columns;
                }
                if (ReadJsonIntField(objectText, "rows", rows) && rows > 0)
                {
                    savedPageRows_[pageId] = rows;
                }
            }
            pos = objectEnd + 1;
        }
    }

    size_t FirstMonitorOrderIndex() const
    {
        if (gridPages_.empty())
        {
            return 0;
        }

        for (size_t i = 0; i < gridPages_.size(); ++i)
        {
            if (!firstPageMonitorId_.empty() && gridPages_[i].monitorId == firstPageMonitorId_)
            {
                return i;
            }
        }

        for (size_t i = 0; i < gridPages_.size(); ++i)
        {
            if (!primaryMonitorId_.empty() && gridPages_[i].monitorId == primaryMonitorId_)
            {
                return i;
            }
        }

        return 0;
    }

    std::vector<size_t> BuildMonitorRenderOrder() const
    {
        std::vector<size_t> order;
        if (gridPages_.empty())
        {
            return order;
        }

        order.reserve(gridPages_.size());
        const size_t first = FirstMonitorOrderIndex();
        for (size_t offset = 0; offset < gridPages_.size(); ++offset)
        {
            order.push_back((first + offset) % gridPages_.size());
        }
        return order;
    }

    int MaxPageOffset() const
    {
        if (savedPageIds_.empty() || gridPages_.empty())
        {
            return 0;
        }

        const int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
        return std::max(0, static_cast<int>(savedPageIds_.size()) - visiblePageCount);
    }

    void ApplyPageMapping()
    {
        lastMonitorPageId_.clear();
        if (gridPages_.empty())
        {
            return;
        }

        if (savedPageIds_.empty())
        {
            for (const auto& page : gridPages_)
            {
                RememberSavedPageId(page.monitorId);
            }
        }

        pageOffset_ = std::clamp(pageOffset_, 0, MaxPageOffset());
        std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
        const size_t numMonitors = monitorOrder.size();
        for (size_t i = 0; i < numMonitors; ++i)
        {
            GridPage& page = gridPages_[monitorOrder[i]];
            const bool isLast = (i == numMonitors - 1);
            const size_t pageIdx = i + (isLast ? static_cast<size_t>(pageOffset_) : 0);
            if (pageIdx < savedPageIds_.size())
            {
                page.id = savedPageIds_[pageIdx];
            }
            else
            {
                page.id = MakeExtraPageId(page.monitorId);
            }
            if (isLast)
            {
                lastMonitorPageId_ = page.id;
            }
        }
        if (!lastMonitorPageId_.empty() && savedPageIds_.size() <= gridPages_.size())
        {
            lastMonitorPageId_.clear();
        }
    }

    const GridPage* FindExactGridPage(const std::wstring& pageId) const
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == pageId)
            {
                return &page;
            }
        }
        return nullptr;
    }

    const GridPage* FindGridPage(const std::wstring& pageId) const
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == pageId)
            {
                return &page;
            }
        }
        return gridPages_.empty() ? nullptr : &gridPages_.front();
    }

    bool HasGridPage(const std::wstring& pageId) const
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == pageId)
            {
                return true;
            }
        }
        return false;
    }

    const GridPage* GridPageFromPoint(POINT point) const
    {
        const GridPage* fallback = gridPages_.empty() ? nullptr : &gridPages_.front();
        for (const auto& page : gridPages_)
        {
            if (PtInRect(&page.bounds, point) || PtInRect(&page.workArea, point))
            {
                return &page;
            }
        }
        return fallback;
    }

    int SlotFromCell(const GridCell& cell) const
    {
        const GridPage* page = FindGridPage(cell.pageId);
        const int rows = page != nullptr ? page->rows : GetRowsPerColumn();
        return std::max(0, cell.column) * std::max(1, rows) + std::max(0, cell.row);
    }

    GridCell CellFromSlot(int slot, const std::wstring& pageId = L"") const
    {
        const GridPage* page = FindGridPage(pageId);
        const int rows = page != nullptr ? page->rows : GetRowsPerColumn();
        GridCell cell;
        cell.pageId = page != nullptr ? page->id : pageId;
        cell.column = std::max(0, slot) / std::max(1, rows);
        cell.row = std::max(0, slot) % std::max(1, rows);
        return cell;
    }

    GridCell CellFromSequentialIndex(int index) const
    {
        int remaining = std::max(0, index);
        for (const auto& page : gridPages_)
        {
            if (IsGeneratedExtraPageId(page.id))
            {
                continue;
            }

            const int capacity = std::max(1, page.columns * page.rows);
            if (remaining < capacity)
            {
                GridCell cell;
                cell.pageId = page.id;
                cell.column = remaining / std::max(1, page.rows);
                cell.row = remaining % std::max(1, page.rows);
                return cell;
            }
            remaining -= capacity;
        }

        GridCell cell;
        if (!gridPages_.empty())
        {
            const GridPage* page = nullptr;
            for (auto it = gridPages_.rbegin(); it != gridPages_.rend(); ++it)
            {
                if (!IsGeneratedExtraPageId(it->id))
                {
                    page = &(*it);
                    break;
                }
            }
            if (page == nullptr)
            {
                page = &gridPages_.back();
            }
            cell.pageId = page->id;
            cell.column = std::max(0, page->columns - 1);
            cell.row = std::max(0, page->rows - 1);
        }
        return cell;
    }

    RECT GetGridRect(const GridCell& cell, GridSpan span = {}) const
    {
        const GridPage* page = FindExactGridPage(cell.pageId);
        if (page == nullptr)
        {
            return MakeRect(0, 0, 0, 0);
        }

        const int column = std::clamp(cell.column, 0, std::max(0, page->columns - 1));
        const int row = std::clamp(cell.row, 0, std::max(0, page->rows - 1));
        const int spanColumns = std::clamp(span.columns, 1, std::max(1, page->columns - column));
        const int spanRows = std::clamp(span.rows, 1, std::max(1, page->rows - row));
        const int x = page->workArea.left + page->marginX + (column * (page->cellWidth + page->gapX));
        const int y = page->workArea.top + page->marginY + (row * (page->cellHeight + page->gapY));
        const int width = (spanColumns * page->cellWidth) + ((spanColumns - 1) * page->gapX);
        const int height = (spanRows * page->cellHeight) + ((spanRows - 1) * page->gapY);
        return MakeRect(x, y, x + width, y + height);
    }

    RECT GetSlotRect(int slot) const
    {
        return GetGridRect(CellFromSlot(slot));
    }

    GridCell CellFromPoint(POINT point) const
    {
        const GridPage* page = GridPageFromPoint(point);
        GridCell cell;
        if (page == nullptr)
        {
            return cell;
        }

        const int stepX = std::max(1, page->cellWidth + page->gapX);
        const int stepY = std::max(1, page->cellHeight + page->gapY);
        cell.pageId = page->id;
        cell.column = std::clamp(static_cast<int>((point.x - page->workArea.left - page->marginX + (stepX / 2)) / stepX), 0, std::max(0, page->columns - 1));
        cell.row = std::clamp(static_cast<int>((point.y - page->workArea.top - page->marginY + (stepY / 2)) / stepY), 0, std::max(0, page->rows - 1));
        return cell;
    }

    int SlotFromPoint(POINT point) const
    {
        return SlotFromCell(CellFromPoint(point));
    }

    POINT GetDragTargetPoint(POINT current) const
    {
        return {
            dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
            dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
        };
    }

    RECT GetTargetRectAt(POINT point) const
    {
        int hit = HitTest(point);
        if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
        {
            return GetItemSelectionRect(items_[static_cast<size_t>(hit)], true);
        }

        return GetGridRect(CellFromPoint(point));
    }

    RECT GetSelectedDragBoundsAt(POINT point) const
    {
        RECT bounds{};
        bool hasBounds = false;
        int dx = point.x - mouseDownPoint_.x;
        int dy = point.y - mouseDownPoint_.y;

        for (const auto& item : items_)
        {
            if (!item.selected)
            {
                continue;
            }
            if (IsRectEmptyRect(item.bounds))
            {
                continue;
            }

            RECT moved = GetItemSelectionRect(item, true);
            OffsetRect(&moved, dx, dy);
            bounds = hasBounds ? UnionCopy(bounds, moved) : moved;
            hasBounds = true;
        }

        return hasBounds ? bounds : RECT{};
    }

    RECT GetInternalDragDirtyRect(POINT point) const
    {
        RECT dirty = GetSelectedDragBoundsAt(point);
        for (const RECT& rect : GetSelectedMovePreviewRects(point))
        {
            dirty = UnionCopy(dirty, rect);
        }
        return InflateCopy(dirty, 16);
    }

    std::vector<RECT> GetSelectedMovePreviewRects(POINT point) const
    {
        std::vector<RECT> rects;
        std::vector<PendingGridMove> moves = BuildSelectedMove(CellFromPoint(point));
        rects.reserve(moves.size());
        for (const PendingGridMove& move : moves)
        {
            RECT bounds = GetGridRect(move.cell, items_[move.index].gridSpan);
            rects.push_back(GetItemSelectionRect(bounds, true));
        }
        return rects;
    }

    RECT GetExternalDragDirtyRect(POINT point) const
    {
        RECT dirty = GetTargetRectAt(point);
        return InflateCopy(dirty, 16);
    }

    void InvalidateFast(RECT dirty)
    {
        if (hwnd_ == nullptr || IsRectEmptyRect(dirty))
        {
            return;
        }

        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT clipped{};
        if (!IntersectRect(&clipped, &dirty, &client))
        {
            return;
        }

        RedrawWindow(hwnd_, &clipped, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    std::wstring MakeInternalDragHint(POINT point) const
    {
        int hit = HitTest(point);
        if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
        {
            return L"释放：交给「" + items_[static_cast<size_t>(hit)].name + L"」处理";
        }

        if (BuildSelectedMove(CellFromPoint(point)).empty())
        {
            return L"释放：当前位置已有图标";
        }

        return L"释放：移动到此空位";
    }

    std::wstring MakeExternalDragHint(POINT point) const
    {
        int hit = HitTest(point);
        if (hit >= 0)
        {
            return L"释放：拖入「" + items_[static_cast<size_t>(hit)].name + L"」";
        }

        return L"释放：拖入桌面空白";
    }

    int HitTest(POINT point) const
    {
        for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i)
        {
            if (IsRectEmptyRect(items_[static_cast<size_t>(i)].bounds))
            {
                continue;
            }
            RECT hitRect = GetItemHitRect(items_[static_cast<size_t>(i)]);
            if (PtInRect(&hitRect, point))
            {
                return i;
            }
        }
        return -1;
    }

    RECT GetItemIconRect(RECT bounds) const
    {
        const int cellW = bounds.right - bounds.left;
        const int cellH = bounds.bottom - bounds.top;
        const int maxIconW = std::max(16, cellW - 8);
        const int maxIconH = std::max(16, cellH - kTextHeight - 8);
        const int iconSz = std::min(maxIconW, maxIconH);
        const int iconX = bounds.left + (cellW - iconSz) / 2;
        const int iconY = bounds.top + 2;
        return MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
    }

    RECT GetItemTextRect(RECT bounds, bool expanded) const
    {
        RECT iconRect = GetItemIconRect(bounds);
        const int textTop = iconRect.bottom + 2;
        const int textH = expanded ? kTextExpandedHeight : kTextCollapsedHeight;
        return MakeRect(
            bounds.left + 4,
            textTop,
            bounds.right - 4,
            textTop + textH);
    }

    RECT GetItemTextRect(const DesktopItem& item, bool expanded) const
    {
        return GetItemTextRect(item.bounds, expanded);
    }

    RECT GetItemSelectionRect(RECT bounds, bool expanded) const
    {
        RECT textRect = GetItemTextRect(bounds, expanded);
        RECT selection = UnionCopy(GetItemIconRect(bounds), textRect);
        selection.left = std::max(bounds.left + 3, selection.left - 4);
        selection.top = std::max(bounds.top, selection.top - 2);
        selection.right = std::min(bounds.right - 3, selection.right + 4);
        selection.bottom = std::min(bounds.bottom - 2, textRect.bottom);
        return selection;
    }

    RECT GetItemSelectionRect(const DesktopItem& item, bool expanded) const
    {
        return GetItemSelectionRect(item.bounds, expanded);
    }

    RECT GetItemHitRect(const DesktopItem& item) const
    {
        return GetItemSelectionRect(item, item.selected);
    }

    void ClearSelection()
    {
        for (auto& item : items_)
        {
            item.selected = false;
        }
        selectedCount_ = 0;
    }

    void SelectOnly(size_t index)
    {
        ClearSelection();
        items_[index].selected = true;
        selectedCount_ = 1;
    }

    void ToggleSelection(size_t index)
    {
        items_[index].selected = !items_[index].selected;
        selectedCount_ += items_[index].selected ? 1 : -1;
    }

    std::wstring GridSlotKey(const GridCell& cell) const
    {
        return cell.pageId + L":" + std::to_wstring(cell.column) + L":" + std::to_wstring(cell.row);
    }

    bool IsGridAreaValid(const GridCell& cell, GridSpan span) const
    {
        const GridPage* page = FindExactGridPage(cell.pageId);
        if (page == nullptr)
        {
            return false;
        }

        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        return cell.column >= 0 &&
            cell.row >= 0 &&
            cell.column + columns <= page->columns &&
            cell.row + rows <= page->rows;
    }

    bool AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span) const
    {
        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        for (int y = 0; y < rows; ++y)
        {
            for (int x = 0; x < columns; ++x)
            {
                GridCell occupied = cell;
                occupied.column += x;
                occupied.row += y;
                if (usedSlots.contains(GridSlotKey(occupied)))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span) const
    {
        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        for (int y = 0; y < rows; ++y)
        {
            for (int x = 0; x < columns; ++x)
            {
                GridCell occupied = cell;
                occupied.column += x;
                occupied.row += y;
                usedSlots.insert(GridSlotKey(occupied));
            }
        }
    }

    bool IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
    {
        const int columns = std::max(1, span.columns);
        const int rows = std::max(1, span.rows);
        for (const auto& item : items_)
        {
            if (item.selected || item.gridCell.pageId != cell.pageId)
            {
                continue;
            }

            const int itemRight = item.gridCell.column + std::max(1, item.gridSpan.columns);
            const int itemBottom = item.gridCell.row + std::max(1, item.gridSpan.rows);
            const int areaRight = cell.column + columns;
            const int areaBottom = cell.row + rows;
            if (cell.column < itemRight &&
                areaRight > item.gridCell.column &&
                cell.row < itemBottom &&
                areaBottom > item.gridCell.row)
            {
                return true;
            }
        }
        return false;
    }

    bool IsSlotOccupiedByUnselected(int slot) const
    {
        GridCell cell = CellFromSlot(slot);
        return IsGridAreaOccupiedByUnselected(cell, {});
    }

    bool IsCellOccupiedByUnselected(const GridCell& cell, GridSpan span = {}) const
    {
        return IsGridAreaOccupiedByUnselected(cell, span);
    }

    bool IsSlotOccupiedByUnselectedLegacy(int slot) const
    {
        for (const auto& item : items_)
        {
            if (item.slot == slot && !item.selected)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<PendingGridMove> BuildSelectedMove(GridCell targetCell) const
    {
        std::vector<PendingGridMove> moves;
        if (selectedCount_ <= 0)
        {
            return moves;
        }

        std::vector<size_t> selectedIndexes;
        selectedIndexes.reserve(static_cast<size_t>(selectedCount_));
        int minColumn = INT_MAX;
        int minRow = INT_MAX;
        int maxColumn = 0;
        int maxRow = 0;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
            {
                selectedIndexes.push_back(i);
                minColumn = std::min(minColumn, items_[i].gridCell.column);
                minRow = std::min(minRow, items_[i].gridCell.row);
                maxColumn = std::max(maxColumn, items_[i].gridCell.column + std::max(1, items_[i].gridSpan.columns));
                maxRow = std::max(maxRow, items_[i].gridCell.row + std::max(1, items_[i].gridSpan.rows));
            }
        }

        if (selectedIndexes.empty())
        {
            return moves;
        }

        const GridPage* page = FindExactGridPage(targetCell.pageId);
        if (page == nullptr)
        {
            return moves;
        }

        const int groupColumns = std::max(1, maxColumn - minColumn);
        const int groupRows = std::max(1, maxRow - minRow);
        targetCell.column = std::clamp(targetCell.column, 0, std::max(0, page->columns - groupColumns));
        targetCell.row = std::clamp(targetCell.row, 0, std::max(0, page->rows - groupRows));

        for (size_t itemIndex : selectedIndexes)
        {
            GridCell movedCell = targetCell;
            movedCell.column += items_[itemIndex].gridCell.column - minColumn;
            movedCell.row += items_[itemIndex].gridCell.row - minRow;

            if (!IsGridAreaValid(movedCell, items_[itemIndex].gridSpan) ||
                IsGridAreaOccupiedByUnselected(movedCell, items_[itemIndex].gridSpan))
            {
                moves.clear();
                return moves;
            }

            moves.push_back({ itemIndex, movedCell });
        }
        return moves;
    }

    void MoveSelectedItemsToCell(GridCell targetCell)
    {
        std::vector<PendingGridMove> moves = BuildSelectedMove(std::move(targetCell));
        if (moves.empty())
        {
            return;
        }

        for (const PendingGridMove& move : moves)
        {
            items_[move.index].gridCell = move.cell;
            items_[move.index].slot = SlotFromCell(move.cell);
        }
        LayoutItems();
        SaveLayoutSlots();
    }

    void MoveSelectedItemsToSlot(int targetSlot)
    {
        MoveSelectedItemsToCell(CellFromSlot(targetSlot));
    }

    int GetRowsPerColumn() const
    {
        return gridPages_.empty() ? 1 : std::max(1, gridPages_.front().rows);
    }

    void MoveKeyboardSelection(int delta)
    {
        if (items_.empty())
        {
            return;
        }

        int current = -1;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
            {
                current = static_cast<int>(i);
                break;
            }
        }

        int next = current < 0 ? 0 : current + delta;
        next = std::clamp(next, 0, static_cast<int>(items_.size()) - 1);
        SelectOnly(static_cast<size_t>(next));
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    std::vector<PCUITEMID_CHILD> SelectedChildPidls() const
    {
        std::vector<PCUITEMID_CHILD> pidls;
        for (const auto& item : items_)
        {
            if (item.selected)
            {
                pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()));
            }
        }
        return pidls;
    }

    ComPtr<IDataObject> CreateSelectedDataObject()
    {
        auto pidls = SelectedChildPidls();
        if (pidls.empty())
        {
            return nullptr;
        }

        ComPtr<IDataObject> dataObject;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            IID_IDataObject,
            nullptr,
            reinterpret_cast<void**>(dataObject.GetAddressOf()));
        if (FAILED(hr))
        {
            return nullptr;
        }

        return dataObject;
    }

    POINT ScreenPointToClient(POINTL screenPoint) const
    {
        POINT point{ screenPoint.x, screenPoint.y };
        ScreenToClient(hwnd_, &point);
        return point;
    }

    POINTL ClientPointToScreenPoint(POINT clientPoint) const
    {
        POINT point = clientPoint;
        ClientToScreen(hwnd_, &point);
        return POINTL{ point.x, point.y };
    }

    DWORD ChooseDropEffect(DWORD keyState, DWORD allowed) const
    {
        DWORD available = allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
        if (available == 0)
        {
            available = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        }

        if ((keyState & MK_CONTROL) != 0 && (available & DROPEFFECT_COPY) != 0)
        {
            return DROPEFFECT_COPY;
        }

        if ((keyState & MK_SHIFT) != 0 && (available & DROPEFFECT_MOVE) != 0)
        {
            return DROPEFFECT_MOVE;
        }

        if ((keyState & MK_ALT) != 0 && (available & DROPEFFECT_LINK) != 0)
        {
            return DROPEFFECT_LINK;
        }

        if ((available & DROPEFFECT_MOVE) != 0)
        {
            return DROPEFFECT_MOVE;
        }

        if ((available & DROPEFFECT_COPY) != 0)
        {
            return DROPEFFECT_COPY;
        }

        return available & DROPEFFECT_LINK;
    }

    ComPtr<IDropTarget> GetShellDropTargetAt(POINT clientPoint, int* targetIndex = nullptr)
    {
        if (targetIndex != nullptr)
        {
            *targetIndex = -1;
        }

        int hit = HitTest(clientPoint);
        if (hit >= 0)
        {
            if (targetIndex != nullptr)
            {
                *targetIndex = hit;
            }

            PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(items_[static_cast<size_t>(hit)].childPidl.get());
            ComPtr<IDropTarget> target;
            HRESULT hr = desktopFolder_->GetUIObjectOf(
                hwnd_,
                1,
                &child,
                IID_IDropTarget,
                nullptr,
                reinterpret_cast<void**>(target.GetAddressOf()));
            if (SUCCEEDED(hr) && target)
            {
                return target;
            }
        }

        ComPtr<IDropTarget> desktopTarget;
        HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IDropTarget, reinterpret_cast<void**>(desktopTarget.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            return desktopTarget;
        }

        return nullptr;
    }

    HRESULT DropDataObjectAt(IDataObject* dataObject, POINT clientPoint, DWORD keyState, DWORD* effect)
    {
        if (dataObject == nullptr || effect == nullptr)
        {
            return E_POINTER;
        }

        DWORD localEffect = *effect;
        if ((localEffect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)) == 0)
        {
            localEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
        }

        ComPtr<IDropTarget> target = GetShellDropTargetAt(clientPoint);
        if (!target)
        {
            *effect = DROPEFFECT_NONE;
            return E_FAIL;
        }

        POINTL screenPoint = ClientPointToScreenPoint(clientPoint);
        HRESULT hr = target->DragEnter(dataObject, keyState, screenPoint, &localEffect);
        if (SUCCEEDED(hr))
        {
            hr = target->DragOver(keyState, screenPoint, &localEffect);
        }
        if (SUCCEEDED(hr))
        {
            hr = target->Drop(dataObject, keyState, screenPoint, &localEffect);
        }
        else
        {
            target->DragLeave();
        }

        *effect = localEffect;
        return hr;
    }

    void OpenItem(size_t index)
    {
        SHELLEXECUTEINFOW exec{};
        exec.cbSize = sizeof(exec);
        exec.fMask = SEE_MASK_IDLIST;
        exec.hwnd = hwnd_;
        exec.nShow = SW_SHOWNORMAL;
        exec.lpIDList = items_[index].absolutePidl.get();
        ShellExecuteExW(&exec);
    }

    void OpenSelected()
    {
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
            {
                OpenItem(i);
                return;
            }
        }
    }

    bool IsProtectedDesktopIcon(const DesktopItem& item) const
    {
        std::wstring clsid = !item.desktopIconClsid.empty()
            ? item.desktopIconClsid
            : ExtractClsidText(item.parsingName);
        return clsid == kDesktopIconClsidThisPC ||
            clsid == kDesktopIconClsidUserFiles ||
            clsid == kDesktopIconClsidNetwork ||
            clsid == kDesktopIconClsidControlPanel ||
            clsid == kDesktopIconClsidRecycleBin;
    }

    bool CanUseSelectedFileCommands() const
    {
        if (selectedCount_ <= 0)
        {
            return false;
        }

        for (const auto& item : items_)
        {
            if (item.selected && IsProtectedDesktopIcon(item))
            {
                return false;
            }
        }
        return true;
    }

    void ShowCustomItemContextMenu(POINT screenPoint)
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const UINT singleItemFlag = selectedCount_ == 1 ? MF_STRING : (MF_STRING | MF_GRAYED);
        const UINT fileCommandFlag = CanUseSelectedFileCommands() ? MF_STRING : (MF_STRING | MF_GRAYED);
        const UINT renameFlag = selectedCount_ == 1 && CanUseSelectedFileCommands() ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(menu, singleItemFlag, kContextOpenCommand, L"打开");
        AppendMenuW(menu, renameFlag, kContextRenameCommand, L"重命名");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, fileCommandFlag, kContextCutCommand, L"剪切");
        AppendMenuW(menu, fileCommandFlag, kContextCopyCommand, L"复制");
        AppendMenuW(menu, fileCommandFlag, kContextDeleteCommand, L"删除");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);
        DestroyMenu(menu);

        switch (command)
        {
        case kContextOpenCommand:
            OpenSelected();
            break;
        case kContextRenameCommand:
            BeginRenameSelected();
            break;
        case kContextCutCommand:
            InvokeSelectedShellVerb("cut");
            break;
        case kContextCopyCommand:
            InvokeSelectedShellVerb("copy");
            break;
        case kContextDeleteCommand:
            InvokeSelectedShellVerb("delete");
            break;
        case kContextMoreCommand:
            ShowShellContextMenu(screenPoint);
            break;
        default:
            break;
        }
    }

    void ShowCustomBackgroundContextMenu(POINT screenPoint)
    {
        lastContextMenuScreenPoint_ = screenPoint;

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, kContextRefreshCommand, L"刷新");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU sortMenu = CreatePopupMenu();
        if (sortMenu != nullptr)
        {
            AppendMenuW(sortMenu, MF_STRING, kContextSortByNameCommand, L"名称");
            AppendMenuW(sortMenu, MF_STRING, kContextSortByTypeCommand, L"类型");
            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
        }

        HMENU gridMenu = CreatePopupMenu();
        if (gridMenu != nullptr)
        {
            AppendMenuW(gridMenu, MF_STRING, kContextGridAddRow, L"增加行");
            AppendMenuW(gridMenu, MF_STRING, kContextGridRemoveRow, L"减少行");
            AppendMenuW(gridMenu, MF_STRING, kContextGridAddColumn, L"增加列");
            AppendMenuW(gridMenu, MF_STRING, kContextGridRemoveColumn, L"减少列");
            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(gridMenu), L"行列调整");
        }

        HMENU zoomMenu = CreatePopupMenu();
        if (zoomMenu != nullptr)
        {
            const int presets[] = {50, 70, 80, 90, 100, 110, 120, 130, 150, 200};
            for (int pct : presets)
            {
                wchar_t label[16]{};
                swprintf_s(label, L"%d%%", pct);
                UINT flags = MF_STRING;
                if (static_cast<int>(gapScale_ * 100) == pct)
                {
                    flags |= MF_CHECKED;
                }
                AppendMenuW(zoomMenu, flags, kContextZoomPresetFirst + static_cast<UINT>(pct), label);
            }
            AppendMenuW(zoomMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(zoomMenu, MF_STRING, kContextZoomIncrease, L"放大 (+10%)");
            AppendMenuW(zoomMenu, MF_STRING, kContextZoomDecrease, L"缩小 (-10%)");
            wchar_t zoomLabel[32]{};
            swprintf_s(zoomLabel, L"缩放：%d%%", static_cast<int>(gapScale_ * 100));
            AppendMenuW(menu, MF_STRING | MF_POPUP, reinterpret_cast<UINT_PTR>(zoomMenu), zoomLabel);
        }

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextThisDisplayFirstCommand, L"当前显示器显示首屏");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextPasteCommand, L"粘贴");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");

        menuIconPool_.clear();
        SetMenuItemIcon(menu, kContextRefreshCommand, RGB(60, 130, 220), L'R');
        SetMenuItemIcon(menu, kContextPasteCommand, RGB(80, 160, 200), L'P');
        SetMenuItemIcon(menu, kContextMoreCommand, RGB(120, 120, 200), L'>');
        if (sortMenu) SetMenuItemIcon(sortMenu, kContextSortByNameCommand, RGB(100, 160, 100), L'N');
        if (sortMenu) SetMenuItemIcon(sortMenu, kContextSortByTypeCommand, RGB(100, 160, 100), L'T');
        if (gridMenu) SetMenuItemIcon(gridMenu, kContextGridAddRow, RGB(180, 140, 60), L'+');
        if (gridMenu) SetMenuItemIcon(gridMenu, kContextGridRemoveRow, RGB(180, 140, 60), L'-');
        if (gridMenu) SetMenuItemIcon(gridMenu, kContextGridAddColumn, RGB(180, 140, 60), L'+');
        if (gridMenu) SetMenuItemIcon(gridMenu, kContextGridRemoveColumn, RGB(180, 140, 60), L'-');

        SetForegroundWindow(hwnd_);
        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);
        DestroyMenu(menu);
        for (HBITMAP bmp : menuIconPool_) { DeleteObject(bmp); }
        menuIconPool_.clear();

        if (command >= kContextZoomPresetFirst && command <= kContextZoomPresetFirst + 150)
        {
            SetZoom(static_cast<float>(command - kContextZoomPresetFirst) / 100.0f);
        }
        else switch (command)
        {
        case kContextRefreshCommand:
            ReloadItems();
            break;
        case kContextSortByNameCommand:
            SortIconsByName();
            break;
        case kContextSortByTypeCommand:
            SortIconsByType();
            break;
        case kContextGridAddRow:
            AdjustGridRows(1);
            break;
        case kContextGridRemoveRow:
            AdjustGridRows(-1);
            break;
        case kContextGridAddColumn:
            AdjustGridColumns(1);
            break;
        case kContextGridRemoveColumn:
            AdjustGridColumns(-1);
            break;
        case kContextZoomIncrease:
            AdjustZoom(+0.1f);
            break;
        case kContextZoomDecrease:
            AdjustZoom(-0.1f);
            break;
        case kContextThisDisplayFirstCommand:
            SetFirstPageMonitorFromPoint(screenPoint);
            break;
        case kContextPasteCommand:
            InvokeDesktopBackgroundVerb("paste");
            break;
        case kContextMoreCommand:
            ShowDesktopBackgroundContextMenu(screenPoint);
            break;
        default:
            break;
        }
    }

    void SetFirstPageMonitorFromPoint(POINT screenPoint)
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        const GridPage* page = GridPageFromPoint(clientPoint);
        if (page == nullptr || page->monitorId.empty())
        {
            return;
        }

        firstPageMonitorId_ = page->monitorId;
        pageOffset_ = 0;
        ApplyPageMapping();
        ApplySavedGridDimensions();
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void ShowShellContextMenu(POINT screenPoint)
    {
        auto pidls = SelectedChildPidls();
        if (pidls.empty())
        {
            return;
        }

        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        constexpr UINT kFirstCommand = 1;
        constexpr UINT kLastCommand = 0x7FFF;
        hr = contextMenu->QueryContextMenu(menu, 0, kFirstCommand, kLastCommand, CMF_NORMAL | CMF_CANRENAME);
        if (FAILED(hr))
        {
            DestroyMenu(menu);
            return;
        }

        contextMenu.As(&activeContextMenu2_);
        contextMenu.As(&activeContextMenu3_);

        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);

        activeContextMenu2_.Reset();
        activeContextMenu3_.Reset();

        if (command != 0)
        {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCommand);
            invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCommand);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
            ReloadItems();
        }

        DestroyMenu(menu);
    }

    void ShowDesktopBackgroundContextMenu(POINT screenPoint)
    {
        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        constexpr UINT kFirstCommand = 1;
        constexpr UINT kLastCommand = 0x7FFF;
        hr = contextMenu->QueryContextMenu(menu, 0, kFirstCommand, kLastCommand, CMF_NORMAL);
        if (FAILED(hr))
        {
            DestroyMenu(menu);
            return;
        }

        contextMenu.As(&activeContextMenu2_);
        contextMenu.As(&activeContextMenu3_);

        UINT command = TrackPopupMenuEx(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON,
            screenPoint.x,
            screenPoint.y,
            hwnd_,
            nullptr);

        activeContextMenu2_.Reset();
        activeContextMenu3_.Reset();

        if (command != 0)
        {
            CMINVOKECOMMANDINFOEX invoke{};
            invoke.cbSize = sizeof(invoke);
            invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
            invoke.hwnd = hwnd_;
            invoke.lpVerb = MAKEINTRESOURCEA(command - kFirstCommand);
            invoke.lpVerbW = MAKEINTRESOURCEW(command - kFirstCommand);
            invoke.nShow = SW_SHOWNORMAL;
            invoke.ptInvoke = screenPoint;
            contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
            ReloadItems();
        }

        DestroyMenu(menu);
    }

    void InvokeSelectedShellVerb(const char* verb)
    {
        auto pidls = SelectedChildPidls();
        if (pidls.empty())
        {
            return;
        }

        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->GetUIObjectOf(
            hwnd_,
            static_cast<UINT>(pidls.size()),
            pidls.data(),
            IID_IContextMenu,
            nullptr,
            reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        CMINVOKECOMMANDINFO invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.hwnd = hwnd_;
        invoke.lpVerb = verb;
        invoke.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(contextMenu->InvokeCommand(&invoke)))
        {
            ReloadItems();
        }
    }

    void InvokeDesktopBackgroundVerb(const char* verb)
    {
        ComPtr<IContextMenu> contextMenu;
        HRESULT hr = desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
        if (FAILED(hr) || !contextMenu)
        {
            return;
        }

        CMINVOKECOMMANDINFO invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.hwnd = hwnd_;
        invoke.lpVerb = verb;
        invoke.nShow = SW_SHOWNORMAL;
        if (SUCCEEDED(contextMenu->InvokeCommand(&invoke)))
        {
            ReloadItems();
        }
    }

    void BeginRenameSelected()
    {
        if (renameEdit_ != nullptr || selectedCount_ != 1)
        {
            return;
        }

        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i].selected)
            {
                continue;
            }

            renameIndex_ = i;
            RECT rect = GetItemTextRect(items_[i], true);
            InflateRect(&rect, 2, 2);
            RECT screenRect = rect;
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);
            renameEdit_ = CreateWindowExW(
                WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                L"EDIT",
                items_[i].name.c_str(),
                WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
                screenRect.left,
                screenRect.top,
                screenRect.right - screenRect.left,
                screenRect.bottom - screenRect.top,
                hwnd_,
                nullptr,
                instance_,
                nullptr);

            if (renameEdit_ == nullptr)
            {
                return;
            }

            if (renameFont_ != nullptr)
            {
                DeleteObject(renameFont_);
            }
            renameFont_ = CreateFontW(
                -15,
                0,
                0,
                0,
                FW_NORMAL,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI");
            SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(renameFont_ != nullptr ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
            SetWindowSubclass(renameEdit_, &SnowDesktopApp::RenameEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
            SetWindowPos(
                renameEdit_,
                HWND_TOPMOST,
                screenRect.left,
                screenRect.top,
                screenRect.right - screenRect.left,
                screenRect.bottom - screenRect.top,
                SWP_SHOWWINDOW);
            SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
            SetFocus(renameEdit_);
            return;
        }
    }

    void CommitRename(bool cancel)
    {
        if (renameEdit_ == nullptr)
        {
            return;
        }

        HWND edit = renameEdit_;
        renameEdit_ = nullptr;
        RemoveWindowSubclass(edit, &SnowDesktopApp::RenameEditSubclassProc, 1);

        std::wstring newName;
        if (!cancel && renameIndex_ < items_.size())
        {
            int length = GetWindowTextLengthW(edit);
            if (length > 0)
            {
                std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
                GetWindowTextW(edit, buffer.data(), length + 1);
                newName.assign(buffer.data());
            }
        }

        DestroyWindow(edit);
        if (renameFont_ != nullptr)
        {
            DeleteObject(renameFont_);
            renameFont_ = nullptr;
        }
        if (renameBackgroundBrush_ != nullptr)
        {
            DeleteObject(renameBackgroundBrush_);
            renameBackgroundBrush_ = nullptr;
        }

        bool keepUpdatedLayoutSlots = false;
        if (!cancel && renameIndex_ < items_.size() && !newName.empty() && newName != items_[renameIndex_].name)
        {
            int oldSlot = items_[renameIndex_].slot;
            PITEMID_CHILD newChild = nullptr;
            HRESULT hr = desktopFolder_->SetNameOf(
                hwnd_,
                reinterpret_cast<PCUITEMID_CHILD>(items_[renameIndex_].childPidl.get()),
                newName.c_str(),
                SHGDN_NORMAL,
                &newChild);
            if (SUCCEEDED(hr))
            {
                if (newChild != nullptr)
                {
                    PIDLIST_ABSOLUTE newAbsolute = ILCombine(desktopPidl_.get(), newChild);
                    std::wstring newParsingName = StrRetToString(desktopFolder_.Get(), newChild, SHGDN_FORPARSING);
                    if (newAbsolute != nullptr)
                    {
                        LayoutRecord record;
                        record.cell = items_[renameIndex_].gridCell;
                        record.span = items_[renameIndex_].gridSpan;
                        record.hasGrid = true;
                        record.legacySlot = oldSlot;
                        layoutRecords_[GetStableLayoutKey(newAbsolute, newParsingName)] = record;
                        keepUpdatedLayoutSlots = true;
                        ILFree(newAbsolute);
                    }
                }
            }
            if (newChild != nullptr)
            {
                ILFree(newChild);
            }

            if (FAILED(hr))
            {
                MessageBeep(MB_ICONWARNING);
            }
        }

        ReloadItems(!keepUpdatedLayoutSlots);
    }

    static D2D1_RECT_F ToD2DRect(const RECT& rect)
    {
        return D2D1::RectF(
            static_cast<float>(rect.left),
            static_cast<float>(rect.top),
            static_cast<float>(rect.right),
            static_cast<float>(rect.bottom));
    }

    ComPtr<ID2D1Bitmap1> CreateD2DBitmapFromHBitmap(HBITMAP hbm)
    {
        ComPtr<ID2D1Bitmap1> bitmap;
        if (hbm == nullptr || bitmapContext_ == nullptr)
        {
            return bitmap;
        }

        BITMAP bm{};
        if (GetObjectW(hbm, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight == 0)
        {
            return bitmap;
        }

        const int width = bm.bmWidth;
        const int height = std::abs(bm.bmHeight);
        std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));

        if (bm.bmBits != nullptr && bm.bmBitsPixel == 32)
        {
            const auto* src = static_cast<const std::uint8_t*>(bm.bmBits);
            const size_t srcPitch = static_cast<size_t>(std::abs(bm.bmWidthBytes));
            for (int y = 0; y < height; ++y)
            {
                std::memcpy(
                    pixels.data() + (static_cast<size_t>(y) * static_cast<size_t>(width)),
                    src + (static_cast<size_t>(y) * srcPitch),
                    static_cast<size_t>(width) * sizeof(std::uint32_t));
            }
        }
        else
        {
            HDC screenDc = GetDC(nullptr);
            if (screenDc == nullptr)
            {
                return bitmap;
            }

            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = width;
            bi.bmiHeader.biHeight = -height;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;

            if (GetDIBits(screenDc, hbm, 0, static_cast<UINT>(height), pixels.data(), &bi, DIB_RGB_COLORS) == 0)
            {
                ReleaseDC(nullptr, screenDc);
                return bitmap;
            }
            ReleaseDC(nullptr, screenDc);
        }

        bool hasAlpha = false;
        bool hasVisiblePixels = false;
        for (std::uint32_t pixel : pixels)
        {
            if (((pixel >> 24) & 0xff) != 0)
            {
                hasAlpha = true;
            }
            if ((pixel & 0x00ffffff) != 0)
            {
                hasVisiblePixels = true;
            }
        }
        if (!hasAlpha && hasVisiblePixels)
        {
            for (std::uint32_t& pixel : pixels)
            {
                if ((pixel & 0x00ffffff) != 0)
                {
                    pixel |= 0xff000000;
                }
            }
        }
        PremultiplyBgraPixels(pixels.data(), width, height);

        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

        bitmapContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height)),
            pixels.data(),
            static_cast<UINT32>(width * sizeof(std::uint32_t)),
            &props,
            &bitmap);
        return bitmap;
    }

    ID2D1Bitmap1* GetOrCreateD2DBitmap(HBITMAP hbm)
    {
        if (hbm == nullptr)
        {
            return nullptr;
        }

        const auto key = reinterpret_cast<std::uintptr_t>(hbm);
        auto found = d2dIconCache_.find(key);
        if (found != d2dIconCache_.end())
        {
            return found->second.Get();
        }

        ComPtr<ID2D1Bitmap1> bitmap = CreateD2DBitmapFromHBitmap(hbm);
        if (!bitmap)
        {
            return nullptr;
        }

        ID2D1Bitmap1* raw = bitmap.Get();
        d2dIconCache_.emplace(key, std::move(bitmap));
        return raw;
    }

    void DrawD2D()
    {
        if (hwnd_ == nullptr || dcompSurface_ == nullptr || dcompDevice_ == nullptr)
        {
            return;
        }

        HRESULT hr = CreateOrResizeCompositionSurface();
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return;
        }

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        hr = dcompSurface_->BeginDraw(
            nullptr,
            __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext),
            &updateOffset);
        if (FAILED(hr))
        {
            lastGraphicsError_ = hr;
            return;
        }

        ComPtr<ID2D1DeviceContext> context;
        context.Attach(rawContext);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x),
            static_cast<float>(updateOffset.y)));
        context->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        DrawD2DScene(context.Get());

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();

        hr = dcompSurface_->EndDraw();
        if (SUCCEEDED(hr))
        {
            hr = dcompDevice_->Commit();
        }
        lastGraphicsError_ = hr;
    }

    void DrawD2DScene(ID2D1DeviceContext* context)
    {
        if (context == nullptr)
        {
            return;
        }

        for (const auto& item : items_)
        {
            if (IsRectEmptyRect(item.bounds))
            {
                continue;
            }
            if (draggingItems_ && item.selected)
            {
                continue;
            }
            DrawD2DItemAt(context, item, item.bounds, item.selected);
        }

        if (draggingItems_)
        {
            DrawD2DDraggedItems(context);
        }

        RECT client{};
        GetClientRect(hwnd_, &client);
        DrawD2DStatus(context, client);

        if (marqueeActive_)
        {
            DrawD2DFilledRectangle(
                context,
                marqueeRect_,
                D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.20f),
                D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f),
                nullptr);
        }

        if (draggingItems_ && !draggingOverNav_)
        {
            for (RECT targetRect : GetSelectedMovePreviewRects(GetDragTargetPoint(dragCurrentPoint_)))
            {
                targetRect.left += 3;
                targetRect.top += 3;
                targetRect.right -= 3;
                targetRect.bottom -= 3;
                DrawD2DFilledRectangle(
                    context,
                    targetRect,
                    D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.14f),
                    D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f),
                    dottedStrokeStyle_.Get());
            }
        }

        if (externalDragActive_)
        {
            RECT target{};
            int hit = HitTest(externalDragPoint_);
            if (hit >= 0)
            {
                target = GetItemSelectionRect(items_[static_cast<size_t>(hit)], true);
            }
            else
            {
                target = GetGridRect(CellFromPoint(externalDragPoint_));
            }

            target.left += 3;
            target.top += 3;
            target.right -= 3;
            target.bottom -= 3;
            DrawD2DRectangle(context, target, D2D1::ColorF(0.25f, 0.55f, 0.95f, 0.75f), dottedStrokeStyle_.Get());
        }

        if (navButtonsVisible_)
        {
            DrawD2DNavigationPanel(context);
        }
    }

    void DrawD2DFilledRectangle(
        ID2D1DeviceContext* context,
        const RECT& rect,
        const D2D1_COLOR_F& fillColor,
        const D2D1_COLOR_F& strokeColor,
        ID2D1StrokeStyle* strokeStyle)
    {
        ComPtr<ID2D1SolidColorBrush> fillBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(fillColor, &fillBrush)) && fillBrush)
        {
            context->FillRectangle(ToD2DRect(rect), fillBrush.Get());
        }

        DrawD2DRectangle(context, rect, strokeColor, strokeStyle);
    }

    void DrawD2DRectangle(ID2D1DeviceContext* context, const RECT& rect, const D2D1_COLOR_F& color, ID2D1StrokeStyle* strokeStyle)
    {
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(context->CreateSolidColorBrush(color, &brush)) || !brush)
        {
            return;
        }

        context->DrawRectangle(ToD2DRect(rect), brush.Get(), 1.0f, strokeStyle);
    }

    void DrawD2DButton(ID2D1DeviceContext* context, const RECT& rect, const std::wstring& label, bool enabled)
    {
        if (context == nullptr || !enabled)
        {
            return;
        }

        constexpr float kRadius = 4.0f;
        D2D1_RECT_F d2dRect = ToD2DRect(rect);
        D2D1_ROUNDED_RECT roundedRect = D2D1::RoundedRect(d2dRect, kRadius, kRadius);

        ComPtr<ID2D1SolidColorBrush> fillBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &fillBrush);
        if (fillBrush)
        {
            context->FillRoundedRectangle(roundedRect, fillBrush.Get());
        }

        ComPtr<ID2D1SolidColorBrush> strokeBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.56f, 0.60f, 0.66f, 0.95f), &strokeBrush);
        if (strokeBrush)
        {
            context->DrawRoundedRectangle(roundedRect, strokeBrush.Get(), 1.0f);
        }

        if (!itemTextFormat_)
        {
            return;
        }

        ComPtr<ID2D1SolidColorBrush> textBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.13f, 0.18f, 1.0f), &textBrush);
        if (!textBrush)
        {
            return;
        }

        D2D1_RECT_F textRect = ToD2DRect(MakeRect(rect.left + 4, rect.top + 5, rect.right - 4, rect.bottom));
        context->DrawTextW(
            label.c_str(),
            static_cast<UINT32>(label.size()),
            itemTextFormat_.Get(),
            &textRect,
            textBrush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void UpdateNavButtonHover(POINT point)
    {
        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr)
        {
            navButtonsVisible_ = false;
            return;
        }

        constexpr LONG kHoverZoneWidth = 220;
        constexpr LONG kHoverZoneHeight = 80;
        RECT hoverZone = MakeRect(
            page->workArea.right - kHoverZoneWidth,
            page->workArea.bottom - kHoverZoneHeight,
            page->workArea.right,
            page->workArea.bottom);
        navButtonsHoverZone_ = hoverZone;

        bool wasVisible = navButtonsVisible_;
        navButtonsVisible_ = PtInRect(&hoverZone, point) != FALSE;

        if (wasVisible != navButtonsVisible_)
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
        }

        if (!wasVisible && navButtonsVisible_)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd_, HOVER_DEFAULT };
            TrackMouseEvent(&tme);
        }
    }

    void DrawD2DNavigationPanel(ID2D1DeviceContext* context)
    {
        if (context == nullptr) return;

        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr) return;

        const bool hasPrev = pageOffset_ > 0;
        const bool hasNext = pageOffset_ < MaxPageOffset();
        if (!hasPrev && !hasNext) return;

        constexpr LONG kButtonWidth = 68;
        constexpr LONG kButtonHeight = 28;
        constexpr LONG kGap = 8;
        constexpr LONG kPanelPaddingX = 10;
        constexpr LONG kPanelPaddingY = 8;
        constexpr float kCornerRadius = 8.0f;

        const int visibleCount = (hasPrev ? 1 : 0) + (hasNext ? 1 : 0);
        const LONG kPanelWidth = kButtonWidth * visibleCount + kGap * (visibleCount - 1) + kPanelPaddingX * 2;
        const LONG kPanelHeight = kButtonHeight + kPanelPaddingY * 2;

        const LONG panelRight = std::max<LONG>(page->workArea.left + kPanelWidth,
            page->workArea.right - page->marginX - 10);
        const LONG panelBottom = std::max<LONG>(page->workArea.top + kPanelHeight,
            page->workArea.bottom - page->marginY - 10);
        const LONG panelLeft = panelRight - kPanelWidth;
        const LONG panelTop = panelBottom - kPanelHeight;

        D2D1_RECT_F panelRect = D2D1::RectF(
            static_cast<float>(panelLeft), static_cast<float>(panelTop),
            static_cast<float>(panelRight), static_cast<float>(panelBottom));
        D2D1_ROUNDED_RECT roundedPanel = D2D1::RoundedRect(panelRect, kCornerRadius, kCornerRadius);

        ComPtr<ID2D1SolidColorBrush> panelBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &panelBrush)) && panelBrush)
        {
            context->FillRoundedRectangle(roundedPanel, panelBrush.Get());
        }

        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.70f), &borderBrush)) && borderBrush)
        {
            context->DrawRoundedRectangle(roundedPanel, borderBrush.Get(), 1.0f);
        }

        LONG btnX = panelLeft + kPanelPaddingX;
        if (hasPrev)
        {
            RECT prevRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            DrawD2DButton(context, prevRect, L"上一页", true);
            btnX += kButtonWidth + kGap;
        }
        if (hasNext)
        {
            RECT nextRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            DrawD2DButton(context, nextRect, L"下一页", true);
        }
    }

    bool GetPageNavigationRects(RECT& previousRect, RECT& nextRect) const
    {
        const GridPage* page = FindExactGridPage(lastMonitorPageId_);
        if (page == nullptr)
        {
            return false;
        }

        constexpr LONG buttonWidth = 68;
        constexpr LONG buttonHeight = 28;
        constexpr LONG gap = 8;
        const LONG right = std::max<LONG>(page->workArea.left + buttonWidth, page->workArea.right - page->marginX - 10);
        const LONG bottom = std::max<LONG>(page->workArea.top + buttonHeight, page->workArea.bottom - page->marginY - 10);
        nextRect = MakeRect(right - buttonWidth, bottom - buttonHeight, right, bottom);
        previousRect = MakeRect(nextRect.left - gap - buttonWidth, nextRect.top, nextRect.left - gap, nextRect.bottom);
        return true;
    }

    void DrawD2DPageNavigationControls(ID2D1DeviceContext* context)
    {
        RECT previousRect{};
        RECT nextRect{};
        if (!GetPageNavigationRects(previousRect, nextRect))
        {
            return;
        }

        DrawD2DButton(context, previousRect, L"上一页", pageOffset_ > 0);
        DrawD2DButton(context, nextRect, L"下一页", pageOffset_ < MaxPageOffset());
    }

    int HitTestNavButton(POINT point) const
    {
        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr) return 0;

        const bool hasPrev = pageOffset_ > 0;
        const bool hasNext = pageOffset_ < MaxPageOffset();
        if (!hasPrev && !hasNext) return 0;

        constexpr LONG kButtonWidth = 68;
        constexpr LONG kButtonHeight = 28;
        constexpr LONG kGap = 8;
        constexpr LONG kPanelPaddingX = 10;
        constexpr LONG kPanelPaddingY = 8;
        const int visibleCount = (hasPrev ? 1 : 0) + (hasNext ? 1 : 0);
        const LONG kPanelWidth = kButtonWidth * visibleCount + kGap * (visibleCount - 1) + kPanelPaddingX * 2;
        const LONG kPanelHeight = kButtonHeight + kPanelPaddingY * 2;

        const LONG panelRight = std::max<LONG>(page->workArea.left + kPanelWidth,
            page->workArea.right - page->marginX - 10);
        const LONG panelBottom = std::max<LONG>(page->workArea.top + kPanelHeight,
            page->workArea.bottom - page->marginY - 10);
        const LONG panelLeft = panelRight - kPanelWidth;
        const LONG panelTop = panelBottom - kPanelHeight;

        LONG btnX = panelLeft + kPanelPaddingX;
        if (hasPrev)
        {
            RECT prevRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&prevRect, point)) return -1;
            btnX += kButtonWidth + kGap;
        }
        if (hasNext)
        {
            RECT nextRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&nextRect, point)) return 1;
        }
        return 0;
    }

    bool HandlePageNavigationClick(POINT point)
    {
        const GridPage* page = nullptr;
        if (!lastMonitorPageId_.empty())
        {
            page = FindExactGridPage(lastMonitorPageId_);
        }
        if (page == nullptr && !gridPages_.empty())
        {
            page = &gridPages_[0];
        }
        if (page == nullptr) return false;

        const bool hasPrev = pageOffset_ > 0;
        const bool hasNext = pageOffset_ < MaxPageOffset();
        if (!hasPrev && !hasNext) return false;

        constexpr LONG kButtonWidth = 68;
        constexpr LONG kButtonHeight = 28;
        constexpr LONG kGap = 8;
        constexpr LONG kPanelPaddingX = 10;
        constexpr LONG kPanelPaddingY = 8;

        const int visibleCount = (hasPrev ? 1 : 0) + (hasNext ? 1 : 0);
        const LONG kPanelWidth = kButtonWidth * visibleCount + kGap * (visibleCount - 1) + kPanelPaddingX * 2;
        const LONG kPanelHeight = kButtonHeight + kPanelPaddingY * 2;

        const LONG panelRight = std::max<LONG>(page->workArea.left + kPanelWidth,
            page->workArea.right - page->marginX - 10);
        const LONG panelBottom = std::max<LONG>(page->workArea.top + kPanelHeight,
            page->workArea.bottom - page->marginY - 10);
        const LONG panelLeft = panelRight - kPanelWidth;
        const LONG panelTop = panelBottom - kPanelHeight;

        LONG btnX = panelLeft + kPanelPaddingX;
        int delta = 0;
        if (hasPrev)
        {
            RECT prevRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&prevRect, point)) delta = -1;
            btnX += kButtonWidth + kGap;
        }
        if (hasNext && delta == 0)
        {
            RECT nextRect = MakeRect(btnX, panelTop + kPanelPaddingY,
                btnX + kButtonWidth, panelTop + kPanelPaddingY + kButtonHeight);
            if (PtInRect(&nextRect, point)) delta = 1;
        }

        if (delta == 0) return false;

        pageOffset_ = std::clamp(pageOffset_ + delta, 0, MaxPageOffset());
        ApplyPageMapping();
        LayoutItems();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    void DrawD2DItemAt(ID2D1DeviceContext* context, const DesktopItem& item, RECT bounds, bool selected)
    {
        if (IsRectEmptyRect(bounds))
        {
            return;
        }

        RECT iconRect = GetItemIconRect(bounds);
        const float iconX = static_cast<float>(iconRect.left);
        const float iconY = static_cast<float>(iconRect.top);
        const float drawSize = static_cast<float>(iconRect.right - iconRect.left);

        if (selected)
        {
            DrawD2DFilledRectangle(
                context,
                GetItemSelectionRect(bounds, true),
                D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.34f),
                D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.55f),
                nullptr);
        }

        ID2D1Bitmap1* iconBitmap = GetOrCreateD2DBitmap(item.iconBitmap);
        if (iconBitmap != nullptr)
        {
            D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + drawSize, iconY + drawSize);
            context->DrawBitmap(iconBitmap, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        else if (item.sysIconIndex >= 0 && sysImageList_)
        {
            HICON icon = nullptr;
            if (SUCCEEDED(sysImageList_->GetIcon(item.sysIconIndex, ILD_TRANSPARENT | ILD_PRESERVEALPHA, &icon)) && icon != nullptr)
            {
                SIZE size{};
                HBITMAP fallbackBitmap = CreateAlphaBitmapFromIcon(icon, static_cast<int>(std::ceil(drawSize)), static_cast<int>(std::ceil(drawSize)), size);
                ComPtr<ID2D1Bitmap1> fallback = CreateD2DBitmapFromHBitmap(fallbackBitmap);
                if (fallback)
                {
                    D2D1_RECT_F dst = D2D1::RectF(iconX, iconY, iconX + drawSize, iconY + drawSize);
                    context->DrawBitmap(fallback.Get(), dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
                }
                if (fallbackBitmap != nullptr)
                {
                    DeleteObject(fallbackBitmap);
                }
                DestroyIcon(icon);
            }
        }

        if (dwriteFactory_ && itemTextFormat_ && !item.name.empty())
        {
            RECT textRect = GetItemTextRect(bounds, selected);
            const float textWidth = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
            const float textHeight = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(dwriteFactory_->CreateTextLayout(
                    item.name.c_str(),
                    static_cast<UINT32>(item.name.size()),
                    itemTextFormat_.Get(),
                    textWidth,
                    textHeight,
                    &layout)) &&
                layout)
            {
                ComPtr<ID2D1SolidColorBrush> shadowBrush;
                ComPtr<ID2D1SolidColorBrush> textBrush;
                context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f), &shadowBrush);
                context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &textBrush);

                const float tx = static_cast<float>(textRect.left);
                const float ty = static_cast<float>(textRect.top);
                if (shadowBrush)
                {
                    const D2D1_POINT_2F offsets[] = {
                        D2D1::Point2F(tx - 1.0f, ty),
                        D2D1::Point2F(tx + 1.0f, ty),
                        D2D1::Point2F(tx, ty - 1.0f),
                        D2D1::Point2F(tx, ty + 1.0f),
                    };
                    for (const D2D1_POINT_2F& point : offsets)
                    {
                        context->DrawTextLayout(point, layout.Get(), shadowBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
                if (textBrush)
                {
                    context->DrawTextLayout(D2D1::Point2F(tx, ty), layout.Get(), textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }
    }

    void DrawD2DDraggedItems(ID2D1DeviceContext* context)
    {
        int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
        int dy = dragCurrentPoint_.y - mouseDownPoint_.y;
        for (const auto& item : items_)
        {
            if (!item.selected)
            {
                continue;
            }
            if (IsRectEmptyRect(item.bounds))
            {
                continue;
            }

            RECT moved = item.bounds;
            OffsetRect(&moved, dx, dy);
            DrawD2DItemAt(context, item, moved, true);
        }
    }

    void DrawD2DStatus(ID2D1DeviceContext* context, const RECT& client)
    {
        if (!dwriteFactory_ || !statusTextFormat_ || client.right <= 0 || client.bottom <= 0)
        {
            return;
        }

        const std::wstring status = L"SnowDesktop 桌面验证  |  F5 重新加载  |  Esc 退出并恢复 Explorer 图标";
        const LONG left = std::max<LONG>(16, client.right - 620);
        const LONG right = std::max<LONG>(left + 1, client.right - 16);
        const LONG top = std::max<LONG>(0, client.bottom - 34);
        const LONG bottom = std::max<LONG>(top + 1, client.bottom - 12);
        const float width = static_cast<float>(right - left);
        const float height = static_cast<float>(bottom - top);

        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwriteFactory_->CreateTextLayout(
                status.c_str(),
                static_cast<UINT32>(status.size()),
                statusTextFormat_.Get(),
                width,
                height,
                &layout)) ||
            !layout)
        {
            return;
        }

        ComPtr<ID2D1SolidColorBrush> shadowBrush;
        ComPtr<ID2D1SolidColorBrush> textBrush;
        context->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.65f), &shadowBrush);
        context->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.96f, 1.0f, 0.92f), &textBrush);
        if (shadowBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(left + 1), static_cast<float>(top + 1)),
                layout.Get(),
                shadowBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (textBrush)
        {
            context->DrawTextLayout(
                D2D1::Point2F(static_cast<float>(left), static_cast<float>(top)),
                layout.Get(),
                textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    void Draw(HDC hdc, const RECT& paintRect)
    {
        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT clippedPaint{};
        if (!IntersectRect(&clippedPaint, &client, &paintRect))
        {
            return;
        }

        int width = clippedPaint.right - clippedPaint.left;
        int height = clippedPaint.bottom - clippedPaint.top;

        HDC memoryDc = CreateCompatibleDC(hdc);
        HBITMAP bitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);

        HBRUSH transparentBrush = CreateSolidBrush(kTransparentKey);
        RECT localPaint{ 0, 0, width, height };
        FillRect(memoryDc, &localPaint, transparentBrush);
        DeleteObject(transparentBrush);
        SetViewportOrgEx(memoryDc, -clippedPaint.left, -clippedPaint.top, nullptr);

        HFONT font = CreateFontW(
            -15,
            0,
            0,
            0,
            FW_BOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(memoryDc, font);
        SetBkMode(memoryDc, TRANSPARENT);

        for (const auto& item : items_)
        {
            if (draggingItems_ && item.selected)
            {
                continue;
            }
            if (!RectsIntersect(item.bounds, clippedPaint))
            {
                continue;
            }
            DrawItem(memoryDc, item);
        }

        if (draggingItems_)
        {
            DrawDraggedItems(memoryDc);
        }

        DrawStatus(memoryDc, client);

        if (marqueeActive_)
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(120, 180, 255));
            HGDIOBJ oldPen = SelectObject(memoryDc, pen);
            HGDIOBJ nullBrush = GetStockObject(NULL_BRUSH);
            HGDIOBJ oldNullBrush = SelectObject(memoryDc, nullBrush);
            Rectangle(memoryDc, marqueeRect_.left, marqueeRect_.top, marqueeRect_.right, marqueeRect_.bottom);
            SelectObject(memoryDc, oldNullBrush);
            SelectObject(memoryDc, oldPen);
            DeleteObject(pen);
        }

        if (draggingItems_ && !draggingOverNav_)
        {
            RECT targetRect = GetTargetRectAt(GetDragTargetPoint(dragCurrentPoint_));
            targetRect.left += 3;
            targetRect.top += 3;
            targetRect.right -= 3;
            targetRect.bottom -= 3;

            HPEN pen = CreatePen(PS_DOT, 1, RGB(120, 180, 255));
            HGDIOBJ oldPen = SelectObject(memoryDc, pen);
            HGDIOBJ oldBrush = SelectObject(memoryDc, GetStockObject(NULL_BRUSH));
            Rectangle(memoryDc, targetRect.left, targetRect.top, targetRect.right, targetRect.bottom);
            SelectObject(memoryDc, oldBrush);
            SelectObject(memoryDc, oldPen);
            DeleteObject(pen);
        }

        if (externalDragActive_)
        {
            DrawExternalDropTarget(memoryDc);
        }

        SetViewportOrgEx(memoryDc, 0, 0, nullptr);
        BitBlt(hdc, clippedPaint.left, clippedPaint.top, width, height, memoryDc, 0, 0, SRCCOPY);

        SelectObject(memoryDc, oldFont);
        DeleteObject(font);
        SelectObject(memoryDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(memoryDc);
    }

    void ClampAlphaToColorKey(HBITMAP bitmap, COLORREF key)
    {
        if (bitmap == nullptr) return;
        BITMAP bm{};
        if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmBitsPixel != 32 || bm.bmBits == nullptr) return;

        int w = bm.bmWidth;
        int absH = std::abs(bm.bmHeight);
        auto* pixels = static_cast<std::uint32_t*>(bm.bmBits);
        size_t count = static_cast<size_t>(w) * static_cast<size_t>(absH);

        for (size_t i = 0; i < count; ++i)
        {
            std::uint8_t a = static_cast<std::uint8_t>((pixels[i] >> 24) & 0xff);
            std::uint8_t r = static_cast<std::uint8_t>((pixels[i] >> 16) & 0xff);
            std::uint8_t g = static_cast<std::uint8_t>((pixels[i] >> 8) & 0xff);
            std::uint8_t b = static_cast<std::uint8_t>(pixels[i] & 0xff);
            if (a < 250 && (static_cast<int>(r) + static_cast<int>(g) + static_cast<int>(b)) < 150)
            {
                pixels[i] = 0;
            }
        }
        UNREFERENCED_PARAMETER(key);
    }

    void SnapPixelsToColorKey(HDC hdc, const RECT& rect, COLORREF key, int threshold) // unused, kept for reference
    {
        int w = rect.right - rect.left;
        int h = rect.bottom - rect.top;
        if (w <= 0 || h <= 0) return;

        BYTE keyR = GetRValue(key);
        BYTE keyG = GetGValue(key);
        BYTE keyB = GetBValue(key);

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::vector<std::uint32_t> pixels(count);

        HBITMAP target = static_cast<HBITMAP>(GetCurrentObject(hdc, OBJ_BITMAP));
        if (target == nullptr || GetDIBits(hdc, target, rect.top, static_cast<UINT>(h),
                pixels.data(), &bi, DIB_RGB_COLORS) == 0)
        {
            return;
        }

        for (size_t i = 0; i < count; ++i)
        {
            BYTE r = static_cast<BYTE>((pixels[i] >> 16) & 0xff);
            BYTE g = static_cast<BYTE>((pixels[i] >> 8) & 0xff);
            BYTE b = static_cast<BYTE>(pixels[i] & 0xff);
            int dr = std::abs(static_cast<int>(r) - static_cast<int>(keyR));
            int dg = std::abs(static_cast<int>(g) - static_cast<int>(keyG));
            int db = std::abs(static_cast<int>(b) - static_cast<int>(keyB));
            if (dr + dg + db <= threshold)
            {
                pixels[i] = (pixels[i] & 0xff000000) |
                    keyB | (static_cast<std::uint32_t>(keyG) << 8) | (static_cast<std::uint32_t>(keyR) << 16);
            }
        }

        SetDIBits(hdc, target, rect.top, static_cast<UINT>(h), pixels.data(), &bi, DIB_RGB_COLORS);
    }

    void DrawAlphaRect(HDC hdc, const RECT& rect, COLORREF color, BYTE alpha)
    {
        int width = std::max(0, static_cast<int>(rect.right - rect.left));
        int height = std::max(0, static_cast<int>(rect.bottom - rect.top));
        if (width == 0 || height == 0)
        {
            return;
        }

        HDC alphaDc = CreateCompatibleDC(hdc);
        HBITMAP alphaBitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ oldBitmap = SelectObject(alphaDc, alphaBitmap);

        HBRUSH brush = CreateSolidBrush(color);
        RECT local{ 0, 0, width, height };
        FillRect(alphaDc, &local, brush);
        DeleteObject(brush);

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = alpha;
        AlphaBlend(hdc, rect.left, rect.top, width, height, alphaDc, 0, 0, width, height, blend);

        SelectObject(alphaDc, oldBitmap);
        DeleteObject(alphaBitmap);
        DeleteDC(alphaDc);
    }

    void DrawItem(HDC hdc, const DesktopItem& item)
    {
        DrawItemAt(hdc, item, item.bounds, item.selected);
    }

    void DrawItemAt(HDC hdc, const DesktopItem& item, RECT bounds, bool selected)
    {
        if (IsRectEmptyRect(bounds))
        {
            return;
        }

        const int iconX = bounds.left + (kCellWidth - kIconSize) / 2;
        const int iconY = bounds.top + 2;
        const int contentBottom = bounds.top + kTextTop + kTextHeight + 6;

        if (selected)
        {
            RECT highlight = MakeRect(bounds.left + 4, bounds.top, bounds.right - 4, contentBottom);

            HPEN pen = CreatePen(PS_SOLID, 1, RGB(155, 200, 255));
            HGDIOBJ oldPen = SelectObject(hdc, pen);
            HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, highlight.left, highlight.top, highlight.right, highlight.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);
        }

        if (item.iconBitmap != nullptr)
        {
            BITMAP bm{};
            if (GetObjectW(item.iconBitmap, sizeof(bm), &bm) != 0 && bm.bmBitsPixel == 32)
            {
                BLENDFUNCTION blend{};
                blend.BlendOp = AC_SRC_OVER;
                blend.SourceConstantAlpha = 255;
                blend.AlphaFormat = AC_SRC_ALPHA;
                HDC iconDc = CreateCompatibleDC(hdc);
                HGDIOBJ oldBmp = SelectObject(iconDc, item.iconBitmap);
                AlphaBlend(hdc, iconX, iconY, kIconSize, kIconSize,
                    iconDc, 0, 0, bm.bmWidth, bm.bmHeight, blend);
                SelectObject(iconDc, oldBmp);
                DeleteDC(iconDc);
            }
        }
        else if (item.sysIconIndex >= 0 && sysImageList_)
        {
            IMAGELISTDRAWPARAMS params{};
            params.cbSize = sizeof(params);
            params.i = item.sysIconIndex;
            params.hdcDst = hdc;
            params.x = iconX;
            params.y = iconY;
            params.cx = kIconSize;
            params.cy = kIconSize;
            params.rgbBk = CLR_NONE;
            params.fStyle = ILD_SCALE | ILD_TRANSPARENT;
            sysImageList_->Draw(&params);
        }

        RECT textRect = MakeRect(bounds.left + 2, bounds.top + kTextTop, bounds.right - 2, bounds.top + kTextTop + kTextHeight);
        constexpr UINT textFlags = DT_CENTER | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;
        RECT shadowRect = textRect;
        SetTextColor(hdc, RGB(0, 0, 0));
        OffsetRect(&shadowRect, -1, 0);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        shadowRect = textRect;
        OffsetRect(&shadowRect, 1, 0);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        shadowRect = textRect;
        OffsetRect(&shadowRect, 0, 1);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        shadowRect = textRect;
        OffsetRect(&shadowRect, 0, -1);
        DrawTextW(hdc, item.name.c_str(), -1, &shadowRect, textFlags);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetTextColor(hdc, RGB(255, 255, 255));
        DrawTextW(hdc, item.name.c_str(), -1, &textRect, textFlags);
    }

    void DrawDraggedItems(HDC hdc)
    {
        int dx = dragCurrentPoint_.x - mouseDownPoint_.x;
        int dy = dragCurrentPoint_.y - mouseDownPoint_.y;
        for (const auto& item : items_)
        {
            if (!item.selected)
            {
                continue;
            }

            RECT moved = item.bounds;
            OffsetRect(&moved, dx, dy);
            DrawItemAt(hdc, item, moved, true);
        }
    }

    void DrawExternalDropTarget(HDC hdc)
    {
        RECT target{};
        int hit = HitTest(externalDragPoint_);
        if (hit >= 0)
        {
            target = items_[static_cast<size_t>(hit)].bounds;
        }
        else
        {
            target = GetGridRect(CellFromPoint(externalDragPoint_));
        }

        target.left += 3;
        target.top += 3;
        target.right -= 3;
        target.bottom -= 3;

        HPEN pen = CreatePen(PS_DOT, 1, RGB(120, 180, 255));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, target.left, target.top, target.right, target.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    void DrawStatus(HDC hdc, const RECT& client)
    {
        std::wstring status = L"SnowDesktop 桌面验证  |  F5 重新加载  |  Esc 退出并恢复 Explorer 图标";
        RECT textRect = MakeRect(client.right - 620, client.bottom - 34, client.right - 16, client.bottom - 12);
        RECT shadowRect = textRect;
        OffsetRect(&shadowRect, 1, 1);

        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextW(hdc, status.c_str(), -1, &shadowRect, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(hdc, RGB(235, 245, 255));
        DrawTextW(hdc, status.c_str(), -1, &textRect, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (activeContextMenu3_)
        {
            LRESULT result = 0;
            if (SUCCEEDED(activeContextMenu3_->HandleMenuMsg2(message, wParam, lParam, &result)))
            {
                return result;
            }
        }
        else if (activeContextMenu2_)
        {
            if (SUCCEEDED(activeContextMenu2_->HandleMenuMsg(message, wParam, lParam)))
            {
                return 0;
            }
        }

        switch (message)
        {
        case WM_PAINT:
        {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            EndPaint(hwnd_, &ps);
            DrawD2D();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            virtualWidth_ = LOWORD(lParam);
            virtualHeight_ = HIWORD(lParam);
            if (dcompVisual_)
            {
                HRESULT hr = CreateOrResizeCompositionSurface();
                if (FAILED(hr))
                {
                    lastGraphicsError_ = hr;
                }
            }
            UpdateLayoutWorkArea();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            virtualLeft_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
            virtualTop_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
            virtualWidth_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            virtualHeight_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (dcompVisual_)
            {
                HRESULT hr = CreateOrResizeCompositionSurface();
                if (FAILED(hr))
                {
                    lastGraphicsError_ = hr;
                }
            }
            UpdateLayoutWorkArea();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_LBUTTONDOWN:
            OnLeftButtonDown(wParam, lParam);
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(wParam, lParam);
            return 0;
        case WM_MOUSELEAVE:
            if (navButtonsVisible_)
            {
                navButtonsVisible_ = false;
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            return 0;
        case WM_LBUTTONUP:
            OnLeftButtonUp(wParam, lParam);
            return 0;
        case WM_LBUTTONDBLCLK:
            OnDoubleClick(lParam);
            return 0;
        case WM_RBUTTONUP:
            OnRightButtonUp(lParam);
            return 0;
        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
        case WM_CTLCOLOREDIT:
            if (reinterpret_cast<HWND>(lParam) == renameEdit_)
            {
                HDC editDc = reinterpret_cast<HDC>(wParam);
                SetTextColor(editDc, RGB(24, 32, 42));
                SetBkColor(editDc, RGB(255, 255, 255));
                if (renameBackgroundBrush_ == nullptr)
                {
                    renameBackgroundBrush_ = CreateSolidBrush(RGB(255, 255, 255));
                }
                return reinterpret_cast<LRESULT>(renameBackgroundBrush_);
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case kTrayCallbackMessage:
            OnTrayCallback(lParam);
            return 0;
        case kShellChangeMessage:
            SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wParam == 1)
            {
                DestroyWindow(hwnd_);
                return 0;
            }
            if (wParam == kShellChangeTimerId)
            {
                KillTimer(hwnd_, kShellChangeTimerId);
                if (!mouseDown_ && !draggingItems_ && !reloading_)
                {
                    ReloadItems();
                }
                return 0;
            }
            if (wParam == kRecycleBinPollTimerId)
            {
                SHQUERYRBINFO info{};
                info.cbSize = sizeof(info);
                if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)))
                {
                    if (lastRecycleBinItemCount_ >= 0 && info.i64NumItems != lastRecycleBinItemCount_)
                    {
                        if (!mouseDown_ && !draggingItems_ && !reloading_)
                        {
                            ReloadItems();
                        }
                    }
                    lastRecycleBinItemCount_ = info.i64NumItems;
                }
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_DESTROY:
            if (renameEdit_ != nullptr)
            {
                CommitRename(true);
            }
            SaveLayoutSlots();
            HideDragHintWindow();
            DestroyDragHintWindow();
            UnregisterOleDropTarget();
            if (shellChangeRegId_ != 0)
            {
                SHChangeNotifyDeregister(shellChangeRegId_);
                shellChangeRegId_ = 0;
            }
            KillTimer(hwnd_, kShellChangeTimerId);
            RemoveTrayIcon();
            RestoreExplorerIcons();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void OnLeftButtonDown(WPARAM wParam, LPARAM lParam)
    {
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        SetFocus(hwnd_);
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (HandlePageNavigationClick(point))
        {
            return;
        }

        SetCapture(hwnd_);
        mouseDown_ = true;
        marqueeActive_ = false;
        mouseDownPoint_ = point;
        dragCurrentPoint_ = mouseDownPoint_;
        marqueeRect_ = MakeRect(mouseDownPoint_.x, mouseDownPoint_.y, mouseDownPoint_.x, mouseDownPoint_.y);

        int hit = HitTest(mouseDownPoint_);
        mouseDownHit_ = hit;
        draggingItems_ = false;
        draggingOverNav_ = false;
        bool ctrl = (wParam & MK_CONTROL) != 0;
        if (hit >= 0)
        {
            int minCol = INT_MAX;
            int minRow = INT_MAX;
            for (const auto& item : items_)
            {
                if (item.selected)
                {
                    minCol = std::min(minCol, item.gridCell.column);
                    minRow = std::min(minRow, item.gridCell.row);
                }
            }
            if (minCol == INT_MAX)
            {
                minCol = items_[static_cast<size_t>(hit)].gridCell.column;
                minRow = items_[static_cast<size_t>(hit)].gridCell.row;
            }
            GridCell groupOrigin = items_[static_cast<size_t>(hit)].gridCell;
            groupOrigin.column = minCol;
            groupOrigin.row = minRow;
            RECT groupRect = GetGridRect(groupOrigin, {1, 1});
            dragGroupOriginX_ = groupRect.left;
            dragGroupOriginY_ = groupRect.top;

            if (ctrl)
            {
                ToggleSelection(static_cast<size_t>(hit));
            }
            else if (!items_[static_cast<size_t>(hit)].selected)
            {
                SelectOnly(static_cast<size_t>(hit));
            }
        }
        else if (!ctrl)
        {
            ClearSelection();
        }

        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OnMouseMove(WPARAM, LPARAM lParam)
    {
        POINT current{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        UpdateNavButtonHover(current);

        if (!mouseDown_)
        {
            return;
        }
        if (std::abs(current.x - mouseDownPoint_.x) > 3 || std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            int hit = mouseDownHit_;
            if (hit >= 0 && items_[static_cast<size_t>(hit)].selected)
            {
                RECT oldDirty = draggingItems_ ? GetInternalDragDirtyRect(dragCurrentPoint_) : GetSelectedDragBoundsAt(mouseDownPoint_);
                draggingItems_ = true;
                dragCurrentPoint_ = current;
                int navDelta = HitTestNavButton(current);
                if (navDelta != 0)
                {
                    draggingOverNav_ = true;
                    dragHint_ = navDelta < 0 ? L"释放：移动到上一页" : L"释放：移动到下一页";
                }
                else
                {
                    draggingOverNav_ = false;
                    dragTargetCell_ = CellFromPoint(GetDragTargetPoint(current));
                    dragHint_ = MakeInternalDragHint(current);
                }
                ShowDragHintWindow(dragCurrentPoint_, dragHint_);
                InvalidateFast(UnionCopy(oldDirty, GetInternalDragDirtyRect(current)));
            }
            else if (hit < 0)
            {
                marqueeActive_ = true;
                marqueeRect_ = NormalizeRect(mouseDownPoint_, current);
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        }
    }

    void OnLeftButtonUp(WPARAM wParam, LPARAM lParam)
    {
        if (GetCapture() == hwnd_)
        {
            ReleaseCapture();
        }

        if (draggingItems_)
        {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            int navDelta = HitTestNavButton(point);
            if (navDelta != 0)
            {
                pageOffset_ = std::clamp(pageOffset_ + navDelta, 0, MaxPageOffset());
                ApplyPageMapping();
                for (auto& item : items_)
                {
                    if (item.selected)
                    {
                        item.gridCell.pageId = lastMonitorPageId_;
                        item.gridCell.column = 0;
                        item.gridCell.row = 0;
                    }
                }
                SaveLayoutSlots();
                ReloadItems(false);
            }
            else
            {
                int hit = HitTest(point);
                if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
                {
                    ComPtr<IDataObject> dataObject = CreateSelectedDataObject();
                    if (dataObject)
                    {
                        DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                        DropDataObjectAt(dataObject.Get(), point, MK_LBUTTON, &effect);
                        ReloadItems();
                    }
                }
                else
                {
                    MoveSelectedItemsToCell(CellFromPoint(GetDragTargetPoint(point)));
                    LayoutItems();
                }
            }
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        else if (marqueeActive_)
        {
            bool ctrl = (wParam & MK_CONTROL) != 0;
            if (!ctrl)
            {
                ClearSelection();
            }

            for (auto& item : items_)
            {
                if (IsRectEmptyRect(item.bounds))
                {
                    continue;
                }
                if (RectsIntersect(GetItemSelectionRect(item, false), marqueeRect_) && !item.selected)
                {
                    item.selected = true;
                    ++selectedCount_;
                }
            }

            marqueeActive_ = false;
            InvalidateRect(hwnd_, nullptr, TRUE);
        }

        mouseDown_ = false;
        draggingItems_ = false;
        draggingOverNav_ = false;
        dragTargetCell_ = {};
        dragHint_.clear();
        HideDragHintWindow();
        mouseDownHit_ = -1;
    }

    void OnDoubleClick(LPARAM lParam)
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTest(point);
        if (hit >= 0)
        {
            OpenItem(static_cast<size_t>(hit));
        }
    }

    void OnRightButtonUp(LPARAM lParam)
    {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hit = HitTest(point);
        if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
        {
            SelectOnly(static_cast<size_t>(hit));
            InvalidateRect(hwnd_, nullptr, TRUE);
        }

        POINT screenPoint = point;
        ClientToScreen(hwnd_, &screenPoint);
        if (hit >= 0)
        {
            if (IsProtectedDesktopIcon(items_[static_cast<size_t>(hit)]))
            {
                ShowShellContextMenu(screenPoint);
            }
            else
            {
                ShowCustomItemContextMenu(screenPoint);
            }
        }
        else
        {
            ShowCustomBackgroundContextMenu(screenPoint);
        }
    }

    void OnCommand(WORD command)
    {
        switch (command)
        {
        case kTrayReloadCommand:
            ReloadItems();
            break;
        case kTraySortByNameCommand:
            SortIconsByName();
            break;
        case kTraySortByTypeCommand:
            SortIconsByType();
            break;
        case kTraySwitchNativeCommand:
            SwitchToNativeDesktop();
            break;
        case kTraySwitchCustomCommand:
            SwitchToCustomDesktop();
            break;
        case kTrayExitCommand:
            DestroyWindow(hwnd_);
            break;
        case kTrayDesktopIconThisPC:
        case kTrayDesktopIconUserFiles:
        case kTrayDesktopIconNetwork:
        case kTrayDesktopIconControlPanel:
        case kTrayDesktopIconRecycleBin:
            ToggleDesktopIconVisibility(command);
            break;
        default:
            break;
        }
    }

    void OnTrayCallback(LPARAM lParam)
    {
        UINT message = LOWORD(lParam);
        if (message == WM_CONTEXTMENU || message == WM_RBUTTONUP)
        {
            if (trayMenuShowing_)
            {
                return;
            }
            trayMenuShowing_ = true;
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
            trayMenuShowing_ = false;
        }
        else if (message == WM_LBUTTONDBLCLK)
        {
            ReloadItems();
        }
    }

    void OnKeyDown(WPARAM key)
    {
        if (renameEdit_ != nullptr)
        {
            return;
        }

        switch (key)
        {
        case VK_ESCAPE:
            DestroyWindow(hwnd_);
            break;
        case VK_F5:
            ReloadItems();
            break;
        case VK_RETURN:
            OpenSelected();
            break;
        case VK_DELETE:
            InvokeSelectedShellVerb("delete");
            break;
        case VK_F2:
            BeginRenameSelected();
            break;
        case VK_LEFT:
            MoveKeyboardSelection(-1);
            break;
        case VK_RIGHT:
            MoveKeyboardSelection(1);
            break;
        case VK_UP:
            MoveKeyboardSelection(-GetRowsPerColumn());
            break;
        case VK_DOWN:
            MoveKeyboardSelection(GetRowsPerColumn());
            break;
        case 'C':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                InvokeSelectedShellVerb("copy");
            }
            break;
        case 'X':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                InvokeSelectedShellVerb("cut");
            }
            break;
        case 'V':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                InvokeDesktopBackgroundVerb("paste");
            }
            break;
        case 'A':
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            {
                ClearSelection();
                for (auto& item : items_)
                {
                    item.selected = true;
                }
                selectedCount_ = static_cast<int>(items_.size());
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
            break;
        default:
            break;
        }
    }

    static LRESULT CALLBACK WindowProcThunk(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        SnowDesktopApp* app = nullptr;
        if (message == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<SnowDesktopApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd_ = hwnd;
        }
        else
        {
            app = reinterpret_cast<SnowDesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (app != nullptr)
        {
            return app->HandleMessage(message, wParam, lParam);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK RenameEditSubclassProc(
        HWND hwnd,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR refData)
    {
        UNREFERENCED_PARAMETER(subclassId);
        auto* app = reinterpret_cast<SnowDesktopApp*>(refData);
        if (app == nullptr)
        {
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        switch (message)
        {
        case WM_KEYDOWN:
            if (wParam == VK_RETURN)
            {
                app->CommitRename(false);
                return 0;
            }
            if (wParam == VK_ESCAPE)
            {
                app->CommitRename(true);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            app->CommitRename(false);
            return 0;
        default:
            break;
        }

        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND hintHwnd_ = nullptr;
    HWND renameEdit_ = nullptr;
    HFONT renameFont_ = nullptr;
    HBRUSH renameBackgroundBrush_ = nullptr;
    size_t renameIndex_ = 0;
    bool trayIconAdded_ = false;
    HICON trayIcon_ = nullptr;
    bool customDesktopVisible_ = true;
    DesktopWindows desktopWindows_{};
    ComPtr<IImageList> sysImageList_;
    ComPtr<ID3D11Device> d3dDevice_;
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> bitmapContext_;
    ComPtr<IDCompositionDesktopDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual2> dcompVisual_;
    ComPtr<IDCompositionSurface> dcompSurface_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteTextFormat> itemTextFormat_;
    ComPtr<IDWriteTextFormat> statusTextFormat_;
    ComPtr<ID2D1StrokeStyle> dottedStrokeStyle_;
    std::unordered_map<std::uintptr_t, ComPtr<ID2D1Bitmap1>> d2dIconCache_;
    ComPtr<IShellFolder> desktopFolder_;
    Pidl desktopPidl_;
    Pidl recycleBinPidl_;
    std::vector<DesktopItem> items_;
    std::vector<GridPage> gridPages_;
    std::vector<std::wstring> savedPageIds_;
    std::unordered_map<std::wstring, int> savedPageColumns_;
    std::unordered_map<std::wstring, int> savedPageRows_;
    std::unordered_map<std::wstring, LayoutRecord> layoutRecords_;
    std::unordered_map<std::wstring, bool> settingsIconVisibility_;
    int selectedCount_ = 0;
    LONG refCount_ = 1;
    int virtualLeft_ = 0;
    int virtualTop_ = 0;
    int virtualWidth_ = 0;
    int virtualHeight_ = 0;
    RECT layoutWorkArea_{};
    bool mouseDown_ = false;
    bool marqueeActive_ = false;
    bool draggingItems_ = false;
    bool draggingOverNav_ = false;
    bool externalDragActive_ = false;
    bool dropTargetRegistered_ = false;
    int mouseDownHit_ = -1;
    GridCell dragTargetCell_;
    int dragGroupOriginX_ = 0;
    int dragGroupOriginY_ = 0;
    POINT mouseDownPoint_{};
    POINT dragCurrentPoint_{};
    POINT externalDragPoint_{};
    std::wstring dragHint_;
    std::wstring externalDragHint_;
    std::wstring primaryMonitorId_;
    std::wstring firstPageMonitorId_;
    std::wstring lastMonitorPageId_;
    int pageOffset_ = 0;
    float gapScale_ = 1.0f;
    ULONG shellChangeRegId_ = 0;
    bool reloading_ = false;
    bool trayMenuShowing_ = false;
    LONGLONG lastRecycleBinItemCount_ = -1;
    std::vector<HBITMAP> menuIconPool_;
    bool navButtonsVisible_ = false;
    RECT navButtonsHoverZone_{};
    POINT lastContextMenuScreenPoint_{};
    RECT marqueeRect_{};
    ComPtr<IContextMenu2> activeContextMenu2_;
    ComPtr<IContextMenu3> activeContextMenu3_;
    DWORD lastCreateWindowError_ = 0;
    HRESULT lastGraphicsError_ = S_OK;
    UINT compositionWidth_ = 0;
    UINT compositionHeight_ = 0;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    if (commandLine != nullptr && wcsstr(commandLine, L"--restore-explorer-icons") != nullptr)
    {
        RestoreExplorerIconLayerNow();
        return 0;
    }

    UINT smokeTestMs = 0;
    if (commandLine != nullptr)
    {
        const wchar_t* smokeArg = wcsstr(commandLine, L"--smoke-test-ms=");
        if (smokeArg != nullptr)
        {
            smokeArg += wcslen(L"--smoke-test-ms=");
            smokeTestMs = static_cast<UINT>(std::max(0, _wtoi(smokeArg)));
        }
    }

    SnowDesktopApp app;
    return app.Run(instance, showCommand, smokeTestMs);
}
