#include "http_runtime.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
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

std::wstring NormalizeHostname(std::wstring value)
{
    value = Lower(std::move(value));
    if (value.size() >= 2 &&
        value.front() == L'[' && value.back() == L']')
    {
        value = value.substr(1, value.size() - 2);
    }
    while (!value.empty() && value.back() == L'.')
        value.pop_back();
    return value;
}

bool IsIpLiteral(std::wstring_view address)
{
    if (!EnsureWinsock()) return false;
    const std::wstring value(address);
    IN_ADDR ipv4{};
    if (InetPtonW(AF_INET, value.c_str(), &ipv4) == 1)
        return true;
    IN6_ADDR ipv6{};
    return InetPtonW(AF_INET6, value.c_str(), &ipv6) == 1;
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

bool snowdesktop::http_security::IsAllowedUrlForDomains(
    const std::wstring& url,
    const std::vector<std::string>& domains,
    bool allowAnyHttpOrHttpsUrl,
    bool allowAnyPublicHttpsUrl)
{
    URL_COMPONENTS components{ sizeof(components) };
    wchar_t host[256]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) return false;
    const bool isHttp = components.nScheme == INTERNET_SCHEME_HTTP;
    const bool isHttps = components.nScheme == INTERNET_SCHEME_HTTPS;
    if (allowAnyHttpOrHttpsUrl)
        return (isHttp || isHttps) && components.dwHostNameLength > 0;
    if (allowAnyPublicHttpsUrl)
        return IsAllowedPublicHttpsUrl(url);
    if (!isHttps) return false;
    std::wstring actual = NormalizeHostname(
        std::wstring(host, components.dwHostNameLength));
    if (actual.empty()) return false;
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
    if (IsIpLiteral(actual) &&
        !IsAllowedRemoteIpLiteral(actual))
        return false;
    for (const auto& raw : domains)
    {
        if (raw.empty() || std::any_of(
                raw.begin(), raw.end(),
                [](unsigned char ch) {
                    return ch > 0x7f;
                }))
            continue;
        std::wstring allowed(raw.begin(), raw.end());
        allowed = NormalizeHostname(std::move(allowed));
        if (allowed.empty() ||
            allowed.find_first_of(L"/*?@#[]") !=
                std::wstring::npos ||
            (allowed.find(L':') != std::wstring::npos &&
                !IsIpLiteral(allowed)))
            continue;
        if (allowed == actual) return true;
    }
    return false;
}

bool snowdesktop::http_security::HaveSameOrigin(
    const std::wstring& left, const std::wstring& right)
{
    const auto readOrigin = [](const std::wstring& url,
        INTERNET_SCHEME& scheme, INTERNET_PORT& port,
        std::wstring& host) {
        URL_COMPONENTS components{ sizeof(components) };
        wchar_t hostBuffer[256]{};
        components.lpszHostName = hostBuffer;
        components.dwHostNameLength =
            static_cast<DWORD>(std::size(hostBuffer));
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
            components.dwHostNameLength == 0)
            return false;
        scheme = components.nScheme;
        port = components.nPort;
        host = NormalizeHostname(std::wstring(
            hostBuffer, components.dwHostNameLength));
        return !host.empty();
    };
    INTERNET_SCHEME leftScheme = static_cast<INTERNET_SCHEME>(0);
    INTERNET_SCHEME rightScheme = static_cast<INTERNET_SCHEME>(0);
    INTERNET_PORT leftPort = 0;
    INTERNET_PORT rightPort = 0;
    std::wstring leftHost;
    std::wstring rightHost;
    return readOrigin(left, leftScheme, leftPort, leftHost) &&
        readOrigin(right, rightScheme, rightPort, rightHost) &&
        leftScheme == rightScheme && leftPort == rightPort &&
        leftHost == rightHost;
}

bool snowdesktop::http_security::IsAllowedPublicHttpsUrl(
    const std::wstring& url)
{
    URL_COMPONENTS components{ sizeof(components) };
    wchar_t host[256]{};
    wchar_t user[2]{};
    wchar_t password[2]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUserName = user;
    components.dwUserNameLength = static_cast<DWORD>(std::size(user));
    components.lpszPassword = password;
    components.dwPasswordLength = static_cast<DWORD>(std::size(password));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.dwHostNameLength == 0 ||
        components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0)
        return false;
    const std::wstring normalized = NormalizeHostname(
        std::wstring(host, components.dwHostNameLength));
    if (normalized.empty()) return false;
    const std::string domain = WideToUtf8Http(normalized);
    return !domain.empty() &&
        IsAllowedUrlForDomains(url, { domain }, false);
}

