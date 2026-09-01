#include "app.h"
#include <urlmon.h>

// OLE file, URL, image and text payload extraction.

namespace
{
std::filesystem::path DropContentDirectory()
{
    const std::filesystem::path root(
        GetDataSubdirectoryPath(L"DropContent"));
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return {};
    return root;
}

std::wstring SafeDropFilename(std::wstring filename,
    std::wstring_view fallback)
{
    for (auto& character : filename)
    {
        if (character < L' ' || character == L'\\' || character == L'/' ||
            character == L':' || character == L'*' || character == L'?' ||
            character == L'"' || character == L'<' || character == L'>' ||
            character == L'|')
            character = L'_';
    }
    while (!filename.empty() &&
        (filename.back() == L' ' || filename.back() == L'.'))
        filename.pop_back();
    if (filename.empty() || filename == L"." || filename == L"..")
        filename = fallback;
    return filename;
}

std::filesystem::path UniqueDropContentPath(std::wstring filename,
    std::wstring_view fallback)
{
    const auto root = DropContentDirectory();
    if (root.empty()) return {};
    filename = SafeDropFilename(std::move(filename), fallback);
    const std::filesystem::path original(filename);
    const std::wstring stem = original.stem().wstring();
    const std::wstring extension = original.extension().wstring();
    for (unsigned int suffix = 0; suffix < 10000; ++suffix)
    {
        const std::filesystem::path candidate = root /
            (suffix == 0 ? filename :
                stem + L" (" + std::to_wstring(suffix) + L")" + extension);
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            GetLastError() == ERROR_FILE_NOT_FOUND)
            return candidate;
    }
    return {};
}

std::wstring ReadWideHGlobalFormat(
    IDataObject* dataObject, CLIPFORMAT format)
{
    if (!dataObject || format == 0)
        return {};

    FORMATETC formatEtc{};
    formatEtc.cfFormat = format;
    formatEtc.dwAspect = DVASPECT_CONTENT;
    formatEtc.lindex = -1;
    formatEtc.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(
            &formatEtc, &medium)))
        return {};

    std::wstring result;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
    {
        const SIZE_T byteCount = GlobalSize(medium.hGlobal);
        const auto* data = static_cast<const wchar_t*>(
            GlobalLock(medium.hGlobal));
        if (data && byteCount >= sizeof(wchar_t))
        {
            const size_t characterCount =
                byteCount / sizeof(wchar_t);
            const auto* terminator = std::find(
                data, data + characterCount, L'\0');
            if (terminator != data + characterCount)
                result.assign(data, terminator);
        }
        if (data)
            GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}

std::wstring ReadAnsiHGlobalFormat(
    IDataObject* dataObject, CLIPFORMAT format)
{
    if (!dataObject || format == 0)
        return {};

    FORMATETC formatEtc{};
    formatEtc.cfFormat = format;
    formatEtc.dwAspect = DVASPECT_CONTENT;
    formatEtc.lindex = -1;
    formatEtc.tymed = TYMED_HGLOBAL;
    STGMEDIUM medium{};
    if (FAILED(dataObject->GetData(
            &formatEtc, &medium)))
        return {};

    std::wstring result;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal)
    {
        const SIZE_T byteCount = GlobalSize(medium.hGlobal);
        const auto* data = static_cast<const char*>(
            GlobalLock(medium.hGlobal));
        if (data && byteCount > 0 &&
            byteCount <= static_cast<SIZE_T>(
                std::numeric_limits<int>::max()))
        {
            const auto* terminator = std::find(
                data, data + byteCount, '\0');
            if (terminator != data + byteCount)
            {
                const int sourceLength = static_cast<int>(
                    terminator - data);
                const int required = MultiByteToWideChar(
                    CP_ACP, 0, data, sourceLength,
                    nullptr, 0);
                if (required > 0)
                {
                    result.resize(
                        static_cast<size_t>(required));
                    if (MultiByteToWideChar(
                            CP_ACP, 0, data, sourceLength,
                            result.data(), required) != required)
                        result.clear();
                }
            }
        }
        if (data)
            GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}
}

