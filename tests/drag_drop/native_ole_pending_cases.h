#pragma once

#include "platform/operation_feedback.h"

namespace native_ole_pending_cases
{
using namespace native_ole_sequence_cases;
inline std::vector<fs::path> NewPaths(const fs::path& directory, const std::vector<fs::path>& originals)
{
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(directory))
        if (std::none_of(originals.begin(),originals.end(),[&](const auto& p) {
            return ToUpperInvariant(p.lexically_normal().wstring()) == ToUpperInvariant(entry.path().lexically_normal().wstring());
        })) result.push_back(entry.path());
    return result;
}
inline void RunMutations()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    for (int mutation : {0,1,2,3,4})
    {
        native_drag_commit_cases::context = "D14 pending mutation=" + std::to_string(mutation);
        const bool mapped = mutation == 2 || mutation == 4;
        const bool append = mutation >= 3;
        const auto root = profile.root / (L"pending-target-" + std::to_wstring(mutation));
        const auto directory = mapped ? root / L"mapped" : desktop;
        std::vector<fs::path> originals;
        for (wchar_t name : {L'A',L'B',L'C',L'D'})
        {
            originals.push_back(directory / (std::wstring(1,name)+L".txt"));
            Write(originals.back(),std::string("old-")+static_cast<char>(name));
        }
        const auto sourcePath = root / L"source/Payload.txt";
        Write(sourcePath,"retained-original");
        const auto replacement = root / L"replacement";
        Write(replacement / L"Unrelated.txt","unrelated-new-mapping");
        auto& app = profile.Model(false,root / L"layout");
        Access::SeedPendingTarget(app,originals,mapped); Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app),"D14: save the initial real target before asynchronous capture");
        const auto point = Access::PendingAnchorPoint(app);
        auto source = ole_file_source_fixture::Make({sourcePath},true);
        auto adapter = Access::ExternalAdapter(app); DWORD effect = 7;
        Require(SUCCEEDED(adapter->DragEnter(source.Get(),MK_LBUTTON|MK_CONTROL,Access::ExternalPoint(point),&effect)) && effect == 1,
            "D14: delayed input must enter the actual host widget target");
        Access::FreshExternalDwell(app);
        const auto capturedIndex = Access::PendingCapturedIndex(app,point);
        Require(capturedIndex && *capturedIndex == 1,
            "D14: real host input must capture insertion immediately before existing middle member B");
        effect = 7;
        Require(SUCCEEDED(adapter->Drop(source.Get(),MK_CONTROL,Access::ExternalPoint(point),&effect)) && effect == 1 &&
            source->starts == 1 && source->ends == 0,"D14: actual widget drop must retain its independent source lease");
        HeldRead held; held.Capture();
        if (mutation == 4) Require(fs::remove(originals[1]),"D14: remove the original mapped anchor physically");
        Access::MutatePendingTarget(app,mutation,replacement);
        Require(Access::Save(app),"D14: persist the independent user target mutation before releasing A");
        const auto expectedTarget = Access::Widgets(app).front();
        held.Deliver();
        Wait([&] { return source->ends == 1 && (mapped ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)); },
            "D14: actual retained task must complete or reject after its original target changes");
        Require(source->starts == 1 && source->ends == 1 && !source->wrongApartment && !source->fileReadBeforeStart &&
            Read(sourcePath) == "retained-original",
            "D14: every retained task must end once and preserve its original source after Copy or rejection");
        auto outputs = NewPaths(directory,originals);
        Require(outputs.size() == (append ? 1u : 0u) && NewPaths(replacement,{replacement/L"Unrelated.txt"}).empty(),
            "D14: invalid captured targets must cancel before creating files and never redirect into replacement mapping");
        Require(append ? SUCCEEDED(source->outcome) : FAILED(source->outcome),
            "D14: source completion must distinguish accepted unchanged target from cancelled deleted/type/path target");
        std::vector<std::wstring> expectedOrder;
        if (append)
        {
            Require(Read(outputs[0]) == "retained-original", "D14: appended output must retain actual payload bytes");
            expectedOrder = {ToUpperInvariant(originals[0].wstring()),ToUpperInvariant(originals[2].wstring()),
                ToUpperInvariant(originals[3].wstring()),ToUpperInvariant(outputs[0].wstring())};
            Require(Access::PendingTargetOrder(app,mapped) == expectedOrder,
                "D14: disappearance of captured middle anchor must append after all surviving original members");
        }
        else Require(Access::Widgets(app).front().itemKeys == expectedTarget.itemKeys,
            "D14: target cancellation cannot add an item to the current unrelated object");
        Require(Read(originals[0]) == "old-A" && Read(originals[2]) == "old-C" && Read(originals[3]) == "old-D" &&
            Read(replacement/L"Unrelated.txt") == "unrelated-new-mapping",
            "D14: asynchronous target change must preserve unrelated contents");
        Require(Access::Save(app),"D14: save actual completed or rejected target mutation");
        Access::PerturbBeforeDiskRead(app); Access::RequestAsync(app,true);
        Wait([&] { return mapped ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app); },
            "D14: target mutation must reload from actual disk and background results");
        const auto& restored = Access::Widgets(app).front();
        Require(restored.id == expectedTarget.id && restored.type == expectedTarget.type &&
            restored.sourceFolderPath == expectedTarget.sourceFolderPath,
            "D14: disk reload must preserve the user's intervening target identity/type/mapping choice");
        if (append)
        {
            if (mapped) Access::RequestMappedRead(app,restored.sourceFolderPath);
            Wait([&] { return !mapped || Access::FolderReaderIdle(app); },"D14: reloaded mapped order must receive its actual directory");
            Require(Access::PendingTargetOrder(app,mapped) == expectedOrder,
                "D14: append fallback must survive save, real reload and directory delivery");
        }
        Access::StopModel(app); CleanDesktop(desktop);
    }
    std::cout << "Native captured-target changes: 5 scenes passed\n";
}

