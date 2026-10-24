#pragma once

#include "widget/runtime/widget_script_read_ahead.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace widget_script_read_ahead_tests
{
inline void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void Await(Predicate predicate, const char* message)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate())
    {
        Require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

struct Fixture
{
    const std::filesystem::path temporary = std::filesystem::temp_directory_path();
    const std::wstring name = L"SnowDesktopScriptReadAhead-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const std::filesystem::path root = temporary / name;
    const std::filesystem::path script = root / L"main.lua";
    const std::filesystem::path manifest = root / L"widget.json";
    const std::filesystem::path retired = temporary / (name + L".retired");
    snowdesktop::widget_runtime::WidgetScriptReadAhead watcher;

    Fixture()
    {
        Require(std::filesystem::create_directory(root), "create isolated read-ahead fixture");
        Write(script, "return { render = function() end }\n");
        Write(manifest, "{\"version\":\"1\"}\n");
    }

    ~Fixture()
    {
        watcher.Stop();
        // The worker owns no host references and releases watches before its
        // running flag clears. Avoid racing fixture cleanup with its last read.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (watcher.Inspect().running && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (const auto& path : {root, retired})
        {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            if (!watcher.Inspect().running && path.parent_path() == temporary &&
                (path.filename() == name || path.filename() == name + L".retired") &&
                attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        }
    }

    static void Write(const std::filesystem::path& path, const std::string& text)
    {
        const auto pending = std::filesystem::path(path.wstring() + L".pending");
        {
            std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
            stream << text;
            stream.close();
            Require(static_cast<bool>(stream), "write source fixture");
        }
        Require(MoveFileExW(pending.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
            "publish complete source fixture without partial-write races");
    }

    static std::string Read(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), {});
    }

    void Ready(std::uint64_t revision)
    {
        Await([&] { const auto snapshot = watcher.Inspect();
            return snapshot.appliedRevision >= revision && snapshot.watches >= 1; },
            "local change watch becomes ready");
    }

    void Prepared(const std::filesystem::path& path, std::uint64_t previous)
    {
        Await([&] { return watcher.PreparedCount(path.wstring()) > previous; },
            "changed source is read ahead through the real directory notification");
    }
};

inline void Run()
{
    Fixture fixture;
    auto& watcher = fixture.watcher;
    Require(watcher.Track(L"first", fixture.script.wstring(), fixture.manifest.wstring()),
        "track first loaded instance");
    fixture.Ready(1);
    Require(watcher.Track(L"second", fixture.script.wstring(), fixture.manifest.wstring()),
        "multiple instances can share one source");
    fixture.Ready(2);
    Require(watcher.Inspect().files == 2 &&
        watcher.PreparedCount(fixture.script.wstring()) == 0 &&
        watcher.PreparedCount(fixture.manifest.wstring()) == 0,
        "tracking shared loaded sources does not reread their baseline revision");

    Fixture::Write(fixture.script, "return { render = function() return 'changed source' end }\n");
    fixture.Prepared(fixture.script, 0);
    Require(Fixture::Read(fixture.script).find("changed source") != std::string::npos,
        "prefetch does not alter script bytes consumed by the synchronous loader");

    const auto preparedScript = watcher.PreparedCount(fixture.script.wstring());
    const auto preparedManifest = watcher.PreparedCount(fixture.manifest.wstring());
    Fixture::Write(fixture.manifest, "{\"version\":\"2\",\"description\":\"new manifest\"}\n");
    fixture.Prepared(fixture.manifest, preparedManifest);
    Require(watcher.PreparedCount(fixture.script.wstring()) == preparedScript,
        "an unrelated source change does not reread the same script revision");

    const auto scans = watcher.Inspect().scans;
    Require(DeleteFileW(fixture.script.c_str()) != FALSE, "delete watched source");
    Await([&] { return watcher.Inspect().scans > scans; }, "observe deletion before recovery");
    Require(!std::filesystem::exists(fixture.script),
        "read-ahead does not recreate a deleted source or replace synchronous missing-file handling");
    const auto afterDeletion = watcher.PreparedCount(fixture.script.wstring());
    Fixture::Write(fixture.script, "this is invalid Lua but remains ordinary source bytes\n");
    fixture.Prepared(fixture.script, afterDeletion);
    Require(Fixture::Read(fixture.script) == "this is invalid Lua but remains ordinary source bytes\n",
        "preparation does not compile invalid Lua or cache a VM failure");

    const auto replacement = fixture.root / L"replacement.lua";
    Fixture::Write(replacement, "return { render = function() return 'replacement' end }\n");
    const auto beforeReplacement = watcher.PreparedCount(fixture.script.wstring());
    Require(MoveFileExW(replacement.c_str(), fixture.script.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
        "atomic editor replacement is allowed while the source is tracked");
    fixture.Prepared(fixture.script, beforeReplacement);
    Require(Fixture::Read(fixture.script).find("replacement") != std::string::npos,
        "replacement bytes remain available to the original synchronous reload");

    const auto beforeFailure = watcher.PreparedCount(fixture.script.wstring());
    const auto beforeFailureCount = watcher.Inspect().failedPreparations;
    HANDLE exclusive = CreateFileW(fixture.script.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(exclusive != INVALID_HANDLE_VALUE, "hold actual read-sharing failure");
    FILETIME modified{};
    const bool previousTimeRead = GetFileTime(exclusive, nullptr, nullptr, &modified) != FALSE;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = modified.dwLowDateTime;
    ticks.HighPart = modified.dwHighDateTime;
    ticks.QuadPart += 10000000;
    modified = {ticks.LowPart, ticks.HighPart};
    const bool timestampChanged = previousTimeRead &&
        SetFileTime(exclusive, nullptr, nullptr, &modified) != FALSE;
    if (!timestampChanged) CloseHandle(exclusive);
    Require(timestampChanged, "change source metadata while reads are denied");
    try
    {
        Await([&] { return watcher.Inspect().failedPreparations > beforeFailureCount; },
            "failed pre-read is observed without delivering a VM result");
        Require(watcher.PreparedCount(fixture.script.wstring()) == beforeFailure,
            "read failure is not reported as successfully prepared");
    }
    catch (...) { CloseHandle(exclusive); throw; }
    CloseHandle(exclusive);
    Fixture::Write(fixture.manifest, "{\"version\":\"3\",\"description\":\"retry\"}\n");
    fixture.Prepared(fixture.script, beforeFailure);
    Require(Fixture::Read(fixture.script).find("replacement") != std::string::npos,
        "a later notification retries failed preparation without changing source bytes");

    const auto beforeDirectoryReplacement = watcher.PreparedCount(fixture.script.wstring());
    Require(MoveFileExW(fixture.root.c_str(), fixture.retired.c_str(),
        MOVEFILE_WRITE_THROUGH) != FALSE,
        "directory notification handles allow an editor to rename the package root");
    Require(std::filesystem::create_directory(fixture.root), "create replacement package root");
    Fixture::Write(fixture.script, "return { render = function() return 'new directory' end }\n");
    Fixture::Write(fixture.manifest, "{\"version\":\"4\"}\n");
    fixture.Prepared(fixture.script, beforeDirectoryReplacement);
    Require(Fixture::Read(fixture.script).find("new directory") != std::string::npos,
        "parent notification restores pre-read on the current directory identity");

    watcher.Forget(L"first");
    fixture.Ready(3);
    Require(watcher.Inspect().files == 2, "removing one instance retains the shared source watch");
    watcher.Forget(L"second");
    Await([&] { const auto snapshot = watcher.Inspect();
        return snapshot.appliedRevision >= 4 && snapshot.files == 0 && snapshot.watches == 0; },
        "unloading the last instance retires its source watches");
    watcher.Stop();
    Await([&] { return !watcher.Inspect().running; }, "read-ahead worker stops without a host callback");
    Require(!watcher.Track(L"late", fixture.script.wstring(), fixture.manifest.wstring()),
        "stopped watcher cannot resurrect a retired engine");
}
}
