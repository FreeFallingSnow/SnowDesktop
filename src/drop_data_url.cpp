#include "drop_data_url.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace snowdesktop::drop_data_url
{
namespace
{

enum class PartStatus
{
    Decoded,
    Malformed,
    TooLarge,
};

struct DecodedPart
{
    PartStatus status = PartStatus::Malformed;
    std::vector<std::uint8_t> bytes;
};

struct ParsedHeader
{
    PartStatus status = PartStatus::Malformed;
    bool base64 = false;
    std::string contentType;
};

constexpr char kDefaultContentType[] =
    "text/plain;charset=US-ASCII";

bool IsAsciiEqualIgnoreCase(wchar_t left, wchar_t right)
{
    if (left >= L'A' && left <= L'Z')
        left = static_cast<wchar_t>(left - L'A' + L'a');
    if (right >= L'A' && right <= L'Z')
        right = static_cast<wchar_t>(right - L'A' + L'a');
    return left == right;
}

bool EqualsIgnoreCase(std::wstring_view left, std::wstring_view right)
{
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(),
            IsAsciiEqualIgnoreCase);
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

int HexValue(wchar_t character)
{
    if (character >= L'0' && character <= L'9')
        return static_cast<int>(character - L'0');
    if (character >= L'a' && character <= L'f')
        return static_cast<int>(character - L'a') + 10;
    if (character >= L'A' && character <= L'F')
        return static_cast<int>(character - L'A') + 10;
    return -1;
}

bool IsRfc2396UrlCharacter(wchar_t character)
{
    if ((character >= L'a' && character <= L'z') ||
        (character >= L'A' && character <= L'Z') ||
        (character >= L'0' && character <= L'9'))
    {
        return true;
    }

    constexpr std::wstring_view allowed =
        L"-_.!~*'();/?:@&=+$,";
    return allowed.find(character) != std::wstring_view::npos;
}

DecodedPart DecodePercentBytes(
    std::wstring_view encoded, std::size_t maximumBytes)
{
    std::size_t decodedLength = 0;
    bool tooLarge = false;
    for (std::size_t index = 0; index < encoded.size(); ++index)
    {
        const wchar_t character = encoded[index];
        if (character < L'!' || character > L'~')
            return {PartStatus::Malformed, {}};

        if (character == L'%')
        {
            if (index + 2 >= encoded.size() ||
                HexValue(encoded[index + 1]) < 0 ||
                HexValue(encoded[index + 2]) < 0)
            {
                return {PartStatus::Malformed, {}};
            }
            index += 2;
        }
        else if (!IsRfc2396UrlCharacter(character))
        {
            return {PartStatus::Malformed, {}};
        }

        if (decodedLength == std::numeric_limits<std::size_t>::max())
            tooLarge = true;
        else
            ++decodedLength;
        if (decodedLength > maximumBytes)
            tooLarge = true;
    }

    if (tooLarge)
        return {PartStatus::TooLarge, {}};

    DecodedPart result;
    result.status = PartStatus::Decoded;
    result.bytes.reserve(decodedLength);
    for (std::size_t index = 0; index < encoded.size(); ++index)
    {
        const wchar_t character = encoded[index];
        if (character == L'%')
        {
            const int high = HexValue(encoded[index + 1]);
            const int low = HexValue(encoded[index + 2]);
            result.bytes.push_back(static_cast<std::uint8_t>(
                (high << 4) | low));
            index += 2;
        }
        else
        {
            result.bytes.push_back(
                static_cast<std::uint8_t>(character));
        }
    }
    return result;
}

bool IsMimeTokenCharacter(unsigned char character)
{
    if (character <= 32 || character >= 127)
        return false;
    constexpr std::string_view separators = "()<>@,;:\\\"/[]?=";
    return separators.find(static_cast<char>(character)) ==
        std::string_view::npos;
}

bool IsMimeToken(std::string_view value)
{
    return !value.empty() &&
        std::all_of(value.begin(), value.end(), [](char character) {
            return IsMimeTokenCharacter(
                static_cast<unsigned char>(character));
        });
}

PartStatus DecodeMetadataComponent(
    std::wstring_view encoded, std::string& decoded)
{
    const auto part = DecodePercentBytes(
        encoded, std::numeric_limits<std::size_t>::max());
    if (part.status != PartStatus::Decoded)
        return part.status;

    decoded.clear();
    decoded.reserve(part.bytes.size());
    for (std::uint8_t byte : part.bytes)
    {
        if (byte < 32 || byte >= 127)
            return PartStatus::Malformed;
        decoded.push_back(static_cast<char>(byte));
    }
    return PartStatus::Decoded;
}

bool AppendChecked(std::string& destination, std::string_view value)
{
    if (value.size() > destination.max_size() - destination.size())
        return false;
    destination.append(value);
    return true;
}

bool AppendParameterValue(
    std::string& destination, std::string_view value)
{
    if (IsMimeToken(value))
        return AppendChecked(destination, value);

    if (!AppendChecked(destination, "\""))
        return false;
    for (char character : value)
    {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 32 || byte >= 127)
            return false;
        if ((character == '\\' || character == '"') &&
            !AppendChecked(destination, "\\"))
        {
            return false;
        }
        if (destination.size() == destination.max_size())
            return false;
        destination.push_back(character);
    }
    return AppendChecked(destination, "\"");
}

ParsedHeader ParseHeader(std::wstring_view header)
{
    ParsedHeader result;

    std::size_t metadataEnd = header.size();
    const std::size_t lastSemicolon = header.rfind(L';');
    if (lastSemicolon != std::wstring_view::npos &&
        EqualsIgnoreCase(header.substr(lastSemicolon + 1), L"base64"))
    {
        result.base64 = true;
        metadataEnd = lastSemicolon;
    }

    const std::wstring_view metadata = header.substr(0, metadataEnd);
    const std::size_t firstSemicolon = metadata.find(L';');
    const std::wstring_view encodedMediaType = metadata.substr(
        0, firstSemicolon == std::wstring_view::npos
            ? metadata.size()
            : firstSemicolon);

    std::string mediaType;
    const PartStatus mediaTypeStatus = DecodeMetadataComponent(
        encodedMediaType, mediaType);
    if (mediaTypeStatus != PartStatus::Decoded)
    {
        result.status = mediaTypeStatus;
        return result;
    }

    const bool defaultMediaType = mediaType.empty();
    if (!defaultMediaType)
    {
        const std::size_t slash = mediaType.find('/');
        if (slash == std::string::npos || slash == 0 ||
            slash + 1 == mediaType.size() ||
            mediaType.find('/', slash + 1) != std::string::npos ||
            !IsMimeToken(std::string_view(mediaType).substr(0, slash)) ||
            !IsMimeToken(std::string_view(mediaType).substr(slash + 1)))
        {
            return result;
        }
    }
    else if (!metadata.empty() && metadata.front() != L';')
    {
        return result;
    }

    struct Parameter
    {
        std::string attribute;
        std::string value;
    };
    std::vector<Parameter> parameters;
    bool hasCharset = false;
    if (firstSemicolon != std::wstring_view::npos)
    {
        std::size_t position = firstSemicolon + 1;
        while (position <= metadata.size())
        {
            const std::size_t separator = metadata.find(L';', position);
            const std::size_t end = separator == std::wstring_view::npos
                ? metadata.size()
                : separator;
            const std::wstring_view encodedParameter = metadata.substr(
                position, end - position);
            const std::size_t equals = encodedParameter.find(L'=');
            if (encodedParameter.empty() || equals == std::wstring_view::npos)
                return result;

            Parameter parameter;
            PartStatus status = DecodeMetadataComponent(
                encodedParameter.substr(0, equals), parameter.attribute);
            if (status != PartStatus::Decoded)
            {
                result.status = status;
                return result;
            }
            status = DecodeMetadataComponent(
                encodedParameter.substr(equals + 1), parameter.value);
            if (status != PartStatus::Decoded)
            {
                result.status = status;
                return result;
            }
            if (!IsMimeToken(parameter.attribute))
                return result;
            hasCharset = hasCharset ||
                EqualsIgnoreCase(parameter.attribute, "charset");
            parameters.push_back(std::move(parameter));

            if (separator == std::wstring_view::npos)
                break;
            position = separator + 1;
        }
    }

    result.contentType = defaultMediaType ? "text/plain" : mediaType;
    if (defaultMediaType && !hasCharset &&
        !AppendChecked(result.contentType, ";charset=US-ASCII"))
    {
        result.status = PartStatus::TooLarge;
        return result;
    }
    for (const auto& parameter : parameters)
    {
        if (!AppendChecked(result.contentType, ";") ||
            !AppendChecked(result.contentType, parameter.attribute) ||
            !AppendChecked(result.contentType, "=") ||
            !AppendParameterValue(result.contentType, parameter.value))
        {
            result.status = PartStatus::TooLarge;
            return result;
        }
    }

    if (metadata.empty())
        result.contentType = kDefaultContentType;
    result.status = PartStatus::Decoded;
    return result;
}

std::size_t MaximumBase64Characters(std::size_t maximumBytes)
{
    const std::size_t completeGroups = maximumBytes / 3;
    if (completeGroups >
        std::numeric_limits<std::size_t>::max() / 4)
    {
        return std::numeric_limits<std::size_t>::max();
    }
    std::size_t result = completeGroups * 4;
    if (maximumBytes % 3 != 0)
    {
        if (result > std::numeric_limits<std::size_t>::max() - 4)
            return std::numeric_limits<std::size_t>::max();
        result += 4;
    }
    return result;
}

int Base64Value(std::uint8_t character)
{
    if (character >= 'A' && character <= 'Z')
        return static_cast<int>(character - 'A');
    if (character >= 'a' && character <= 'z')
        return static_cast<int>(character - 'a') + 26;
    if (character >= '0' && character <= '9')
        return static_cast<int>(character - '0') + 52;
    if (character == '+')
        return 62;
    if (character == '/')
        return 63;
    return -1;
}

DecodedPart DecodeBase64(
    std::wstring_view payload, std::size_t maximumBytes)
{
    DecodedPart encoded = DecodePercentBytes(
        payload, MaximumBase64Characters(maximumBytes));
    if (encoded.status != PartStatus::Decoded)
        return encoded;
    if (encoded.bytes.size() % 4 != 0)
        return {PartStatus::Malformed, {}};

    std::size_t padding = 0;
    if (!encoded.bytes.empty() && encoded.bytes.back() == '=')
    {
        padding = 1;
        if (encoded.bytes.size() >= 2 &&
            encoded.bytes[encoded.bytes.size() - 2] == '=')
        {
            padding = 2;
        }
    }
    const std::size_t groups = encoded.bytes.size() / 4;
    const std::size_t decodedLength = groups * 3 - padding;
    if (decodedLength > maximumBytes)
        return {PartStatus::TooLarge, {}};

    DecodedPart result;
    result.status = PartStatus::Decoded;
    result.bytes.reserve(decodedLength);
    for (std::size_t index = 0; index < encoded.bytes.size(); index += 4)
    {
        const bool lastGroup = index + 4 == encoded.bytes.size();
        const int first = Base64Value(encoded.bytes[index]);
        const int second = Base64Value(encoded.bytes[index + 1]);
        const bool thirdPadding = encoded.bytes[index + 2] == '=';
        const bool fourthPadding = encoded.bytes[index + 3] == '=';
        const int third = thirdPadding
            ? 0
            : Base64Value(encoded.bytes[index + 2]);
        const int fourth = fourthPadding
            ? 0
            : Base64Value(encoded.bytes[index + 3]);

        if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
            (thirdPadding && !fourthPadding) ||
            ((thirdPadding || fourthPadding) && !lastGroup) ||
            (thirdPadding && (second & 0x0f) != 0) ||
            (!thirdPadding && fourthPadding && (third & 0x03) != 0))
        {
            return {PartStatus::Malformed, {}};
        }

        result.bytes.push_back(static_cast<std::uint8_t>(
            (first << 2) | (second >> 4)));
        if (!thirdPadding)
        {
            result.bytes.push_back(static_cast<std::uint8_t>(
                (second << 4) | (third >> 2)));
        }
        if (!fourthPadding)
        {
            result.bytes.push_back(static_cast<std::uint8_t>(
                (third << 6) | fourth));
        }
    }
    if (result.bytes.size() != decodedLength ||
        result.bytes.size() > maximumBytes)
    {
        return {result.bytes.size() > maximumBytes
                ? PartStatus::TooLarge
                : PartStatus::Malformed,
            {}};
    }
    return result;
}

