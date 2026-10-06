#include "test_source_boundary.h"
#include "../src/winui/widgets_page_backend_state.h"
#include "../src/winui/source_search_worker.h"
#include "../src/common/bounded_file_query.h"

#include <chrono>
#include <future>

namespace
{
int failures = 0;
void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestOutstandingOperationLedgerBehavior()
{
    using namespace snowdesktop::winui::widgets_page_backend_detail;
    Check(ShouldUnsubscribeWorkshopBeforeUninstall(true, true, true) &&
            !ShouldUnsubscribeWorkshopBeforeUninstall(false, true, true) &&
            !ShouldUnsubscribeWorkshopBeforeUninstall(true, false, true) &&
            !ShouldUnsubscribeWorkshopBeforeUninstall(true, true, false),
        "Workshop unsubscription is optional and local uninstall remains available without its capability");

    ReviewedPackageFileIdentity reviewedIdentity;
    reviewedIdentity.volumeSerialNumber = 9;
    reviewedIdentity.fileId[0] = 42;
    ReviewedPackageFileIdentity sameIdentity = reviewedIdentity;
    ReviewedPackageFileIdentity replacementIdentity = reviewedIdentity;
    replacementIdentity.fileId[0] = 43;
    ReviewedPackageFileIdentity otherVolume = reviewedIdentity;
    otherVolume.volumeSerialNumber = 10;
    Check(reviewedIdentity == sameIdentity &&
            reviewedIdentity != replacementIdentity &&
            reviewedIdentity != otherVolume,
        "reviewed package identity binds both volume and FileId");

    OutstandingOperationLedger ledger;
    const OutstandingOperationIdentity search{
        7, 11, 101, OutstandingOperationKind::Search};
    const OutstandingOperationIdentity synchronization{
        7, 11, 102, OutstandingOperationKind::SourceSynchronization};
    const OutstandingOperationIdentity unsubscribe{
        7, 11, 103, OutstandingOperationKind::WorkshopUnsubscribe};

    Check(ledger.Begin(search) && ledger.Begin(synchronization) &&
            ledger.Begin(unsubscribe) && ledger.Busy() &&
            ledger.Contains(search.taskId) &&
            ledger.Contains(synchronization) &&
            !ledger.Contains({7, 12, synchronization.taskId,
                OutstandingOperationKind::SourceSynchronization}) &&
            !ledger.Contains({7, 11, synchronization.taskId,
                OutstandingOperationKind::WorkshopUnsubscribe}),
        "nonterminal search and synchronization operations close the mutation gate");
    Check(!ledger.Complete({7, 12, search.taskId,
                OutstandingOperationKind::Search}) &&
            ledger.Busy() && ledger.Contains(search.taskId),
        "a completion from another activation cannot release an old operation");
    Check(ledger.Complete(search) && ledger.Busy() &&
            !ledger.Contains(search.taskId) &&
            ledger.Tasks(OutstandingOperationKind::Search).empty(),
        "the exact terminal search completion releases only its own ledger entry");
    Check(ledger.Complete(synchronization) && ledger.Busy() &&
            !ledger.Contains(synchronization) &&
            ledger.Contains(unsubscribe),
        "a late synchronization completion releases its detached page operation without releasing a concurrent unsubscribe");
    Check(ledger.Complete(unsubscribe) && !ledger.Busy(),
        "the mutation gate opens only after every background operation is terminal");
}

void TestBoundedLibraryIo()
{
    using Query = snowdesktop::BoundedFileQuery<int>;
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future().share();
    auto cache = std::make_unique<Query>();
    std::atomic<int> calls = 0;
    auto ticket = cache->Request(L"offline-library", [&entered, gate, &calls] {
        ++calls;
        entered.set_value();
        gate.wait();
        return 7;
    });
    Check(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "library I/O reaches the controlled redirector boundary");
    auto same = cache->Request(L"offline-library", [&calls] { ++calls; return 99; });
    Check(same == ticket && calls == 1,
        "repeated searches share one blocked library worker");
    Check(!Query::Wait(ticket, Query::Clock::now() + std::chrono::milliseconds(10)),
        "a blocked library returns no authority at the caller deadline");
    auto healthy = cache->Request(L"local-library", [] { return 11; });
    Check(Query::Wait(healthy, Query::Clock::now() + std::chrono::seconds(2)) == 11,
        "an unavailable library does not prevent a healthy root from completing");
    const auto beforeClose = Query::Clock::now();
    cache.reset();
    Check(Query::Clock::now() - beforeClose < std::chrono::seconds(1),
        "cache destruction cannot join blocked filesystem I/O");
    release.set_value();
    Check(Query::Wait(ticket, Query::Clock::now() + std::chrono::seconds(2)) == 7,
        "the retained result remains valid after the caller/cache is destroyed");
}

struct SearchResult { bool cancelled = false; };
struct ExitSignal
{
    std::promise<void> destroyed;
    ~ExitSignal() { destroyed.set_value(); }
};
struct SearchWork
{
    std::uint64_t taskId = 0;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::function<void(SearchResult)> completion;
    std::shared_ptr<ExitSignal> retained;
};

void TestSearchCloseAndCancellation()
{
    using Worker = snowdesktop::winui::widgets_page_backend_detail::
        SourceSearchWorker<SearchWork, SearchResult>;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto worker = std::make_unique<Worker>([&entered, gate](SearchWork&) {
        entered.set_value();
        gate.wait();
        return SearchResult{};
    });
    auto retained = std::make_shared<ExitSignal>();
    auto destroyed = retained->destroyed.get_future();
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    std::atomic<int> activeCompletions = 0, displacedCompletions = 0;
    Check(worker->Submit({1, cancellation,
        [&activeCompletions](auto) { ++activeCompletions; }, retained}),
        "production search worker accepts the original operation");
    retained.reset();
    Check(entered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "search is actively querying before cancellation/close");
    Check(worker->Submit({2, std::make_shared<std::atomic_bool>(false),
        [&displacedCompletions](auto result) { if (result.cancelled) ++displacedCompletions; }, {}}) &&
        worker->Submit({3, std::make_shared<std::atomic_bool>(false), {}, {}}) &&
        displacedCompletions == 1,
        "superseding pending search delivers exactly its cancelled terminal result");
    Check(worker->RequestCancel(1) && cancellation->load() && activeCompletions == 0,
        "active cancellation keeps its operation pending until I/O actually returns");
    const auto beforeClose = std::chrono::steady_clock::now();
    worker->Shutdown();
    worker.reset();
    Check(std::chrono::steady_clock::now() - beforeClose < std::chrono::seconds(1),
        "closing settings cannot synchronously join an active provider");
    release.set_value();
    Check(destroyed.wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
        activeCompletions == 0,
        "closed search retains its input until exit and never calls a released page");

    std::promise<void> cancelEntered, cancelRelease;
    auto cancelGate = cancelRelease.get_future().share();
    std::promise<bool> completed;
    auto completion = completed.get_future();
    Worker live([&cancelEntered, cancelGate](SearchWork&) {
        cancelEntered.set_value();
        cancelGate.wait();
        return SearchResult{};
    });
    Check(live.Submit({41, std::make_shared<std::atomic_bool>(false),
        [&completed](auto result) { completed.set_value(result.cancelled); }, {}}),
        "live page starts the cancellation terminal-result scenario");
    Check(cancelEntered.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
        live.RequestCancel(41), "live page cancels the active operation");
    cancelRelease.set_value();
    Check(completion.wait_for(std::chrono::seconds(2)) == std::future_status::ready && completion.get(),
        "active query exit delivers the cancellation needed to release the mutation gate");
}

}

int main(int argc, char** argv)
{
    TestOutstandingOperationLedgerBehavior();
    TestBoundedLibraryIo();
    TestSearchCloseAndCancellation();
    Check(argc == 2, "source root is supplied");
    if (argc == 2)
        Check(snowdesktop::test::CheckSourceBoundaries(argv[1], {
            {"src/winui/widgets_page_backend.cpp", "", "",
             {"ContentDialog", "FileOpenPicker", "ShellExecute", "QuerySteamWorkshopSubscriptions(",
              "ApplySteamWorkshopSubscriptions(", "ExportDevelopmentPackageFile(", "schemaVersion =", "apiVersion =", "luaopen_", "ImGui"}},
        }), "Widgets backend ownership boundaries");
    if (!failures)
        std::cout << "Widget operation, source worker lifecycle, bounded library I/O and source boundaries passed; page actions were not exercised.\n";
    return failures ? 1 : 0;
}
