#include "calendar_subscriptions.h"

#include "calendar_ics.h"
#include "common/utils.h"
#include "data/atomic_file.h"
#include "platform/http_runtime.h"

#include <Windows.h>
#include <dpapi.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>

namespace snowdesktop::calendar
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto RefreshInterval = std::chrono::minutes{30};
constexpr auto RetryInterval = std::chrono::minutes{5};
constexpr std::size_t MaxSubscriptions = 16;
constexpr std::size_t MaxFeedBytes = 4 * 1024 * 1024;
constexpr std::size_t MaxLinkLength = 4096;
constexpr std::size_t MaxNameLength = 100;
constexpr std::size_t MaxConfigBytes = 512 * 1024;
constexpr std::string_view LegacyConfigVersion = "SnowDesktopCalendarSubscriptions1";
constexpr std::string_view ConfigVersion = "SnowDesktopCalendarSubscriptions2";

struct Source
{
    SubscriptionInfo info;
    std::wstring url;
    std::wstring managementUrl;
    // A feed change gets a new cache identity while keeping the subscription ID.
    std::string cacheId;
    std::vector<CalendarEvent> events;
    Clock::time_point nextRefresh{};
    bool loadCache = false;
};

std::optional<std::string> ReadFile(const std::filesystem::path& path, std::size_t limit)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > limit) return std::nullopt;
    std::ifstream file(path, std::ios::binary);
    std::string data(static_cast<std::size_t>(size), '\0');
    if (!file.read(data.data(), static_cast<std::streamsize>(size)) || file.peek() != EOF)
        return std::nullopt;
    return data;
}

std::optional<std::string> Protect(std::string& text, bool encrypt)
{
    DATA_BLOB input{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(text.data())};
    DATA_BLOB output{};
    const BOOL ok = encrypt
        ? CryptProtectData(&input, L"SnowDesktop calendar subscriptions", nullptr, nullptr, nullptr,
              CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    const std::unique_ptr<BYTE, decltype(&LocalFree)> memory(output.pbData, LocalFree);
    if (!ok) return std::nullopt;
    return std::string(reinterpret_cast<const char*>(memory.get()), output.cbData);
}

bool SaveProtected(const std::filesystem::path& path, std::string& text)
{
    const auto data = Protect(text, true);
    return data && atomic_file::WriteAll(path, *data);
}

std::optional<std::string> LoadProtected(const std::filesystem::path& path, std::size_t limit)
{
    auto encrypted = ReadFile(path, limit + 4096);
    auto text = encrypted ? Protect(*encrypted, false) : std::nullopt;
    return text && text->size() <= limit ? std::move(text) : std::nullopt;
}

std::wstring Trim(std::wstring value)
{
    const auto begin = value.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return {};
    return value.substr(begin, value.find_last_not_of(L" \t\r\n") - begin + 1);
}

bool NormalizeLink(std::wstring& url)
{
    url = Trim(std::move(url));
    if (url.size() > MaxLinkLength ||
        std::ranges::any_of(url, [](wchar_t ch) { return ch <= L' ' || ch == L'#' || ch == L'\\'; }))
        return false;
    if (url.size() >= 9 && CompareStringOrdinal(url.data(), 9, L"webcal://", 9, TRUE) == CSTR_EQUAL)
        url.replace(0, 9, L"https://");
    else if (url.size() >= 8 && CompareStringOrdinal(url.data(), 8, L"https://", 8, TRUE) == CSTR_EQUAL)
        url.replace(0, 8, L"https://");
    return url.starts_with(L"https://") && http_security::IsAllowedPublicHttpsUrl(url);
}

bool NormalizeManagementLink(std::wstring& url)
{
    url = Trim(std::move(url));
    return url.empty() || (url.size() <= MaxLinkLength &&
        std::ranges::none_of(url, [](wchar_t ch) { return ch <= L' ' || ch == L'\\'; }) &&
        http_security::IsAllowedPublicHttpsUrl(url));
}

std::string NewSourceId()
{
    GUID guid{};
    wchar_t id[39]{};
    if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(guid, id, static_cast<int>(std::size(id))) != 39)
        return {};
    return WideToUtf8(std::wstring(id + 1, id + 37));
}