std::vector<std::wstring> DesktopApp::GetDropPaths(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    FORMATETC fmt{};
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (SUCCEEDED(dataObject->GetData(&fmt, &med)))
    {
        HDROP hDrop = static_cast<HDROP>(med.hGlobal);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                paths.push_back(path);
        }
        ReleaseStgMedium(&med);
    }
    return paths;
}

std::wstring DesktopApp::ExtractDropUrl(IDataObject* dataObject)
{
    if (!dataObject) return {};

    std::wstring url;
    const CLIPFORMAT wideUrlFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_INETURLW));
    url = ReadWideHGlobalFormat(dataObject, wideUrlFormat);
    if (url.empty())
    {
        const CLIPFORMAT ansiUrlFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_INETURLA));
        url = ReadAnsiHGlobalFormat(dataObject, ansiUrlFormat);
    }
    if (url.empty())
        url = ReadWideHGlobalFormat(dataObject, CF_UNICODETEXT);

    size_t first = 0;
    while (first < url.size() && iswspace(url[first])) ++first;
    size_t last = url.size();
    while (last > first && iswspace(url[last - 1])) --last;
    url = url.substr(first, last - first);

    const bool supported =
        _wcsnicmp(url.c_str(), L"http://", 7) == 0 ||
        _wcsnicmp(url.c_str(), L"https://", 8) == 0 ||
        _wcsnicmp(url.c_str(), L"ftp://", 6) == 0;
    return supported ? url : std::wstring{};
}

/**
 * @brief 判断 URL 是否指向可下载的文件
 * @param url URL 字符串
 * @param fileName [out] 解析出的文件名
 * @return 是可下载文件返回 true
 */
bool DesktopApp::IsFileDownloadUrl(const std::wstring& url, std::wstring& fileName)
{
    const wchar_t* afterScheme = wcschr(url.c_str(), L':');
    if (!afterScheme || afterScheme[1] != L'/' || afterScheme[2] != L'/')
        return false;
    const wchar_t* hostStart = afterScheme + 3;
    const wchar_t* pathStart = wcschr(hostStart, L'/');
    if (!pathStart) return false;

    const wchar_t* p = pathStart + 1;
    const wchar_t* lastSlash = pathStart;
    const wchar_t* queryStart = wcschr(p, L'?');
    const wchar_t* fragStart = wcschr(p, L'#');
    const wchar_t* end = p + wcslen(p);
    if (queryStart && queryStart < end) end = queryStart;
    if (fragStart && fragStart < end) end = fragStart;

    for (const wchar_t* s = p; s < end; ++s)
        if (*s == L'/') lastSlash = s;

    if (lastSlash >= end - 1) return false;
    fileName.assign(lastSlash + 1, end - lastSlash - 1);
    if (fileName.empty()) return false;

    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return false;

    std::wstring ext = fileName.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));

    static const std::vector<std::wstring> webExts = {
        L".html", L".htm", L".php", L".asp", L".aspx", L".jsp", L".cfm", L".shtml", L".xhtml"
    };
    for (const auto& we : webExts)
        if (ext == we) return false;

    return true;
}

/**
 * @brief 处理 URL 内容：文件链接则下载，否则创建 .lnk
 * @param url URL 字符串
 * @return data 目录中的持久内容路径
 */
std::wstring DesktopApp::HandleUrlContent(const std::wstring& url)
{
    std::wstring result;

    std::wstring fileName;
    if (IsFileDownloadUrl(url, fileName))
    {
        const auto destination = UniqueDropContentPath(
            std::move(fileName), L"download");
        if (!destination.empty())
        {
            if (SUCCEEDED(URLDownloadToFileW(
                    nullptr, url.c_str(), destination.c_str(), 0, nullptr)))
                result = destination.wstring();
            else
                DeleteFileW(destination.c_str());
        }
    }

    if (!result.empty()) return result;
    return CreateUrlShortcut(url);
}

