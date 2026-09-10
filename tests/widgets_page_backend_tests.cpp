#include "test_source_boundary.h"
#include "../src/winui/widgets_page_backend_state.h"

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

}

int main(int argc, char** argv)
{
    TestOutstandingOperationLedgerBehavior();
    Check(argc == 2, "source root is supplied");
    if (argc == 2)
        Check(snowdesktop::test::CheckSourceBoundaries(argv[1], {
            {"src/winui/widgets_page_backend.cpp", "", "",
             {"ContentDialog", "FileOpenPicker", "ShellExecute", "QuerySteamWorkshopSubscriptions(",
              "ApplySteamWorkshopSubscriptions(", "schemaVersion =", "apiVersion =", "luaopen_", "ImGui"}},
        }), "Widgets backend ownership boundaries");
    if (!failures)
        std::cout << "Widget operation rules and source boundaries passed; page actions were not exercised.\n";
    return failures ? 1 : 0;
}