class Feedback
{
    std::mutex mutex_;
    snowdesktop::operation_feedback::Reporter previous_;
public:
    std::vector<snowdesktop::operation_feedback::Failure> failures;
    Feedback()
    {
        { std::lock_guard lock(snowdesktop::operation_feedback::reporterMutex); previous_ = snowdesktop::operation_feedback::reporter; }
        snowdesktop::operation_feedback::SetReporter([this](const auto& failure) { std::lock_guard lock(mutex_); failures.push_back(failure); });
    }
    ~Feedback() { snowdesktop::operation_feedback::SetReporter(std::move(previous_)); }
    bool Includes(const fs::path& path)
    {
        std::lock_guard lock(mutex_);
        return std::any_of(failures.begin(),failures.end(),[&](const auto& f) {
            return ToUpperInvariant(f.detail).find(ToUpperInvariant(path.wstring())) != std::wstring::npos;
        });
    }
};
class ShellGate
{
    struct State
    {
        HANDLE entered = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        HANDLE release = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        ~State() { if (release) CloseHandle(release); if (entered) CloseHandle(entered); }
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    explicit ShellGate(DesktopApp& app)
    {
        Require(state_->entered && state_->release,"D13: create owned bounded worker-order events");
        Require(Access::QueuePendingShellGate(app,[state=state_] {
            SetEvent(state->entered);
            return WaitForSingleObject(state->release,12000) == WAIT_OBJECT_0;
        }),"D13: queue a real prior worker job to control when the native request can start");
        Wait([&] { return WaitForSingleObject(state_->entered,0) == WAIT_OBJECT_0; },"D13: prior real worker job must enter its controlled boundary");
    }
    ~ShellGate() { Release(); }
    void Release() { SetEvent(state_->release); }
};
inline void RunPartial()
{
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    native_drag_commit_cases::context = "D13 partial native desktop duplicate A/B/C";
    const std::vector<fs::path> files{desktop/L"A.txt",desktop/L"B.txt",desktop/L"C.txt"};
    Write(files[0],"committed-A"); Write(files[1],"will-disappear-B"); Write(files[2],"unstarted-C");
    auto& app = profile.Model(false,profile.root/L"partial-layout");
    Access::SeedSequenceOrigin(app,files,{},0); Access::EnlargePendingPage(app);
    Access::PrepareReload(app); Access::BindAsync(app);
    Require(Access::Save(app),"D13: persist original native source identities");
    Feedback feedback;
    ShellGate gate(app);
    Access::BeginModel(app,0,3);
    auto data = Access::BeginSequenceSelfOle(app,false);
    auto adapter = Access::ExternalAdapter(app); DWORD effect = 7; const POINT point{650,450};
    Require(SUCCEEDED(adapter->DragEnter(data.Get(),MK_LBUTTON|MK_CONTROL,Access::ExternalPoint(point),&effect)),
        "D13: actual internal batch must re-enter its real native OLE route");
    effect = 7;
    Require(SUCCEEDED(adapter->Drop(data.Get(),MK_CONTROL,Access::ExternalPoint(point),&effect)),
        "D13: actual native release must queue the real desktop duplicate batch");
    Access::EndSequenceSelfOle(app); data.Reset();
    Require(NewPaths(desktop,files).empty(),"D13: blocked worker must not have executed the queued file request");
    Require(fs::remove(files[1]),"D13: make the second captured physical source unavailable after submission");
    gate.Release();
    Wait([&] { return Access::FileDropIdle(app); },"D13: actual partial Shell result and retained landing must finish refreshing");
    const auto outputs = NewPaths(desktop,files);
    std::cerr << "D13 actual-output-count=" << outputs.size() << " source-A=" << fs::exists(files[0]) <<
        " source-C=" << fs::exists(files[2]) << '\n';
    for (const auto& path : outputs) std::cerr << "D13 output=" << path.filename().string() << " bytes=" << Read(path) << '\n';
    Require(outputs.size() == 1 && Read(outputs[0]) == "committed-A" && Read(files[0]) == "committed-A" &&
        Read(files[2]) == "unstarted-C",
        "D13: keep actually committed A and stop unstarted C after the second source fails");
    Require(feedback.Includes(files[1]) && feedback.Includes(files[2]),
        "D13: actual operation feedback must identify failed and unstarted batch members");
    const auto& output = Find(app,ToUpperInvariant(outputs[0].wstring()));
    Require(Access::HasVisibleIcon(app,output) && Access::HasMatchingRuntimeSlot(app,output),
        "D13: successful partial output must retain its actual native landing despite batch failure");
    ReloadAndCheck(app,{{output.layoutKey,output.gridCell}});
    Require(Read(outputs[0]) == "committed-A" && Read(files[2]) == "unstarted-C",
        "D13: persistence must retain the real success and preserve unstarted source contents");
    Access::StopModel(app); CleanDesktop(desktop);
    std::cout << "Native partial batch: 1 scene passed\n";
}
}
