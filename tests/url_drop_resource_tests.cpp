#include "../src/url_drop_resource.h"
#include "../src/url_drop_download_worker.h"

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

void ExpectShortcut(std::wstring_view url, std::wstring_view contentType,
    std::wstring_view disposition = {})
{
    const auto decision = snowdesktop::url_drop_resource::Decide(
        url, contentType, disposition);
    Expect(decision.action ==
        snowdesktop::url_drop_resource::Action::Shortcut,
        "response should remain a shortcut");
    Expect(decision.suggestedFileName.empty(),
        "shortcut should not expose a staged file name");
}

} // namespace

int main()
{
    snowdesktop::UrlDropDownloadResult retryableFailure;
    retryableFailure.outcome =
        snowdesktop::UrlDropDownloadOutcome::Failed;
    retryableFailure.retryableCandidateFailure = true;
    snowdesktop::UrlDropDownloadResult localFailure;
    localFailure.outcome =
        snowdesktop::UrlDropDownloadOutcome::Failed;
    snowdesktop::UrlDropDownloadResult normalShortcut;
    normalShortcut.outcome =
        snowdesktop::UrlDropDownloadOutcome::Shortcut;
    normalShortcut.retryableCandidateFailure = true;
    Expect(retryableFailure.CanRetryAlternateUrl() &&
            !localFailure.CanRetryAlternateUrl() &&
            !normalShortcut.CanRetryAlternateUrl(),
        "only candidate-specific transport failures try another URL field");

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

    ExpectShortcut(L"https://example.com/page.html",
        L"application/octet-stream");
    ExpectDownload(L"https://example.com/page.html",
        L"application/octet-stream", L"page.html.bin",
        L"attachment; filename=page.html");
    ExpectShortcut(L"https://example.com/setup.exe",
        L"application/octet-stream");
    ExpectShortcut(L"https://example.com/object",
        L"application/x-msdownload",
        L"attachment; filename=payload.LNK");
    ExpectDownload(L"https://example.com/object",
        L"image/webp", L"photongp.exe.webp",
        L"attachment; filename*=UTF-8''photo%E2%80%AEngp.exe");
    ExpectShortcut(L"https://example.com/object",
        L"application/x-unknown",
        L"attachment; filename=payload.vBs");
    ExpectShortcut(L"https://example.com/payload%2ELNK%20",
        L"application/x-unknown");
    ExpectShortcut(L"https://example.com/object",
        L"application/x-unknown",
        L"attachment; filename*=UTF-8''payload.%E2%80%AELNK.");
    ExpectShortcut(L"https://example.com/payload.jpg.LNK",
        L"application/octet-stream");
    ExpectDownload(L"https://example.com/payload.LNK.jpg",
        L"application/octet-stream", L"payload.LNK.jpg");
    ExpectDownload(L"https://example.com/object",
        L"image/jpeg", L"payload.LNK.jpg",
        L"attachment; filename=payload.LNK");

    decision = snowdesktop::url_drop_resource::Decide(
        L"https://example.com/object", L"application/x-unknown");
    Expect(decision.action ==
        snowdesktop::url_drop_resource::Action::Shortcut,
        "unknown extensionless response should remain a shortcut");

    ExpectDownload(L"https://example.com/download",
        L"application/x-unknown", L"page.htm.bin",
        L"attachment; filename=page.htm");
    ExpectShortcut(L"https://example.com/page.mHTML",
        L"application/x-unknown");
    ExpectDownload(L"https://example.com/object",
        L"application/x-unknown", L"page.HTML.bin",
        L"attachment; filename=payload.LNK; "
        L"filename*=UTF-8''page.HTML");
    ExpectShortcut(L"https://example.com/object",
        L"text/html", L"attachment; filename=safe.bin");

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
