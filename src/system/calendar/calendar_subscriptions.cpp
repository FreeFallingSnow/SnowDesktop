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
constexpr std::string_view ConfigVersion = "SnowDesktopCalendarSubscriptions1";

struct Source
{
    SubscriptionInfo info;
    std::wstring url;
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

    bool Save(const Source* addition = nullptr, std::string_view removal = {}) const
    {
        std::ostringstream output;
        output << ConfigVersion << '\n';
        const auto record = [&](const Source& source) {
            output << std::quoted(source.info.id) << ' ' << std::quoted(WideToUtf8(source.info.name))
                   << ' ' << std::quoted(WideToUtf8(source.url)) << '\n';
        };
        for (const auto& source : sources)
            if (source.info.id != removal) record(source);
        if (addition) record(*addition);
        auto text = output.str();
        return SaveProtected(ConfigPath(), text);
    }

    void Start(Source& source, bool cache)
    {
        const auto& id = source.info.id;
        const auto path = CachePath(id);
        std::packaged_task<WorkResult(std::stop_token)> work(
            [id, url = source.url, path, cache](std::stop_token token) {
                return ReadSource(id, url, path, cache, token);
            });
        auto future = work.get_future();
        job.emplace(Job{id, cache, std::move(future), std::jthread(
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
    const auto text = LoadProtected(state_->ConfigPath(), 256 * 1024);
    if (!text) return;
    std::istringstream input(*text);
    std::string version;
    if (!std::getline(input, version) || version != ConfigVersion) return;
    std::vector<Source> sources;
    while (input >> std::ws && !input.eof())
    {
        Source source;
        std::string name, url;
        if (!(input >> std::quoted(source.info.id) >> std::quoted(name) >> std::quoted(url)) ||
            !ValidSourceId(source.info.id) || sources.size() >= MaxSubscriptions)
            return;
        source.info.name = Utf8ToWide(name);
        source.url = Utf8ToWide(url);
        if (source.info.name.empty() || source.info.name.size() > MaxNameLength || !NormalizeLink(source.url) ||
            std::ranges::any_of(sources, [&](const Source& other) {
                return other.info.id == source.info.id || other.url == source.url;
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
        if (source != sources.end())
        {
            const bool current = result.events && (job.cache || SaveProtected(state_->CachePath(job.id), result.body));
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

SubscriptionResult CalendarSubscriptions::Apply(SubscriptionCommand command)
{
    if (!state_->loaded) return SubscriptionResult::Unavailable;
    auto& sources = state_->sources;
    if (command.action == SubscriptionAction::Add)
    {
        if (sources.size() >= MaxSubscriptions) return SubscriptionResult::Limit;
        if (!NormalizeLink(command.url)) return SubscriptionResult::InvalidLink;
        command.name = Trim(std::move(command.name));
        if (command.name.empty() || command.name.size() > MaxNameLength ||
            std::ranges::any_of(command.name, [](wchar_t ch) { return ch < L' '; }))
            return SubscriptionResult::InvalidName;
        if (std::ranges::any_of(sources, [&](const Source& item) { return item.url == command.url; }))
            return SubscriptionResult::Duplicate;
        Source source;
        source.info.id = NewSourceId();
        source.info.name = std::move(command.name);
        source.url = std::move(command.url);
        if (source.info.id.empty() || !state_->Save(&source)) return SubscriptionResult::SaveFailed;
        sources.push_back(std::move(source));
        return SubscriptionResult::Applied;
    }
    const auto source = std::ranges::find_if(sources, [&](const Source& item) { return item.info.id == command.id; });
    if (source == sources.end()) return SubscriptionResult::NotFound;
    const bool inFlight = state_->job && state_->job->id == command.id;
    if (command.action == SubscriptionAction::Refresh)
    {
        if (!inFlight)
        {
            source->nextRefresh = {};
            source->info.status = SubscriptionStatus::Pending;
        }
        return SubscriptionResult::Applied;
    }
    if (command.action != SubscriptionAction::Remove) return SubscriptionResult::Unavailable;
    if (!state_->Save(nullptr, command.id)) return SubscriptionResult::SaveFailed;
    if (inFlight) state_->job->worker.request_stop();
    sources.erase(source);
    std::error_code error;
    std::filesystem::remove(state_->CachePath(command.id), error);
    return SubscriptionResult::Applied;
}

void CalendarSubscriptions::AppendEvents(std::vector<CalendarEvent>& result,
    const std::string& from, const std::string& to) const
{
    for (const auto& source : state_->sources)
    {
        const auto begin = std::ranges::lower_bound(source.events, from, {}, &CalendarEvent::date);
        const auto end = std::ranges::upper_bound(begin, source.events.end(), to, {}, &CalendarEvent::date);
        result.insert(result.end(), begin, end);
    }
}

std::optional<CalendarEvent> CalendarSubscriptions::EventById(const std::string& id) const
{
    for (const auto& source : state_->sources)
    {
        const auto event = std::ranges::find(source.events, id, &CalendarEvent::id);
        if (event != source.events.end()) return *event;
    }
    return std::nullopt;
}

} // namespace snowdesktop::calendar
