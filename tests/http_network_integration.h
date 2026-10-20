#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace http_network_test
{
// Exercise the real AsyncHttpService against an isolated loopback origin.
// A URL-only test cannot catch the former DNS and peer-address rejection.
class Origin
{
public:
    explicit Origin(bool rejectAll = false) : rejectAll_(rejectAll)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
        started_ = true;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listener_, SOMAXCONN) != 0) return;
        int size = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) != 0) return;
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token token) { Serve(token); });
    }

    ~Origin()
    {
        worker_.request_stop();
        ReleaseBlockedResponse();
        if (worker_.joinable()) worker_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        if (started_) WSACleanup();
    }

    std::wstring Url(std::wstring_view path) const
    {
        return L"http://127.0.0.1:" + std::to_wstring(port_) + std::wstring(path);
    }
    bool Ready() const { return port_ != 0; }
    int RequestCount() const { return requestCount_.load(); }
    bool WaitForBlockedResponse()
    {
        std::unique_lock lock(gateMutex_);
        return gate_.wait_for(lock, std::chrono::seconds(2),
            [this] { return blockedResponse_; });
    }
    void ReleaseBlockedResponse()
    {
        std::scoped_lock lock(gateMutex_);
        releasedResponse_ = true;
        gate_.notify_all();
    }

private:
    void Serve(std::stop_token token)
    {
        while (!token.stop_requested())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener_, &readable);
            timeval timeout{0, 50000};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            SOCKET client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD ioTimeout = 1000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
            std::string request;
            char buffer[2048];
            while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
            {
                const int count = recv(client, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                request.append(buffer, count);
            }
            std::string response;
            const auto pathStart = request.find(' ');
            const auto pathEnd = request.find(' ', pathStart + 1);
            const std::string path = pathStart != std::string::npos &&
                pathEnd != std::string::npos
                ? request.substr(pathStart + 1, pathEnd - pathStart - 1) : "";
            const int pathCount = ++pathCounts_[path];
            ++requestCount_;
            if (path == "/cancel")
            {
                std::unique_lock lock(gateMutex_);
                blockedResponse_ = true;
                gate_.notify_all();
                gate_.wait_for(lock, std::chrono::seconds(2),
                    [this] { return releasedResponse_; });
            }
            if (path == "/retry-transport" && pathCount == 1)
            {
                closesocket(client);
                continue;
            }
            if (path == "/retry-read" && pathCount == 1)
            {
                response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                    "3\r\nbad\r\ninvalid-chunk\r\n";
            }
            else if (rejectAll_ || path == "/always-error" || path == "/cancel" ||
                (path == "/retry-status" && pathCount == 1))
            {
                response = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 5\r\nConnection: close\r\n\r\nproxy";
            }
            else if (request.starts_with("GET /redirect "))
            {
                response = "HTTP/1.1 302 Found\r\nLocation: http://localhost:" +
                    std::to_string(port_) + "/feed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
            else
            {
                const std::string body = path == "/large" ? std::string(6000, 'x') :
                    path == "/rss-large" ? std::string(2 * 1024 * 1024, 'r') :
                    path == "/rss-too-large" ? std::string(4 * 1024 * 1024 + 1, 'r') :
                    "<rss><channel><title>Local feed</title></channel></rss>";
                response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            }
            std::size_t offset = 0;
            while (offset < response.size())
            {
                const int count = send(client, response.data() + offset,
                    static_cast<int>(response.size() - offset), 0);
                if (count <= 0) break;
                offset += count;
            }
            closesocket(client);
        }
    }

    bool started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::jthread worker_;
    std::atomic<int> requestCount_{0};
    std::map<std::string, int> pathCounts_;
    bool rejectAll_ = false;
    std::mutex gateMutex_;
    std::condition_variable gate_;
    bool blockedResponse_ = false;
    bool releasedResponse_ = false;
};

inline HttpResponse AwaitResponse(AsyncHttpService& service)
{
    HttpResponse failure;
    failure.error = "request was rejected or did not complete";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto responses = service.Drain();
        if (!responses.empty()) return std::move(responses.front());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    service.Stop();
    return failure;
}
inline HttpResponse Fetch(AsyncHttpService& service, HttpRequestOptions options)
{
    options.widgetId = L"loopback-network-regression";
    options.timeoutMs = 1000;
    options.allowHttpAndLocalTargets = true;
    if (service.Submit(std::move(options)) <= 0)
    {
        HttpResponse failure;
        failure.error = "request was rejected";
        return failure;
    }
    return AwaitResponse(service);
}
inline HttpResponse Fetch(HttpRequestOptions options)
{
    AsyncHttpService service;
    return Fetch(service, std::move(options));
}
}