std::wstring DesktopApp::CreateUrlShortcut(const std::wstring& url)
{
    std::wstring result;
    std::wstring hostName;
    const wchar_t* afterScheme = wcschr(url.c_str(), L':');
    if (afterScheme && afterScheme[1] == L'/' && afterScheme[2] == L'/')
    {
        const wchar_t* hostStart = afterScheme + 3;
        const wchar_t* hostEnd = wcschr(hostStart, L'/');
        if (!hostEnd) hostEnd = wcschr(hostStart, L'?');
        if (!hostEnd) hostEnd = wcschr(hostStart, L'#');
        if (!hostEnd) hostEnd = hostStart + wcslen(hostStart);
        hostName.assign(hostStart, hostEnd - hostStart);
    }
    if (hostName.size() > 4 && _wcsnicmp(hostName.c_str(), L"www.", 4) == 0)
        hostName = hostName.substr(4);
    if (hostName.empty()) hostName = _LW("app.interact.link");

    const auto linkPath = UniqueDropContentPath(
        hostName + L".lnk", L"link.lnk");
    if (linkPath.empty()) return {};

    ComPtr<IShellLinkW> shellLink;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&shellLink))))
    {
        shellLink->SetPath(url.c_str());
        shellLink->SetDescription(url.c_str());
        ComPtr<IPersistFile> persistFile;
        if (SUCCEEDED(shellLink.As(&persistFile)))
        {
            if (SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE)))
                result = linkPath.wstring();
        }
    }
    if (result.empty())
        DeleteFileW(linkPath.c_str());
    return result;
}

/**
 * @brief 从数据对象中提取 URL 并处理（下载文件或创建 .lnk）
 * @param dataObject COM 数据对象
 * @return data 目录中的持久内容路径列表
 */
std::vector<std::wstring> DesktopApp::TryExtractUrlFromDataObject(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    const std::wstring url = ExtractDropUrl(dataObject);
    if (url.empty()) return paths;

    std::wstring resultPath = HandleUrlContent(url);
    if (!resultPath.empty())
        paths.push_back(resultPath);
    return paths;
}

/**
 * @brief 从数据对象中提取位图图像并保存为 PNG 文件
 * @param dataObject COM 数据对象
 * @return data 目录中的持久 PNG 文件路径列表
 */