snowdesktop::http_stream::Result
snowdesktop::http_stream::StreamPublicHttpsGet(
    const Options& options, std::stop_token token,
    const HeadCallback& headCallback,
    const ChunkSink& chunkSink)
{
    Result result;
    if (options.url.empty() || !headCallback || !chunkSink ||
        options.maximumResponseBytes == 0 ||
        !http_security::IsAllowedPublicHttpsUrl(options.url))
    {
        result.error = "Invalid public HTTPS stream request";
        return result;
    }

    const int timeoutMs = std::clamp(options.timeoutMs, 1000, 60000);
    const int maximumRedirects = std::clamp(options.maxRedirects, 0, 10);
    HINTERNET session = WinHttpOpen(L"SnowDesktop/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        result.error = "WinHttpOpen failed";
        return result;
    }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs,
        timeoutMs, timeoutMs);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY,
        &redirectPolicy, sizeof(redirectPolicy));

    const auto queryHeader = [](HINTERNET request,
        DWORD query, const wchar_t* customName = nullptr) {
        DWORD size = 0;
        WinHttpQueryHeaders(request, query,
            customName ? customName : WINHTTP_HEADER_NAME_BY_INDEX,
            nullptr, &size, WINHTTP_NO_HEADER_INDEX);
        if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return std::wstring{};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(request, query,
                customName ? customName : WINHTTP_HEADER_NAME_BY_INDEX,
                value.data(), &size, WINHTTP_NO_HEADER_INDEX))
            return std::wstring{};
        value.resize(wcslen(value.c_str()));
        return value;
    };
    const auto parseLength = [](const std::wstring& value)
        -> std::optional<std::uint64_t> {
        if (value.empty()) return std::nullopt;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long long parsed = wcstoull(
            value.c_str(), &end, 10);
        while (end && *end && iswspace(*end)) ++end;
        if (errno == ERANGE || end == value.c_str() ||
            (end && *end != L'\0'))
            return std::nullopt;
        return static_cast<std::uint64_t>(parsed);
    };

    std::wstring currentUrl = options.url;
    for (int redirectCount = 0;
        redirectCount <= maximumRedirects && !token.stop_requested();
        ++redirectCount)
    {
        if (!http_security::IsAllowedPublicHttpsUrl(currentUrl))
        {
            result.error = "Redirect URL is not an allowed public HTTPS URL";
            break;
        }

        URL_COMPONENTS components{ sizeof(components) };
        wchar_t host[256]{};
        wchar_t path[2048]{};
        wchar_t extra[4096]{};
        components.lpszHostName = host;
        components.dwHostNameLength = static_cast<DWORD>(std::size(host));
        components.lpszUrlPath = path;
        components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
        components.lpszExtraInfo = extra;
        components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
        if (!WinHttpCrackUrl(currentUrl.c_str(), 0, 0, &components) ||
            components.nScheme != INTERNET_SCHEME_HTTPS)
        {
            result.error = "Invalid HTTPS URL";
            break;
        }

        const std::wstring currentHost(host, components.dwHostNameLength);
        std::wstring pinnedAddress;
        if (!ResolvePinnedPublicAddress(currentHost, pinnedAddress))
        {
            result.error =
                "Host resolves to a private, local, or unavailable address";
            break;
        }

        HINTERNET connection = WinHttpConnect(session,
            currentHost.c_str(), components.nPort, 0);
        if (!connection)
        {
            result.error = "WinHttpConnect failed";
            break;
        }
        const std::wstring requestPath =
            std::wstring(path, components.dwUrlPathLength) +
            std::wstring(extra, components.dwExtraInfoLength);
        const wchar_t* acceptedTypes[] = { L"*/*", nullptr };
        HINTERNET request = WinHttpOpenRequest(connection, L"GET",
            requestPath.c_str(), nullptr, WINHTTP_NO_REFERER,
            acceptedTypes,
            WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
        if (!request)
        {
            WinHttpCloseHandle(connection);
            result.error = "WinHttpOpenRequest failed";
            break;
        }

        if (!pinnedAddress.empty())
        {
            const DWORD pinnedAddressBytes = static_cast<DWORD>(
                (pinnedAddress.size() + 1) * sizeof(wchar_t));
            WinHttpSetOption(request, WINHTTP_OPTION_RESOLUTION_HOSTNAME,
                pinnedAddress.data(), pinnedAddressBytes);
        }
        DWORD disabledFeatures =
            WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
        if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                &disabledFeatures, sizeof(disabledFeatures)))
        {
            result.error = "Cannot apply HTTP request security policy";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }

        constexpr wchar_t headers[] = L"Accept-Encoding: identity\r\n";
        const BOOL sent = WinHttpSendRequest(request, headers,
            static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent || !WinHttpReceiveResponse(request, nullptr))
        {
            result.error = "HTTP request failed";
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
            result.error = "HTTP connection reached a non-public address";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX))
        {
            result.error = "Cannot read HTTP status";
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            break;
        }

        if (status >= 300 && status < 400)
        {
            if (redirectCount == maximumRedirects)
            {
                result.error = "Too many redirects";
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                break;
            }
            const std::wstring location = queryHeader(
                request, WINHTTP_QUERY_LOCATION);
            if (location.empty())
            {
                result.error = "Redirect is missing Location";
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                break;
            }
            wchar_t combined[8192]{};
            DWORD combinedLength = static_cast<DWORD>(std::size(combined));
            if (FAILED(UrlCombineW(currentUrl.c_str(), location.c_str(),
                    combined, &combinedLength, URL_ESCAPE_UNSAFE)))
            {
                result.error = "Invalid redirect URL";
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                break;
            }
            currentUrl.assign(combined, combinedLength);
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            continue;
        }

        result.head.status = static_cast<int>(status);
        result.head.finalUrl = currentUrl;
        result.head.contentType = queryHeader(
            request, WINHTTP_QUERY_CONTENT_TYPE);
        result.head.contentDisposition = queryHeader(
            request, WINHTTP_QUERY_CUSTOM, L"Content-Disposition");
        result.head.contentEncoding = queryHeader(
            request, WINHTTP_QUERY_CONTENT_ENCODING);
        result.head.contentLength = parseLength(queryHeader(
            request, WINHTTP_QUERY_CONTENT_LENGTH));

        if (status < 200 || status >= 300)
            result.error = "HTTP response is not successful";
        else if (result.head.contentLength &&
            *result.head.contentLength > options.maximumResponseBytes)
            result.error = "Response too large";
        else
        {
            try
            {
                result.responseAccepted = headCallback(result.head);
            }
            catch (...)
            {
                result.error = "Response callback failed";
            }
        }

        while (result.error.empty() && result.responseAccepted &&
            !token.stop_requested())
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available))
            {
                result.error = "Cannot read HTTP response";
                break;
            }
            if (available == 0) break;
            const std::uint64_t remaining =
                options.maximumResponseBytes - result.bytesReceived;
            if (remaining == 0 ||
                static_cast<std::uint64_t>(available) > remaining)
            {
                result.error = "Response too large";
                break;
            }
            std::vector<std::byte> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read))
            {
                result.error = "Cannot read HTTP response";
                break;
            }
            if (read == 0) break;
            chunk.resize(read);
            bool consumed = false;
            try
            {
                consumed = chunkSink(chunk);
            }
            catch (...)
            {
                result.error = "Response sink failed";
                break;
            }
            if (!consumed)
            {
                result.error = "Response sink failed";
                break;
            }
            result.bytesReceived += read;
        }

        if (token.stop_requested())
        {
            result.cancelled = true;
            result.error = "Cancelled";
        }
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        break;
    }

    if (token.stop_requested() && !result.cancelled)
    {
        result.cancelled = true;
        result.error = "Cancelled";
    }
    WinHttpCloseHandle(session);
    return result;
}