DecodeResult DecodeImpl(
    std::wstring_view uri, std::size_t maximumBytes)
{
    constexpr std::wstring_view prefix = L"data:";
    if (uri.size() < prefix.size() ||
        !EqualsIgnoreCase(uri.substr(0, prefix.size()), prefix))
    {
        return {DecodeStatus::NotDataUrl, {}, {}};
    }

    const std::size_t comma = uri.find(L',', prefix.size());
    if (comma == std::wstring_view::npos)
        return {DecodeStatus::Malformed, {}, {}};
    constexpr std::size_t kMaximumHeaderCharacters = 8192;
    if (comma - prefix.size() > kMaximumHeaderCharacters)
        return {DecodeStatus::TooLarge, {}, {}};

    ParsedHeader header = ParseHeader(uri.substr(
        prefix.size(), comma - prefix.size()));
    if (header.status != PartStatus::Decoded)
    {
        return {header.status == PartStatus::TooLarge
                ? DecodeStatus::TooLarge
                : DecodeStatus::Malformed,
            {}, {}};
    }

    DecodedPart payload = header.base64
        ? DecodeBase64(uri.substr(comma + 1), maximumBytes)
        : DecodePercentBytes(uri.substr(comma + 1), maximumBytes);
    if (payload.status != PartStatus::Decoded)
    {
        return {payload.status == PartStatus::TooLarge
                ? DecodeStatus::TooLarge
                : DecodeStatus::Malformed,
            {}, {}};
    }
    if (payload.bytes.size() > maximumBytes)
        return {DecodeStatus::TooLarge, {}, {}};

    DecodeResult result;
    result.status = DecodeStatus::Decoded;
    result.contentType = std::move(header.contentType);
    result.bytes = std::move(payload.bytes);
    return result;
}

} // namespace

DecodeResult Decode(
    std::wstring_view uri, std::size_t maximumBytes) noexcept
{
    try
    {
        return DecodeImpl(uri, maximumBytes);
    }
    catch (const std::bad_alloc&)
    {
        return {DecodeStatus::TooLarge, {}, {}};
    }
    catch (const std::length_error&)
    {
        return {DecodeStatus::TooLarge, {}, {}};
    }
    catch (...)
    {
        return {DecodeStatus::Malformed, {}, {}};
    }
}

} // namespace snowdesktop::drop_data_url
