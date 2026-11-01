// Historical regressions, grouped by observable failure rather than a matrix.
#include "app/drag_drop/ole_drag_drop_adapter.h"
#include "drag_drop/external_drop_resources.h"
#include "layout/item_location.h"
#include <wrl/implements.h>
#include <shldisp.h>
#include <atomic>

namespace behavior
{
// Only the external source is controlled. The real Shell data object supplies
// paths; production marshaling, Drop routing, workers and completion stay live.
class DragFileSource final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject, IDataObjectAsyncCapability>
{
public:
    Microsoft::WRL::ComPtr<IDataObject> shell;
    const DWORD thread = GetCurrentThreadId();
    std::atomic<int> starts{0}, ends{0}, reads{0};
    std::atomic<bool> wrongThread{false}, prematureRead{false};
    HRESULT outcome = E_PENDING;
    DWORD effect = 0;
    BOOL asynchronous = TRUE;
    void CheckThread() { if (GetCurrentThreadId()!=thread) wrongThread = true; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override
    {
        CheckThread();
        if (f && f->cfFormat == CF_HDROP) { ++reads; if (starts==0) prematureRead = true; }
        return shell->GetData(f,m);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* f, STGMEDIUM* m) override { CheckThread(); return shell->GetDataHere(f,m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override { CheckThread(); return shell->QueryGetData(f); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* m) override { CheckThread(); return shell->GetCanonicalFormatEtc(f,m); }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) override { CheckThread(); return shell->SetData(f,m,release); }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD d, IEnumFORMATETC** e) override { CheckThread(); return shell->EnumFormatEtc(d,e); }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*,DWORD,IAdviseSink*,DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override { asynchronous=enabled; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override { if (!enabled) return E_POINTER; *enabled=asynchronous; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { CheckThread(); ++starts; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override { if (!active) return E_POINTER; *active=starts>ends; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT hr,IBindCtx*,DWORD completedEffect) override
    { CheckThread(); outcome=hr; effect=completedEffect; ++ends; return S_OK; }
};
struct DragInput
{
    snowdesktop::private_pointer::Device device;
    DragInput()
    {
        const HWND window=Access::completionWindow;
        std::string error;
        Require(device.Begin({0,0,400,300},{50,50},[window](POINT){return window;},
            [window](HWND candidate){return candidate==window;},error),"isolate pointer/modifier input: "+error);
    }
    void Move(POINT point)
    {
        std::string error;
        Require(device.Move(point,error),"queue private pointer sample: "+error);
        Wait([&]{return device.Current().pendingEvents==0;},"private pointer sample delivered",[&]{return device.Current().error;});
    }
};
bool SameCell(const GridCell& first,const GridCell& second)
{ return first.pageId==second.pageId&&first.column==second.column&&first.row==second.row; }
template<class Paths>
void RequirePaths(const Paths& actual, const std::vector<fs::path>& expected, const std::string& reason)
{
    Require(actual.size()==expected.size(),reason+": item count");
    for (std::size_t i=0;i<expected.size();++i) Require(fs::equivalent(actual[i],expected[i]),reason+": item identity/order");
}
void WaitForHost(HostProfile& profile, const std::string& reason)
{
    Wait([&]{return Access::BackgroundIdle(*profile.app);},reason,[&]{return Access::AsyncState(*profile.app);});
}
void DragSource()
{
    // fbd6ca6b / 2fe31371: visibility and later selection are not the payload.
    HostProfile profile; DragInput input;
    std::vector<fs::path> files;
    for (int i=0;i<25;++i) {
        files.push_back(profile.desktop/(L"member-"+std::to_wstring(100+i)+L".txt"));
        Write(files.back(),"original-member-"+std::to_string(i));
    }
    Access::AddCollection(*profile.app,L"source",0,files);
    const std::vector<fs::path> selected(files.begin(),files.begin()+24);
    Access::Select(*profile.app,selected);
    const auto visible = Access::VisibleMembers(*profile.app);
    Require(visible>0 && visible<selected.size(),"fixture has both visible and hidden selected members");
    Access::BeginMembers(*profile.app,L"source");
    const auto captured = Access::Sources(*profile.app);
    RequirePaths(captured.FilePaths(),selected,"hidden selected members belong to the complete drag payload");
    Access::CancelDrag(*profile.app);
    Require(!Access::HasDrag(*profile.app),"Escape cancels the captured drag");

    const std::vector<fs::path> original{files[0],files[1]};
    Access::Select(*profile.app,original); Access::BeginMembers(*profile.app,L"source");
    Access::Select(*profile.app,{files[2]});
    Access::RebuildDrag(*profile.app);
    RequirePaths(Access::Sources(*profile.app).FilePaths(),original,"rebuild retains original identities rather than the new selection");
    Access::CancelDrag(*profile.app);
    for (std::size_t i=0;i<files.size();++i)
        Require(Read(files[i])=="original-member-"+std::to_string(i),"capture/rebuild/cancel leaves every source file intact");
}
void DragFiles()
{
    // DND-01 and d325b085: acceptance alone is not a committed file result.
    HostProfile profile(true); Access::EnableFileDispatch(*profile.app);
    Access::AddCollection(*profile.app,L"receive",0);
    const auto input = profile.root/L"source"/L"copy.txt";
    Write(input,std::string_view("copy\0tail",9));
    auto source = Microsoft::WRL::Make<DragFileSource>(); source->shell = Access::ShellSource(*profile.app,input);
    Require(source->shell!=nullptr,"create actual Shell source");
    auto target = Access::Adapter(*profile.app);
    const auto point = Access::OlePoint({50,70});
    DWORD effect = DROPEFFECT_COPY;
    Require(SUCCEEDED(target->DragEnter(source.Get(),MK_LBUTTON|MK_CONTROL,point,&effect)),"OLE enter");
    effect = DROPEFFECT_COPY;
    const HRESULT dropped = target->Drop(source.Get(),MK_CONTROL,point,&effect);
    Wait([&]{return source->ends==1 && Access::BackgroundIdle(*profile.app);},"OLE extraction, physical copy, host completion and refresh",[&] {
        return Access::AsyncState(*profile.app)+" start="+std::to_string(source->starts.load())+" end="+std::to_string(source->ends.load());
    });
    Require(SUCCEEDED(dropped) && effect==DROPEFFECT_COPY && SUCCEEDED(source->outcome) && source->effect==DROPEFFECT_COPY,
        "asynchronous copy reports its actual successful effect");
    Require(source->starts==1 && source->reads>0 && !source->prematureRead && !source->wrongThread,
        "one source lease, no delayed file read before StartOperation, source STA preserved");
    const auto copied = profile.desktop/input.filename();
    Require(Read(copied)==Read(input),"real copied bytes match while the source remains intact");
    RequirePaths(Access::CollectionPaths(*profile.app,L"receive"),{copied},"OLE completion actually commits into the target collection");
    Require(Access::fileCompletionMessages>0,"file completion traversed the production window procedure");

    // Use the native Shell spelling. Exercise 8.3 canonicalization when the
    // volume provides a short name; otherwise still check the physical move.
    const auto moveInput = profile.root/L"source"/L"move.txt";
    Write(moveInput,"moved bytes");
    auto nativeInput=moveInput; nativeInput.make_preferred();
    auto alias=nativeInput;
    std::vector<wchar_t> shortName(32768);
    const DWORD shortLength=GetShortPathNameW(alias.c_str(),shortName.data(),static_cast<DWORD>(shortName.size()));
    if (shortLength>0 && shortLength<shortName.size()) alias=fs::path(shortName.data());
    std::cout << "INFO drag-files short-alias=" << (_wcsicmp(alias.c_str(),nativeInput.c_str())!=0) << '\n';
    Microsoft::WRL::ComPtr<IShellItem> aliasItem;
    const HRESULT parsedAlias=SHCreateItemFromParsingName(alias.c_str(),nullptr,IID_PPV_ARGS(&aliasItem));
    Require(SUCCEEDED(parsedAlias) && fs::equivalent(alias,moveInput),
        "fixture alias is a real Shell-resolvable source; hr="+std::to_string(parsedAlias));
    auto outputs = std::make_shared<snowdesktop::ShellFileOperationResult>();
    bool done=false, succeeded=false;
    const bool queued = Access::SubmitFiles(*profile.app,{alias},L"",{260,250},MK_SHIFT,
        [&](bool ok){succeeded=ok;done=true;},outputs);
    Wait([&]{return done && Access::BackgroundIdle(*profile.app);},"aliased move result delivered and reloaded",[&]{return Access::AsyncState(*profile.app);});
    const auto moved = profile.desktop/moveInput.filename();
    Require(queued && succeeded,"a completed aliased move is not falsely reported as failure; queued="+
        std::to_string(queued)+" completed="+std::to_string(succeeded)+
        " sourceExists="+std::to_string(fs::exists(moveInput))+" targetExists="+std::to_string(fs::exists(moved))+
        " outputs="+std::to_string(outputs->outputs.size())+" aliasParse="+std::to_string(parsedAlias));
    Require(!fs::exists(moveInput) && Read(moved)=="moved bytes" && outputs->outputs.size()==1,
        "move has one actual output and removes exactly its source");
    Require(fs::equivalent(outputs->outputs.front().destination,moved),"reported move output is the actual file");
}
void DragBatches()
{
    // f91bb2c8: one completion must not replace another unconsumed landing.
    HostProfile profile(true); Access::EnableFileDispatch(*profile.app);
    const auto hold = profile.desktop/L"hold.txt"; Write(hold,"untouched");
    Access::PopulateModel(*profile.app,{hold},0);
    Access::AddCollection(*profile.app,L"batch-a",0);
    Access::AddCollection(*profile.app,L"batch-b",2);
    Access::Select(*profile.app,{hold}); Access::BeginMembers(*profile.app,L"");
    const auto a=profile.root/L"inputs"/L"a.txt", b=profile.root/L"inputs"/L"b.txt";
    Write(a,"batch-a"); Write(b,"batch-b");
    bool aDone=false,bDone=false,aOk=false,bOk=false;
    auto aOut=std::make_shared<snowdesktop::ShellFileOperationResult>();
    auto bOut=std::make_shared<snowdesktop::ShellFileOperationResult>();
    Require(Access::SubmitFiles(*profile.app,{a},L"batch-a",{50,70},MK_CONTROL,[&](bool ok){aOk=ok;aDone=true;},aOut),"queue first real file request");
    Require(Access::SubmitFiles(*profile.app,{b},L"batch-b",{250,70},MK_CONTROL,[&](bool ok){bOk=ok;bDone=true;},bOut),"queue second real file request");
    Wait([&]{return aDone&&bDone&&Access::FileOperationsIdle(*profile.app);},"two completions while an active drag defers model refresh",[&]{return Access::AsyncState(*profile.app);});
    Require(aOk&&bOk,"both physical operations completed");
    Access::CancelDrag(*profile.app); Access::RequestAsync(*profile.app);
    WaitForHost(profile,"both completed batches consumed by the real model refresh");
    RequirePaths(Access::CollectionPaths(*profile.app,L"batch-a"),{profile.desktop/a.filename()},"first request retains its own target");
    RequirePaths(Access::CollectionPaths(*profile.app,L"batch-b"),{profile.desktop/b.filename()},"second request retains its own target");
    Require(Read(profile.desktop/a.filename())=="batch-a" && Read(profile.desktop/b.filename())=="batch-b" && Read(hold)=="untouched",
        "refresh preserves actual request bytes and the unrelated drag source");
}
void DragPartial()
{
    // DND-04: failure of a later item cannot invalidate a successful reference.
    HostProfile profile(true); Access::EnableFileDispatch(*profile.app);
    const auto first=profile.root/L"staging"/L"kept.txt", second=profile.root/L"staging"/L"unused.txt";
    const auto link=profile.root/L"kept.lnk";
    Write(first,"referenced bytes"); Write(second,"unreferenced bytes");
    auto result=std::make_shared<snowdesktop::ShellFileOperationResult>();
    auto content=snowdesktop::external_drop_content::MakeContent(result);
    content->owned=true; content->paths={first.wstring(),second.wstring()};
    snowdesktop::ShellFileOperationRequest request; request.result=result;
    request.shortcuts={{first.wstring(),link.wstring(),{},true},
        {second.wstring(),(profile.root/L"absent"/L"fail.lnk").wstring(),{},true}};
    bool done=false,ok=true;
    Require(Access::QueuePartial(*profile.app,std::move(request),[&](bool value){ok=value;done=true;}),"queue genuinely partially failing shortcut batch");
    Wait([&]{return done&&Access::FileOperationsIdle(*profile.app);},"partial result delivered through host completion",[&]{return Access::AsyncState(*profile.app);});
    Require(!ok && result->outputs.size()==1 && fs::exists(link),"partial failure still reports its successfully created link");
    content.reset(); // The same production deleter used by owned external drops.
    Require(fs::exists(first) && !fs::exists(second),"partial cleanup preserves referenced content and removes only unused owned content");
    const fs::path resolved(snowdesktop::item_location::ResolveRevealPath(link.wstring()));
    Require(fs::equivalent(resolved,first) && Read(resolved)=="referenced bytes","successful link remains usable after partial-batch cleanup");
}
void DragRelease()
{
    // 66421876: require the actual release, commit, save and reload boundary.
    HostProfile profile(true); DragInput input; Access::EnableFileDispatch(*profile.app);
    const auto file=profile.desktop/L"release.txt"; Write(file,"original release bytes");
    Access::PopulateModel(*profile.app,{file},0);
    Require(Access::Save(*profile.app),"seed the old persisted position");
    Access::Select(*profile.app,{file}); Access::BeginMembers(*profile.app,L"");
    input.Move({260,160}); Access::ReleaseDrag(*profile.app,{260,160});
    const GridCell expected{L"__page:1",2,1};
    Require(!Access::HasDrag(*profile.app) && SameCell(Access::Cell(*profile.app,file),expected),"release commits the requested final cell and ends the session");
    Wait([&]{return Access::LayoutSaved(*profile.app);},"production deferred layout save completed",[&]{return Access::AsyncState(*profile.app);});
    Access::PerturbCell(*profile.app); Access::RequestAsync(*profile.app,true);
    WaitForHost(profile,"actual disk layout and Shell snapshot applied after release");
    Require(SameCell(Access::Cell(*profile.app,file),expected),"real layout reload restores the released position");
    Access::Select(*profile.app,{file}); Access::BeginMembers(*profile.app,L"");
    Access::CancelDrag(*profile.app); Access::RequestAsync(*profile.app,true);
    WaitForHost(profile,"actual disk reload after cancellation");
    Require(!Access::HasDrag(*profile.app) && SameCell(Access::Cell(*profile.app,file),expected) && Read(file)=="original release bytes",
        "cancel leaves persisted placement and source bytes unchanged");
}
}
