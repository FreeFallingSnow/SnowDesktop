#include "drop_text_rules.h"

#include <cstdlib>
#include <iostream>

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void Expect(snowdesktop::drop_text_rules::Kind expected,
    std::wstring_view input,
    snowdesktop::drop_text_rules::Source source,
    const char* message)
{
    const auto result = snowdesktop::drop_text_rules::Classify(
        input, source);
    if (result.kind == expected)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

} // namespace

int main()
{
    using snowdesktop::drop_text_rules::Kind;
    using snowdesktop::drop_text_rules::Source;

    Expect(Kind::Empty, L" \r\n\t ", Source::UnicodeText,
        "whitespace-only text is empty");
    Expect(Kind::PlainText, L"meeting notes", Source::UnicodeText,
        "ordinary text remains text");
    Expect(Kind::PlainText, L"TODO:fix drag behavior", Source::UnicodeText,
        "a prose label is not mistaken for a URI");
    Expect(Kind::PlainText, L"C:\\Users\\Maya\\photo.png",
        Source::UnicodeText,
        "a Windows drive path is not mistaken for a URI scheme");
    Expect(Kind::OpaqueUri, L"x://resource/id",
        Source::UnicodeText,
        "a one-letter hierarchical scheme is not mistaken for a drive");
    Expect(Kind::PlainText, L"https://example.test/a\ncaption",
        Source::UnicodeText,
        "multiline clipboard text is not collapsed into a URL");

    Expect(Kind::HttpUrl, L" http://127.0.0.1/photo ",
        Source::UnicodeText,
        "HTTP resources are recognized without a public-host policy");
    Expect(Kind::HttpsUrl, L"HTTPS://example.test/photo",
        Source::AdvertisedUri,
        "HTTPS matching is case insensitive");
    Expect(Kind::OpaqueUri, L"https://", Source::AdvertisedUri,
        "a network URI without a host cannot hide a later fallback");
    Expect(Kind::OpaqueUri, L"https://bad host/photo",
        Source::AdvertisedUri,
        "a network URI with whitespace in its host is not actionable");
    Expect(Kind::FtpUrl, L"ftp://example.test/file",
        Source::AdvertisedUri,
        "legacy FTP references retain their existing classification");
    Expect(Kind::FileUrl, L"file:///C:/Users/Maya/photo.png",
        Source::UnicodeText,
        "local file URLs have their own materialization path");
    Expect(Kind::DataUrl, L"data:image/png;base64,iVBORw0KGgo=",
        Source::UnicodeText,
        "inline data URLs are not saved as ordinary text");

    Expect(Kind::OpaqueUri,
        L"native-resource://sdk/image?key=img_v3_example",
        Source::UnicodeText,
        "an app-private hierarchical URI is not saved as ordinary text");
    Expect(Kind::OpaqueUri,
        L"native-resource://sdk/image?key=id\r\nURL=https://evil.test",
        Source::AdvertisedUri,
        "control characters make an advertised URI opaque");
    Expect(Kind::OpaqueUri, L"blob:https://example.test/id",
        Source::AdvertisedUri,
        "a renderer-local blob URI is not sent to the network downloader");
    Expect(Kind::OpaqueUri, L"blob:https://example.test/id",
        Source::UnicodeText,
        "a renderer-local blob URI is not saved as ordinary text");
    Expect(Kind::OpaqueUri, L"mailto:user@example.test",
        Source::AdvertisedUri,
        "an explicitly advertised non-download URI remains opaque");
    Expect(Kind::PlainText, L"mailto:user@example.test",
        Source::UnicodeText,
        "plain text uses conservative URI recognition");

    const auto privateOnly = snowdesktop::drop_text_rules::
        SelectResourceReference(
            L"native-resource://sdk/image?key=id", L"", L"");
    Check(privateOnly.kind == Kind::OpaqueUri,
        "a private URI remains opaque when no standard fallback exists");
    const auto publicFallback = snowdesktop::drop_text_rules::
        SelectResourceReference(
            L"native-resource://sdk/image?key=id", L"",
            L"https://cdn.example.test/image.png");
    Check(publicFallback.kind == Kind::HttpsUrl,
        "a private URI must not hide a standard Unicode URL fallback");
    const auto ansiFallback = snowdesktop::drop_text_rules::
        SelectResourceReference(
            L"native-resource://sdk/image?key=id",
            L"http://127.0.0.1/image.png", L"caption");
    Check(ansiFallback.kind == Kind::HttpUrl,
        "a private wide URI must not hide an ANSI HTTP fallback");
    const auto inlineFallback = snowdesktop::drop_text_rules::
        SelectResourceReference(
            L"native-resource://sdk/image?key=id", L"",
            L"data:image/png;base64,iVBORw0KGgo=");
    Check(inlineFallback.kind == Kind::DataUrl,
        "a private URI must not hide inline standard resource bytes");
    const auto privateWithCaption = snowdesktop::drop_text_rules::
        SelectResourceReference(
            L"native-resource://sdk/image?key=id", L"", L"image");
    Check(privateWithCaption.kind == Kind::OpaqueUri,
        "a caption must not replace an opaque private resource marker");
    using snowdesktop::drop_text_rules::
        IsPrivateHierarchicalResource;
    Check(IsPrivateHierarchicalResource(privateOnly),
        "a private hierarchical resource marker enables content fallback");
    Check(!IsPrivateHierarchicalResource(
            snowdesktop::drop_text_rules::Classify(
                L"blob:https://example.test/id",
                Source::AdvertisedUri)) &&
            !IsPrivateHierarchicalResource(
                snowdesktop::drop_text_rules::Classify(
                    L"mailto:user@example.test",
                    Source::AdvertisedUri)) &&
            !IsPrivateHierarchicalResource(
                snowdesktop::drop_text_rules::Classify(
                    L"https://", Source::AdvertisedUri)),
        "blob, message, and malformed web URIs do not enable private content fallback");
    const auto malformedInlineWithNetworkFallback = snowdesktop::
        drop_text_rules::ClassifyResourceCandidates(
            L"data:image/png;base64,%%%", L"",
            L"https://cdn.example.test/image.png");
    Check(malformedInlineWithNetworkFallback[0].kind == Kind::DataUrl &&
            malformedInlineWithNetworkFallback[2].kind == Kind::HttpsUrl,
        "all standard candidates remain available after one needs fallback");
    const auto malformedNetworkFallback = snowdesktop::drop_text_rules::
        SelectResourceReference(
            L"https://", L"",
            L"https://cdn.example.test/image.png");
    Check(malformedNetworkFallback.kind == Kind::HttpsUrl &&
            malformedNetworkFallback.value ==
                L"https://cdn.example.test/image.png",
        "a malformed advertised network URI does not hide a valid fallback");

    using snowdesktop::drop_text_rules::IsAbsoluteLocalDrivePath;
    Check(IsAbsoluteLocalDrivePath(L"C:\\Users\\Maya\\photo.png"),
        "an absolute drive path passes the local file URL gate");
    Check(!IsAbsoluteLocalDrivePath(L"\\\\server\\share\\photo.png") &&
            !IsAbsoluteLocalDrivePath(L"\\\\?\\C:\\photo.png") &&
            !IsAbsoluteLocalDrivePath(L"photo.png") &&
            !IsAbsoluteLocalDrivePath(L"C:\\photo.png:stream"),
        "UNC, device, relative, and alternate-stream paths are rejected");

    using snowdesktop::drop_text_rules::HierarchicalUriHost;
    Check(HierarchicalUriHost(
            L"https://user:password@example.test:8443?next=/path") ==
            L"example.test" &&
            HierarchicalUriHost(L"https://[::1]:8443/image") == L"::1",
        "shortcut labels exclude credentials, ports, queries, and brackets");

    if (failures != 0)
    {
        std::cerr << failures << " drop text rule test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Drop text rule tests passed\n";
    return EXIT_SUCCESS;
}
