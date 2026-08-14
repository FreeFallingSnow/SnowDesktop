#include "widget_app_task_executor.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace
{
using snowdesktop::widget_runtime::WidgetAppCatalogEntry;
using snowdesktop::widget_runtime::WidgetAppTaskExecutor;
using snowdesktop::widget_runtime::MakeWidgetAppReference;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<snowdesktop::widget_runtime::WidgetAppSearchCompletion>
WaitFor(WidgetAppTaskExecutor& executor)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        auto results = executor.DrainCompletions();
        if (!results.empty()) return results;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}

WidgetAppCatalogEntry App(const char* id, const char* title,
    const char* folded, const char* pinyin, const char* initials)
{
    WidgetAppCatalogEntry result;
    result.id = id;
    result.title = title;
    result.launchTarget = std::string("shell:AppsFolder\\") + id;
    result.foldedTitle = folded;
    result.pinyinFull = pinyin;
    result.pinyinInitials = initials;
    return result;
}

void TestRankedPagination()
{
    WidgetAppTaskExecutor executor;
    std::vector<WidgetAppCatalogEntry> catalog{
        App("paint", "Paint", "PAINT", "PAINT", "PAINT"),
        App("notes", "Snow Notes", "SNOW NOTES", "SNOWNOTES", "SN"),
        App("weather", "Snow Weather", "SNOW WEATHER", "SNOWWEATHER", "SW"),
        App("music", "Music", "MUSIC", "MUSIC", "MUSIC"),
    };
    Check(executor.StartSearch(11, "SNOW", "SNOW", 0, 1, 7,
            catalog),
        "valid app search must start");
    auto first = WaitFor(executor);
    Check(first.size() == 1 && first[0].ok &&
            first[0].catalogRevision == 7 &&
            first[0].items.size() == 1 &&
            first[0].items[0].id == "notes" &&
            first[0].hasMore && first[0].nextOffset == 1,
        "search must rank and paginate immutable catalog results");

    Check(executor.StartSearch(12, "SNOW", "SNOW", 1, 2, 7,
            std::move(catalog)),
        "next app search page must start");
    auto second = WaitFor(executor);
    Check(second.size() == 1 && second[0].ok &&
            second[0].items.size() == 1 &&
            second[0].items[0].id == "weather" &&
            !second[0].hasMore && second[0].nextOffset == 2,
        "offset must continue the ranked result sequence");
}

void TestPinyinAndCancellation()
{
    WidgetAppTaskExecutor executor;
    std::vector<WidgetAppCatalogEntry> catalog{
        App("wechat", "WeChat", "WECHAT", "WEIXIN", "WX"),
        App("word", "Word", "WORD", "WORD", "WORD"),
    };
    Check(executor.StartSearch(21, "WX", "WX", 0, 10, 9,
            catalog),
        "pinyin-initial search must start");
    auto pinyin = WaitFor(executor);
    Check(pinyin.size() == 1 && pinyin[0].ok &&
            pinyin[0].items.size() == 1 &&
            pinyin[0].items[0].id == "wechat",
        "precomputed pinyin initials must participate in ranking");

    Check(executor.StartSearch(22, "WORD", "WORD", 0, 10, 9,
            std::move(catalog)) && executor.Cancel(22),
        "owned app search must be cancelable");
    auto canceled = WaitFor(executor);
    Check(canceled.size() == 1 && !canceled[0].ok &&
            canceled[0].error == "canceled" &&
            canceled[0].items.empty(),
        "canceled searches must not publish catalog data");
}

void TestInputLimits()
{
    WidgetAppTaskExecutor executor;
    Check(!executor.StartSearch(0, "A", "A", 0, 10, 1, {}),
        "zero task IDs must be rejected");
    Check(!executor.StartSearch(1, "", "", 0, 10, 1, {}),
        "empty queries must be rejected");
    Check(!executor.StartSearch(1, "A", "A", 0, 101, 1, {}),
        "result limits above 100 must be rejected");
    Check(!executor.StartSearch(1, "A", "A", 10001, 10, 1, {}),
        "offsets above the pagination bound must be rejected");
}

void TestOpaqueReferences()
{
    const std::string first = MakeWidgetAppReference(
        "Contoso.Reader_123!App");
    const std::string repeated = MakeWidgetAppReference(
        "Contoso.Reader_123!App");
    const std::string second = MakeWidgetAppReference(
        "Contoso.Writer_123!App");
    Check(first.starts_with("app:") && first.size() == 36 &&
            first == repeated && first != second &&
            first.find("Contoso") == std::string::npos,
        "application references must be stable, opaque, and identity-specific");
    Check(MakeWidgetAppReference({}).empty(),
        "empty catalog identities must not create usable references");
}
}

int main()
{
    TestRankedPagination();
    TestPinyinAndCancellation();
    TestInputLimits();
    TestOpaqueReferences();
    std::cout << "widget app task executor tests passed\n";
    return 0;
}
