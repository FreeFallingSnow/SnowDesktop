#include "drop_text_rules.h"

#include <algorithm>
#include <cstddef>
#include <cwctype>

namespace snowdesktop::drop_text_rules
{
namespace
{

bool IsSchemeFirst(wchar_t character)
{
    return (character >= L'a' && character <= L'z') ||
        (character >= L'A' && character <= L'Z');
}

bool IsSchemeRest(wchar_t character)
{
    return IsSchemeFirst(character) ||
        (character >= L'0' && character <= L'9') ||
        character == L'+' || character == L'-' || character == L'.';
}

std::wstring Lower(std::wstring_view value)
{
    std::wstring result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(towlower(character));
        });
    return result;
}

} // namespace

Classification Classify(std::wstring_view input, Source source)
{
    size_t first = 0;
    while (first < input.size() && iswspace(input[first]))
        ++first;
    size_t last = input.size();
    while (last > first && iswspace(input[last - 1]))
        --last;

    Classification result;
    result.value.assign(input.substr(first, last - first));
    if (result.value.empty())
        return result;

    // A multiline Unicode payload is user text, even if its first line starts
    // with something URI-like. URI clipboard formats remain authoritative.
    if (source == Source::UnicodeText &&
        result.value.find_first_of(L"\r\n") != std::wstring::npos)
    {
        result.kind = Kind::PlainText;
        return result;
    }

    const size_t colon = result.value.find(L':');
    if (colon == std::wstring::npos || colon == 0 ||
        !IsSchemeFirst(result.value.front()) ||
        !std::all_of(result.value.begin() + 1,
            result.value.begin() + static_cast<std::ptrdiff_t>(colon),
            IsSchemeRest))
    {
        result.kind = Kind::PlainText;
        return result;
    }

    // Do not mistake an ordinary Windows drive path for a one-letter URI.
    if (colon == 1 && result.value.size() > 2 &&
        (result.value[2] == L'\\' ||
            (result.value[2] == L'/' &&
                (result.value.size() == 3 ||
                    result.value[3] != L'/'))))
    {
        result.kind = Kind::PlainText;
        return result;
    }

    const std::wstring scheme = Lower(
        std::wstring_view(result.value).substr(0, colon));
    const bool hierarchical = result.value.size() >= colon + 3 &&
        result.value[colon + 1] == L'/' &&
        result.value[colon + 2] == L'/';
    const bool explicitUri = source == Source::AdvertisedUri ||
        hierarchical || scheme == L"blob" || scheme == L"data" ||
        scheme == L"file";
    if (!explicitUri)
    {
        result.kind = Kind::PlainText;
        return result;
    }

    if (std::any_of(result.value.begin(), result.value.end(),
            [](wchar_t character) {
                return character < L' ' || character == L'\x7f';
            }))
    {
        result.kind = Kind::OpaqueUri;
        return result;
    }

    const std::wstring networkHost = hierarchical
        ? HierarchicalUriHost(result.value) : std::wstring{};
    const bool usableNetworkHost = !networkHost.empty() &&
        std::none_of(networkHost.begin(), networkHost.end(),
            [](wchar_t character) {
                return iswspace(character) || character < L' ' ||
                    character == L'\x7f';
            });
    if (scheme == L"http" && hierarchical && usableNetworkHost)
        result.kind = Kind::HttpUrl;
    else if (scheme == L"https" && hierarchical && usableNetworkHost)
        result.kind = Kind::HttpsUrl;
    else if (scheme == L"ftp" && hierarchical && usableNetworkHost)
        result.kind = Kind::FtpUrl;
    else if (scheme == L"file")
        result.kind = Kind::FileUrl;
    else if (scheme == L"data")
        result.kind = Kind::DataUrl;
    else
        result.kind = Kind::OpaqueUri;
    return result;
}

