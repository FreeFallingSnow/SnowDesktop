#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetAppCatalogEntry
{
    std::string id;
    std::string title;
    std::string launchTarget;
    std::string foldedTitle;
    std::string pinyinFull;
    std::string pinyinInitials;
    std::string source = "Applications";
    std::string type = "application";
};

struct WidgetAppSearchResult
{
    std::string id;
    std::string title;
    std::string launchTarget;
    std::string source;
    std::string type;
};

struct WidgetAppSearchCompletion
{
    std::uint64_t id = 0;
    std::uint64_t catalogRevision = 0;
    bool ok = false;
    std::string error;
    std::vector<WidgetAppSearchResult> items;
    std::size_t nextOffset = 0;
    bool hasMore = false;
};

/** Searches immutable app-catalog copies away from the desktop UI thread. */
class WidgetAppTaskExecutor
{
public:
    WidgetAppTaskExecutor() = default;
    ~WidgetAppTaskExecutor();

    WidgetAppTaskExecutor(const WidgetAppTaskExecutor&) = delete;
    WidgetAppTaskExecutor& operator=(
        const WidgetAppTaskExecutor&) = delete;

    bool StartSearch(std::uint64_t id,
        std::string foldedQuery, std::string pinyinQuery,
        std::size_t offset, std::size_t limit,
        std::uint64_t catalogRevision,
        std::vector<WidgetAppCatalogEntry> catalog);
    bool Cancel(std::uint64_t id);
    std::vector<WidgetAppSearchCompletion> DrainCompletions();
    std::size_t ActiveCount() const;

private:
    struct Request
    {
        std::uint64_t id = 0;
        std::string foldedQuery;
        std::string pinyinQuery;
        std::size_t offset = 0;
        std::size_t limit = 0;
        std::uint64_t catalogRevision = 0;
        std::vector<WidgetAppCatalogEntry> catalog;
    };

    static int MatchRank(const WidgetAppCatalogEntry& entry,
        const Request& request) noexcept;
    void WorkerMain(std::stop_token stopToken);

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Request> requests_;
    std::unordered_set<std::uint64_t> active_;
    std::unordered_set<std::uint64_t> canceled_;
    std::vector<WidgetAppSearchCompletion> completions_;
    std::jthread worker_;
    bool stopping_ = false;
};
}
