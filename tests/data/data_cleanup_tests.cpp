#include "data/data_cleanup.h"
#include "data/atomic_file.h"
#include "drag_drop/drop_staging.h"
#include <chrono>
#include <iostream>

int RunDataCleanupTests()
{
    namespace fs = std::filesystem;
    int failures = 0;
    const auto check = [&](bool ok, const char* reason) { if (!ok) { ++failures; std::cerr << "FAILED: " << reason << '\n'; } };
    const auto root = fs::temp_directory_path() / (L"SDCleanupTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto write = [&](const fs::path& path, std::string_view bytes = "keep") {
        fs::create_directories(path.parent_path());
        check(snowdesktop::atomic_file::WriteAll(path, bytes), "write isolated cleanup fixture");
    };
    write(root / L"SnowDesktop.layout.json", "{}");
    write(root / L"SnowDesktop.storage.json", "{}");
    write(root / L"DropContent" / L"unused.txt");
    const auto referenced = root / L"DropContent" / L"referenced.txt";
    write(referenced);
    write(root / L"backups" / L"valuable.json");
    write(root / L"widgets" / L"storage" / L"value.json");
    write(root / L"migrations" / L"done");
    write(root / L"unknown" / L"user.txt");
    const auto dead = root / L"ThemeWorkshop" / L"staging" / L"upload-4294967295-1-preview";
    const auto live = root / L"ThemeWorkshop" / L"staging" / (L"upload-" + std::to_wstring(GetCurrentProcessId()) + L"-2-preview");
    write(dead / L"cover.png"); write(live / L"cover.png");
    std::stop_source cancelled; cancelled.request_stop();
    check(snowdesktop::data_cleanup::Collect(root, {}, cancelled.get_token()) == 0 && fs::exists(root / L"DropContent" / L"unused.txt"),
        "cancelled maintenance cannot remove data from a superseded snapshot");
    snowdesktop::data_cleanup::Collect(root, {referenced});
    check(!fs::exists(root / L"DropContent" / L"unused.txt") && fs::exists(referenced), "retire legacy staging without breaking existing path references");
    check(!fs::exists(dead) && fs::exists(live / L"cover.png"), "retire an exited publish owner and keep the active preparation");
    check(fs::exists(root / L"backups" / L"valuable.json") && fs::exists(root / L"widgets" / L"storage" / L"value.json") &&
        fs::exists(root / L"migrations" / L"done") && fs::exists(root / L"unknown" / L"user.txt"), "maintenance preserves user state, backups, migration markers and unknown directories");
    write(root / L"SnowDesktop.storage.json", "invalid JSON");
    snowdesktop::data_cleanup::Collect(root, {});
    check(fs::exists(referenced), "unreadable saved references prevent legacy staging collection");
    write(root / L"SnowDesktop.storage.json", "{}");
    for (int i = 0; i < 7; ++i)
    {
        const auto dump = root / L"crashdumps" / (i % 2 ? L"wer" : L"") /
            (L"SnowDesktop_" + std::to_wstring(i) + L"_1.dmp");
        write(dump);
        fs::last_write_time(dump, fs::file_time_type::clock::now() - std::chrono::hours(i));
    }
    write(root / L"crashdumps" / L"crash.log");
    write(root / L"crashdumps" / L"other-program.dmp");
    snowdesktop::data_cleanup::Collect(root, {});
    int dumps = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root / L"crashdumps"))
        if (entry.path().filename().wstring().starts_with(L"SnowDesktop_") && entry.path().extension() == L".dmp") ++dumps;
    check(dumps == 5 && fs::exists(root / L"crashdumps" / L"SnowDesktop_0_1.dmp") &&
        fs::exists(root / L"crashdumps" / L"crash.log") && fs::exists(root / L"crashdumps" / L"other-program.dmp"),
        "one retention limit covers owned WER and host dumps while preserving logs and other applications");
    check(!fs::exists(root / L"DropContent"), "legacy DropContent disappears once its last reference is gone");
    snowdesktop::drop_staging::RemoveTree(root);
    return failures;
}
