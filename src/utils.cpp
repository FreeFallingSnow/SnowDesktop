#include "utils.h"
#include "resource.h"

#include <commoncontrols.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <d2d1_1.h>
#include <dwrite_3.h>

#include <algorithm>
#include <cstdlib>
#include <unordered_set>
#include <vector>

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
    HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON,
        32, 32,
        LR_DEFAULTSIZE));
    return icon;
}

HANDLE LoadFontAwesome()
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_FA_FONT), RT_RCDATA);
    if (res == nullptr) return nullptr;
    HGLOBAL handle = LoadResource(module, res);
    if (handle == nullptr) return nullptr;
    void* data = LockResource(handle);
    DWORD size = SizeofResource(module, res);
    if (data == nullptr || size == 0) return nullptr;

    DWORD count = 0;
    HANDLE fontHandle = AddFontMemResourceEx(data, size, nullptr, &count);
    return fontHandle;
}

namespace
{
    const void* g_faFontData = nullptr;
    DWORD g_faFontSize = 0;

    ComPtr<IDWriteFontCollection1> BuildFaFontCollection(IDWriteFactory* factory)
    {
        ComPtr<IDWriteFactory5> factory5;
        if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        {
            return nullptr;
        }

        ComPtr<IDWriteInMemoryFontFileLoader> loader;
        if (FAILED(factory5->CreateInMemoryFontFileLoader(&loader)))
        {
            return nullptr;
        }
        if (FAILED(factory5->RegisterFontFileLoader(loader.Get())))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontFile> fontFile;
        if (FAILED(loader->CreateInMemoryFontFileReference(
                factory5.Get(),
                g_faFontData,
                g_faFontSize,
                nullptr,
                &fontFile)))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontSetBuilder1> builder;
        if (FAILED(factory5->CreateFontSetBuilder(&builder)))
        {
            return nullptr;
        }
        if (FAILED(builder->AddFontFile(fontFile.Get())))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontSet> fontSet;
        if (FAILED(builder->CreateFontSet(&fontSet)))
        {
            return nullptr;
        }

        ComPtr<IDWriteFontCollection1> collection;
        factory5->CreateFontCollectionFromFontSet(fontSet.Get(), collection.GetAddressOf());
        return collection;
    }
}

IDWriteTextFormat* CreateFaTextFormat(IDWriteFactory* factory, float fontSize)
{
    if (factory == nullptr) return nullptr;

    // Cache font data on first call
    if (g_faFontData == nullptr)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(IDR_FA_FONT), RT_RCDATA);
        if (res != nullptr)
        {
            HGLOBAL handle = LoadResource(module, res);
            if (handle != nullptr)
            {
                g_faFontData = LockResource(handle);
                g_faFontSize = SizeofResource(module, res);
            }
        }
    }

    ComPtr<IDWriteFontCollection1> fontCollection;
    if (g_faFontData != nullptr)
    {
        fontCollection = BuildFaFontCollection(factory);
    }

    IDWriteTextFormat* format = nullptr;
    HRESULT hr = factory->CreateTextFormat(
        L"Font Awesome 6 Free Solid",
        fontCollection.Get(),
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        fontSize,
        L"",
        &format);
    if (FAILED(hr)) return nullptr;

    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return format;
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
        SIZE size{ kIconBitmapSize, kIconBitmapSize };
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
            HBITMAP bitmap = CreateAlphaBitmapFromIcon(icon, kIconBitmapSize, kIconBitmapSize, bitmapSize);
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
        HBITMAP bitmap = CreateAlphaBitmapFromIcon(icon, kIconBitmapSize, kIconBitmapSize, bitmapSize);
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
