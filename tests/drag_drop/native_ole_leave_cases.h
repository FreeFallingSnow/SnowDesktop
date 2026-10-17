#pragma once

namespace native_ole_leave_cases
{
using namespace native_ole_file_cases;

enum class Sequence { Outside, Expire, ReturnOver, ReplaceSource, Drop, NewGrace };
inline constexpr Sequence sequences[]{Sequence::Outside, Sequence::Expire, Sequence::ReturnOver,
    Sequence::ReplaceSource, Sequence::Drop, Sequence::NewGrace};

inline std::vector<fs::path> Files(const fs::path& directory, std::size_t count, const std::string& name)
{
    fs::create_directories(directory);
    std::vector<fs::path> files{directory / (name + ".txt")};
    { std::ofstream stream(files[0], std::ios::binary); stream << name << "-file"; }
    if (count == 2)
    {
        files.push_back(directory / (name + "-folder"));
        fs::create_directory(files[1]);
        std::ofstream stream(files[1] / L"nested.txt", std::ios::binary); stream << name << "-directory";
    }
    return files;
}

inline void CheckFiles(const std::vector<fs::path>& files, const std::string& name)
{
    for (std::size_t i = 0; i < files.size(); ++i)
        Require(Read(i ? files[i] / L"nested.txt" : files[i]) == name + (i ? "-directory" : "-file"),
            "native OLE leave and return cannot mutate either original source");
}

inline MSG HoldGraceTick()
{
    const auto started = GetTickCount64();
    MSG message{};
    for (;;)
    {
        Require(GetTickCount64() - started < 2000, "native leave fixture must receive its real grace timer");
        if (PeekMessageW(&message, Access::completionWindow, WM_TIMER, WM_TIMER, PM_REMOVE))
        {
            if (message.wParam == kExternalOleDragLeaveGraceTimerId)
            {
                Require(!message.lParam, "native grace timer must use the actual host message dispatch");
                return message;
            }
            DispatchMessageW(&message);
            Access::ThrowAsyncException();
        }
        else MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_TIMER, MWMO_INPUTAVAILABLE);
    }
}

inline void Deliver(MSG& message)
{
    DispatchMessageW(&message);
    Access::ThrowAsyncException();
}

