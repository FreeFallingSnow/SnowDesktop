#pragma once
#include "common/l10n.h"
#include "ui/input/private_pointer.h"

namespace behavior
{
struct ContainerRow
{
    std::string_view name;
    DesktopWidgetType root, member;
    bool grouped, mapped;
};
// These six rows represent distinct ownership/persistence models; gestures,
// file count and presentation are not multiplied into every row.
inline constexpr ContainerRow containerRows[]{
    {"container-collection",DesktopWidgetType::Collection,DesktopWidgetType::Collection,false,false},
    {"container-files",DesktopWidgetType::FileCategories,DesktopWidgetType::FileCategories,false,false},
    {"container-mapping",DesktopWidgetType::FolderMapping,DesktopWidgetType::FolderMapping,false,true},
    {"container-collection-group",DesktopWidgetType::CollectionGroup,DesktopWidgetType::Collection,true,false},
    {"container-file-group",DesktopWidgetType::FileGroup,DesktopWidgetType::FileCategories,true,false},
    {"container-mapping-group",DesktopWidgetType::FileGroup,DesktopWidgetType::FolderMapping,true,true},
};
const DesktopWidget& ContainerWidget(DesktopApp& app, std::wstring_view id)
{
    const auto& all = Access::Widgets(app);
    const auto it = std::find_if(all.begin(),all.end(),[&](const auto& w) { return w.id == id; });
    Require(it != all.end(),"stable container identity must survive"); return *it;
}
bool SameFiles(const std::vector<fs::path>& actual, const std::vector<fs::path>& expected)
{
    if (actual.size() != expected.size()) return false;
    for (const auto& path : expected)
        if (std::count_if(actual.begin(),actual.end(),[&](const auto& other) {
            std::error_code error; return fs::equivalent(path,other,error) && !error;
        }) != 1) return false;
    return true;
}
void RefreshContainer(Profile& p, bool fromDisk, const std::wstring& memberId, bool mapped)
{
    Access::RequestAsync(*p.app,fromDisk);
    Wait([&] { return Access::ContainerWorkIdle(*p.app); });
    if (mapped)
    {
        const auto path = ContainerWidget(*p.app,memberId).sourceFolderPath;
        Access::ReadMappedContainer(*p.app,path);
        Wait([&] { return Access::ContainerWorkIdle(*p.app); });
    }
}
void SeedDestination(Profile& p, const ContainerRow& row, fs::path& existing)
{
    existing = row.mapped ? p.root / L"mapped/Existing.txt" : p.desktop / L"Sentinel.txt";
    if (row.mapped) Write(existing,"existing");
    Access::ResetContainerScene(*p.app,row.mapped ? p.desktop / L"Sentinel.txt" : fs::path{});
    Access::SeedExternalStorage(*p.app,row.root,row.member,false,existing);
    Require(Access::Save(*p.app),"save actual destination before receiving files");
    RefreshContainer(p,false,row.grouped ? L"ingress-active" : L"ingress-root",row.mapped);
}
void ReceiveContainer(const ContainerRow& row)
{
    Profile p; fs::path existing; SeedDestination(p,row,existing);
    const auto directory = row.mapped ? existing.parent_path() : p.desktop;
    const auto sourcePath = p.root / L"incoming/Payload.txt";
    Write(sourcePath,"container-payload");
    const auto memberId = row.grouped ? L"ingress-active" : L"ingress-root";
    const auto rootBefore = ContainerWidget(*p.app,L"ingress-root");
    const auto memberBefore = ContainerWidget(*p.app,memberId);
    const auto expectedOutput = directory / L"Payload.txt";
    auto source = ole_file_source_fixture::Make({sourcePath},true);
    auto adapter = Access::ExternalAdapter(*p.app); DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    const POINT point{350,250};
    Require(SUCCEEDED(adapter->DragEnter(source.Get(),MK_LBUTTON | MK_CONTROL,Access::ExternalPoint(point),&effect)) && effect == DROPEFFECT_COPY,
        "file must negotiate Copy with actual native target");
    Require(Access::Target(*p.app) == Access::MemberContainer(*p.app,L"ingress-root") && source->starts == 0,
        "real hit routing selects the intended container without reading delayed files");
    Access::FreshExternalDwell(*p.app); effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    Require(SUCCEEDED(adapter->Drop(source.Get(),MK_CONTROL,Access::ExternalPoint(point),&effect)) && effect == DROPEFFECT_COPY,
        "release submits the accepted file verb");
    Wait([&] { return source->ends == 1 && Access::ContainerWorkIdle(*p.app) &&
        SameFiles(Access::ContainerMembers(*p.app,memberId),{existing,expectedOutput}); });
    const auto check = [&] {
        const auto& root = ContainerWidget(*p.app,L"ingress-root");
        const auto& member = ContainerWidget(*p.app,memberId);
        Require(root.type == row.root && root.title == rootBefore.title && root.customTitle == rootBefore.customTitle &&
            root.autoCollect == rootBefore.autoCollect && root.singleCellIconMode == rootBefore.singleCellIconMode &&
            root.gridSpan.columns == rootBefore.gridSpan.columns && root.gridSpan.rows == rootBefore.gridSpan.rows && Same(root.gridCell,rootBefore.gridCell) &&
            root.childWidgetIds == rootBefore.childWidgetIds && root.activeCategoryId == rootBefore.activeCategoryId,
            "receiving files preserves root identity, settings, location and child relationship");
        Require(member.type == row.member && member.sourceFolderPath == memberBefore.sourceFolderPath &&
            SameFiles(Access::ContainerMembers(*p.app,memberId),{existing,expectedOutput}),
            "accepted file belongs once to captured physical/logical source");
        if (row.grouped) Require(ContainerWidget(*p.app,L"ingress-inactive").itemKeys.empty() &&
            ContainerWidget(*p.app,L"ingress-inactive").privacyMode,"inactive group member must retain its settings without receiving the file");
        Require(Access::FreeDesktopCopies(*p.app,expectedOutput)==0,"collected or mapped output cannot also appear as a free desktop duplicate");
        std::vector<fs::path> visible;
        for (const auto& slot : Access::MemberContainer(*p.app,L"ingress-root")->GetSlots())
            if (const auto* item = slot->GetItem(); item && !item->GetPath().empty()) visible.emplace_back(item->GetPath());
        Require(SameFiles(visible,{existing,expectedOutput}),"runtime slots expose exactly the intended accessible members");
        Require(Read(sourcePath) == "container-payload" && Read(expectedOutput) == "container-payload" &&
            Read(p.desktop / L"Sentinel.txt") == "untouched" && Read(existing)==(row.mapped ? "existing" : "untouched"), "Copy preserves input and unrelated bytes");
        Require(static_cast<std::size_t>(std::distance(fs::directory_iterator(directory),fs::directory_iterator{}))==2,
            "physical destination contains exactly its existing file and one accepted output");
    };
    Require(source->starts == 1 && source->ends == 1 && SUCCEEDED(source->outcome) &&
        !source->wrongApartment && !source->fileReadBeforeStart,"source lease is balanced on its original apartment");
    check(); Require(Access::Save(*p.app),"persist container membership");
    RefreshContainer(p,true,memberId,row.mapped); check();
}
UINT MenuChoice(HMENU menu, const wchar_t* label)
{
    for (int index=0; index<GetMenuItemCount(menu); ++index)
    {
        std::array<wchar_t,256> text{};
        if (GetMenuStringW(menu,static_cast<UINT>(index),text.data(),static_cast<int>(text.size()),MF_BYPOSITION)>0 &&
            std::wstring_view(text.data()) == label)
        {
            const UINT command = GetMenuItemID(menu,index);
            Require(command != static_cast<UINT>(-1),"selected action must be a concrete menu item"); return command;
        }
    }
    Require(false,"requested user-visible menu action must be available"); return 0;
}
void RightDecision(bool cancel)
{
    Profile p; const auto sourcePath=p.root/L"incoming/Right.txt"; Write(sourcePath,"right-payload");
    const auto before=Read(Access::LayoutPath(*p.app));
    auto source=ole_file_source_fixture::Make({sourcePath},true);
    int menus=0;
    Access::RightMenuBridge bridge(*p.app,[&](HMENU menu,POINT,HWND owner) {
        ++menus;
        Require(owner == Access::completionWindow && IsMenu(menu) && Outputs(p).empty() && source->starts == 0 &&
            Read(Access::LayoutPath(*p.app)) == before,"menu selection precedes all file and saved-state changes");
        return MenuChoice(menu,_LW(cancel ? "app.settings.cancel" : "widget.base.copy_label"));
    });
    ComPtr<OleDragDropAdapter> adapter; adapter.Attach(new OleDragDropAdapter(&bridge));
    const POINT point{150,150}; DWORD effect=DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    Require(SUCCEEDED(adapter->DragEnter(source.Get(),MK_RBUTTON,Access::ExternalPoint(point),&effect)),"right drag reaches adapter");
    Access::FreshExternalDwell(*p.app); effect=DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    Require(SUCCEEDED(adapter->Drop(source.Get(),0,Access::ExternalPoint(point),&effect)) && menus==1,"one real host menu resolves right drag");
    if(cancel)
    {
        Require(effect==DROPEFFECT_NONE && source->starts==0 && source->ends==0 && Outputs(p).empty() &&
            Read(sourcePath)=="right-payload" && Read(Access::LayoutPath(*p.app))==before,
            "Cancel cannot submit an unselected move/copy/link or change saved state");
        return;
    }
    const auto output=p.desktop/L"Right.txt";
    Wait([&] { return source->ends==1 && Access::FileDropIdle(*p.app) && Access::Items(*p.app).size()==2; });
    Require(effect==DROPEFFECT_COPY && source->starts==1 && SUCCEEDED(source->outcome) &&
        Read(sourcePath)=="right-payload" && Read(output)=="right-payload" && Outputs(p).size()==1,
        "chosen Copy performs exactly one physical copy and retains source");
    const auto cell=Find(*p.app,output).gridCell;
    Require(cell.column==1 && cell.row==1,"right decision retains requested landing"); Reload(p,{{output,cell}});
}
struct IsolatedPointer
{
    snowdesktop::private_pointer::Device device;
    IsolatedPointer()
    {
        std::string error;
        const bool started=device.Begin({0,0,1200,800},{50,50},[](POINT) { return Access::completionWindow; },
            [](HWND window) { return window == Access::completionWindow; },error);
        Require(started,"isolate native release from real desktop input: "+error);
    }
};
void InternalMember(bool files)
{
    Profile p; const auto moving=p.desktop/L"Moving.txt"; Write(moving,"move-logical-owner");
    Access::ResetContainerScene(*p.app);
    Access::PopulateModel(*p.app,{moving,p.desktop/L"Sentinel.txt"},files ? 2 : 1);
    Require(Access::Save(*p.app),"save native source membership");
    {
        IsolatedPointer pointer;
        Access::BeginContainerMembers(*p.app,L"native-origin",{moving});
        Access::ReleaseContainerMembers(*p.app,{650,450});
    }
    const auto check=[&] {
        const auto& source=ContainerWidget(*p.app,L"native-origin");
        Require(source.itemKeys.size()==1 && SameFiles(Access::ContainerMembers(*p.app,L"native-origin"),{p.desktop/L"Sentinel.txt"}),
            "moving a selected member removes only that logical ownership");
        const auto& moved=Find(*p.app,moving);
        Require(Access::FreeDesktopCopies(*p.app,moving)==1 && moved.gridCell.pageId==L"__page:1" && moved.gridCell.column==6 && moved.gridCell.row==4,
            "released member becomes an accessible file at its requested desktop cell");
        Require(Read(moving)=="move-logical-owner" && Read(p.desktop/L"Sentinel.txt")=="untouched",
            "logical ownership transfer leaves physical files and unselected bytes intact");
    };
    check(); Require(Access::Save(*p.app),"persist member transfer");
    RefreshContainer(p,true,L"native-origin",false); check();
}
void ReorderMembers()
{
    Profile p; std::vector<fs::path> paths;
    for(wchar_t letter : {L'A',L'B',L'C'})
    {
        paths.push_back(p.desktop/(std::wstring(1,letter)+L".txt"));
        Write(paths.back(),std::string(1,static_cast<char>(letter)));
    }
    Access::SeedMemberOrder(*p.app,paths,p.desktop/L"Sentinel.txt");
    Require(Access::Save(*p.app),"save automatic name order");
    const auto& slots=Access::MemberContainer(*p.app,L"members")->GetSlots();
    Require(!slots.empty() && slots.front()->GetItem(),"first real member must be hittable");
    Require(fs::equivalent(slots.front()->GetItem()->GetPath(),paths[0]),"name sorting places A first before the manual insertion");
    const RECT frame=slots.front()->GetBounds(); const POINT beforeA{frame.left+1,(frame.top+frame.bottom)/2};
    {
        IsolatedPointer pointer;
        Access::BeginContainerMembers(*p.app,L"members",{paths[1]});
        Access::ReleaseContainerMembers(*p.app,beforeA);
    }
    const auto check=[&] {
        const auto actual=Access::ContainerMembers(*p.app,L"members");
        const std::vector<fs::path> expected{paths[1],paths[0],paths[2]};
        Require(actual.size()==expected.size() && std::equal(actual.begin(),actual.end(),expected.begin(),[](const auto& a,const auto& b) { return fs::equivalent(a,b); }) &&
            Access::ManualMemberOrder(*p.app),"manual insertion before A yields B,A,C and exits automatic sorting");
        for(std::size_t i=0;i<paths.size();++i) Require(Read(paths[i])==std::string(1,static_cast<char>('A'+i)),"reorder cannot change file bytes");
    };
    check(); Require(Access::Save(*p.app),"persist manual ordering"); RefreshContainer(p,true,L"members",false); check();
}
void ChangedTarget(bool mapped)
{
    Profile p; fs::path existing;
    const ContainerRow row=mapped ? containerRows[2] : containerRows[0]; SeedDestination(p,row,existing);
    const auto sourcePath=p.root/L"incoming/Late.txt", replacement=p.root/L"replacement";
    Write(sourcePath,"late-payload"); Write(replacement/L"Keep.txt","replacement-owned");
    auto source=ole_file_source_fixture::Make({sourcePath},true);
    auto adapter=Access::ExternalAdapter(*p.app); DWORD effect=DROPEFFECT_COPY | DROPEFFECT_MOVE;
    const POINT point{350,250};
    Require(SUCCEEDED(adapter->DragEnter(source.Get(),MK_LBUTTON|MK_CONTROL,Access::ExternalPoint(point),&effect)) && effect==DROPEFFECT_COPY,
        "capture original container through actual OLE route");
    Access::FreshExternalDwell(*p.app); effect=DROPEFFECT_COPY | DROPEFFECT_MOVE;
    Require(SUCCEEDED(adapter->Drop(source.Get(),MK_CONTROL,Access::ExternalPoint(point),&effect)) && source->starts==1,"submit independently retained delayed read");
    HeldRead gate; gate.Capture();
    if(mapped) Access::ChangeContainerMapping(*p.app,replacement); else Access::ReplaceContainerIdentity(*p.app);
    Require(Access::Save(*p.app),"persist user's intervening target change");
    const auto saved=Read(Access::LayoutPath(*p.app));
    gate.Deliver();
    Wait([&] { return source->ends==1 && Access::ContainerWorkIdle(*p.app); });
    Require(FAILED(source->outcome) && source->starts==1 && source->ends==1 && Read(sourcePath)=="late-payload" &&
        !fs::exists(existing.parent_path()/L"Late.txt") && !fs::exists(replacement/L"Late.txt") && Read(replacement/L"Keep.txt")=="replacement-owned",
        "changed identity or mapping cancels unsubmitted file work without redirecting or losing the source");
    Require(Read(Access::LayoutPath(*p.app))==saved,"cancelled late work cannot overwrite user's replacement target");
    const auto id=mapped ? L"ingress-root" : L"replacement";
    RefreshContainer(p,true,id,mapped);
    const auto& restored=ContainerWidget(*p.app,id);
    Require(mapped ? restored.sourceFolderPath==replacement.wstring() : restored.id==L"replacement",
        "target change survives actual disk reload after late completion");
}
bool RunContainerScenario(std::string_view row)
{
    const auto target=std::find_if(std::begin(containerRows),std::end(containerRows),[&](const auto& v) { return v.name==row; });
    if(target!=std::end(containerRows)) ReceiveContainer(*target);
    else if(row=="member-collection") InternalMember(false);
    else if(row=="member-files") InternalMember(true);
    else if(row=="member-reorder") ReorderMembers();
    else if(row=="right-copy") RightDecision(false);
    else if(row=="right-cancel") RightDecision(true);
    else if(row=="target-replaced") ChangedTarget(false);
    else if(row=="target-remapped") ChangedTarget(true);
    else return false;
    return true;
}
}