ResourceCandidates ClassifyResourceCandidates(
    std::wstring_view advertisedWide,
    std::wstring_view advertisedAnsi,
    std::wstring_view unicodeText)
{
    return {
        Classify(advertisedWide, Source::AdvertisedUri),
        Classify(advertisedAnsi, Source::AdvertisedUri),
        Classify(unicodeText, Source::UnicodeText),
    };
}

Classification SelectResourceReference(
    std::wstring_view advertisedWide,
    std::wstring_view advertisedAnsi,
    std::wstring_view unicodeText)
{
    const auto candidates = ClassifyResourceCandidates(
        advertisedWide, advertisedAnsi, unicodeText);
    return SelectResourceReference(candidates);
}

Classification SelectResourceReference(
    const ResourceCandidates& candidates)
{
    auto actionable = [](Kind kind) {
        return kind == Kind::HttpUrl || kind == Kind::HttpsUrl ||
            kind == Kind::FtpUrl || kind == Kind::FileUrl ||
            kind == Kind::DataUrl;
    };
    for (const auto& candidate : candidates)
    {
        if (actionable(candidate.kind))
            return candidate;
    }
    for (const auto& candidate : candidates)
    {
        if (candidate.kind != Kind::Empty)
            return candidate;
    }
    return {};
}

bool IsPrivateHierarchicalResource(
    const Classification& classification)
{
    if (classification.kind != Kind::OpaqueUri)
        return false;

    const size_t colon = classification.value.find(L':');
    if (colon == std::wstring::npos || colon == 0 ||
        classification.value.size() < colon + 3 ||
        classification.value[colon + 1] != L'/' ||
        classification.value[colon + 2] != L'/')
        return false;

    const std::wstring scheme = Lower(
        std::wstring_view(classification.value).substr(0, colon));
    if (scheme == L"http" || scheme == L"https" ||
        scheme == L"ftp" || scheme == L"file" ||
        scheme == L"data" || scheme == L"blob" ||
        scheme == L"mailto")
        return false;

    const std::wstring host = HierarchicalUriHost(
        classification.value);
    return !host.empty() &&
        std::none_of(host.begin(), host.end(),
            [](wchar_t character) {
                return iswspace(character) || character < L' ' ||
                    character == L'\x7f';
            }) &&
        std::none_of(classification.value.begin(),
            classification.value.end(),
            [](wchar_t character) {
                return character < L' ' || character == L'\x7f';
            });
}

bool IsAbsoluteLocalDrivePath(std::wstring_view path)
{
    return path.size() >= 3 && IsSchemeFirst(path[0]) &&
        path[1] == L':' && (path[2] == L'\\' || path[2] == L'/') &&
        path.find(L':', 2) == std::wstring_view::npos;
}

std::wstring HierarchicalUriHost(std::wstring_view uri)
{
    const size_t colon = uri.find(L':');
    if (colon == std::wstring_view::npos || colon + 2 >= uri.size() ||
        uri[colon + 1] != L'/' || uri[colon + 2] != L'/')
        return {};
    const size_t authorityStart = colon + 3;
    const size_t authorityEnd = uri.find_first_of(
        L"/?#", authorityStart);
    std::wstring_view authority = uri.substr(authorityStart,
        authorityEnd == std::wstring_view::npos
            ? std::wstring_view::npos
            : authorityEnd - authorityStart);
    const size_t userInfo = authority.rfind(L'@');
    if (userInfo != std::wstring_view::npos)
        authority.remove_prefix(userInfo + 1);
    if (authority.empty())
        return {};
    if (authority.front() == L'[')
    {
        const size_t close = authority.find(L']');
        return close == std::wstring_view::npos
            ? std::wstring{}
            : std::wstring(authority.substr(1, close - 1));
    }
    const size_t port = authority.rfind(L':');
    if (port != std::wstring_view::npos)
        authority = authority.substr(0, port);
    return std::wstring(authority);
}

} // namespace snowdesktop::drop_text_rules