bool ValidSourceId(std::string_view id)
{
    return id.size() == 36 && std::ranges::all_of(id, [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
            (ch >= 'A' && ch <= 'F') || ch == '-';
    });
}

struct WorkResult
{
    std::optional<std::vector<CalendarEvent>> events;
    std::string body;
};

WorkResult ReadSource(const std::string& id, const std::wstring& url,
    const std::filesystem::path& cachePath, bool cache, std::stop_token token)
{
    WorkResult result;
    if (cache)
    {
        if (const auto text = LoadProtected(cachePath, MaxFeedBytes))
            result.events = ParseIcsEvents(*text, id, CalendarService::CurrentLocalNow().date, token);
        return result;
    }
    http_stream::Options options;
    options.url = url;
    options.maximumResponseBytes = MaxFeedBytes;
    options.timeoutMs = 8000;
    options.totalTimeoutMs = 20000;
    options.publicHttpsOnly = true;
    const auto download = http_stream::StreamHttpGet(options, token,
        [](const http_stream::ResponseHead& head) { return head.status == 200; },
        [&](std::span<const std::byte> chunk) {
            result.body.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
            return true;
        });
    if (download.responseAccepted && download.error.empty() && !token.stop_requested())
        result.events = ParseIcsEvents(result.body, id, CalendarService::CurrentLocalNow().date, token);
    return result;
}

bool SameEvents(const std::vector<CalendarEvent>& before, const std::vector<CalendarEvent>& after)
{
    return std::ranges::equal(before, after, [](const auto& a, const auto& b) {
        return a.id == b.id && a.date == b.date && a.title == b.title && a.notes == b.notes &&
            a.allDay == b.allDay && a.startMinutes == b.startMinutes && a.endMinutes == b.endMinutes;
    });
}
} // namespace

struct CalendarSubscriptions::State
{
    struct Job
    {
        std::string id;
        std::string cacheId;
        bool cache;
        std::future<WorkResult> result;
        std::jthread worker;
    };

    std::filesystem::path directory;
    std::vector<Source> sources;
    std::optional<Job> job;
    bool loaded = false;

    std::filesystem::path ConfigPath() const { return directory / L"sources.dat"; }
    std::filesystem::path CachePath(const std::string& id) const { return directory / (id + ".dat"); }

    bool Save(const Source* changed = nullptr, std::string_view removal = {}) const
    {
        std::ostringstream output;
        output << ConfigVersion << '\n';
        const auto record = [&](const Source& source) {
            output << std::quoted(source.info.id) << ' ' << std::quoted(WideToUtf8(source.info.name))
                   << ' ' << std::quoted(WideToUtf8(source.url))
                   << ' ' << std::quoted(WideToUtf8(source.managementUrl))
                   << ' ' << std::quoted(source.cacheId) << '\n';
        };
        bool replaced = false;
        for (const auto& source : sources)
        {
            if (source.info.id == removal) continue;
            if (changed && changed->info.id == source.info.id)
            {
                record(*changed);
                replaced = true;
            }
            else record(source);
        }
        if (changed && !replaced) record(*changed);
        auto text = output.str();
        return SaveProtected(ConfigPath(), text);
    }

    void Start(Source& source, bool cache)
    {
        const auto& id = source.info.id;
        const auto path = CachePath(source.cacheId);
        std::packaged_task<WorkResult(std::stop_token)> work(
            [id, url = source.url, path, cache](std::stop_token token) {
                return ReadSource(id, url, path, cache, token);
            });
        auto future = work.get_future();
        job.emplace(Job{id, source.cacheId, cache, std::move(future), std::jthread(
            [task = std::move(work)](std::stop_token token) mutable { task(token); })});
        source.loadCache = false;
        source.info.status = SubscriptionStatus::Pending;
    }
};

CalendarSubscriptions::CalendarSubscriptions(std::filesystem::path directory)
    : state_(std::make_unique<State>())
{
    state_->directory = std::move(directory);
}

CalendarSubscriptions::~CalendarSubscriptions() = default;

