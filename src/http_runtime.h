#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace snowdesktop::http_security
{
bool IsAllowedRemoteIpLiteral(std::wstring_view address);
bool IsAllowedUrlForDomains(const std::wstring& url,
    const std::vector<std::string>& domains,
    bool allowAnyHttpOrHttpsUrl = false,
    bool allowAnyPublicHttpsUrl = false);
bool IsAllowedPublicHttpsUrl(const std::wstring& url);
bool HaveSameOrigin(const std::wstring& left, const std::wstring& right);
}

namespace snowdesktop::http_stream
{
struct ResponseHead
{
    int status = 0;
    std::wstring finalUrl;
    std::wstring contentType;
    std::wstring contentDisposition;
    std::wstring contentEncoding;
    std::optional<std::uint64_t> contentLength;
};

struct Options
{
    std::wstring url;
    int timeoutMs = 10000;
    std::uint64_t maximumResponseBytes = 64ull * 1024ull * 1024ull;
    int maxRedirects = 3;
};

struct Result
{
    ResponseHead head;
    std::uint64_t bytesReceived = 0;
    std::string error;
    bool cancelled = false;
    bool responseAccepted = false;
};

using HeadCallback = std::function<bool(const ResponseHead&)>;
using ChunkSink = std::function<bool(std::span<const std::byte>)>;

/**
 * Streams one public HTTPS resource on the calling thread.
 *
 * Redirects are handled explicitly and every hop is checked against the same
 * DNS and connected-address policy used by AsyncHttpService. Returning false
 * from headCallback intentionally declines the response without reading its
 * body. Returning false from chunkSink reports a sink failure.
 */
Result StreamPublicHttpsGet(const Options& options,
    std::stop_token token, const HeadCallback& headCallback,
    const ChunkSink& chunkSink);
}

struct HttpRequestOptions
{
    std::wstring widgetId;
    std::wstring url;
    std::wstring method = L"GET";
    std::wstring headers;
    std::string body;
    int timeoutMs = 10000;
    int cacheSeconds = 0;
    std::uint32_t maximumResponseBytes = 1024 * 1024;
    std::vector<std::string> allowedDomains;
    bool allowAnyHttpOrHttpsUrl = false;
    bool allowAnyPublicHttpsUrl = false;
    bool sameOriginRedirectsOnly = false;
};

struct HttpResponse
{
    int id = 0;
    std::wstring widgetId;
    int status = 0;
    std::string body;
    std::string error;
    bool fromCache = false;
};

class AsyncHttpService
{
public:
    AsyncHttpService() = default;
    ~AsyncHttpService();

    int Submit(HttpRequestOptions options);
    bool Cancel(const std::wstring& widgetId, int requestId);
    std::vector<HttpResponse> Drain();
    void CancelWidget(const std::wstring& widgetId);
    void Stop();

private:
    struct RequestState
    {
        std::wstring widgetId;
        std::jthread worker;
    };
    struct CacheEntry
    {
        HttpResponse response;
        std::chrono::steady_clock::time_point expires;
    };

    static HttpResponse Execute(int id, const HttpRequestOptions& options, std::stop_token token);
    void Complete(HttpResponse response);

    std::atomic<int> nextId_{ 1 };
    std::mutex mutex_;
    std::unordered_map<int, std::unique_ptr<RequestState>> requests_;
    std::deque<HttpResponse> completed_;
    std::unordered_map<std::wstring, CacheEntry> cache_;
};
