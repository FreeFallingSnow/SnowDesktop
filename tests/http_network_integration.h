#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace http_network_test
{
// Exercise the real AsyncHttpService against an isolated loopback origin.
// A URL-only test cannot catch the former DNS and peer-address rejection.
class Origin
{
public:
    Origin()
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
        if (worker_.joinable()) worker_.join();
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        if (started_) WSACleanup();
    }

    std::wstring Url(std::wstring_view path) const
    {
        return L"http://127.0.0.1:" + std::to_wstring(port_) + std::wstring(path);
    }
    bool Ready() const { return port_ != 0; }

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
            if (request.starts_with("GET /redirect "))
            {
                response = "HTTP/1.1 302 Found\r\nLocation: http://localhost:" +
                    std::to_string(port_) + "/feed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
            else
            {
                const std::string body = request.starts_with("GET /large ")
                    ? std::string(6000, 'x') : "<rss><channel><title>Local feed</title></channel></rss>";
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
};

inline HttpResponse Fetch(HttpRequestOptions options)
{
    AsyncHttpService service;
    options.widgetId = L"loopback-network-regression";
    options.timeoutMs = 1000;
    options.allowHttpAndLocalTargets = true;
    HttpResponse failure;
    failure.error = "request was rejected or did not complete";
    if (service.Submit(std::move(options)) <= 0) return failure;
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
    if (!failures) std::cout << "HTTP network integration tests passed\n";
    return failures ? 1 : 0;
}
