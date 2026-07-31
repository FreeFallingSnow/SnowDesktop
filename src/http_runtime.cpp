#include "http_runtime.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace
{
constexpr DWORD kMaxResponseBytes = 1024 * 1024;

bool IsBlockedIpv4(const IN_ADDR& address)
{
    const std::uint32_t value = ntohl(address.S_un.S_addr);
    const std::uint8_t first = static_cast<std::uint8_t>(value >> 24);
    const std::uint8_t second = static_cast<std::uint8_t>(value >> 16);
    if (first == 0 || first == 10 || first == 127 || first >= 224)
        return true;
    if (first == 100 && second >= 64 && second <= 127) return true;
    if (first == 169 && second == 254) return true;
    if (first == 172 && second >= 16 && second <= 31) return true;
    if (first == 192 && second == 0 &&
        static_cast<std::uint8_t>(value >> 8) == 0)
        return true;
    if (first == 192 && second == 0 &&
        static_cast<std::uint8_t>(value >> 8) == 2)
        return true;
    if (first == 192 && second == 168) return true;
    // 198.18.0.0/15 is widely used by local proxy/VPN Fake-IP modes.
    // The request remains authenticated against the original HTTPS hostname.
    if (first == 198 && second == 51 &&
        static_cast<std::uint8_t>(value >> 8) == 100)
        return true;
    if (first == 203 && second == 0 &&
        static_cast<std::uint8_t>(value >> 8) == 113)
        return true;
    return false;
}

bool IsBlockedIpv6(const IN6_ADDR& address)
{
    if (IN6_IS_ADDR_UNSPECIFIED(&address) ||
        IN6_IS_ADDR_LOOPBACK(&address) ||
        IN6_IS_ADDR_LINKLOCAL(&address) ||
        IN6_IS_ADDR_SITELOCAL(&address) ||
        IN6_IS_ADDR_MULTICAST(&address))
        return true;
    const auto* bytes = address.u.Byte;
    if (IN6_IS_ADDR_V4MAPPED(&address))
    {
        IN_ADDR mapped{};
        std::memcpy(&mapped, bytes + 12, sizeof(mapped));
        return IsBlockedIpv4(mapped);
    }
    if (bytes[0] == 0x20 && bytes[1] == 0x01 &&
        ((bytes[2] == 0x0d && bytes[3] == 0xb8) ||
         (bytes[2] == 0x00 && bytes[3] == 0x02)))
        return true;
    // Public IPv6 unicast addresses are currently allocated from 2000::/3.
    return (bytes[0] & 0xe0) != 0x20;
}

bool IsAllowedRemoteSockaddr(const SOCKADDR* address)
{
    if (!address) return false;
    if (address->sa_family == AF_INET)
        return !IsBlockedIpv4(
            reinterpret_cast<const SOCKADDR_IN*>(address)->sin_addr);
    if (address->sa_family == AF_INET6)
        return !IsBlockedIpv6(
            reinterpret_cast<const SOCKADDR_IN6*>(address)->sin6_addr);
    return false;
}

bool SockaddrToIpLiteral(const SOCKADDR* address, std::wstring& output)
{
    wchar_t buffer[INET6_ADDRSTRLEN]{};
    if (address->sa_family == AF_INET)
    {
        const auto* ipv4 = reinterpret_cast<const SOCKADDR_IN*>(address);
        if (!InetNtopW(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr),
                buffer, static_cast<DWORD>(std::size(buffer))))
            return false;
    }
    else if (address->sa_family == AF_INET6)
    {
        const auto* ipv6 = reinterpret_cast<const SOCKADDR_IN6*>(address);
        if (!InetNtopW(AF_INET6, const_cast<IN6_ADDR*>(&ipv6->sin6_addr),
                buffer, static_cast<DWORD>(std::size(buffer))))
            return false;
    }
    else
        return false;
    output = buffer;
    return true;
}

