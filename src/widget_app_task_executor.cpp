#include "widget_app_task_executor.h"

#include <array>
#include <iomanip>
#include <sstream>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr int kNoMatchRank = 9;

std::uint64_t HashReference(
    std::string_view value, std::uint64_t seed) noexcept
{
    std::uint64_t hash = seed;
    for (const unsigned char byte : value)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool StartsWith(const std::string& value, const std::string& prefix) noexcept
{
    return value.size() >= prefix.size() &&
        value.compare(0, prefix.size(), prefix) == 0;
}
}

std::string MakeWidgetAppReference(std::string_view catalogId)
{
    if (catalogId.empty()) return {};
    std::ostringstream stream;
    stream << "app:" << std::hex << std::setfill('0')
           << std::setw(16)
           << HashReference(catalogId, 14695981039346656037ull)
           << std::setw(16)
           << HashReference(catalogId, 1099511628211ull);
    return stream.str();
}

std::string MakeWidgetItemReference(
    std::string_view scope, std::string_view identity)
{
    if (scope.empty() || identity.empty()) return {};
    std::uint64_t first = HashReference(
        scope, 14695981039346656037ull);
    first = HashReference(identity, first);
    std::uint64_t second = HashReference(
        scope, 1099511628211ull);
    second = HashReference(identity, second);
    std::ostringstream stream;
    stream << "item:" << std::hex << std::setfill('0')
           << std::setw(16) << first
           << std::setw(16) << second;
    return stream.str();
}

WidgetAppTaskExecutor::~WidgetAppTaskExecutor()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

bool WidgetAppTaskExecutor::StartSearch(std::uint64_t id,
    std::string foldedQuery, std::string pinyinQuery,
    std::size_t offset, std::size_t limit,
    std::uint64_t catalogRevision,
    std::vector<WidgetAppCatalogEntry> catalog)
{
    if (id == 0 || foldedQuery.empty() || foldedQuery.size() > 512 ||
        pinyinQuery.size() > 512 || limit == 0 || limit > 100 ||
        offset > 10000 || catalog.size() > 20000)
        return false;
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id)) return false;
    active_.insert(id);
    requests_.push_back({ id, std::move(foldedQuery),
        std::move(pinyinQuery), offset, limit, catalogRevision,
        std::move(catalog) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return true;
}

bool WidgetAppTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (active_.contains(id))
    {
        canceled_.insert(id);
        condition_.notify_all();
        return true;
    }
    const auto completion = std::find_if(
        completions_.begin(), completions_.end(),
        [id](const auto& candidate) { return candidate.id == id; });
    if (completion == completions_.end()) return false;
    completion->ok = false;
    completion->error = "canceled";
    completion->items.clear();
    completion->nextOffset = 0;
    completion->hasMore = false;
    return true;
}

std::vector<WidgetAppSearchCompletion>
WidgetAppTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetAppTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

int WidgetAppTaskExecutor::MatchRank(
    const WidgetAppCatalogEntry& entry,
    const Request& request) noexcept
{
    if (entry.foldedTitle == request.foldedQuery) return 0;
    if (!request.pinyinQuery.empty())
    {
        if (entry.pinyinFull == request.pinyinQuery) return 1;
        if (entry.pinyinInitials == request.pinyinQuery) return 2;
        if (StartsWith(entry.pinyinFull, request.pinyinQuery)) return 3;
        if (StartsWith(entry.pinyinInitials, request.pinyinQuery)) return 4;
    }
    if (StartsWith(entry.foldedTitle, request.foldedQuery)) return 5;
    if (!request.pinyinQuery.empty())
    {
        if (entry.pinyinFull.find(request.pinyinQuery) != std::string::npos)
            return 6;
        if (entry.pinyinInitials.find(request.pinyinQuery) !=
            std::string::npos)
            return 7;
    }
    return entry.foldedTitle.find(request.foldedQuery) != std::string::npos
        ? 8 : kNoMatchRank;
}

