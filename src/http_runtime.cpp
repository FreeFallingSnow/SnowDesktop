#include "http_runtime.h"

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>

#include <algorithm>

namespace
{
constexpr DWORD kMaxResponseBytes = 1024 * 1024;

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
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS)
        return false;
    std::wstring actual = Lower(std::wstring(host, components.dwHostNameLength));
    for (const auto& raw : domains)
    {
        std::wstring allowed(raw.begin(), raw.end());
        allowed = Lower(allowed);
        if (allowed == actual) return true;
        if (allowed.starts_with(L"*."))
        {
            std::wstring suffix = allowed.substr(1);
            if (actual.size() > suffix.size() &&
                actual.compare(actual.size() - suffix.size(), suffix.size(), suffix) == 0)
                return true;
        }
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
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
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
        HINTERNET connection = WinHttpConnect(session,
            std::wstring(host, components.dwHostNameLength).c_str(),
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
