#pragma once

namespace native_drag_async_reload_cases
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
using native_drag_commit_cases::Require;
using native_drag_commit_cases::Read;
using native_drag_reload_cases::Find;

inline void PumpOne()
{
    MSG message{};
    if (PeekMessageW(&message, Access::completionWindow, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    else MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    Access::ThrowAsyncException();
}

template<class Predicate>
void Wait(Predicate predicate, const char* failure)
{
    const auto started = GetTickCount64();
    while (!predicate())
    {
        Require(GetTickCount64() - started < 8000, failure);
        PumpOne();
    }
}

inline void WaitQueuedRead()
{
    const auto started = GetTickCount64();
    MSG message{};
    // Hold the real worker message in the OS queue while a later filesystem
    // event invalidates its revision. Do not fabricate a snapshot or result.
    while (!PeekMessageW(&message, Access::completionWindow,
        kBackgroundShellReadyMessage, kBackgroundShellReadyMessage, PM_NOREMOVE))
    {
        Require(GetTickCount64() - started < 8000, "actual native read must publish a completion message before supersession");
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

inline bool Contains(const DesktopApp& app, const fs::path& file)
{
    const auto key = ToUpperInvariant(file.wstring());
    return std::any_of(Access::Items(app).begin(), Access::Items(app).end(),
        [&](const auto& item) { return item.layoutKey == key; });
}

inline void Refresh(DesktopApp& app, bool disk = false)
{
    const auto messages = Access::asyncReadMessages;
    Access::RequestAsync(app, disk);
    Wait([&] { return Access::AsyncIdle(app) && Access::asyncReadMessages > messages; },
        "actual native async read, completion dispatch and model application must finish");
}

inline void CheckMetadata(const DesktopApp& app, const std::vector<std::wstring>& keys)
{
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const auto& item = Find(app, keys[i]);
        Require(item.fileSize && *item.fileSize == ("async-file-" + std::to_string(i)).size() &&
            item.modifiedTime.has_value() && item.absolutePidl.get(),
            "actual native async enumeration must deliver real file metadata and independently owned Shell identity");
    }
}

inline void CheckAdded(DesktopApp& app, const fs::path& file, std::string_view content)
{
    Require(Contains(app, file), "actual native async reload must publish a newly created file");
    const auto& item = Find(app, ToUpperInvariant(file.wstring()));
    Require(item.fileSize && *item.fileSize == content.size() && Access::HasVisibleIcon(app, item) &&
        Access::HasMatchingRuntimeSlot(app, item) && Read(file) == content,
        "actual native async completion must publish the new file's real content, metadata and visible frame");
    for (const auto& other : Access::Items(app))
        if (&other != &item && Access::HasVisibleIcon(app, other))
            Require(item.gridCell.pageId != other.gridCell.pageId || item.gridCell.column != other.gridCell.column ||
                item.gridCell.row != other.gridCell.row, "native asynchronously enumerated files cannot overlap existing ordinary desktop items");
}

inline void Run()
{
    native_drag_commit_cases::Profile profile;
    std::size_t cases = 0;
    std::uint64_t delivered = 0;
    for (const bool secondary : {false, true})
        for (const int source : {0, 1, 2})
            for (const std::size_t count : {1u, 2u})
            {
                auto& context = native_drag_commit_cases::context;
                const auto scenario = "async source=" + std::to_string(source) + " count=" + std::to_string(count) +
                    " secondary=" + std::to_string(secondary);
                context = scenario + " phase=commit";
                const auto directory = profile.root / (L"async-" + std::to_wstring(cases++));
                const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
                const std::vector<fs::path> files{desktop / L"A.txt", desktop / L"B.txt"};
                const auto added = desktop / L"C.txt";
                for (std::size_t i = 0; i < files.size(); ++i)
                { std::ofstream stream(files[i], std::ios::binary); stream << "async-file-" << i; }
                auto& app = profile.Model(secondary, directory / L"layout");
                Access::PopulateModel(app, files, source);
                Access::PrepareReload(app);
                Access::BindAsync(app);
                Require(Access::Save(app), "save actual native model before asynchronous refresh");
                std::vector<std::wstring> keys;
                for (const auto& item : Access::Items(app)) keys.push_back(item.layoutKey);
                Access::BeginModel(app, source, count);
                const auto preview = Access::ObserveModelPreview(app, {secondary ? -650L : 150L, 150});
                Access::DropModel(app);

                context = scenario + " phase=real-read";
                Refresh(app);
                CheckMetadata(app, keys);
                native_drag_reload_cases::Check(app, keys, preview, source, count);

                context = scenario + " phase=timer-drag-fence";
                const auto saved = Read(Access::LayoutPath(app));
                const auto* runtime = Access::Desktop(app);
                const auto timers = Access::asyncShellTimers;
                Access::BeginDesktopGesture(app, keys[0]);
                { std::ofstream stream(added, std::ios::binary); stream << "during-drag"; }
                Access::RequestAsync(app);
                Wait([&] { return Access::asyncShellTimers > timers; }, "real native debounce timer must reach the active-drag fence");
                Require(!Access::AsyncRunning(app) && !Contains(app, added) && Access::Desktop(app) == runtime &&
                    Read(Access::LayoutPath(app)) == saved,
                    "actual native timer must not read, replace the model or save while a drag owns its runtime tree");
                Access::EndActualGesture(app);
                Wait([&] { return Access::AsyncIdle(app) && Contains(app, added); },
                    "native async reload must resume and publish a real new file after the drag ends");
                CheckAdded(app, added, "during-drag");
                native_drag_reload_cases::Check(app, keys, preview, source, count, 3);

                context = scenario + " phase=real-delete";
                Require(fs::remove(added), "remove only the isolated added file before native refresh");
                Refresh(app);
                Require(!Contains(app, added), "actual native async reload must remove the deleted file's runtime entry");
                native_drag_reload_cases::Check(app, keys, preview, source, count);

                context = scenario + " phase=superseded-message";
                const auto messages = Access::asyncReadMessages;
                Access::RequestAsync(app);
                Wait([&] { return Access::AsyncRunning(app); }, "actual native timer must start its background read");
                WaitQueuedRead();
                Access::BeginDesktopGesture(app, keys[0]);
                { std::ofstream stream(added, std::ios::binary); stream << "after-first-read"; }
                Access::RequestAsync(app);
                Access::EndActualGesture(app);
                Wait([&] { return Access::AsyncIdle(app) && Contains(app, added); },
                    "actual native async reload must discard superseded completion and apply a fresh real directory read");
                Require(Access::asyncReadMessages >= messages + 2,
                    "native supersession must deliver both old and current real worker completions");
                CheckAdded(app, added, "after-first-read");
                native_drag_reload_cases::Check(app, keys, preview, source, count, 3);
                Require(fs::remove(added), "remove the isolated supersession file");
                Refresh(app);

                context = scenario + " phase=async-disk-replacement";
                auto& writer = profile.Model(secondary, directory / L"layout");
                Access::Load(writer);
                Access::ReadSnapshot(writer, files, true);
                Require(Access::WriteReplacementPositions(writer, keys, count), "save replacement before native async disk reload");
                Refresh(app, true);
                Require(Access::ReloadSettingsAgree(app), "native async disk reload must synchronize the actual settings mirrors");
                for (std::size_t i = 0; i < count; ++i)
                {
                    const auto& item = Find(app, keys[i]);
                    Require(item.gridCell.pageId == L"__page:1" && item.gridCell.column == static_cast<int>(i) * 2 + 1 &&
                        item.gridCell.row == 2 && Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item),
                        "native async disk reload must display the newer persisted file positions");
                }
                CheckMetadata(app, keys);
                for (std::size_t i = 0; i < files.size(); ++i)
                    Require(Read(files[i]) == "async-file-" + std::to_string(i), "native async reload preserves the logical files' real bytes");
                delivered += Access::asyncReadMessages;
                Access::StopModel(writer); Access::StopModel(app);
                for (const auto& file : files) fs::remove(file);
            }
    std::cout << cases << " actual native asynchronous read, message, revision and restoration scenes passed; "
        << delivered << " real background messages dispatched\n";
}
}
