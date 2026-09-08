#include "drop_data_url.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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

void ExpectStatus(snowdesktop::drop_data_url::DecodeStatus expected,
    std::wstring_view uri,
    std::size_t maximumBytes,
    const char* message)
{
    const auto result = snowdesktop::drop_data_url::Decode(
        uri, maximumBytes);
    Check(result.status == expected, message);
}

} // namespace

int main()
{
    using snowdesktop::drop_data_url::Decode;
    using snowdesktop::drop_data_url::DecodeStatus;

    const auto png = Decode(
        L"data:image/png;base64,iVBORw0KGgo=", 8);
    constexpr std::array<std::uint8_t, 8> pngSignature{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    Check(png.status == DecodeStatus::Decoded,
        "a base64 PNG fragment decodes");
    Check(png.contentType == "image/png",
        "the declared PNG content type is retained");
    Check(png.bytes.size() == pngSignature.size() &&
            std::equal(png.bytes.begin(), png.bytes.end(),
                pngSignature.begin()),
        "the PNG signature bytes are exact");

    const auto text = Decode(
        L"data:text/plain;charset=UTF-8,Hello%2C%20Snow%21", 12);
    constexpr std::string_view expectedText = "Hello, Snow!";
    Check(text.status == DecodeStatus::Decoded,
        "percent-encoded text decodes");
    Check(text.contentType == "text/plain;charset=UTF-8",
        "media type parameters are retained");
    Check(text.bytes.size() == expectedText.size() &&
            std::equal(text.bytes.begin(), text.bytes.end(),
                expectedText.begin()),
        "percent escapes are decoded as individual bytes");

    const auto defaults = Decode(L"data:,plain", 5);
    Check(defaults.status == DecodeStatus::Decoded,
        "an omitted media type is valid");
    Check(defaults.contentType == "text/plain;charset=US-ASCII",
        "the RFC 2397 default media type is reported");

    const auto shorthand = Decode(L"data:;base64,QQ==", 1);
    Check(shorthand.status == DecodeStatus::Decoded &&
            shorthand.contentType ==
                "text/plain;charset=US-ASCII" &&
            shorthand.bytes.size() == 1 &&
            shorthand.bytes.front() == 'A',
        "the base64 shorthand uses the default media type");

    ExpectStatus(DecodeStatus::TooLarge,
        L"data:image/png;base64,iVBORw0KGgo=", 7,
        "the decoded size limit is enforced for base64 data");
    ExpectStatus(DecodeStatus::TooLarge,
        L"data:,1234", 3,
        "the decoded size limit is enforced before percent output allocation");
    std::wstring oversizedHeader = L"data:";
    oversizedHeader.append(8193, L'a');
    oversizedHeader.push_back(L',');
    ExpectStatus(DecodeStatus::TooLarge,
        oversizedHeader, 64,
        "metadata length is bounded independently from payload size");

    ExpectStatus(DecodeStatus::Malformed,
        L"data:;base64,A===", 64,
        "excess base64 padding is rejected");
    ExpectStatus(DecodeStatus::Malformed,
        L"data:;base64,AB==", 64,
        "non-canonical base64 pad bits are rejected");
    ExpectStatus(DecodeStatus::Malformed,
        L"data:;base64,QQ=Q", 64,
        "base64 padding is only accepted at the end");
    ExpectStatus(DecodeStatus::Malformed,
        L"data:,bad%2", 64,
        "a truncated percent escape is rejected");
    ExpectStatus(DecodeStatus::Malformed,
        L"data:,bad%XZ", 64,
        "a non-hex percent escape is rejected");
    ExpectStatus(DecodeStatus::Malformed,
        L"data:,line\nbreak", 64,
        "an unescaped control character is rejected");
    ExpectStatus(DecodeStatus::Malformed,
        L"data:,caf\x00e9", 64,
        "an unescaped non-ASCII character is rejected");
    ExpectStatus(DecodeStatus::NotDataUrl,
        L"native-resource://sdk/image", 64,
        "an unrelated private URI is not treated as malformed data");

    if (failures != 0)
    {
        std::cerr << failures << " data URL test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Data URL tests passed\n";
    return EXIT_SUCCESS;
}
