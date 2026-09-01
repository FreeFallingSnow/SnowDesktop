#include "../src/url_drop_resource.h"

#include <iostream>
#include <string>

namespace
{

int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void ExpectDownload(std::wstring_view url, std::wstring_view contentType,
    std::wstring_view expectedName,
    std::wstring_view disposition = {})
{
    const auto decision = snowdesktop::url_drop_resource::Decide(
        url, contentType, disposition);
    Expect(decision.action ==
        snowdesktop::url_drop_resource::Action::Download,
        "response should be downloaded");
    Expect(decision.suggestedFileName == expectedName,
        "download file name should match");
}

} // namespace

int main()
{
    constexpr std::wstring_view douyinUrl =
        L"https://p3-pc-sign.douyinpic.com/obj/tos-cn-i-tsj2vxp0zn/"
        L"9a45bb9c0b8a4963b858846b079cfe2f?lk3s=343af0a2&x-signature=a%2Fb";

    ExpectDownload(douyinUrl, L"image/jpeg",
        L"9a45bb9c0b8a4963b858846b079cfe2f.jpg");
    ExpectDownload(douyinUrl, L" Image/WebP; charset=binary ",
        L"9a45bb9c0b8a4963b858846b079cfe2f.webp");

    auto decision = snowdesktop::url_drop_resource::Decide(
        L"https://example.com/articles/42?format=image",
        L"text/html; charset=utf-8");
    Expect(decision.action ==
        snowdesktop::url_drop_resource::Action::Shortcut,
        "HTML article should remain a shortcut");

    decision = snowdesktop::url_drop_resource::Decide(
        L"https://example.com/error.jpg", L"application/xhtml+xml");
    Expect(decision.action ==
        snowdesktop::url_drop_resource::Action::Shortcut,
        "HTML response should override a misleading image suffix");

    ExpectDownload(
        L"https://example.com/assets/photo.png?token=abc#ignored",
        L"image/png", L"photo.png");
    ExpectDownload(L"https://example.com/", L"image/jpeg",
        L"download.jpg");
    ExpectDownload(L"https://example.com/object",
        L"application/octet-stream", L"object.bin");

    decision = snowdesktop::url_drop_resource::Decide(
        L"https://example.com/object", L"application/x-unknown");
    Expect(decision.action ==
        snowdesktop::url_drop_resource::Action::Shortcut,
        "unknown extensionless response should remain a shortcut");

    ExpectDownload(L"https://cdn.example.com/fallback",
        L"image/webp", L"redirected.webp",
        L"inline; filename=\"redirected\"");
    ExpectDownload(L"https://example.com/file",
        L"application/pdf", L"报告.pdf",
        L"attachment; filename=old.pdf; filename*=UTF-8''%E6%8A%A5%E5%91%8A.pdf");
    ExpectDownload(L"https://example.com/%2E%2E%2FCON",
        L"image/webp", L"_CON.webp");
    ExpectDownload(L"https://example.com/photo.php",
        L"image/webp", L"photo.php.webp");
    ExpectDownload(L"https://example.com/archive.zip",
        L"", L"archive.zip");

    if (failures != 0)
    {
        std::cerr << failures << " url drop resource test(s) failed\n";
        return 1;
    }
    std::cout << "url drop resource tests passed\n";
    return 0;
}
