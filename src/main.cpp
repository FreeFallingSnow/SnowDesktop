#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>
#include <cstdint>
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
constexpr int kMinCellHeight = 128;
constexpr int kMarginX = 0;
constexpr int kMarginY = 6;
constexpr int kTextTop = 70;
constexpr int kTextHeight = 42;
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
    std::wstring typeName;
    Pidl absolutePidl;
    Pidl childPidl;
    HBITMAP iconBitmap = nullptr;
    SIZE iconBitmapSize{};
    int sysIconIndex = -1;
    RECT bounds{};
    int slot = 0;
    bool selected = false;

    DesktopItem() = default;

    DesktopItem(DesktopItem&& other) noexcept
        : name(std::move(other.name)),
          parsingName(std::move(other.parsingName)),
          typeName(std::move(other.typeName)),
          absolutePidl(std::move(other.absolutePidl)),
          childPidl(std::move(other.childPidl)),
          iconBitmap(other.iconBitmap),
          iconBitmapSize(other.iconBitmapSize),
          sysIconIndex(other.sysIconIndex),
          bounds(other.bounds),
          slot(other.slot),
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
            typeName = std::move(other.typeName);
            absolutePidl = std::move(other.absolutePidl);
            childPidl = std::move(other.childPidl);
            iconBitmap = other.iconBitmap;
            iconBitmapSize = other.iconBitmapSize;
            sysIconIndex = other.sysIconIndex;
            bounds = other.bounds;
            slot = other.slot;
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