void CalendarSubscriptions::Load()
{
    if (state_->loaded) return;
    std::error_code error;
    if (!std::filesystem::exists(state_->ConfigPath(), error) && !error)
    {
        state_->loaded = true;
        return;
    }
    const auto text = LoadProtected(state_->ConfigPath(), MaxConfigBytes);
    if (!text) return;
    std::istringstream input(*text);
    std::string version;
    if (!std::getline(input, version) || (version != ConfigVersion && version != LegacyConfigVersion)) return;
    std::vector<Source> sources;
    while (input >> std::ws && !input.eof())
    {
        Source source;
        std::string name, url, managementUrl;
        if (!(input >> std::quoted(source.info.id) >> std::quoted(name) >> std::quoted(url)) ||
            !ValidSourceId(source.info.id) || sources.size() >= MaxSubscriptions)
            return;
        if (version == LegacyConfigVersion)
            source.cacheId = source.info.id;
        else if (!(input >> std::quoted(managementUrl) >> std::quoted(source.cacheId)) ||
            !ValidSourceId(source.cacheId))
            return;
        source.info.name = Utf8ToWide(name);
        source.url = Utf8ToWide(url);
        source.managementUrl = Utf8ToWide(managementUrl);
        if (source.info.name.empty() || source.info.name.size() > MaxNameLength || !NormalizeLink(source.url) ||
            !NormalizeManagementLink(source.managementUrl) ||
            std::ranges::any_of(sources, [&](const Source& other) {
                return other.info.id == source.info.id || other.url == source.url || other.cacheId == source.cacheId;
            }))
            return;
        source.loadCache = true;
        sources.push_back(std::move(source));
    }
    state_->sources = std::move(sources);
    state_->loaded = true;
}

