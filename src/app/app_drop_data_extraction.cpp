#include "app.h"
#include "../drop_data_url.h"
#include "../drop_image_data.h"
#include "../drop_text_rules.h"
#include "../url_drop_resource.h"
#include "../virtual_file_drop.h"

#include <cstddef>
#include <limits>

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

std::filesystem::path UniqueDropContentPath(std::wstring filename,
    std::wstring_view fallback)
{
    const auto root = DropContentDirectory();
    if (root.empty()) return {};
    if (filename.empty())
        filename.assign(fallback);
    filename = snowdesktop::virtual_file_drop::
        SanitizeSuggestedFileName(filename);
    const std::filesystem::path original(filename);
    const std::wstring stem = original.stem().wstring();
    const std::wstring extension = original.extension().wstring();
    for (unsigned int suffix = 0; suffix < 10000; ++suffix)
    {
        const std::filesystem::path candidate = root /
            (suffix == 0 ? filename :
                stem + L" (" + std::to_wstring(suffix) + L")" + extension);
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
                return candidate;
            return {};
        }
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
        constexpr SIZE_T kMaximumTextFormatBytes =
            16ull * 1024ull * 1024ull;
        const SIZE_T byteCount = GlobalSize(medium.hGlobal);
        const auto* data = static_cast<const wchar_t*>(
            GlobalLock(medium.hGlobal));
        if (data && byteCount >= sizeof(wchar_t) &&
            byteCount <= kMaximumTextFormatBytes)
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
        constexpr SIZE_T kMaximumTextFormatBytes =
            16ull * 1024ull * 1024ull;
        const SIZE_T byteCount = GlobalSize(medium.hGlobal);
        const auto* data = static_cast<const char*>(
            GlobalLock(medium.hGlobal));
        if (data && byteCount > 0 &&
            byteCount <= kMaximumTextFormatBytes &&
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

std::vector<std::wstring> TryGetLocalFileUrlPaths(
    const snowdesktop::drop_text_rules::Classification& reference)
{
    using snowdesktop::drop_text_rules::Kind;
    if (reference.kind != Kind::FileUrl || reference.value.empty())
        return {};

    std::vector<wchar_t> path(32768, L'\0');
    DWORD pathLength = static_cast<DWORD>(path.size());
    if (FAILED(PathCreateFromUrlW(reference.value.c_str(),
            path.data(), &pathLength, 0)))
        return {};
    const std::wstring resolved(path.data());
    if (!snowdesktop::drop_text_rules::
            IsAbsoluteLocalDrivePath(resolved))
        return {};

    const wchar_t driveRoot[]{
        resolved[0], L':', L'\\', L'\0'
    };
    const UINT driveType = GetDriveTypeW(driveRoot);
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE &&
        driveType != DRIVE_CDROM && driveType != DRIVE_RAMDISK)
        return {};

    const DWORD attributes = GetFileAttributesW(resolved.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return {};
    return { resolved };
}

bool IsBlockedVirtualFileName(std::wstring_view fileName)
{
    const std::wstring safeName = snowdesktop::virtual_file_drop::
        SanitizeSuggestedFileName(fileName);
    const wchar_t* extension = PathFindExtensionW(safeName.c_str());
    if (!extension || !*extension)
        return false;
    return AssocIsDangerous(extension) ||
        _wcsicmp(extension, L".lnk") == 0 ||
        _wcsicmp(extension, L".url") == 0 ||
        _wcsicmp(extension, L".website") == 0 ||
        _wcsicmp(extension, L".scf") == 0;
}

DWORD WriteNewDropContentFile(const std::filesystem::path& path,
    const void* data, std::size_t byteCount)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0,
        nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError();

    const auto* bytes = reinterpret_cast<const std::byte*>(
        data);
    std::size_t offset = 0;
    DWORD error = ERROR_SUCCESS;
    while (offset < byteCount)
    {
        const DWORD requested = static_cast<DWORD>(
            std::min<std::size_t>(byteCount - offset,
                std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, requested,
                &written, nullptr))
        {
            error = GetLastError();
            if (error == ERROR_SUCCESS)
                error = ERROR_WRITE_FAULT;
            break;
        }
        if (written == 0)
        {
            error = ERROR_WRITE_FAULT;
            break;
        }
        offset += written;
    }
    if (error == ERROR_SUCCESS && !FlushFileBuffers(file))
    {
        error = GetLastError();
        if (error == ERROR_SUCCESS)
            error = ERROR_WRITE_FAULT;
    }
    if (error == ERROR_SUCCESS)
    {
        FILE_BASIC_INFO basicInformation{};
        basicInformation.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(file, FileBasicInfo,
                &basicInformation, sizeof(basicInformation)))
        {
            error = GetLastError();
            if (error == ERROR_SUCCESS)
                error = ERROR_WRITE_FAULT;
        }
    }
    if (error != ERROR_SUCCESS)
    {
        FILE_DISPOSITION_INFO disposition{ TRUE };
        if (!SetFileInformationByHandle(file, FileDispositionInfo,
                &disposition, sizeof(disposition)))
        {
            LARGE_INTEGER beginning{};
            if (SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN))
                (void)SetEndOfFile(file);
        }
    }
    if (!CloseHandle(file) && error == ERROR_SUCCESS)
    {
        error = GetLastError();
        if (error == ERROR_SUCCESS)
            error = ERROR_WRITE_FAULT;
    }
    return error;
}