inline int RunHttpNetworkIntegrationTests()
{
    using namespace http_network_test;
    Origin origin;
    if (!origin.Ready())
    {
        std::cerr << "FAILED: cannot create isolated loopback HTTP origin\n";
        return 1;
    }
    int failures = 0;
    const auto expect = [&](bool condition, const char* message, const HttpResponse& response) {
        if (condition) return;
        ++failures;
        std::cerr << "FAILED: " << message << "; status=" << response.status
                  << "; error=" << response.error << '\n';
    };
    HttpRequestOptions options;
    options.url = origin.Url(L"/feed");
    auto response = Fetch(options);
    expect(response.error.empty() && response.status == 200 &&
        response.body == "<rss><channel><title>Local feed</title></channel></rss>",
        "authorized HTTP request reaches and reads the local origin", response);

    options.url = origin.Url(L"/redirect");
    response = Fetch(options);
    expect(response.error.empty() && response.status == 200 &&
        response.body == "<rss><channel><title>Local feed</title></channel></rss>",
        "unscoped requests can follow a redirect to another local hostname", response);

    options.allowedDomains = {"127.0.0.1"};
    response = Fetch(options);
    expect(response.error == "Redirect URL is not allowed" && response.body.empty(),
        "declared host scope still rejects an out-of-scope redirect", response);

    options.allowedDomains.clear();
    options.sameOriginRedirectsOnly = true;
    response = Fetch(options);
    expect(response.error == "Redirect changed origin for credential-bearing request" &&
        response.body.empty(), "secret-bearing requests still reject origin changes", response);

    options.sameOriginRedirectsOnly = false;
    options.url = origin.Url(L"/large");
    options.maximumResponseBytes = 4096;
    response = Fetch(options);
    expect(response.error == "Response too large" && response.body.size() == 4096,
        "local responses remain bounded by the caller's response limit", response);

    options.url = origin.Url(L"/rss-large");
    options.maximumResponseBytes = 1024 * 1024;
    response = Fetch(options);
    expect(response.error == "Response too large" && response.body.size() == 1024 * 1024,
        "existing one-MiB callers keep their response ceiling", response);
    options.maximumResponseBytes = 4 * 1024 * 1024;
    response = Fetch(options);
    expect(response.error.empty() && response.status == 200 &&
        response.body == std::string(2 * 1024 * 1024, 'r'),
        "explicit large-response requests can receive a two-MiB feed", response);
    options.url = origin.Url(L"/rss-too-large");
    response = Fetch(options);
    expect(response.error == "Response too large" && response.body.size() == 4 * 1024 * 1024,
        "the extended response ceiling still rejects over-limit feeds", response);

    // Real requests protect against duplicate writes, endless retries and
    // returning a partial/old failure body after a successful direct attempt.
    options.maximumResponseBytes = 131072;
    options.retryDirect = true;
    for (const auto* path : {L"/retry-status", L"/retry-transport", L"/retry-read"})
    {
        const int before = origin.RequestCount();
        options.url = origin.Url(path);
        response = Fetch(options);
        expect(response.error.empty() && response.status == 200 &&
            response.body == "<rss><channel><title>Local feed</title></channel></rss>" &&
            origin.RequestCount() - before == 2,
            "read failures retry once and return only the successful response", response);
    }
    options.url = origin.Url(L"/always-error");
    int before = origin.RequestCount();
    response = Fetch(options);
    expect(response.error.empty() && response.status == 503 && response.body == "proxy" &&
        origin.RequestCount() - before == 2,
        "a failed direct attempt is final without a third request", response);

    options.retryDirect = false;
    before = origin.RequestCount();
    response = Fetch(options);
    expect(response.status == 503 && origin.RequestCount() - before == 1,
        "existing callers do not implicitly bypass the proxy", response);
    options.retryDirect = true;
    for (const auto* method : {L"POST", L"PUT", L"PATCH", L"DELETE"})
    {
        options.method = method;
        before = origin.RequestCount();
        response = Fetch(options);
        expect(response.status == 503 && origin.RequestCount() - before == 1,
            "write methods cannot be repeated even if native caller opts in", response);
    }
    options.method = L"GET";
    options.body = "payload";
    before = origin.RequestCount();
    response = Fetch(options);
    expect(response.status == 503 && origin.RequestCount() - before == 1,
        "requests with a body cannot be repeated", response);
    options.body.clear();
    options.url = origin.Url(L"/large");
    options.maximumResponseBytes = 4096;
    before = origin.RequestCount();
    response = Fetch(options);
    expect(response.error == "Response too large" && origin.RequestCount() - before == 1,
        "direct retry cannot bypass response size enforcement", response);
    options.maximumResponseBytes = 131072;
    options.url = origin.Url(L"/redirect");
    options.allowedDomains = {"127.0.0.1"};
    before = origin.RequestCount();
    response = Fetch(options);
    expect(response.error == "Redirect URL is not allowed" && origin.RequestCount() - before == 1,
        "direct retry cannot bypass domain restrictions", response);
    options.allowedDomains.clear();
    options.sameOriginRedirectsOnly = true;
    before = origin.RequestCount();
    response = Fetch(options);
    expect(response.error == "Redirect changed origin for credential-bearing request" &&
        origin.RequestCount() - before == 1,
        "direct retry preserves credential redirect restrictions", response);

    AsyncHttpService cachedService;
    options.sameOriginRedirectsOnly = false;
    options.url = origin.Url(L"/feed");
    options.cacheSeconds = 60;
    response = Fetch(cachedService, options);
    before = origin.RequestCount();
    response = Fetch(cachedService, options);
    expect(response.fromCache && origin.RequestCount() == before,
        "successful retry-enabled reads remain cacheable", response);
    options.retryDirect = false;
    response = Fetch(cachedService, options);
    expect(!response.fromCache && origin.RequestCount() - before == 1,
        "cache entries cannot cross the opted-in proxy policy", response);

    AsyncHttpService cancelledService;
    options.url = origin.Url(L"/cancel");
    options.widgetId = L"cancelled-retry-regression";
    options.allowHttpAndLocalTargets = true;
    options.cacheSeconds = 0;
    options.retryDirect = true;
    before = origin.RequestCount();
    const int cancelledId = cancelledService.Submit(options);
    expect(cancelledId > 0 && origin.WaitForBlockedResponse(),
        "cancellation fixture observes the first in-flight request", response);
    const bool cancelled = cancelledService.Cancel(options.widgetId, cancelledId);
    origin.ReleaseBlockedResponse();
    response = AwaitResponse(cancelledService);
    expect(cancelled && response.error == "Cancelled" &&
        origin.RequestCount() - before == 1,
        "cancelling the first attempt prevents the direct attempt", response);
    if (!failures) std::cout << "HTTP network integration tests passed\n";
    return failures ? 1 : 0;
}