bool CalendarSubscriptions::Tick()
{
    if (!state_->loaded) return false;
    bool changed = false;
    auto& sources = state_->sources;
    const auto now = Clock::now();
    if (state_->job)
    {
        auto& job = *state_->job;
        if (job.result.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
            return false;
        WorkResult result;
        try { result = job.result.get(); }
        catch (const std::exception&) {}
        const auto source = std::ranges::find_if(sources, [&](const Source& item) { return item.info.id == job.id; });
        if (source != sources.end() && source->cacheId == job.cacheId)
        {
            const bool current = result.events && (job.cache || SaveProtected(state_->CachePath(job.cacheId), result.body));
            if (current)
            {
                changed = !SameEvents(source->events, *result.events);
                source->events = std::move(*result.events);
                source->info.hasCache = true;
            }
            if (!job.cache)
            {
                source->info.status = current ? SubscriptionStatus::Current : SubscriptionStatus::Failed;
                source->nextRefresh = now + (current ? RefreshInterval : RetryInterval);
            }
        }
        state_->job.reset();
    }

    auto next = std::ranges::find_if(sources, [](const Source& item) { return item.loadCache; });
    const bool cache = next != sources.end();
    if (!cache)
        next = std::ranges::min_element(sources, {}, &Source::nextRefresh);
    if (next != sources.end() && (cache || next->nextRefresh <= now))
    {
        try { state_->Start(*next, cache); }
        catch (const std::system_error&)
        {
            next->loadCache = false;
            next->info.status = SubscriptionStatus::Failed;
            next->nextRefresh = now + RetryInterval;
        }
    }
    return changed;
}

std::vector<SubscriptionInfo> CalendarSubscriptions::List() const
{
    std::vector<SubscriptionInfo> result;
    for (const auto& source : state_->sources) result.push_back(source.info);
    return result;
}

std::optional<SubscriptionDetails> CalendarSubscriptions::Details(const std::string& id) const
{
    const auto source = std::ranges::find_if(state_->sources, [&](const Source& item) { return item.info.id == id; });
    if (source == state_->sources.end()) return std::nullopt;
    return SubscriptionDetails{source->info.name, source->url, source->managementUrl};
}

SubscriptionResult CalendarSubscriptions::Apply(SubscriptionCommand command)
{
    if (!state_->loaded) return SubscriptionResult::Unavailable;
    auto& sources = state_->sources;
    const bool adding = command.action == SubscriptionAction::Add;
    const auto source = std::ranges::find_if(sources, [&](const Source& item) { return item.info.id == command.id; });
    if (!adding && source == sources.end()) return SubscriptionResult::NotFound;
    const bool inFlight = state_->job && state_->job->id == command.id;
    if (adding || command.action == SubscriptionAction::Edit)
    {
        if (adding && sources.size() >= MaxSubscriptions) return SubscriptionResult::Limit;
        if (!NormalizeLink(command.url)) return SubscriptionResult::InvalidLink;
        if (!NormalizeManagementLink(command.managementUrl)) return SubscriptionResult::InvalidManagementLink;
        command.name = Trim(std::move(command.name));
        if (command.name.empty() || command.name.size() > MaxNameLength ||
            std::ranges::any_of(command.name, [](wchar_t ch) { return ch < L' '; }))
            return SubscriptionResult::InvalidName;
        if (std::ranges::any_of(sources, [&](const Source& item) {
            return (adding || item.info.id != command.id) && item.url == command.url;
        }))
            return SubscriptionResult::Duplicate;
        const bool feedChanged = !adding && source->url != command.url;
        if (!adding && !feedChanged && source->info.name == command.name &&
            source->managementUrl == command.managementUrl)
            return SubscriptionResult::Applied;
        Source updated;
        updated.info.id = adding ? NewSourceId() : source->info.id;
        updated.info.name = std::move(command.name);
        updated.url = std::move(command.url);
        updated.managementUrl = std::move(command.managementUrl);
        updated.cacheId = adding ? updated.info.id : feedChanged ? NewSourceId() : source->cacheId;
        if (updated.info.id.empty() || updated.cacheId.empty()) return SubscriptionResult::SaveFailed;
        const auto oldCache = feedChanged ? state_->CachePath(source->cacheId) : std::filesystem::path{};
        if (adding) sources.reserve(sources.size() + 1);
        if (!state_->Save(&updated)) return SubscriptionResult::SaveFailed;
        if (adding) sources.push_back(std::move(updated));
        else if (feedChanged)
        {
            if (inFlight) state_->job->worker.request_stop();
            *source = std::move(updated);
            std::error_code error;
            std::filesystem::remove(oldCache, error);
        }
        else
        {
            source->info.name = std::move(updated.info.name);
            source->managementUrl = std::move(updated.managementUrl);
        }
        return SubscriptionResult::Applied;
    }
    if (command.action == SubscriptionAction::Refresh)
    {
        if (!inFlight || state_->job->cacheId != source->cacheId)
        {
            source->nextRefresh = {};
            source->info.status = SubscriptionStatus::Pending;
        }
        return SubscriptionResult::Applied;
    }
    if (command.action != SubscriptionAction::Remove) return SubscriptionResult::Unavailable;
    const auto oldCache = state_->CachePath(source->cacheId);
    if (!state_->Save(nullptr, command.id)) return SubscriptionResult::SaveFailed;
    if (inFlight) state_->job->worker.request_stop();
    sources.erase(source);
    std::error_code error;
    std::filesystem::remove(oldCache, error);
    return SubscriptionResult::Applied;
}

void CalendarSubscriptions::AppendEvents(std::vector<CalendarEvent>& result,
    const std::string& from, const std::string& to) const
{
    for (const auto& source : state_->sources)
    {
        const auto begin = std::ranges::lower_bound(source.events, from, {}, &CalendarEvent::date);
        const auto end = std::ranges::upper_bound(begin, source.events.end(), to, {}, &CalendarEvent::date);
        const auto firstAdded = result.size();
        result.insert(result.end(), begin, end);
        for (auto index = firstAdded; index < result.size(); ++index)
        {
            result[index].subscriptionId = source.info.id;
            result[index].subscriptionName = source.info.name;
        }
    }
}

std::optional<CalendarEvent> CalendarSubscriptions::EventById(const std::string& id) const
{
    for (const auto& source : state_->sources)
    {
        const auto event = std::ranges::find(source.events, id, &CalendarEvent::id);
        if (event != source.events.end())
        {
            auto result = *event;
            result.subscriptionId = source.info.id;
            result.subscriptionName = source.info.name;
            return result;
        }
    }
    return std::nullopt;
}

} // namespace snowdesktop::calendar