std::wstring WriteUniqueDropContentFile(
    std::wstring suggestedFileName,
    std::wstring_view fallbackFileName,
    const void* data,
    std::size_t byteCount)
{
    if (byteCount != 0 && !data)
        return {};
    for (unsigned int attempt = 0; attempt < 16; ++attempt)
    {
        const auto path = UniqueDropContentPath(
            suggestedFileName, fallbackFileName);
        if (path.empty())
            return {};
        const DWORD error = WriteNewDropContentFile(
            path, data, byteCount);
        if (error == ERROR_SUCCESS)
        {
            try
            {
                return path.wstring();
            }
            catch (...)
            {
                (void)DeleteFileW(path.c_str());
                return {};
            }
        }
        if (error != ERROR_FILE_EXISTS &&
            error != ERROR_ALREADY_EXISTS)
            return {};
    }
    return {};
}

std::vector<std::byte> EncodeUtf8WithBom(std::wstring_view text)
{
    if (text.empty() || text.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return {};

    const int sourceLength = static_cast<int>(text.size());
    const int encodedLength = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), sourceLength,
        nullptr, 0, nullptr, nullptr);
    if (encodedLength <= 0)
        return {};

    std::vector<std::byte> bytes(
        static_cast<std::size_t>(encodedLength) + 3);
    bytes[0] = std::byte{0xef};
    bytes[1] = std::byte{0xbb};
    bytes[2] = std::byte{0xbf};
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            text.data(), sourceLength,
            reinterpret_cast<char*>(bytes.data() + 3),
            encodedLength, nullptr, nullptr) != encodedLength)
        return {};
    return bytes;
}
}

DesktopApp::DropReferenceSnapshot
DesktopApp::ReadDropReferenceSnapshot(IDataObject* dataObject)
{
    DropReferenceSnapshot snapshot;
    if (!dataObject)
        return snapshot;

    const std::wstring advertisedWide = ReadWideHGlobalFormat(
        dataObject, static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_INETURLW)));
    const std::wstring advertisedAnsi = ReadAnsiHGlobalFormat(
        dataObject, static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(CFSTR_INETURLA)));
    snapshot.unicodeText = ReadWideHGlobalFormat(
        dataObject, CF_UNICODETEXT);
    snapshot.candidates = snowdesktop::drop_text_rules::
        ClassifyResourceCandidates(advertisedWide, advertisedAnsi,
            snapshot.unicodeText);
    snapshot.reference = snowdesktop::drop_text_rules::
        SelectResourceReference(snapshot.candidates);
    return snapshot;
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

std::vector<std::wstring>
DesktopApp::TryExtractLocalFileUrlFromDataObject(
    const DropReferenceSnapshot& snapshot)
{
    for (const auto& candidate : snapshot.candidates)
    {
        auto paths = TryGetLocalFileUrlPaths(candidate);
        if (!paths.empty())
            return paths;
    }
    return {};
}

std::wstring DesktopApp::ExtractDropUrl(
    const DropReferenceSnapshot& snapshot)
{
    auto urls = ExtractDropUrls(snapshot);
    return urls.empty() ? std::wstring{} : std::move(urls.front());
}

std::vector<std::wstring> DesktopApp::ExtractDropUrls(
    const DropReferenceSnapshot& snapshot)
{
    using snowdesktop::drop_text_rules::Kind;
    std::vector<std::wstring> urls;
    for (const auto& candidate : snapshot.candidates)
    {
        if (candidate.kind == Kind::HttpUrl ||
            candidate.kind == Kind::HttpsUrl ||
            candidate.kind == Kind::FtpUrl)
        {
            if (std::find(urls.begin(), urls.end(), candidate.value) ==
                urls.end())
                urls.push_back(candidate.value);
        }
    }
    return urls;
}

