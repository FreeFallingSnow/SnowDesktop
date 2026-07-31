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

    if (failures == 0)
        std::cout << "HTTP runtime security tests passed\n";
    return failures == 0 ? 0 : 1;
}