bool EnsureWinsock()
{
    static std::once_flag winsockOnce;
    static bool winsockReady = false;
    std::call_once(winsockOnce, []
    {
        WSADATA data{};
        winsockReady = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return winsockReady;
}

bool ResolvePinnedPublicAddress(
    const std::wstring& host, std::wstring& pinnedAddress)
{
    if (!EnsureWinsock()) return false;
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    PADDRINFOW addresses = nullptr;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &addresses) != 0 ||
        !addresses)
        return false;
    bool foundPublic = false;
    bool foundNonPublic = false;
    for (auto* current = addresses; current; current = current->ai_next)
    {
        if (!IsAllowedRemoteSockaddr(current->ai_addr))
        {
            foundNonPublic = true;
            continue;
        }
        foundPublic = true;
        if (pinnedAddress.empty())
        {
            if (!SockaddrToIpLiteral(current->ai_addr, pinnedAddress))
                foundNonPublic = true;
        }
    }
    FreeAddrInfoW(addresses);
    return foundPublic && !foundNonPublic && !pinnedAddress.empty();
}

std::string WideToUtf8Http(const std::wstring& value)
{
    if (value.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
}

bool snowdesktop::http_security::IsAllowedRemoteIpLiteral(
    std::wstring_view address)
{
    if (!EnsureWinsock()) return false;
    const std::wstring value(address);
    IN_ADDR ipv4{};
    if (InetPtonW(AF_INET, value.c_str(), &ipv4) == 1)
        return !IsBlockedIpv4(ipv4);
    IN6_ADDR ipv6{};
    if (InetPtonW(AF_INET6, value.c_str(), &ipv6) == 1)
        return !IsBlockedIpv6(ipv6);
    return false;
}

AsyncHttpService::~AsyncHttpService()
{
    Stop();
}

bool AsyncHttpService::IsDomainAllowed(const std::wstring& url,
    const std::vector<std::string>& domains)
{
    URL_COMPONENTS components{ sizeof(components) };
    wchar_t host[256]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) return false;
    if (components.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    std::wstring actual = Lower(std::wstring(host, components.dwHostNameLength));
    if (actual == L"localhost" || actual == L"::1" ||
        actual.ends_with(L".localhost") || actual.ends_with(L".local") ||
        actual.starts_with(L"127.") || actual.starts_with(L"10.") ||
        actual.starts_with(L"192.168.") || actual.starts_with(L"169.254.") ||
        actual.starts_with(L"0."))
        return false;
    if (actual.starts_with(L"172."))
    {
        const size_t nextDot = actual.find(L'.', 4);
        if (nextDot != std::wstring::npos)
        {
            const int secondOctet = _wtoi(
                actual.substr(4, nextDot - 4).c_str());
            if (secondOctet >= 16 && secondOctet <= 31) return false;
        }
    }
    for (const auto& raw : domains)
    {
        std::wstring allowed(raw.begin(), raw.end());
        allowed = Lower(allowed);
        if (allowed == actual) return true;
    }
    return false;
}

int AsyncHttpService::Submit(HttpRequestOptions options)
{
    if (!IsDomainAllowed(options.url, options.allowedDomains)) return 0;
    std::scoped_lock lock(mutex_);
    int activeForWidget = 0;
    for (const auto& [id, request] : requests_)
        if (request->widgetId == options.widgetId) ++activeForWidget;
    if (activeForWidget >= 4) return 0;

    for (auto it = cache_.begin(); it != cache_.end();)
    {
        if (std::chrono::steady_clock::now() >= it->second.expires)
            it = cache_.erase(it);
        else
            ++it;
    }
    const bool cacheable = options.cacheSeconds > 0 && Lower(options.method) == L"get";
    const std::wstring cacheKey = options.widgetId + L"\n" + options.method + L"\n" +
        options.url + L"\n" + options.headers;
    auto cached = cache_.find(cacheKey);
    if (cacheable && cached != cache_.end() &&
        std::chrono::steady_clock::now() < cached->second.expires)
    {
        HttpResponse response = cached->second.response;
        response.id = nextId_.fetch_add(1);
        response.widgetId = options.widgetId;
        response.fromCache = true;
        completed_.push_back(std::move(response));
        return completed_.back().id;
    }

    int id = nextId_.fetch_add(1);
    auto state = std::make_unique<RequestState>();
    state->widgetId = options.widgetId;
    state->worker = std::jthread([this, id, options = std::move(options), cacheKey, cacheable](std::stop_token token) {
        HttpResponse response = Execute(id, options, token);
        if (cacheable && response.error.empty() && response.status >= 200 && response.status < 300)
        {
            std::scoped_lock cacheLock(mutex_);
            cache_[cacheKey] = { response,
                std::chrono::steady_clock::now() + std::chrono::seconds(options.cacheSeconds) };
        }
        Complete(std::move(response));
    });
    requests_[id] = std::move(state);
    return id;
}

bool AsyncHttpService::Cancel(const std::wstring& widgetId, int requestId)
{
    std::scoped_lock lock(mutex_);
    auto it = requests_.find(requestId);
    if (it == requests_.end() || it->second->widgetId != widgetId) return false;
    it->second->worker.request_stop();
    return true;
}

void AsyncHttpService::CancelWidget(const std::wstring& widgetId)
{
    std::scoped_lock lock(mutex_);
    for (auto& [id, request] : requests_)
        if (request->widgetId == widgetId) request->worker.request_stop();
    const std::wstring prefix = widgetId + L"\n";
    for (auto it = cache_.begin(); it != cache_.end();)
    {
        if (it->first.starts_with(prefix))
            it = cache_.erase(it);
        else
            ++it;
    }
}

void AsyncHttpService::Stop()
{
    std::unordered_map<int, std::unique_ptr<RequestState>> requests;
    {
        std::scoped_lock lock(mutex_);
        requests.swap(requests_);
    }
    for (auto& [id, request] : requests)
        request->worker.request_stop();
}

void AsyncHttpService::Complete(HttpResponse response)
{
    std::scoped_lock lock(mutex_);
    completed_.push_back(std::move(response));
}

std::vector<HttpResponse> AsyncHttpService::Drain()
{
    std::vector<HttpResponse> result;
    std::vector<std::unique_ptr<RequestState>> finished;
    {
        std::scoped_lock lock(mutex_);
        while (!completed_.empty())
        {
            int id = completed_.front().id;
            result.push_back(std::move(completed_.front()));
            completed_.pop_front();
            auto it = requests_.find(id);
            if (it != requests_.end())
            {
                finished.push_back(std::move(it->second));
                requests_.erase(it);
            }
        }
    }
    return result;
}

HttpResponse AsyncHttpService::Execute(int id, const HttpRequestOptions& options,
    std::stop_token token)
{
    HttpResponse response;
    response.id = id;
    response.widgetId = options.widgetId;

    HINTERNET session = WinHttpOpen(L"SnowDesktop/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { response.error = "WinHttpOpen failed"; return response; }
    WinHttpSetTimeouts(session, options.timeoutMs, options.timeoutMs,
        options.timeoutMs, options.timeoutMs);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
        &redirectPolicy, sizeof(redirectPolicy));

    std::wstring currentUrl = options.url;
    for (int redirectCount = 0; redirectCount <= 3 && !token.stop_requested(); ++redirectCount)
    {
        if (!IsDomainAllowed(currentUrl, options.allowedDomains))
        {
            response.error = "Redirect domain is not allowed";
            break;
        }
        URL_COMPONENTS components{ sizeof(components) };
        wchar_t host[256]{};
        wchar_t path[2048]{};
        wchar_t extra[2048]{};
        components.lpszHostName = host;
        components.dwHostNameLength = static_cast<DWORD>(std::size(host));
        components.lpszUrlPath = path;
        components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        components.lpszExtraInfo = extra;
        components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
        if (!WinHttpCrackUrl(currentUrl.c_str(), 0, 0, &components))
        {
            response.error = "Invalid URL";
            break;
        }
        const std::wstring currentHost(host, components.dwHostNameLength);
        std::wstring pinnedAddress;
        if (!ResolvePinnedPublicAddress(currentHost, pinnedAddress))
        {
            response.error =
                "Host resolves to a private, local, or unavailable address";
            break;
        }
        HINTERNET connection = WinHttpConnect(session,
            currentHost.c_str(),
            components.nPort, 0);
        if (!connection)
        {
            response.error = "WinHttpConnect failed";
            break;
        }
        const std::wstring requestPath = std::wstring(path, components.dwUrlPathLength) +
            std::wstring(extra, components.dwExtraInfoLength);
        DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connection, options.method.c_str(),
            requestPath.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            response.error = "WinHttpOpenRequest failed";
            break;
        }
        const DWORD pinnedAddressBytes = static_cast<DWORD>(
            (pinnedAddress.size() + 1) * sizeof(wchar_t));
        if (!WinHttpSetOption(request, WINHTTP_OPTION_RESOLUTION_HOSTNAME,
                pinnedAddress.data(), pinnedAddressBytes))
        {
            response.error =
                "This Windows version cannot securely pin the resolved host";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }
        DWORD disabledFeatures =
            WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                &disabledFeatures, sizeof(disabledFeatures)))
        {
            response.error = "Cannot apply HTTP request security policy";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }
        BOOL sent = WinHttpSendRequest(request,
            options.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : options.headers.c_str(),
            options.headers.empty() ? 0 : static_cast<DWORD>(-1L),
            options.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(options.body.data()),
            static_cast<DWORD>(options.body.size()), static_cast<DWORD>(options.body.size()), 0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr))
        {
            response.error = "HTTP request failed";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }
        WINHTTP_CONNECTION_INFO connectionInfo{};
        connectionInfo.cbSize = sizeof(connectionInfo);
        DWORD connectionInfoSize = sizeof(connectionInfo);
        if (!WinHttpQueryOption(request, WINHTTP_OPTION_CONNECTION_INFO,
                &connectionInfo, &connectionInfoSize) ||
            !IsAllowedRemoteSockaddr(reinterpret_cast<const SOCKADDR*>(
                &connectionInfo.RemoteAddress)))
        {
            response.error = "HTTP connection reached a non-public address";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);

        if (status >= 300 && status < 400)
        {
            if (redirectCount == 3)
            {
                response.error = "Too many redirects";
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                break;
            }
            DWORD locationSize = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &locationSize, WINHTTP_NO_HEADER_INDEX);
            std::wstring location(locationSize / sizeof(wchar_t), L'\0');
            if (locationSize == 0 || !WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX, location.data(), &locationSize, WINHTTP_NO_HEADER_INDEX))
            {
                response.error = "Redirect is missing Location";
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                break;
            }
            location.resize(wcslen(location.c_str()));
            wchar_t combined[4096]{};
            DWORD combinedLength = static_cast<DWORD>(std::size(combined));
            if (FAILED(UrlCombineW(currentUrl.c_str(), location.c_str(),
                combined, &combinedLength, URL_ESCAPE_UNSAFE)))
            {
                response.error = "Invalid redirect URL";
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                break;
            }
            currentUrl = combined;
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            continue;
        }

        while (!token.stop_requested() && response.body.size() <= kMaxResponseBytes)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            DWORD remaining = kMaxResponseBytes + 1 - static_cast<DWORD>(response.body.size());
            available = std::min(available, remaining);
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            response.body.append(chunk.data(), read);
        }
        if (token.stop_requested()) response.error = "Cancelled";
        else if (response.body.size() > kMaxResponseBytes)
        {
            response.body.resize(kMaxResponseBytes);
            response.error = "Response too large";
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        break;
    }

    if (token.stop_requested() && response.error.empty())
        response.error = "Cancelled";
    WinHttpCloseHandle(session);
    return response;
}