/**
 * @brief 将 URL 保存为标准 Internet Shortcut
 * @param url URL 字符串
 * @return data 目录中的持久内容路径
 */
std::wstring DesktopApp::HandleUrlContent(const std::wstring& url)
{
    return CreateUrlShortcut(url);
}

std::wstring DesktopApp::CreateUrlShortcut(const std::wstring& url)
{
    if (url.empty() || url.size() > 32768 ||
        url.find_first_of(L"\r\n") != std::wstring::npos)
        return {};

    std::wstring hostName = snowdesktop::drop_text_rules::
        HierarchicalUriHost(url);
    if (hostName.size() > 4 && _wcsnicmp(hostName.c_str(), L"www.", 4) == 0)
        hostName = hostName.substr(4);
    if (hostName.empty()) hostName = _LW("app.interact.link");

    std::wstring content;
    content.reserve(url.size() + 32);
    content.append(1, L'\ufeff');
    content.append(L"[InternetShortcut]\r\nURL=");
    content.append(url);
    content.append(L"\r\n");
    return WriteUniqueDropContentFile(
        hostName + L".url", L"link.url", content.data(),
        content.size() * sizeof(wchar_t));
}

/**
 * @brief 从数据对象中提取 URL 并创建 .url 回退
 * @param dataObject COM 数据对象
 * @return data 目录中的持久内容路径列表
 */
std::vector<std::wstring> DesktopApp::TryExtractUrlFromDataObject(
    const DropReferenceSnapshot& snapshot)
{
    std::vector<std::wstring> paths;
    std::wstring url = ExtractDropUrl(snapshot);
    if (url.empty() && snowdesktop::drop_text_rules::
            IsPrivateHierarchicalResource(snapshot.reference))
        url = snapshot.reference.value;
    if (url.empty()) return paths;

    std::wstring resultPath = HandleUrlContent(url);
    if (!resultPath.empty())
        paths.push_back(resultPath);
    return paths;
}

std::vector<std::wstring>
DesktopApp::TryExtractDataUrlFromDataObject(
    const DropReferenceSnapshot& snapshot)
{
    using snowdesktop::drop_text_rules::Kind;
    constexpr std::size_t kMaximumInlineResourceBytes =
        16ull * 1024ull * 1024ull;
    for (const auto& candidate : snapshot.candidates)
    {
        if (candidate.kind != Kind::DataUrl)
            continue;
        const auto decoded = snowdesktop::drop_data_url::Decode(
            candidate.value, kMaximumInlineResourceBytes);
        if (!decoded)
            continue;

        const std::wstring contentType(
            decoded.contentType.begin(), decoded.contentType.end());
        const auto decision = snowdesktop::url_drop_resource::Decide(
            L"https://inline.invalid/resource", contentType,
            L"attachment; filename=inline-resource");
        if (decision.action !=
                snowdesktop::url_drop_resource::Action::Download ||
            decision.suggestedFileName.empty())
            continue;

        const std::wstring path = WriteUniqueDropContentFile(
            decision.suggestedFileName, L"inline-resource.bin",
            decoded.bytes.data(), decoded.bytes.size());
        if (!path.empty())
            return {path};
    }
    return {};
}

std::vector<std::wstring>
DesktopApp::TryMaterializeVirtualFilesFromDataObject(
    IDataObject* dataObject,
    const std::vector<snowdesktop::virtual_file_drop::
        VirtualFileDescriptor>& descriptors,
    bool* allEntriesMaterialized)
{
    std::vector<std::wstring> paths;
    if (allEntriesMaterialized)
        *allEntriesMaterialized = false;
    if (!dataObject)
        return paths;
    const auto destination = DropContentDirectory();
    if (destination.empty())
        return paths;
    if (descriptors.empty())
        return paths;

    // Sources without IDataObjectAsyncCapability are rendered synchronously
    // by OLE. Keep that compatibility fallback bounded so one drop cannot
    // make the desktop copy an arbitrarily large private stream on its STA.
    constexpr std::uint64_t kMaximumSynchronousDropBytes =
        64ull * 1024ull * 1024ull;
    std::uint64_t remainingBytes = kMaximumSynchronousDropBytes;
    constexpr std::size_t kMaximumSynchronousFileCount = 256;
    paths.reserve(std::min(descriptors.size(),
        kMaximumSynchronousFileCount));
    std::size_t attempted = 0;
    bool complete = descriptors.size() <=
        kMaximumSynchronousFileCount;
    for (const auto& descriptor : descriptors)
    {
        if (attempted++ >= kMaximumSynchronousFileCount)
            break;
        auto safeDescriptor = descriptor;
        safeDescriptor.suggestedFileName = snowdesktop::
            virtual_file_drop::SanitizeSuggestedFileName(
                descriptor.suggestedFileName);
        if (IsBlockedVirtualFileName(
                safeDescriptor.suggestedFileName))
        {
            complete = false;
            continue;
        }
        if (safeDescriptor.advertisedFileSize &&
            *safeDescriptor.advertisedFileSize > remainingBytes)
        {
            complete = false;
            continue;
        }
        auto materialized = snowdesktop::virtual_file_drop::
            MaterializeFileContents(dataObject, safeDescriptor,
                destination.wstring(), remainingBytes);
        if (!materialized || materialized->path.empty() ||
            materialized->sizeBytes > remainingBytes)
        {
            complete = false;
            continue;
        }
        remainingBytes -= materialized->sizeBytes;
        paths.push_back(std::move(materialized->path));
        if (remainingBytes == 0)
        {
            if (attempted < descriptors.size())
                complete = false;
            break;
        }
    }
    if (allEntriesMaterialized)
        *allEntriesMaterialized = complete;
    return paths;
}