inline void Run()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    std::size_t scenes = 0;
    std::uint64_t ticks = 0, completions = 0;
    for (const bool secondary : {false, true})
        for (const bool asynchronous : {false, true})
            for (const std::size_t count : {1u, 2u})
                for (const auto sequence : sequences)
                {
                    native_drag_commit_cases::context = "OLE leave sequence=" + std::to_string(static_cast<int>(sequence)) +
                        " count=" + std::to_string(count) + " async=" + std::to_string(asynchronous) +
                        " secondary=" + std::to_string(secondary);
                    const auto directory = profile.root / (L"leave-" + std::to_wstring(scenes));
                    const auto originals = Files(directory / L"source", count, "original");
                    const auto replacements = Files(directory / L"replacement", count == 1 ? 2 : 1, "replacement");
                    auto source = ole_file_source_fixture::Make(originals, asynchronous);
                    auto replacement = ole_file_source_fixture::Make(replacements, asynchronous);
                    const std::vector<fs::path> existing{desktop / L"Existing-A.txt", desktop / L"Existing-B.txt"};
                    for (std::size_t i = 0; i < existing.size(); ++i)
                    { std::ofstream stream(existing[i], std::ios::binary); stream << "existing-file-" << i; }
                    auto& app = profile.Model(secondary, directory / L"layout");
                    Access::PopulateModel(app, existing, 0);
                    Access::PrepareReload(app);
                    Access::BindAsync(app);
                    Require(Access::Save(app), "save native layout before the leave lifecycle");
                    const auto saved = Read(Access::LayoutPath(app));
                    const POINT point{secondary ? -650L : 150L, 150};
                    auto entered = Enter(app, *source.Get(), gestures[0], 7, asynchronous ? 1 : count, point, false);
                    const bool grace = sequence != Sequence::Outside;
                    Require(SUCCEEDED(Access::ExternalLeave(app, grace)), "actual native leave policy must return successfully");
                    ole_file_source_fixture::Source* submitted = nullptr;
                    const std::vector<fs::path>* submittedFiles = nullptr;
                    std::string submittedName;
                    if (sequence == Sequence::Outside)
                        Require(Access::LeaveCleared(app), "outside leave must clear transport, context and cached preview without submission");
                    else
                    {
                        Require(Access::LeavePending(app) && Access::ExternalSession(app),
                            "surface leave must retain the current native context while its own timer is pending");
                        if (sequence == Sequence::Expire)
                            Wait([&] { return Access::LeaveCleared(app); }, "actual grace expiry must finish native cleanup");
                        else
                        {
                            auto oldTick = HoldGraceTick();
                            if (sequence == Sequence::ReturnOver)
                            {
                                DWORD effect = 7;
                                Require(SUCCEEDED(entered.adapter->DragOver(MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)) &&
                                    effect == DROPEFFECT_COPY && !Access::LeavePending(app),
                                    "actual over must cancel pending leave and preserve the current accepted effect");
                                Deliver(oldTick);
                                Require(Access::ExternalSession(app), "old leave delivery after return cannot end the current native session");
                            }
                            else if (sequence == Sequence::Drop)
                            {
                                submitted = source.Get(); submittedFiles = &originals; submittedName = "original";
                            }
                            else
                            {
                                DWORD effect = 7;
                                Require(SUCCEEDED(entered.adapter->DragEnter(replacement.Get(), MK_LBUTTON | MK_CONTROL,
                                        Access::ExternalPoint(point), &effect)) && effect == DROPEFFECT_COPY &&
                                    Access::ExternalSummary(app).fileCount == static_cast<int>(asynchronous ? 1 : replacements.size()) &&
                                    !Access::LeavePending(app),
                                    "actual reenter must cancel old leave and probe the replacement source summary");
                                entered.preview = Access::ObserveOlePreview(app, point, MK_CONTROL);
                                if (sequence == Sequence::ReplaceSource)
                                {
                                    Deliver(oldTick);
                                    Require(Access::ExternalSession(app), "queued old cleanup cannot cancel a replacement source");
                                    submitted = replacement.Get(); submittedFiles = &replacements; submittedName = "replacement";
                                }
                                else
                                {
                                    const auto armed = GetTickCount64();
                                    Require(SUCCEEDED(Access::ExternalLeave(app, true)) && Access::LeavePending(app),
                                        "replacement native session must start its own grace interval");
                                    Require(GetTickCount64() - armed < kExternalOleDragLeaveGraceMs,
                                        "native stale-timer fixture must deliver before the new grace can expire");
                                    Deliver(oldTick);
                                    Require(Access::LeavePending(app) && Access::ExternalSession(app),
                                        "old grace completion cannot expire a newer native session before its own grace interval");
                                    Wait([&] { return Access::LeaveCleared(app); },
                                        "the replacement session's actual grace timer must eventually clean up");
                                }
                            }
                            if (submitted)
                            {
                                DWORD effect = 7;
                                Require(SUCCEEDED(entered.adapter->Drop(submitted, MK_CONTROL, Access::ExternalPoint(point), &effect)),
                                    "native returned drop must use the real adapter and host handler");
                                if (sequence == Sequence::Drop) Deliver(oldTick);
                            }
                        }
                    }
                    if (submitted)
                    {
                        Wait([&] { return Access::FileDropIdle(app) && Access::Items(app).size() == 2 + submittedFiles->size() &&
                            (!asynchronous || submitted->ends == 1); },
                            "leave and old-timer delivery cannot prevent the accepted physical drop or its placement refresh");
                        Require(!Access::LeavePending(app) && !Access::ExternalSession(app),
                            "accepted native drop must close the leave context without dropping its file completion");
                        Require(submitted->starts == (asynchronous ? 1 : 0) && submitted->ends == (asynchronous ? 1 : 0),
                            "only the selected source may own one asynchronous operation after return");
                    }
                    else
                    {
                        Require(Outputs(desktop, existing).empty() && Access::Items(app).size() == 2 && Read(Access::LayoutPath(app)) == saved,
                            "native cancellation and grace processing must preserve physical and saved layout state");
                        Require(SUCCEEDED(Access::ExternalLeave(app, false)) && Access::LeaveCleared(app),
                            "repeated leave cleanup must remain idempotent and clear cached feedback");
                    }
                    for (const auto* provider : {source.Get(), replacement.Get()})
                        Require(!provider->wrongApartment && !provider->fileReadBeforeStart &&
                            (provider == submitted || (provider->starts == 0 && provider->ends == 0)),
                            "cancelled and superseded native sources cannot start leases or read delayed content");
                    CheckFiles(originals, "original"); CheckFiles(replacements, "replacement");
                    const auto outputs = Outputs(desktop, existing);
                    Require(outputs.size() == (submitted ? submittedFiles->size() : 0),
                        "native leave sequences must create exactly the selected source outputs once");
                    for (std::size_t i = 0; submitted && i < submittedFiles->size(); ++i)
                    {
                        const auto output = std::find_if(outputs.begin(), outputs.end(),
                            [&](const auto& path) { return path.filename() == (*submittedFiles)[i].filename(); });
                        Require(output != outputs.end() && Read(i ? *output / L"nested.txt" : *output) ==
                                submittedName + (i ? "-directory" : "-file"),
                            "native return must submit the current source bytes rather than a cancelled or superseded source");
                    }
                    CheckOriginals(app, existing);
                    ticks += Access::graceTimerMessages; completions += Access::fileCompletionMessages;
                    Access::StopModel(app);
                    source.Reset(); replacement.Reset();
                    for (const auto& output : outputs)
                    {
                        Require(fs::weakly_canonical(output.parent_path()) == fs::weakly_canonical(desktop),
                            "leave fixture may remove only its isolated native desktop outputs");
                        fs::remove_all(output);
                    }
                    for (const auto& file : existing) fs::remove(file);
                    ++scenes;
                }
    std::cout << scenes << " native OLE leave/cancel/return scenes passed; " << ticks << " grace timer messages and "
        << completions << " file completions dispatched\n";
}
}