std::vector<std::wstring> DesktopApp::TryExtractImageFromDataObject(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    if (!dataObject) return paths;

    FORMATETC fmt{};
    fmt.cfFormat = CF_DIB;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (FAILED(dataObject->GetData(&fmt, &med)) || !med.hGlobal)
        return paths;

    BITMAPINFOHEADER* bmih = static_cast<BITMAPINFOHEADER*>(GlobalLock(med.hGlobal));
    if (!bmih)
    {
        ReleaseStgMedium(&med);
        return paths;
    }

    int colorsUsed = bmih->biClrUsed;
    if (colorsUsed == 0 && bmih->biBitCount <= 8)
        colorsUsed = 1 << bmih->biBitCount;
    int colorTableSize = colorsUsed * sizeof(RGBQUAD);
    if (bmih->biCompression == BI_BITFIELDS)
        colorTableSize = 3 * sizeof(DWORD);

    BYTE* pixelData = reinterpret_cast<BYTE*>(bmih) + bmih->biSize + colorTableSize;

    HDC screenDc = GetDC(nullptr);
    HBITMAP hBitmap = nullptr;

    if (bmih->biCompression == BI_RGB || bmih->biCompression == BI_BITFIELDS)
    {
        hBitmap = CreateDIBitmap(screenDc, bmih, CBM_INIT, pixelData,
            reinterpret_cast<const BITMAPINFO*>(bmih), DIB_RGB_COLORS);
    }

    if (!hBitmap)
    {
        GlobalUnlock(med.hGlobal);
        ReleaseStgMedium(&med);
        ReleaseDC(nullptr, screenDc);
        return paths;
    }

    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    std::filesystem::path pngPath;

    if (SUCCEEDED(hr))
    {
        ComPtr<IWICBitmap> wicBitmap;
        hr = wicFactory->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUseAlpha, &wicBitmap);

        if (SUCCEEDED(hr))
        {
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t nameBuf[64]{};
            swprintf_s(nameBuf, L"snow_image_%04d%02d%02d_%02d%02d%02d.png",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            pngPath = UniqueDropContentPath(nameBuf, L"snow_image.png");
            if (pngPath.empty()) hr = E_FAIL;

            ComPtr<IWICStream> stream;
            if (SUCCEEDED(hr)) hr = wicFactory->CreateStream(&stream);
            if (SUCCEEDED(hr))
            {
                hr = stream->InitializeFromFilename(
                    pngPath.c_str(), GENERIC_WRITE);
                if (SUCCEEDED(hr))
                {
                    ComPtr<IWICBitmapEncoder> encoder;
                    hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
                    if (SUCCEEDED(hr))
                    {
                        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
                        if (SUCCEEDED(hr))
                        {
                            ComPtr<IWICBitmapFrameEncode> frame;
                            hr = encoder->CreateNewFrame(&frame, nullptr);
                            if (SUCCEEDED(hr))
                            {
                                hr = frame->Initialize(nullptr);
                                if (SUCCEEDED(hr))
                                {
                                    hr = frame->WriteSource(wicBitmap.Get(), nullptr);
                                    if (SUCCEEDED(hr))
                                    {
                                        hr = frame->Commit();
                                        if (SUCCEEDED(hr))
                                        {
                                            hr = encoder->Commit();
                                            if (SUCCEEDED(hr))
                                                paths.push_back(
                                                    pngPath.wstring());
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    DeleteObject(hBitmap);
    GlobalUnlock(med.hGlobal);
    ReleaseStgMedium(&med);
    ReleaseDC(nullptr, screenDc);
    if (paths.empty() && !pngPath.empty())
        DeleteFileW(pngPath.c_str());
    return paths;
}

/**
 * @brief 从数据对象中提取文本并保存为 UTF-8 .txt 文件
 * @param dataObject COM 数据对象
 * @return data 目录中的持久 .txt 文件路径列表
 */
std::vector<std::wstring> DesktopApp::TryExtractTextFromDataObject(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    if (!dataObject) return paths;

    FORMATETC fmt{};
    fmt.cfFormat = CF_UNICODETEXT;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (FAILED(dataObject->GetData(&fmt, &med)) || !med.hGlobal)
        return paths;

    const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(med.hGlobal));
    if (!data)
    {
        ReleaseStgMedium(&med);
        return paths;
    }

    std::wstring text(data);
    GlobalUnlock(med.hGlobal);
    ReleaseStgMedium(&med);

    size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'\t' || text[start] == L'\r' || text[start] == L'\n'))
        ++start;
    size_t end = text.size();
    while (end > start && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r' || text[end - 1] == L'\n'))
        --end;
    text = text.substr(start, end - start);
    if (text.empty()) return paths;

    bool textIsUrl = (_wcsnicmp(text.c_str(), L"http://", 7) == 0 ||
                     _wcsnicmp(text.c_str(), L"https://", 8) == 0 ||
                     _wcsnicmp(text.c_str(), L"ftp://", 6) == 0);
    if (textIsUrl)
    {
        std::wstring resultPath = HandleUrlContent(text);
        if (!resultPath.empty())
            paths.push_back(resultPath);
        return paths;
    }

    std::wstring firstLine = text;
    size_t nl = firstLine.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) firstLine = firstLine.substr(0, nl);
    if (firstLine.size() > 30) firstLine = firstLine.substr(0, 30);

    for (auto& ch : firstLine)
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
            ch = L'_';

    std::wstring baseName = firstLine.empty() ? L"snow_text" : firstLine;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timePart[32]{};
    swprintf_s(timePart, L"_%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring name = baseName + timePart + L".txt";
    const auto textPath = UniqueDropContentPath(
        std::move(name), L"snow_text.txt");
    if (textPath.empty()) return paths;

    FILE* f = nullptr;
    if (_wfopen_s(&f, textPath.c_str(), L"w,ccs=UTF-8") == 0 && f)
    {
        fputws(text.c_str(), f);
        fclose(f);
        paths.push_back(textPath.wstring());
    }

    return paths;
}

/**
 * @brief 尝试从非文件拖放格式中提取内容，优先：图像 > URL > 文本
 * @param dataObject COM 数据对象
 * @return 临时文件路径列表
 */
std::vector<std::wstring> DesktopApp::TryGetNonFileDropPaths(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;

    paths = TryExtractImageFromDataObject(dataObject);
    if (!paths.empty()) return paths;

    paths = TryExtractUrlFromDataObject(dataObject);
    if (!paths.empty()) return paths;

    paths = TryExtractTextFromDataObject(dataObject);
    return paths;
}

/**
 * @brief 从完整路径中提取文件名部分
 * @param path 完整路径
 * @return 文件名
 */

std::wstring DesktopApp::FileNameFromPath(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

/**
 * @brief 匹配待处理文件名（支持快捷方式和副本后缀的模糊匹配）
 * @param itemName 现有项名称
 * @param srcFileName 源文件名
 * @return 是否匹配成功
 */
bool DesktopApp::MatchPendingName(const std::wstring& itemName, const std::wstring& srcFileName)
{
    const std::vector<std::wstring> shortcutSuffixes =
        Locale::Instance().TranslationValues(
            L10N_KEY("app.interact.shortcut_suffix"));
    const std::vector<std::wstring> copySuffixes =
        Locale::Instance().TranslationValues(
            L10N_KEY("app.interact.copy_suffix"));

    auto stripLnk = [](const std::wstring& s) -> std::wstring {
        if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".lnk") == 0)
            return s.substr(0, s.size() - 4);
        return s;
    };
    auto stripExt = [](const std::wstring& s) -> std::wstring {
        size_t dot = s.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) return s;
        return s.substr(0, dot);
    };
    auto stripLocalizedSuffix = [](const std::wstring& text,
        const std::vector<std::wstring>& suffixes) -> std::wstring {
        for (const std::wstring& suffix : suffixes)
        {
            if (!suffix.empty() && text.size() > suffix.size() &&
                _wcsicmp(text.c_str() + text.size() - suffix.size(),
                    suffix.c_str()) == 0)
            {
                return text.substr(0, text.size() - suffix.size());
            }
        }
        return text;
    };
    auto stripShortcut = [&](const std::wstring& s) -> std::wstring {
        std::wstring stripped = stripLocalizedSuffix(s, shortcutSuffixes);
        if (stripped != s)
            return stripped;
        return s;
    };
    auto stripCopy = [&](const std::wstring& s) -> std::wstring {
        std::wstring value = s;
        size_t paren = value.rfind(L" (");
        if (paren != std::wstring::npos && value.ends_with(L")"))
            value = value.substr(0, paren);
        std::wstring stripped = stripLocalizedSuffix(value, copySuffixes);
        return stripped != value ? stripped : s;
    };
    auto eqi = [](const std::wstring& a, const std::wstring& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (towlower(a[i]) != towlower(b[i])) return false;
        return true;
    };
    if (eqi(itemName, srcFileName)) return true;

    std::wstring nameNoLnk = stripLnk(itemName);
    std::wstring srcNoExt = stripExt(srcFileName);

    if (eqi(nameNoLnk, srcFileName)) return true;
    if (eqi(itemName, srcNoExt)) return true;
    if (eqi(nameNoLnk, srcNoExt)) return true;

    std::wstring nameNoShortcut = stripShortcut(itemName);
    std::wstring nameNoLnkNoShortcut = stripShortcut(nameNoLnk);

    if (eqi(nameNoShortcut, srcFileName)) return true;
    if (eqi(nameNoShortcut, srcNoExt)) return true;
    if (eqi(nameNoLnkNoShortcut, srcFileName)) return true;
    if (eqi(nameNoLnkNoShortcut, srcNoExt)) return true;

    std::wstring nameNoCopy = stripCopy(itemName);
    std::wstring nameNoLnkNoCopy = stripCopy(nameNoLnk);
    std::wstring nameNoExtNoCopy = stripCopy(stripExt(itemName));
    if (eqi(nameNoCopy, srcFileName)) return true;
    if (eqi(nameNoCopy, srcNoExt)) return true;
    if (eqi(nameNoLnkNoCopy, srcFileName)) return true;
    if (eqi(nameNoLnkNoCopy, srcNoExt)) return true;
    if (eqi(nameNoExtNoCopy, srcNoExt)) return true;

    return false;
}

// ── Drag hint ────────────────────────────────────────────────

/**
 * @brief 确保拖拽提示窗口已创建
 * @return 窗口是否可用
 */
