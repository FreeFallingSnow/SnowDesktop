#include "drag_drop/drop_staging.h"
#include <fstream>
#include <iostream>

int RunDropStagingTests()
{
    namespace fs = std::filesystem;
    namespace staging = snowdesktop::drop_staging;
    int failures = 0;
    const auto check = [&](bool ok, const char* reason) { if (!ok) { ++failures; std::cerr << "FAILED: " << reason << '\n'; } };
    const auto root = fs::temp_directory_path() / (L"SDDropStagingTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    const auto retired = root / L"session-retired";
    fs::create_directory(retired);
    std::ofstream(retired / L".owner.lock") << "";
    std::ofstream(retired / L"payload.txt") << "abandoned content";
    const auto unrelated = root / L"user-project";
    fs::create_directory(unrelated);
    std::ofstream(unrelated / L"payload.txt") << "keep";
    fs::path firstPath;
    {
        staging::Session first(root);
        firstPath = first.Path();
        check(!firstPath.empty() && fs::is_directory(firstPath), "a drop session creates its own scratch directory");
        check(!fs::exists(retired) && fs::exists(unrelated / L"payload.txt"), "recover abandoned owned scratch without deleting unrelated content");
        const auto file = firstPath / L"dropped.txt";
        std::ofstream(file) << "source content";
        {
            staging::Session second(root);
            staging::CollectRetired(root);
            check(second.Path() != firstPath && fs::exists(file), "concurrent sessions are isolated and a live owner prevents collection");
            const auto destination = root / L"published.txt";
            check(CopyFileW(file.c_str(), destination.c_str(), TRUE) != FALSE, "publish a real file from the staged payload");
        }
        check(fs::exists(file), "ending another session does not discard an in-flight payload");
    }
    check(!fs::exists(firstPath) && fs::exists(root / L"published.txt"), "completion removes scratch and preserves the published file");
    check(staging::Directory().is_absolute() && !staging::Directory().empty(), "production staging resolves to an independent temporary session");
    staging::RemoveTree(root);
    return failures;
}
