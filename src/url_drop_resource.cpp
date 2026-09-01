#include "url_drop_resource.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cwctype>
#include <optional>
#include <vector>

namespace snowdesktop::url_drop_resource
{
namespace
{

std::wstring Trim(std::wstring_view value)
{
    size_t first = 0;
    while (first < value.size() && iswspace(value[first]))
        ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1]))
        --last;
    return std::wstring(value.substr(first, last - first));
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
    return value;
}

std::wstring NormalizeContentType(std::wstring_view raw)
{
    const size_t separator = raw.find(L';');
    return Lower(Trim(raw.substr(0, separator)));
}

bool StartsWith(std::wstring_view value, std::wstring_view prefix)
{
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(INT_MAX)) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), length) != length)
        return {};
    return result;
}

int HexValue(wchar_t character)
{
    if (character >= L'0' && character <= L'9')
        return character - L'0';
    if (character >= L'a' && character <= L'f')
        return character - L'a' + 10;
    if (character >= L'A' && character <= L'F')
        return character - L'A' + 10;
    return -1;
}

std::wstring PercentDecodeUtf8(std::wstring_view value)
{
    std::wstring result;
    std::string encodedBytes;
    const auto flushBytes = [&]() {
        if (encodedBytes.empty()) return true;
        std::wstring decoded = Utf8ToWide(encodedBytes);
        encodedBytes.clear();
        if (decoded.empty()) return false;
        result.append(decoded);
        return true;
    };

    for (size_t index = 0; index < value.size();)
    {
        if (value[index] == L'%' && index + 2 < value.size())
        {
            const int high = HexValue(value[index + 1]);
            const int low = HexValue(value[index + 2]);
            if (high >= 0 && low >= 0)
            {
                encodedBytes.push_back(static_cast<char>((high << 4) | low));
                index += 3;
                continue;
            }
        }
        if (!flushBytes())
            return std::wstring(value);
        result.push_back(value[index]);
        ++index;
    }
    if (!flushBytes())
        return std::wstring(value);
    return result;
}

std::wstring UrlFileName(std::wstring_view url)
{
    size_t end = url.find_first_of(L"?#");
    if (end == std::wstring_view::npos)
        end = url.size();
    const size_t slash = url.substr(0, end).find_last_of(L'/');
    if (slash == std::wstring_view::npos || slash + 1 >= end)
        return L"download";
    return PercentDecodeUtf8(url.substr(slash + 1, end - slash - 1));
}

struct ContentDisposition
{
    bool attachment = false;
    std::wstring fileName;
};

std::vector<std::wstring> SplitDisposition(std::wstring_view raw)
{
    std::vector<std::wstring> parts;
    size_t start = 0;
    bool quoted = false;
    bool escaped = false;
    for (size_t index = 0; index <= raw.size(); ++index)
    {
        const bool atEnd = index == raw.size();
        const wchar_t character = atEnd ? L';' : raw[index];
        if (!atEnd && escaped)
        {
            escaped = false;
            continue;
        }
        if (!atEnd && quoted && character == L'\\')
        {
            escaped = true;
            continue;
        }
        if (!atEnd && character == L'"')
        {
            quoted = !quoted;
            continue;
        }
        if (character == L';' && !quoted)
        {
            parts.push_back(Trim(raw.substr(start, index - start)));
            start = index + 1;
        }
    }
    return parts;
}

std::wstring Unquote(std::wstring value)
{
    value = Trim(value);
    if (value.size() < 2 || value.front() != L'"' ||
        value.back() != L'"')
        return value;
    std::wstring result;
    result.reserve(value.size() - 2);
    bool escaped = false;
    for (size_t index = 1; index + 1 < value.size(); ++index)
    {
        const wchar_t character = value[index];
        if (escaped)
        {
            result.push_back(character);
            escaped = false;
        }
        else if (character == L'\\')
            escaped = true;
        else
            result.push_back(character);
    }
    if (escaped) result.push_back(L'\\');
    return result;
}

