#include "http_runtime.h"

#include <iostream>

namespace
{
int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}
}

int main()
{
    using snowdesktop::http_security::IsAllowedRemoteIpLiteral;
    using snowdesktop::http_security::IsAllowedPublicHttpsUrl;
    using snowdesktop::http_security::IsAllowedUrlForDomains;

    Expect(IsAllowedRemoteIpLiteral(L"8.8.8.8"),
        "a public IPv4 address is accepted");
    Expect(IsAllowedRemoteIpLiteral(L"2606:4700:4700::1111"),
        "a public IPv6 address is accepted");
    Expect(IsAllowedRemoteIpLiteral(L"198.18.0.1"),
        "the proxy and VPN Fake-IP range is accepted");

    for (const auto* address : {
        L"0.0.0.0", L"10.0.0.1", L"100.64.0.1", L"127.0.0.1",
        L"169.254.1.1", L"172.16.0.1", L"192.0.2.1",
        L"192.168.0.1", L"198.51.100.1",
        L"203.0.113.1", L"224.0.0.1" })
    {
        Expect(!IsAllowedRemoteIpLiteral(address),
            "non-public IPv4 ranges are rejected");
    }

    for (const auto* address : {
        L"::", L"::1", L"::ffff:127.0.0.1", L"fc00::1",
        L"fe80::1", L"ff02::1", L"2001:db8::1" })
    {
        Expect(!IsAllowedRemoteIpLiteral(address),
            "non-public IPv6 ranges are rejected");
    }

    Expect(!IsAllowedRemoteIpLiteral(L"not-an-address"),
        "invalid IP literals are rejected");

    struct UrlPolicyCase
    {
        const wchar_t* url;
        std::vector<std::string> domains;
        bool expected;
        const char* message;
    };
    const std::vector<UrlPolicyCase> urlCases{
        { L"https://example.com/feed", {"example.com"}, true,
            "an exact HTTPS allowlist match is accepted" },
        { L"https://EXAMPLE.COM:8443/feed?q=1", {"Example.Com"}, true,
            "host matching is case-insensitive and independent of port" },
        { L"https://example.com./feed", {"example.com."}, true,
            "equivalent trailing-dot hostnames are normalized" },
        { L"https://xn--bcher-kva.example/", {"xn--bcher-kva.example"}, true,
            "IDNs use an explicit ASCII punycode allowlist" },
        { L"https://8.8.8.8/", {"8.8.8.8"}, true,
            "an explicitly allowed public IPv4 literal is accepted" },
        { L"https://[2606:4700:4700::1111]/",
            {"2606:4700:4700::1111"}, true,
            "an explicitly allowed public IPv6 literal is accepted" },
        { L"http://example.com/", {"example.com"}, false,
            "plaintext HTTP is rejected" },
        { L"https://sub.example.com/", {"example.com"}, false,
            "subdomains require their own exact allowlist entry" },
        { L"https://example.com.evil.test/", {"example.com"}, false,
            "suffix-confusion hosts are rejected" },
        { L"https://example.com/", {"*.example.com"}, false,
            "wildcard allowlist entries are rejected" },
        { L"https://example.com/", {"https://example.com"}, false,
            "allowlist entries cannot contain URL syntax" },
        { L"https://localhost/", {"localhost"}, false,
            "localhost is rejected even when declared" },
        { L"https://service.local/", {"service.local"}, false,
            "local DNS suffixes are rejected" },
        { L"https://127.0.0.1/", {"127.0.0.1"}, false,
            "loopback IPv4 is rejected even when declared" },
        { L"https://[fc00::1]/", {"fc00::1"}, false,
            "private IPv6 is rejected even when declared" },
        { L"https://example.com@evil.test/", {"example.com"}, false,
            "userinfo cannot disguise a different target host" },
        { L"not a URL", {"example.com"}, false,
            "malformed URLs are rejected" },
        { L"https://example.com/", {"例子.测试"}, false,
            "non-ASCII allowlist entries must be supplied as punycode" },
    };
    for (const auto& test : urlCases)
    {
        Expect(IsAllowedUrlForDomains(
                test.url, test.domains) == test.expected,
            test.message);
    }

    Expect(IsAllowedUrlForDomains(
            L"https://hnrss.org/frontpage", {}, true),
        "widget HTTP mode accepts an arbitrary HTTPS domain");
    Expect(IsAllowedUrlForDomains(
            L"https://feeds.example.net/rss", {"unrelated.example"}, true),
        "widget HTTP mode bypasses the declared domain allowlist");
    Expect(IsAllowedUrlForDomains(
            L"http://hnrss.org/frontpage", {}, true),
        "widget HTTP mode accepts plaintext HTTP");
    Expect(IsAllowedUrlForDomains(
            L"https://localhost/feed", {}, true),
        "widget HTTP mode accepts localhost");
    Expect(IsAllowedUrlForDomains(
            L"https://192.168.1.10/feed", {}, true),
        "widget HTTP mode accepts private IPv4 targets");
    Expect(IsAllowedUrlForDomains(
            L"https://[fc00::1]/feed", {}, true),
        "widget HTTP mode accepts private IPv6 targets");
    Expect(IsAllowedUrlForDomains(
            L"http://nas.local/feed", {}, true),
        "widget HTTP mode accepts local HTTP hosts");
    Expect(!IsAllowedUrlForDomains(
            L"ftp://example.com/feed", {}, true),
        "widget HTTP mode rejects non-HTTP URL schemes");
    Expect(!IsAllowedUrlForDomains(
            L"not a URL", {}, true),
        "widget HTTP mode rejects malformed URLs");

    Expect(IsAllowedPublicHttpsUrl(L"https://example.com/article?id=1"),
        "shell HTTPS policy accepts a public URL");
    Expect(!IsAllowedPublicHttpsUrl(L"http://example.com/article"),
        "shell HTTPS policy rejects plaintext HTTP");
    Expect(!IsAllowedPublicHttpsUrl(L"https://user@example.com/article"),
        "shell HTTPS policy rejects embedded credentials");
    Expect(!IsAllowedPublicHttpsUrl(L"https://localhost/article") &&
            !IsAllowedPublicHttpsUrl(L"https://192.168.1.2/article"),
        "shell HTTPS policy rejects local targets");

    if (failures == 0)
        std::cout << "HTTP security tests passed\n";
    return failures == 0 ? 0 : 1;
}