HICON LoadAppIcon()
{
    HICON icon = LoadIconW(nullptr, IDI_APPLICATION);
    HICON copy = nullptr;
    if (icon != nullptr)
    {
        copy = CopyIcon(icon);
    }
    return copy != nullptr ? copy : icon;
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

bool IsVisibleByDesktopIconSettings(const std::wstring& parsingName, const std::unordered_map<std::wstring, bool>& settingsIconVisibility)
{
    std::wstring clsid = ExtractClsidText(parsingName);
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
        { L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}", false }, // This PC
        { L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}", false }, // User files
        { L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", false }, // Network
        { L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}", false }, // Control Panel
        { L"{645FF040-5081-101B-9F08-00AA002F954E}", true },  // Recycle Bin
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
        LoadSettings();
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
            swprintf_s(
                message,
                L"Unable to create the SnowDesktop desktop window.\nGetLastError=%lu",
                lastCreateWindowError_);
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

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        return static_cast<int>(msg.wParam);
    }

private:
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

        DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW;
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
        SetLayeredWindowAttributes(hwnd_, kTransparentKey, 255, LWA_COLORKEY);
        return true;
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
            SetForegroundWindow(hwnd_);
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
        POINT windowPos{ screenPoint.x + 34, screenPoint.y + 22 };

        HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo))
        {
            windowPos.x = std::clamp(windowPos.x, monitorInfo.rcWork.left + 8, monitorInfo.rcWork.right - width - 8);
            windowPos.y = std::clamp(windowPos.y, monitorInfo.rcWork.top + 8, monitorInfo.rcWork.bottom - height - 8);
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

        const std::uint32_t background = argb(178, 24, 32, 42);
        const std::uint32_t border = argb(230, 110, 170, 240);
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
        SetTextColor(memoryDc, RGB(245, 250, 255));

        RECT textRect{ 10, 0, width - 10, height };
        DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        for (int i = 0; i < width * height; ++i)
        {
            std::uint32_t pixel = pixels[i];
            std::uint8_t a = static_cast<std::uint8_t>((pixel >> 24) & 0xff);
            std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
            std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
            std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);

            if (r > 150 && g > 150 && b > 150)
            {
                a = 255;
            }

            r = static_cast<std::uint8_t>((static_cast<int>(r) * a) / 255);
            g = static_cast<std::uint8_t>((static_cast<int>(g) * a) / 255);
            b = static_cast<std::uint8_t>((static_cast<int>(b) * a) / 255);
            pixels[i] = argb(a, r, g, b);
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
            { L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}", false },
            { L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}", false },
            { L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", false },
            { L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}", false },
            { L"{645FF040-5081-101B-9F08-00AA002F954E}", true },
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
            { kTrayDesktopIconThisPC, L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}" },
            { kTrayDesktopIconUserFiles, L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}" },
            { kTrayDesktopIconNetwork, L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}" },
            { kTrayDesktopIconControlPanel, L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}" },
            { kTrayDesktopIconRecycleBin, L"{645FF040-5081-101B-9F08-00AA002F954E}" },
        };

        auto it = commandToClsid.find(command);
        if (it == commandToClsid.end())
        {
            return;
        }

        bool currentVisible = IsClsidCurrentlyVisible(it->second);
        settingsIconVisibility_[it->second] = !currentVisible;
        SaveSettings();
        ReloadItems();
    }

    void ShowTrayMenu(POINT screenPoint)
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, kTrayReloadCommand, L"重新加载");
        AppendMenuW(menu, MF_STRING, kTraySortByNameCommand, L"图标排序：按名称");
        AppendMenuW(menu, MF_STRING, kTraySortByTypeCommand, L"图标排序：按类型");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

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
                { kTrayDesktopIconThisPC, L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"计算机" },
                { kTrayDesktopIconUserFiles, L"{59031A47-3F72-44A7-89C5-5595FE6B30EE}", L"用户的文件" },
                { kTrayDesktopIconNetwork, L"{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}", L"网络" },
                { kTrayDesktopIconControlPanel, L"{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}", L"控制面板" },
                { kTrayDesktopIconRecycleBin, L"{645FF040-5081-101B-9F08-00AA002F954E}", L"回收站" },
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
        AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出");

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

    void ReloadItems()
    {
        items_.clear();
        selectedCount_ = 0;

        ComPtr<IEnumIDList> enumItems;
        if (FAILED(desktopFolder_->EnumObjects(
                hwnd_,
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                &enumItems)) ||
            !enumItems)
        {
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }

        wchar_t userDesktopPath[MAX_PATH]{};
        wchar_t commonDesktopPath[MAX_PATH]{};
        SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
        SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
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

            SHFILEINFOW info{};
            SHGetFileInfoW(
                reinterpret_cast<LPCWSTR>(absolute),
                0,
                &info,
                sizeof(info),
                SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME | SHGFI_TYPENAME);

            std::wstring parsingName = StrRetToString(
                desktopFolder_.Get(),
                reinterpret_cast<PCUITEMID_CHILD>(child),
                SHGDN_FORPARSING);
            if (!IsVisibleByDesktopIconSettings(parsingName, settingsIconVisibility_))
            {
                ILFree(absolute);
                ILFree(child);
                continue;
            }

            if (ExtractClsidText(parsingName).empty())
            {
                wchar_t itemPath[MAX_PATH]{};
                if (!SHGetPathFromIDListW(absolute, itemPath) || itemPath[0] == L'\0')
                {
                    ILFree(absolute);
                    ILFree(child);
                    continue;
                }
                bool underUser = _wcsnicmp(itemPath, userDesktopPath, userDesktopLen) == 0 &&
                    itemPath[userDesktopLen] == L'\\';
                bool underCommon = _wcsnicmp(itemPath, commonDesktopPath, commonDesktopLen) == 0 &&
                    itemPath[commonDesktopLen] == L'\\';
                if (!underUser && !underCommon)
                {
                    ILFree(absolute);
                    ILFree(child);
                    continue;
                }
            }

            DesktopItem item;
            item.absolutePidl.reset(absolute);
            item.childPidl.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(child));
            item.parsingName = std::move(parsingName);
            item.name = info.szDisplayName[0] != L'\0'
                ? info.szDisplayName
                : StrRetToString(desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()), SHGDN_NORMAL);
            item.typeName = info.szTypeName;
            item.iconBitmap = GetHighResolutionShellIconBitmap(item.absolutePidl.get(), info.iIcon, item.iconBitmapSize);
            ClampAlphaToColorKey(item.iconBitmap, kTransparentKey);
            item.sysIconIndex = info.iIcon;
            std::wstring key = GetStableLayoutKey(item.absolutePidl.get(), item.parsingName);
            if (seenKeys.contains(key))
            {
                continue;
            }
            seenKeys.insert(key);
            auto knownSlot = layoutSlots_.find(key);
            if (knownSlot != layoutSlots_.end())
            {
                item.slot = knownSlot->second;
            }
            else
            {
                item.slot = -1;
            }
            items_.push_back(std::move(item));
        }

        std::unordered_map<int, bool> usedSlots;
        int nextTailSlot = 0;
        for (auto& item : items_)
        {
            if (item.slot >= 0 && !usedSlots.contains(item.slot))
            {
                usedSlots[item.slot] = true;
                nextTailSlot = std::max(nextTailSlot, item.slot + 1);
            }
            else
            {
                item.slot = -1;
            }
        }

        for (auto& item : items_)
        {
            if (item.slot >= 0)
            {
                continue;
            }

            while (usedSlots.contains(nextTailSlot))
            {
                ++nextTailSlot;
            }
            item.slot = nextTailSlot;
            usedSlots[item.slot] = true;
            ++nextTailSlot;
        }

        std::sort(items_.begin(), items_.end(), [](const DesktopItem& a, const DesktopItem& b) {
            return a.slot < b.slot;
        });
        for (size_t i = 0; i < items_.size(); ++i)
        {
            items_[i].slot = static_cast<int>(i);
        }

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
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

    std::wstring GetStableLayoutKey(PCIDLIST_ABSOLUTE pidl, const std::wstring& parsingName) const
    {
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidl, path) && path[0] != L'\0')
        {
            return ToUpperInvariant(path);
        }
        return ToUpperInvariant(parsingName);
    }

    std::wstring GetSettingsPath() const
    {
        wchar_t modulePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        PathRemoveFileSpecW(modulePath);
        PathAppendW(modulePath, L"SnowDesktop.settings.json");
        return modulePath;
    }

    void LoadSettings()
    {
        settingsIconVisibility_.clear();

        std::ifstream file(GetSettingsPath(), std::ios::binary);
        if (!file)
        {
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string text = buffer.str();

        size_t pos = 0;
        while ((pos = text.find('{', pos)) != std::string::npos)
        {
            size_t close = text.find('}', pos);
            if (close == std::string::npos)
            {
                break;
            }

            std::string clsidUtf8 = text.substr(pos, close - pos + 1);
            std::wstring clsid = Utf8ToWide(clsidUtf8);

            size_t colon = text.find(':', close);
            if (colon == std::string::npos)
            {
                pos = close + 1;
                continue;
            }

            size_t truePos = text.find("true", colon);
            size_t falsePos = text.find("false", colon);
            bool visible = false;
            if (truePos != std::string::npos && (falsePos == std::string::npos || truePos < falsePos))
            {
                visible = true;
            }

            settingsIconVisibility_[ToUpperInvariant(clsid)] = visible;
            pos = close + 1;
        }
    }

    void SaveSettings()
    {
        std::ofstream file(GetSettingsPath(), std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return;
        }

        file << "{\n";
        size_t count = 0;
        for (const auto& [clsid, visible] : settingsIconVisibility_)
        {
            file << "  \"" << JsonEscapeUtf8(clsid) << "\": " << (visible ? "true" : "false");
            ++count;
            file << (count == settingsIconVisibility_.size() ? "\n" : ",\n");
        }
        file << "}\n";
    }

    void LoadLayoutSlots()
    {
        layoutSlots_.clear();

        std::ifstream file(GetLayoutPath(), std::ios::binary);
        if (!file)
        {
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string text = buffer.str();

        size_t pos = 0;
        while ((pos = text.find("\"key\"", pos)) != std::string::npos)
        {
            size_t colon = text.find(':', pos);
            size_t quote = text.find('"', colon == std::string::npos ? pos : colon);
            if (quote == std::string::npos)
            {
                break;
            }

            std::string keyUtf8;
            size_t afterKey = 0;
            if (!ParseJsonStringAt(text, quote, keyUtf8, afterKey))
            {
                pos = quote + 1;
                continue;
            }

            size_t slotName = text.find("\"slot\"", afterKey);
            size_t slotColon = text.find(':', slotName == std::string::npos ? afterKey : slotName);
            if (slotColon == std::string::npos)
            {
                pos = afterKey;
                continue;
            }

            size_t numberStart = text.find_first_of("-0123456789", slotColon + 1);
            if (numberStart == std::string::npos)
            {
                pos = afterKey;
                continue;
            }

            int slot = std::atoi(text.c_str() + numberStart);
            layoutSlots_[NormalizeLayoutKey(Utf8ToWide(keyUtf8))] = slot;
            pos = numberStart + 1;
        }
    }

    void SaveLayoutSlots()
    {
        layoutSlots_.clear();
        for (const auto& item : items_)
        {
            if (!item.parsingName.empty())
            {
                layoutSlots_[GetStableLayoutKey(item.absolutePidl.get(), item.parsingName)] = item.slot;
            }
        }

        std::vector<const DesktopItem*> sortedItems;
        sortedItems.reserve(items_.size());
        for (const auto& item : items_)
        {
            sortedItems.push_back(&item);
        }
        std::sort(sortedItems.begin(), sortedItems.end(), [](const DesktopItem* left, const DesktopItem* right) {
            return left->slot < right->slot;
        });

        std::ofstream file(GetLayoutPath(), std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return;
        }

        file << "{\n  \"items\": [\n";
        for (size_t i = 0; i < sortedItems.size(); ++i)
        {
            const DesktopItem* item = sortedItems[i];
            file << "    { \"key\": \"" << JsonEscapeUtf8(GetStableLayoutKey(item->absolutePidl.get(), item->parsingName)) << "\", \"slot\": " << item->slot << " }";
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
            items_[order[i]].slot = static_cast<int>(i);
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
            bool leftIsClsid = items_[left].parsingName.find(L'{') != std::wstring::npos;
            bool rightIsClsid = items_[right].parsingName.find(L'{') != std::wstring::npos;
            if (leftIsClsid != rightIsClsid)
            {
                return leftIsClsid;
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
            items_[order[i]].slot = static_cast<int>(i);
        }

        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void LayoutItems()
    {
        for (auto& item : items_)
        {
            item.bounds = GetSlotRect(item.slot);
        }
    }

    void UpdateLayoutWorkArea()
    {
        layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);

        POINT startPoint{ virtualLeft_ + 1, virtualTop_ + 1 };
        HMONITOR monitor = MonitorFromPoint(startPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo))
        {
            layoutWorkArea_.left = std::clamp(static_cast<int>(monitorInfo.rcWork.left - virtualLeft_), 0, virtualWidth_);
            layoutWorkArea_.top = std::clamp(static_cast<int>(monitorInfo.rcWork.top - virtualTop_), 0, virtualHeight_);
            layoutWorkArea_.right = std::clamp(static_cast<int>(monitorInfo.rcWork.right - virtualLeft_), static_cast<int>(layoutWorkArea_.left), virtualWidth_);
            layoutWorkArea_.bottom = std::clamp(static_cast<int>(monitorInfo.rcWork.bottom - virtualTop_), static_cast<int>(layoutWorkArea_.top), virtualHeight_);
        }
    }

    int GetCellHeight() const
    {
        int rows = GetRowsPerColumn();
        int usableHeight = std::max(kMinCellHeight, static_cast<int>((layoutWorkArea_.bottom - layoutWorkArea_.top) - (kMarginY * 2)));
        return std::max(kMinCellHeight, usableHeight / rows);
    }

    RECT GetSlotRect(int slot) const
    {
        int rows = GetRowsPerColumn();
        int row = std::max(0, slot) % rows;
        int column = std::max(0, slot) / rows;
        int x = layoutWorkArea_.left + kMarginX + (column * kCellWidth);
        int y = layoutWorkArea_.top + kMarginY + (row * GetCellHeight());
        return MakeRect(x, y, x + kCellWidth, y + GetCellHeight());
    }

    int SlotFromPoint(POINT point) const
    {
        int column = std::max(0, static_cast<int>((point.x - layoutWorkArea_.left - kMarginX) / kCellWidth));
        int row = std::clamp(static_cast<int>((point.y - layoutWorkArea_.top - kMarginY) / GetCellHeight()), 0, GetRowsPerColumn() - 1);
        return column * GetRowsPerColumn() + row;
    }

    RECT GetTargetRectAt(POINT point) const
    {
        int hit = HitTest(point);
        if (hit >= 0 && !items_[static_cast<size_t>(hit)].selected)
        {
            return items_[static_cast<size_t>(hit)].bounds;
        }

        return GetSlotRect(SlotFromPoint(point));
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

            RECT moved = item.bounds;
            OffsetRect(&moved, dx, dy);
            bounds = hasBounds ? UnionCopy(bounds, moved) : moved;
            hasBounds = true;
        }

        return hasBounds ? bounds : RECT{};
    }

    RECT GetInternalDragDirtyRect(POINT point) const
    {
        RECT dirty = GetSelectedDragBoundsAt(point);
        dirty = UnionCopy(dirty, GetTargetRectAt(point));
        return InflateCopy(dirty, 16);
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

        int slot = SlotFromPoint(point);
        if (IsSlotOccupiedByUnselected(slot))
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
            if (PtInRect(&items_[static_cast<size_t>(i)].bounds, point))
            {
                return i;
            }
        }
        return -1;
    }

    RECT GetItemTextRect(const DesktopItem& item) const
    {
        return MakeRect(
            item.bounds.left + 6,
            item.bounds.top + kTextTop - 2,
            item.bounds.right - 6,
            item.bounds.top + kTextTop + kTextHeight + 2);
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

    bool IsSlotOccupiedByUnselected(int slot) const
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

    void MoveSelectedItemsToSlot(int targetSlot)
    {
        if (targetSlot < 0 || selectedCount_ <= 0 || IsSlotOccupiedByUnselected(targetSlot))
        {
            return;
        }

        std::vector<size_t> selectedIndexes;
        selectedIndexes.reserve(static_cast<size_t>(selectedCount_));
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].selected)
            {
                selectedIndexes.push_back(i);
            }
        }

        int slot = targetSlot;
        for (size_t itemIndex : selectedIndexes)
        {
            while (IsSlotOccupiedByUnselected(slot))
            {
                ++slot;
            }

            items_[itemIndex].slot = slot;
            ++slot;
        }
        LayoutItems();
        SaveLayoutSlots();
    }

    int GetRowsPerColumn() const
    {
        int usableHeight = std::max(kMinCellHeight, static_cast<int>((layoutWorkArea_.bottom - layoutWorkArea_.top) - (kMarginY * 2)));
        return std::max(1, usableHeight / kMinCellHeight);
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
            RECT rect = GetItemTextRect(items_[i]);
            renameEdit_ = CreateWindowExW(
                WS_EX_CLIENTEDGE,
                L"EDIT",
                items_[i].name.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER,
                rect.left,
                rect.top,
                rect.right - rect.left,
                rect.bottom - rect.top,
                hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenameEditId)),
                instance_,
                nullptr);

            if (renameEdit_ == nullptr)
            {
                return;
            }

            SetWindowSubclass(renameEdit_, &SnowDesktopApp::RenameEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
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

        if (!cancel && renameIndex_ < items_.size() && !newName.empty() && newName != items_[renameIndex_].name)
        {
            PITEMID_CHILD newChild = nullptr;
            HRESULT hr = desktopFolder_->SetNameOf(
                hwnd_,
                reinterpret_cast<PCUITEMID_CHILD>(items_[renameIndex_].childPidl.get()),
                newName.c_str(),
                SHGDN_NORMAL,
                &newChild);
            if (newChild != nullptr)
            {
                ILFree(newChild);
            }

            if (FAILED(hr))
            {
                MessageBeep(MB_ICONWARNING);
            }
        }

        ReloadItems();
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

        if (draggingItems_)
        {
            RECT targetRect = GetTargetRectAt(dragCurrentPoint_);
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
            target = GetSlotRect(SlotFromPoint(externalDragPoint_));
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
            HDC hdc = BeginPaint(hwnd_, &ps);
            Draw(hdc, ps.rcPaint);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            virtualWidth_ = LOWORD(lParam);
            virtualHeight_ = HIWORD(lParam);
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
        case WM_COMMAND:
            OnCommand(LOWORD(wParam));
            return 0;
        case kTrayCallbackMessage:
            OnTrayCallback(lParam);
            return 0;
        case WM_TIMER:
            if (wParam == 1)
            {
                DestroyWindow(hwnd_);
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_DESTROY:
            SaveLayoutSlots();
            HideDragHintWindow();
            DestroyDragHintWindow();
            UnregisterOleDropTarget();
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
        SetCapture(hwnd_);
        mouseDown_ = true;
        marqueeActive_ = false;
        mouseDownPoint_ = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        dragCurrentPoint_ = mouseDownPoint_;
        marqueeRect_ = MakeRect(mouseDownPoint_.x, mouseDownPoint_.y, mouseDownPoint_.x, mouseDownPoint_.y);

        int hit = HitTest(mouseDownPoint_);
        mouseDownHit_ = hit;
        draggingItems_ = false;
        bool ctrl = (wParam & MK_CONTROL) != 0;
        if (hit >= 0)
        {
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
        if (!mouseDown_)
        {
            return;
        }

        POINT current{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (std::abs(current.x - mouseDownPoint_.x) > 3 || std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            int hit = HitTest(mouseDownPoint_);
            if (hit >= 0 && items_[static_cast<size_t>(hit)].selected)
            {
                RECT oldDirty = draggingItems_ ? GetInternalDragDirtyRect(dragCurrentPoint_) : GetSelectedDragBoundsAt(mouseDownPoint_);
                draggingItems_ = true;
                dragCurrentPoint_ = current;
                dragTargetSlot_ = SlotFromPoint(current);
                dragHint_ = MakeInternalDragHint(current);
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
                MoveSelectedItemsToSlot(SlotFromPoint(point));
                LayoutItems();
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
                if (RectsIntersect(item.bounds, marqueeRect_) && !item.selected)
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
        dragTargetSlot_ = -1;
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
            ShowShellContextMenu(screenPoint);
        }
        else
        {
            ShowDesktopBackgroundContextMenu(screenPoint);
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
            POINT point{};
            GetCursorPos(&point);
            ShowTrayMenu(point);
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
    size_t renameIndex_ = 0;
    bool trayIconAdded_ = false;
    HICON trayIcon_ = nullptr;
    bool customDesktopVisible_ = true;
    DesktopWindows desktopWindows_{};
    ComPtr<IImageList> sysImageList_;
    ComPtr<IShellFolder> desktopFolder_;
    Pidl desktopPidl_;
    std::vector<DesktopItem> items_;
    std::unordered_map<std::wstring, int> layoutSlots_;
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
    bool externalDragActive_ = false;
    bool dropTargetRegistered_ = false;
    int mouseDownHit_ = -1;
    int dragTargetSlot_ = -1;
    POINT mouseDownPoint_{};
    POINT dragCurrentPoint_{};
    POINT externalDragPoint_{};
    std::wstring dragHint_;
    std::wstring externalDragHint_;
    RECT marqueeRect_{};
    ComPtr<IContextMenu2> activeContextMenu2_;
    ComPtr<IContextMenu3> activeContextMenu3_;
    DWORD lastCreateWindowError_ = 0;
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