std::wstring DecodeExtendedFileName(std::wstring_view value)
{
    std::wstring unquoted = Unquote(std::wstring(value));
    const size_t firstQuote = unquoted.find(L'\'');
    const size_t secondQuote = firstQuote == std::wstring::npos
        ? std::wstring::npos : unquoted.find(L'\'', firstQuote + 1);
    if (firstQuote == std::wstring::npos ||
        secondQuote == std::wstring::npos)
        return {};
    const std::wstring charset = Lower(
        unquoted.substr(0, firstQuote));
    if (charset != L"utf-8") return {};
    return PercentDecodeUtf8(
        std::wstring_view(unquoted).substr(secondQuote + 1));
}

ContentDisposition ParseContentDisposition(std::wstring_view raw)
{
    ContentDisposition result;
    const auto parts = SplitDisposition(raw);
    if (parts.empty()) return result;
    result.attachment = Lower(parts.front()) == L"attachment";
    std::wstring fallbackFileName;
    for (size_t index = 1; index < parts.size(); ++index)
    {
        const size_t equals = parts[index].find(L'=');
        if (equals == std::wstring::npos) continue;
        const std::wstring name = Lower(Trim(
            std::wstring_view(parts[index]).substr(0, equals)));
        const std::wstring_view value(parts[index].data() + equals + 1,
            parts[index].size() - equals - 1);
        if (name == L"filename*")
        {
            std::wstring decoded = DecodeExtendedFileName(value);
            if (!decoded.empty())
            {
                result.fileName = std::move(decoded);
                break;
            }
        }
        else if (name == L"filename" && fallbackFileName.empty())
            fallbackFileName = Unquote(std::wstring(value));
    }
    if (result.fileName.empty())
        result.fileName = std::move(fallbackFileName);
    return result;
}

std::wstring ExtensionForContentType(std::wstring_view type)
{
    static constexpr std::array<std::pair<std::wstring_view,
        std::wstring_view>, 42> mappings{{
        {L"image/jpeg", L".jpg"},
        {L"image/png", L".png"},
        {L"image/gif", L".gif"},
        {L"image/webp", L".webp"},
        {L"image/bmp", L".bmp"},
        {L"image/tiff", L".tiff"},
        {L"image/svg+xml", L".svg"},
        {L"image/avif", L".avif"},
        {L"image/heic", L".heic"},
        {L"image/heif", L".heif"},
        {L"image/x-icon", L".ico"},
        {L"audio/mpeg", L".mp3"},
        {L"audio/mp4", L".m4a"},
        {L"audio/wav", L".wav"},
        {L"audio/flac", L".flac"},
        {L"audio/ogg", L".ogg"},
        {L"audio/webm", L".webm"},
        {L"audio/aac", L".aac"},
        {L"video/mp4", L".mp4"},
        {L"video/webm", L".webm"},
        {L"video/quicktime", L".mov"},
        {L"video/x-msvideo", L".avi"},
        {L"video/mpeg", L".mpeg"},
        {L"video/x-matroska", L".mkv"},
        {L"application/pdf", L".pdf"},
        {L"application/zip", L".zip"},
        {L"application/gzip", L".gz"},
        {L"application/x-7z-compressed", L".7z"},
        {L"application/vnd.rar", L".rar"},
        {L"application/x-rar-compressed", L".rar"},
        {L"application/x-tar", L".tar"},
        {L"application/octet-stream", L".bin"},
        {L"application/json", L".json"},
        {L"application/wasm", L".wasm"},
        {L"application/msword", L".doc"},
        {L"application/vnd.openxmlformats-officedocument.wordprocessingml.document", L".docx"},
        {L"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", L".xlsx"},
        {L"application/vnd.openxmlformats-officedocument.presentationml.presentation", L".pptx"},
        {L"text/plain", L".txt"},
        {L"text/csv", L".csv"},
        {L"font/woff", L".woff"},
        {L"font/woff2", L".woff2"},
    }};
    for (const auto& [candidate, extension] : mappings)
        if (type == candidate) return std::wstring(extension);
    if (StartsWith(type, L"image/")) return L".img";
    if (StartsWith(type, L"audio/")) return L".audio";
    if (StartsWith(type, L"video/")) return L".video";
    if (StartsWith(type, L"font/")) return L".font";
    return {};
}

bool IsWebPageType(std::wstring_view type)
{
    return type == L"text/html" || type == L"application/xhtml+xml";
}

