#pragma once
#include "types.h"

#include <wrl/client.h>
#include <shlobj.h>
#include <cstdint>
#include <string>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

BOOL CALLBACK FindDefViewProc(HWND hwnd, LPARAM lParam);
BOOL CALLBACK EnumGridPageMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM lParam);

HICON LoadAppIcon();
DesktopWindows FindDesktopWindows();
void RestoreExplorerIconLayerNow();

std::wstring StrRetToString(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags);
std::wstring ToUpperInvariant(std::wstring value);
std::wstring ExtractClsidText(const std::wstring& parsingName);
std::wstring TrimTrailingPathSeparators(std::wstring path);
bool PathsEqualInsensitive(std::wstring left, std::wstring right);
std::wstring ResolveDesktopIconClsid(
    const std::wstring& parsingName,
    const std::wstring& itemPath,
    const std::wstring& userProfilePath);

bool TryReadDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD& value);
bool TryWriteDesktopIconRegistryValue(HKEY root, const wchar_t* subKey, const std::wstring& clsid, DWORD value);
void WriteDesktopIconRegistryValue(const std::wstring& clsid, bool visible);
bool TryReadDesktopIconRegistryValueAnyRoot(const std::wstring& clsid, DWORD& value);
bool IsVisibleByDesktopIconSettings(const std::wstring& desktopIconClsid, const std::unordered_map<std::wstring, bool>& settingsIconVisibility);

HBITMAP CreateTopDown32BppDib(HDC referenceDc, int width, int height, void** bits);
void PremultiplyBgraPixels(std::uint32_t* pixels, int width, int height);
HBITMAP CopyBitmapToAlphaDib(HBITMAP source, SIZE& size);
HBITMAP CreateAlphaBitmapFromIcon(HICON icon, int width, int height, SIZE& size);
HBITMAP GetHighResolutionShellIconBitmap(PCIDLIST_ABSOLUTE pidl, int fallbackIndex, SIZE& bitmapSize);

RECT MakeRect(int left, int top, int right, int bottom);
bool RectsIntersect(const RECT& a, const RECT& b);
RECT NormalizeRect(POINT a, POINT b);
bool IsRectEmptyRect(const RECT& rect);
RECT InflateCopy(RECT rect, int amount);
RECT UnionCopy(const RECT& a, const RECT& b);

std::string WideToUtf8(const std::wstring& value);
std::wstring Utf8ToWide(const std::string& value);
std::string JsonEscapeUtf8(const std::wstring& value);
bool ParseJsonStringAt(const std::string& text, size_t quote, std::string& value, size_t& end);
