#include "test_source_boundary.h"
#include "../src/winui/backup_operation_control.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace
{
using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestAtomicCancellationBoundary()
{
    using snowdesktop::winui::BackupOperationControl;
    using snowdesktop::winui::BackupOperationPhase;

    {
        auto control = std::make_shared<BackupOperationControl>();
        std::promise<BackupOperationPhase> observed;
        auto ready = observed.get_future();
        std::jthread worker([control, &observed] {
            control->WaitUntilStarted();
            observed.set_value(control->Phase());
        });
        Check(control->Phase() == BackupOperationPhase::Starting &&
                !control->TryBeginNonInterruptible(),
            "an unpublished operation cannot cross the commit gate");
        Check(control->RequestCancellation(),
            "a synchronous snapshot callback can cancel during Starting");
        Check(ready.wait_for(1s) == std::future_status::ready &&
                ready.get() == BackupOperationPhase::CancellationRequested &&
                !control->TryBeginNonInterruptible(),
            "Starting cancellation wakes the worker and permanently closes the commit gate");
    }

    {
        BackupOperationControl control;
        Check(control.EnableCancellation() &&
                control.TryBeginNonInterruptible() &&
                !control.RequestCancellation() &&
                control.NonInterruptible(),
            "an operation that wins the commit gate cannot later report cancellation as accepted");
        control.Finish();
        Check(control.Phase() == BackupOperationPhase::Finished,
            "completion seals the operation phase");
    }

    for (int attempt = 0; attempt < 64; ++attempt)
    {
        BackupOperationControl control;
        Check(control.EnableCancellation(),
            "a fresh operation enters its cancellable phase");
        std::atomic_bool start{false};
        bool cancelled = false;
        bool committed = false;
        std::jthread cancelThread([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            cancelled = control.RequestCancellation();
        });
        std::jthread commitThread([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            committed = control.TryBeginNonInterruptible();
        });
        start.store(true, std::memory_order_release);
        cancelThread.join();
        commitThread.join();
        Check(cancelled != committed,
            "exactly one side wins a concurrent cancel-versus-commit race");
    }
}
} // namespace

int main(int argc, char** argv)
{
    TestAtomicCancellationBoundary();
    Check(argc == 2,
        "source root is supplied for the Backup/data backend contract");
    if (argc == 2)
        Check(snowdesktop::test::CheckSourceBoundaries(argv[1], {
            {"src/winui/backup_data_page_backend.cpp", "", "",
             {"backup.json", "snowbackup_manifest", "layout_storage::SaveDocument", "atomic_file::WriteAll"}},
            {"src/winui/backup_data_page_backend.cpp", "void CompleteQueuedReplacement", "void Finish(",
             {"FlushPending(", "FlushAll("}},
            {"src/winui/backup_data_page_backend.cpp", "void Deactivate() noexcept", "BackupDataPageBackend::BackupDataPageBackend(",
             {".join(", "WaitForSingleObject("}},
            {"src/app/app_settings_apply.cpp", "DesktopApp::SetTemporaryGridInitialization",
             "class DesktopApp::SettingsHostActionsAdapter", {"ClearLayoutAndStorage", "remove_all"}},
            {"src/windows_desktop_layout.cpp", "", "",
             {"->SelectAndPositionItems(", "->SetCurrentFolderFlags(", "->SetViewModeAndIconSize(", "LVM_SETITEMPOSITION"}},
        }), "backup data ownership boundaries");

    if (failures != 0)
    {
        std::cerr << failures
                  << " Backup/data backend check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Backup cancellation rules and source boundaries passed; backend operations were not exercised\n";
    return EXIT_SUCCESS;
}