void WidgetAppTaskExecutor::WorkerMain(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Request request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] {
                return stopToken.stop_requested() || !requests_.empty();
            });
            if (stopToken.stop_requested()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (canceled_.erase(request.id) > 0)
            {
                active_.erase(request.id);
                completions_.push_back({ request.id,
                    request.catalogRevision, false, "canceled" });
                continue;
            }
        }

        std::array<std::vector<const WidgetAppCatalogEntry*>,
            kNoMatchRank> buckets;
        for (const auto& entry : request.catalog)
        {
            const int rank = MatchRank(entry, request);
            if (rank >= 0 && rank < kNoMatchRank)
                buckets[static_cast<std::size_t>(rank)].push_back(&entry);
        }

        WidgetAppSearchCompletion completion;
        completion.id = request.id;
        completion.catalogRevision = request.catalogRevision;
        completion.ok = true;
        std::size_t matched = 0;
        bool done = false;
        for (const auto& bucket : buckets)
        {
            for (const WidgetAppCatalogEntry* entry : bucket)
            {
                if (matched++ < request.offset) continue;
                if (completion.items.size() >= request.limit)
                {
                    completion.hasMore = true;
                    done = true;
                    break;
                }
                completion.items.push_back({ entry->id, entry->title,
                    entry->launchTarget, entry->source, entry->type });
            }
            if (done) break;
        }
        completion.nextOffset = request.offset + completion.items.size();

        {
            std::scoped_lock lock(mutex_);
            if (canceled_.erase(request.id) > 0)
            {
                completion.ok = false;
                completion.error = "canceled";
                completion.items.clear();
                completion.hasMore = false;
            }
            active_.erase(request.id);
            completions_.push_back(std::move(completion));
        }
    }
}

WidgetExternalSearchTaskExecutor::~WidgetExternalSearchTaskExecutor()
{
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    if (worker_.joinable())
    {
        worker_.request_stop();
        condition_.notify_all();
        worker_.join();
    }
}

bool WidgetExternalSearchTaskExecutor::StartSearch(
    std::uint64_t id, std::string query,
    std::size_t offset, std::size_t limit, Provider provider)
{
    if (id == 0 || query.empty() || query.size() > 256 ||
        offset > 100 || limit == 0 || limit > 100 || !provider)
        return false;
    std::scoped_lock lock(mutex_);
    if (stopping_ || active_.contains(id)) return false;
    active_.insert(id);
    requests_.push_back({ id, std::move(query), offset, limit,
        std::move(provider) });
    if (!worker_.joinable())
    {
        worker_ = std::jthread(
            [this](std::stop_token stopToken) {
                WorkerMain(stopToken);
            });
    }
    condition_.notify_one();
    return true;
}

bool WidgetExternalSearchTaskExecutor::Cancel(std::uint64_t id)
{
    std::scoped_lock lock(mutex_);
    if (active_.contains(id))
    {
        canceled_.insert(id);
        condition_.notify_all();
        return true;
    }
    const auto completion = std::find_if(
        completions_.begin(), completions_.end(),
        [id](const auto& candidate) { return candidate.id == id; });
    if (completion == completions_.end()) return false;
    completion->ok = false;
    completion->error = "canceled";
    completion->items.clear();
    completion->nextOffset = 0;
    completion->hasMore = false;
    return true;
}

std::vector<WidgetAppSearchCompletion>
WidgetExternalSearchTaskExecutor::DrainCompletions()
{
    std::scoped_lock lock(mutex_);
    return std::exchange(completions_, {});
}

std::size_t WidgetExternalSearchTaskExecutor::ActiveCount() const
{
    std::scoped_lock lock(mutex_);
    return active_.size();
}

void WidgetExternalSearchTaskExecutor::WorkerMain(
    std::stop_token stopToken)
{
    constexpr std::size_t MaximumProviderResults = 200;
    while (!stopToken.stop_requested())
    {
        Request request;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] {
                return stopToken.stop_requested() || !requests_.empty();
            });
            if (stopToken.stop_requested()) break;
            request = std::move(requests_.front());
            requests_.pop_front();
            if (canceled_.erase(request.id) > 0)
            {
                active_.erase(request.id);
                completions_.push_back(
                    { request.id, 1, false, "canceled" });
                continue;
            }
        }

        WidgetAppSearchCompletion completion;
        completion.id = request.id;
        completion.catalogRevision = 1;
        try
        {
            const std::size_t requested = MaximumProviderResults;
            std::vector<WidgetAppSearchResult> found =
                request.provider(request.query, requested);
            if (found.size() > requested) found.resize(requested);
            const std::size_t begin = std::min(
                request.offset, found.size());
            const std::size_t end = std::min(
                found.size(), begin + request.limit);
            completion.items.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index)
                completion.items.push_back(std::move(found[index]));
            completion.nextOffset =
                request.offset + completion.items.size();
            completion.hasMore = end < found.size();
            completion.ok = true;
        }
        catch (...)
        {
            completion.ok = false;
            completion.error = "providerFailed";
        }

        {
            std::scoped_lock lock(mutex_);
            if (canceled_.erase(request.id) > 0)
            {
                completion.ok = false;
                completion.error = "canceled";
                completion.items.clear();
                completion.hasMore = false;
            }
            active_.erase(request.id);
            completions_.push_back(std::move(completion));
        }
    }
}
}