bool IsWebPageExtension(std::wstring_view extension)
{
    static constexpr std::array<std::wstring_view, 9> extensions{{
        L".html", L".htm", L".php", L".asp", L".aspx",
        L".jsp", L".cfm", L".shtml", L".xhtml",
    }};
    return std::find(extensions.begin(), extensions.end(), extension) !=
        extensions.end();
}

std::wstring FileExtension(std::wstring_view fileName)
{
    const size_t slash = fileName.find_last_of(L"/\\");
    const size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring_view::npos || dot == 0 ||
        (slash != std::wstring_view::npos && dot < slash))
        return {};
    return Lower(std::wstring(fileName.substr(dot)));
}

bool ExtensionMatchesType(std::wstring_view extension,
    std::wstring_view type, std::wstring_view expected)
{
    if (extension == expected) return true;
    if (type == L"image/jpeg")
        return extension == L".jpeg" || extension == L".jpe";
    if (type == L"image/tiff")
        return extension == L".tif";
    if (type == L"video/mpeg")
        return extension == L".mpg";
    if (type == L"audio/wav")
        return extension == L".wave";
    return false;
}

bool IsReservedWindowsBaseName(std::wstring_view fileName)
{
    const size_t dot = fileName.find(L'.');
    const std::wstring base = Lower(std::wstring(
        fileName.substr(0, dot)));
    if (base == L"con" || base == L"prn" || base == L"aux" ||
        base == L"nul")
        return true;
    if (base.size() == 4 &&
        (base.substr(0, 3) == L"com" ||
         base.substr(0, 3) == L"lpt") &&
        base[3] >= L'1' && base[3] <= L'9')
        return true;
    return false;
}

std::wstring SafeFileName(std::wstring value)
{
    const size_t slash = value.find_last_of(L"/\\");
    if (slash != std::wstring::npos)
        value.erase(0, slash + 1);
    for (auto& character : value)
    {
        if (character < L' ' || character == L'\\' || character == L'/' ||
            character == L':' || character == L'*' || character == L'?' ||
            character == L'"' || character == L'<' || character == L'>' ||
            character == L'|')
            character = L'_';
    }
    while (!value.empty() &&
        (value.back() == L' ' || value.back() == L'.'))
        value.pop_back();
    if (value.empty() || value == L"." || value == L"..")
        value = L"download";
    if (IsReservedWindowsBaseName(value))
        value.insert(value.begin(), L'_');
    constexpr size_t kMaximumFileNameLength = 180;
    if (value.size() > kMaximumFileNameLength)
    {
        const std::wstring extension = FileExtension(value);
        const size_t extensionLength = std::min<size_t>(
            extension.size(), 20);
        value.resize(kMaximumFileNameLength - extensionLength);
        value.append(extension.substr(extension.size() - extensionLength));
    }
    return value;
}

} // namespace

Decision Decide(std::wstring_view effectiveUrl,
    std::wstring_view rawContentType,
    std::wstring_view rawContentDisposition)
{
    Decision result;
    result.normalizedContentType =
        NormalizeContentType(rawContentType);
    if (IsWebPageType(result.normalizedContentType))
        return result;

    const ContentDisposition disposition =
        ParseContentDisposition(rawContentDisposition);
    std::wstring fileName = disposition.fileName.empty()
        ? UrlFileName(effectiveUrl) : disposition.fileName;
    fileName = SafeFileName(std::move(fileName));

    const std::wstring expectedExtension =
        ExtensionForContentType(result.normalizedContentType);
    std::wstring existingExtension = FileExtension(fileName);
    const bool knownResourceType = !expectedExtension.empty();
    const bool reliableUrlExtension = !existingExtension.empty() &&
        !IsWebPageExtension(existingExtension);
    if (!knownResourceType && !disposition.attachment &&
        !reliableUrlExtension)
        return result;

    if (knownResourceType)
    {
        if (existingExtension.empty())
            fileName.append(expectedExtension);
        else if (result.normalizedContentType !=
                L"application/octet-stream" &&
            !ExtensionMatchesType(existingExtension,
                result.normalizedContentType, expectedExtension))
            fileName.append(expectedExtension);
    }
    else if (disposition.attachment && existingExtension.empty())
        fileName.append(L".bin");

    result.action = Action::Download;
    result.suggestedFileName = SafeFileName(std::move(fileName));
    return result;
}

} // namespace snowdesktop::url_drop_resource
