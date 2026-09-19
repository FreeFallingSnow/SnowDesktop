#include "pch.h"
#include "context_menu_page_presenter.h"
#include "settings_presenter_controls.h"
#include "../shell_extension_menu_presentation.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <set>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace ext = snowdesktop::shell_extensions;
using presenter_controls::SettingRow;
struct ContextMenuPagePresenter::Impl : std::enable_shared_from_this<Impl>
{
    LocalizeCallback localize;
    PersonalizationPageActions actions;
    muxc::StackPanel root, rows;
    muxc::ToggleSwitch enabled, background;
    muxc::TextBox sample, search;
    muxc::TextBlock status, hint;
    muxc::CommandBar toolbar;
    muxc::AppBarButton file, folder, refresh, preview;
    SettingRow enabledRow, backgroundRow;
    mux::DispatcherTimer timer;
    std::vector<std::function<void()>> revoke, rowRevoke;
    ext::Preferences prefs;
    ext::Request request;
    std::vector<ext::Entry> catalogue;
    std::map<std::string,std::vector<ext::Entry>> commands;
    std::unique_ptr<ext::Session> session;
    std::string pendingProvider;
    std::uint64_t generation=0;
    int appearance=0;
    bool active=false,closed=false,updating=false,initialized=false;
    std::wstring L(std::string_view key)const{return localize?localize(key):std::wstring{};}
    Impl(LocalizeCallback l,const mux::Style& card):localize(std::move(l))
    {
        root.Spacing(8);rows.Spacing(6);
        muxc::Border preferences;preferences.Style(card);muxc::StackPanel controls;controls.Spacing(8);
        enabledRow.Initialize(enabled);backgroundRow.Initialize(background);
        controls.Children().Append(enabledRow.root);controls.Children().Append(backgroundRow.root);
        preferences.Child(controls);root.Children().Append(preferences);
        sample.IsReadOnly(true);sample.TextWrapping(mux::TextWrapping::Wrap);root.Children().Append(sample);
        toolbar.DefaultLabelPosition(muxc::CommandBarDefaultLabelPosition::Right);
        for(auto b:{file,folder,refresh,preview})toolbar.PrimaryCommands().Append(b);
        root.Children().Append(toolbar);root.Children().Append(search);root.Children().Append(hint);root.Children().Append(status);root.Children().Append(rows);
        hint.TextWrapping(mux::TextWrapping::Wrap);status.TextWrapping(mux::TextWrapping::Wrap);
        timer.Interval(std::chrono::milliseconds(100));
        auto tick=timer.Tick([this](const auto&,const auto&){Poll();});revoke.push_back([t=timer,tick]{t.Tick(tick);});
        auto toggle=enabled.Toggled([this](const auto&,const auto&){
            if(updating||!active||!initialized||!actions.updateGeneral)return;
            const bool value=enabled.IsOn();actions.updateGeneral(generation,SettingsUpdateMode::PreviewAndCommit,[value](auto& s){s.shellExtensions.enabled=value;});
        });revoke.push_back([c=enabled,toggle]{c.Toggled(toggle);});
        auto bg=background.Toggled([this](const auto&,const auto&){if(!updating&&active){request.background=background.IsOn();Reload();}});
        revoke.push_back([c=background,bg]{c.Toggled(bg);});
        auto filter=search.TextChanged([this](const auto&,const auto&){if(!closed)BuildRows();});revoke.push_back([c=search,filter]{c.TextChanged(filter);});
        Click(file,[this]{Pick(false);});Click(folder,[this]{Pick(true);});Click(refresh,[this]{Reload();});Click(preview,[this]{Preview();});
        PWSTR desktop=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop,0,nullptr,&desktop)))
        {request.paths.emplace_back(desktop);CoTaskMemFree(desktop);}
        request.background=true;updating=true;background.IsOn(true);updating=false;
        Text();
    }
    template<class F>void Click(muxc::AppBarButton b,F f)
    {auto token=b.Click([this,f](const auto&,const auto&){if(active&&!closed)f();});revoke.push_back([b,token]{b.Click(token);});}
    void Text()
    {
        enabledRow.SetText(L("settings.contextMenu.enabled"),L("settings.contextMenu.enabledDescription"));
        backgroundRow.SetText(L("settings.contextMenu.background"),L("settings.contextMenu.backgroundDescription"));
        file.Label(L("settings.contextMenu.file"));folder.Label(L("settings.contextMenu.folder"));refresh.Label(L("settings.contextMenu.refresh"));preview.Label(L("settings.contextMenu.preview"));
        search.PlaceholderText(L("settings.contextMenu.search"));sample.Header(winrt::box_value(L("settings.contextMenu.sample")));
        hint.Text(L("settings.contextMenu.hint"));BuildRows();
    }
    void Pick(bool folders)
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
        DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|(folders?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST));
        if(FAILED(dialog->Show(GetActiveWindow()))||closed||!active)return;
        Microsoft::WRL::ComPtr<IShellItem> selected;PWSTR path=nullptr;
        if(SUCCEEDED(dialog->GetResult(&selected))&&SUCCEEDED(selected->GetDisplayName(SIGDN_FILESYSPATH,&path)))
        {request.paths={path};CoTaskMemFree(path);if(!folders){updating=true;background.IsOn(false);updating=false;request.background=false;}Reload();}
    }
    void Cancel(){timer.Stop();session.reset();pendingProvider.clear();}
    void Query(std::string provider)
    {
        Cancel();if(!active||closed||request.paths.empty())return;
        auto query=request;query.catalogueOnly=provider.empty();if(!provider.empty())query.providers={provider};
        pendingProvider=std::move(provider);status.Text(L("settings.contextMenu.loading"));
        try{session=std::make_unique<ext::Session>(query);timer.Start();}catch(...){status.Text(L("settings.contextMenu.failed"));}
    }
    void Reload()
    {catalogue.clear();commands.clear();BuildRows();sample.Text(request.paths.empty()?L"":request.paths.front());Query({});}
    void Poll()
    {
        if(!session)return;auto result=session->Poll();if(!result)return;timer.Stop();
        if(!result->ok){status.Text(L("settings.contextMenu.failed"));session.reset();return;}
        if(pendingProvider.empty())catalogue=std::move(result->entries);
        else{auto& list=commands[pendingProvider];list.clear();for(auto& group:result->entries)list.insert(list.end(),group.children.begin(),group.children.end());}
        status.Text(catalogue.empty()?L("settings.contextMenu.empty"):L"" );session.reset();pendingProvider.clear();BuildRows();
    }
    void Save(const ext::Entry& entry,int index,bool individual)
    {
        if(updating||!active||!initialized||!actions.updateGeneral||index<0)return;
        ext::Selection selection{entry.provider,individual?entry.key:"",winrt::to_string(entry.label),static_cast<ext::Placement>(individual?index-1:index)};
        const bool inherit=individual&&index==0;
        actions.updateGeneral(generation,SettingsUpdateMode::PreviewAndCommit,[selection,inherit](auto& s){
            auto& values=s.shellExtensions.selections;
            std::erase_if(values,[&](const auto& old){return old.provider==selection.provider&&old.command==selection.command;});
            if(!inherit)values.push_back(selection);
            ext::Normalize(s.shellExtensions);
        });
    }
    void Row(muxc::StackPanel parent,const ext::Entry& entry,bool individual)
    {
        muxc::ComboBox choice;if(individual)choice.Items().Append(winrt::box_value(L("settings.contextMenu.inherit")));
        for(const char* key:{"settings.contextMenu.hidden","settings.contextMenu.submenu","settings.contextMenu.root"})choice.Items().Append(winrt::box_value(L(key)));
        int index=0;for(const auto& s:prefs.selections)if(s.provider==entry.provider&&s.command==(individual?entry.key:""))index=static_cast<int>(s.placement)+(individual?1:0);
        choice.SelectedIndex(index);choice.MinWidth(150);
        auto row=std::make_shared<SettingRow>();row->Initialize(choice);row->SetText(entry.label,individual?L"":L("settings.contextMenu.group"));
        row->SetControlAlignment(mux::HorizontalAlignment::Right);parent.Children().Append(row->root);
        auto token=choice.SelectionChanged([this,entry,individual,choice,row](const auto&,const auto&){Save(entry,choice.SelectedIndex(),individual);});
        rowRevoke.push_back([choice,token,row]{choice.SelectionChanged(token);});
    }
    void Children(muxc::StackPanel parent,const std::vector<ext::Entry>& list,const std::wstring& prefix=L"")
    {
        for(auto entry:list)
        {
            if(entry.separator)continue;entry.label=prefix+entry.label;
            if(!entry.children.empty()){Children(parent,entry.children,entry.label+L" / ");continue;}
            if(entry.key.empty()||entry.native){muxc::TextBlock text;text.Text(entry.label+L" · "+L("settings.contextMenu.groupOnly"));text.TextWrapping(mux::TextWrapping::Wrap);parent.Children().Append(text);}
            else Row(parent,entry,true);
        }
    }
    void BuildRows()
    {
        for(auto& r:rowRevoke)r();rowRevoke.clear();rows.Children().Clear();
        const auto filter=winrt::to_string(search.Text());
        std::vector<ext::Entry> visible=catalogue;std::set<std::string> known;for(const auto& p:catalogue)known.insert(p.provider);
        // Preserve choices for uninstalled or context-dependent providers and
        // allow users to remove them even when the current sample lacks them.
        for(const auto& s:prefs.selections)if(known.insert(s.provider).second)
        {ext::Entry missing;missing.provider=s.provider;missing.label=winrt::to_hstring(s.label);missing.label+=L" · "+L("settings.contextMenu.unavailable");visible.push_back(std::move(missing));}
        for(const auto& entry:visible)
        {
            if(!filter.empty())
            {auto label=entry.label;auto query=std::wstring(search.Text());for(auto& c:label)c=towlower(c);for(auto& c:query)c=towlower(c);if(label.find(query)==std::wstring::npos)continue;}
            muxc::Expander expander;expander.HorizontalAlignment(mux::HorizontalAlignment::Stretch);expander.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            muxc::StackPanel header;Row(header,entry,false);expander.Header(header);
            muxc::StackPanel details;details.Spacing(6);
            const auto found=commands.find(entry.provider);
            if(found!=commands.end()){Children(details,found->second);expander.IsExpanded(true);}
            else
            {
                muxc::Button inspect;inspect.Content(winrt::box_value(L("settings.contextMenu.inspect")));details.Children().Append(inspect);
                auto token=inspect.Click([this,id=entry.provider](const auto&,const auto&){Query(id);});rowRevoke.push_back([inspect,token]{inspect.Click(token);});
            }
            expander.Content(details);rows.Children().Append(expander);
        }
    }
    void Preview()
    {
        Cancel();if(request.paths.empty())return;
        ext::Presentation presentation(request,prefs,L("settings.contextMenu.extensions"),L("settings.contextMenu.loading"),L("settings.contextMenu.failed"),L("settings.contextMenu.native"));
        modern_menu::Item title;title.label=L("settings.contextMenu.previewHint");title.enabled=false;
        std::vector<modern_menu::Item> items{title};modern_menu::Options options;options.owner=GetActiveWindow();GetCursorPos(&options.anchor);
        options.dpi=GetDpiForWindow(options.owner);options.appearance=static_cast<modern_menu::Appearance>(appearance);
        presentation.Attach(items,options);modern_menu::Show(items,options);
    }
    void Deactivate(){active=false;Cancel();}
    void Close(){if(closed)return;closed=true;Deactivate();for(auto& r:rowRevoke)r();rowRevoke.clear();for(auto& r:revoke)r();revoke.clear();actions={};}
};
ContextMenuPagePresenter::ContextMenuPagePresenter(LocalizeCallback l,const mux::Style& s):impl_(std::make_shared<Impl>(std::move(l),s)){}
ContextMenuPagePresenter::~ContextMenuPagePresenter(){Close();}
void ContextMenuPagePresenter::SetActions(PersonalizationPageActions a){impl_->actions=std::move(a);}
mux::UIElement ContextMenuPagePresenter::Content()const{return impl_->root;}
void ContextMenuPagePresenter::ApplySnapshot(const SettingsSnapshot& s)
{
    if(impl_->closed)return;
    if(impl_->generation!=s.generation)impl_->Cancel();impl_->generation=s.generation;impl_->initialized=s.initialized;
    const bool changed=impl_->prefs!=s.values.general.shellExtensions;
    impl_->prefs=s.values.general.shellExtensions;impl_->appearance=s.values.personalization.contextMenuStyle;
    impl_->updating=true;impl_->enabled.IsOn(impl_->prefs.enabled);impl_->updating=false;
    if(changed)impl_->BuildRows();if(!s.sessionActive)impl_->Deactivate();
}
void ContextMenuPagePresenter::RefreshLocalizedText(){impl_->Text();}
void ContextMenuPagePresenter::Activate(){if(impl_->closed)return;const bool was=impl_->active;impl_->active=true;if(!was&&impl_->catalogue.empty())impl_->Reload();}
void ContextMenuPagePresenter::Deactivate(){impl_->Deactivate();}
void ContextMenuPagePresenter::Close(){if(impl_)impl_->Close();}
void ContextMenuPagePresenter::RegisterFocusTargets(const FocusRegistrar& r)const{r("contextMenu.extensions",impl_->enabled);r("contextMenu.items",impl_->search);}
}