int AsyncHttpService::Submit(HttpRequestOptions options)
{
    if (!snowdesktop::http_security::
            IsAllowedUrlForDomains(
                options.url, options.allowedDomains,
                options.allowAnyHttpOrHttpsUrl,
                options.allowAnyPublicHttpsUrl))
        return 0;
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
        options.url + L"\n" + options.headers + L"\n" +
        std::to_wstring(options.maximumResponseBytes);
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
        if (options.sameOriginRedirectsOnly &&
            !snowdesktop::http_security::HaveSameOrigin(
                options.url, currentUrl))
        {
            response.error = "Redirect changed origin for credential-bearing request";
            break;
        }
        if (!snowdesktop::http_security::
                IsAllowedUrlForDomains(
                    currentUrl, options.allowedDomains,
                    options.allowAnyHttpOrHttpsUrl,
                    options.allowAnyPublicHttpsUrl))
        {
            response.error = "Redirect URL is not allowed";
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
        if (!options.allowAnyHttpOrHttpsUrl &&
            !ResolvePinnedPublicAddress(currentHost, pinnedAddress))
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
        // Resolution pinning is a best-effort hardening measure. Some
        // WinHTTP versions expose the option in the SDK but reject it at run
        // time, so attempt it directly and continue with the checked hostname
        // connection when it is unavailable.
        if (!pinnedAddress.empty())
        {
            const DWORD pinnedAddressBytes = static_cast<DWORD>(
                (pinnedAddress.size() + 1) * sizeof(wchar_t));
            WinHttpSetOption(request, WINHTTP_OPTION_RESOLUTION_HOSTNAME,
                pinnedAddress.data(), pinnedAddressBytes);
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
        if (!options.allowAnyHttpOrHttpsUrl)
        {
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

        const DWORD maximumResponseBytes = std::clamp<DWORD>(
            options.maximumResponseBytes, 4096, kMaxResponseBytes);
        while (!token.stop_requested() &&
            response.body.size() <= maximumResponseBytes)
        {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            DWORD remaining = maximumResponseBytes + 1 -
                static_cast<DWORD>(response.body.size());
            available = std::min(available, remaining);
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
            response.body.append(chunk.data(), read);
        }
        if (token.stop_requested()) response.error = "Cancelled";
        else if (response.body.size() > maximumResponseBytes)
        {
            response.body.resize(maximumResponseBytes);
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