/**
 * @brief 从数据对象中提取位图图像并保存为 PNG 文件
 * @param dataObject COM 数据对象
 * @return data 目录中的持久 PNG 文件路径列表
 */
std::vector<std::wstring> DesktopApp::TryExtractImageFromDataObject(
    IDataObject* dataObject, bool allowStreamInput)
{
    std::vector<std::wstring> paths;
    if (!dataObject)
        return paths;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t nameBuf[64]{};
    swprintf_s(nameBuf, L"snow_image_%04d%02d%02d_%02d%02d%02d.png",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::filesystem::path pngPath = UniqueDropContentPath(
        nameBuf, L"snow_image.png");
    if (pngPath.empty())
        return paths;

    snowdesktop::drop_image_data::Limits limits;
    limits.allowStreamInput = allowStreamInput;
    const auto result = snowdesktop::drop_image_data::SaveAsPng(
        dataObject, pngPath, limits);
    if (result)
    {
        try
        {
            paths.push_back(pngPath.wstring());
        }
        catch (...)
        {
            (void)DeleteFileW(pngPath.c_str());
        }
    }
    return paths;
}

/**
 * @brief 从数据对象中提取文本并保存为 UTF-8 .txt 文件
 * @param dataObject COM 数据对象
 * @return data 目录中的持久 .txt 文件路径列表
 */
std::vector<std::wstring> DesktopApp::TryExtractTextFromDataObject(
    const DropReferenceSnapshot& snapshot)
{
    std::vector<std::wstring> paths;

    using snowdesktop::drop_text_rules::Kind;
    const auto& advertisedReference = snapshot.reference;
    if (advertisedReference.kind != Kind::PlainText)
        return paths;

    auto classified = snowdesktop::drop_text_rules::Classify(
        snapshot.unicodeText,
        snowdesktop::drop_text_rules::Source::UnicodeText);
    if (classified.kind != Kind::PlainText)
        return paths;
    std::wstring text = std::move(classified.value);

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
    const auto encodedText = EncodeUtf8WithBom(text);
    if (encodedText.empty()) return paths;
    const std::wstring textPath = WriteUniqueDropContentFile(
        std::move(name), L"snow_text.txt", encodedText.data(),
        encodedText.size());
    if (!textPath.empty())
        paths.push_back(textPath);

    return paths;
}

/**
 * @brief 尝试从非文件拖放格式中提取内容，优先：图像 > 内联数据 > 网络 URL > 文本
 * @param dataObject COM 数据对象
 * @return 临时文件路径列表
 */
std::vector<std::wstring> DesktopApp::TryGetNonFileDropPaths(
    IDataObject* dataObject,
    const DropReferenceSnapshot& snapshot)
{
    std::vector<std::wstring> paths;

    paths = TryExtractImageFromDataObject(dataObject);
    if (!paths.empty()) return paths;

    paths = TryExtractDataUrlFromDataObject(snapshot);
    if (!paths.empty()) return paths;

    paths = TryExtractUrlFromDataObject(snapshot);
    if (!paths.empty()) return paths;

    paths = TryExtractTextFromDataObject(snapshot);
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
        {
            const std::wstring_view number(value.data() + paren + 2,
                value.size() - paren - 3);
            if (!number.empty() &&
                std::all_of(number.begin(), number.end(),
                    [](wchar_t character) {
                        return iswdigit(character) != 0;
                    }))
                value = value.substr(0, paren);
        }
        std::wstring stripped = stripLocalizedSuffix(value, copySuffixes);
        return stripped != value ? stripped : value;
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
