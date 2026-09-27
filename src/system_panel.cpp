#include "system_panel.h"
#include "system_panel_model.h"
#include "system_control_prompt.h"
#include "app/desktop_backdrop_compositor.h"
#include "quick_navigation_animation_rules.h"
#include "animation_settings.h"
#include "l10n.h"
#include "diagnostic_log.h"
#include "native_tooltip.h"
#include "widget_accessibility_provider.h"
#include <d2d1_1helper.h>
#include <wrl/client.h>
#include <windowsx.h>
#include <commctrl.h>
#include <cmath>
#include <utility>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace ui=native_ui;
namespace wr=widget_runtime;
namespace
{
constexpr UINT kOpenPending=WM_APP+211;
constexpr UINT_PTR kTrayMenuTimer=2;
bool HighContrast(){HIGHCONTRASTW h{sizeof(h)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(h),&h,0);return(h.dwFlags&HCF_HIGHCONTRASTON)!=0;}
D2D1_COLOR_F SystemColor(int index){const auto c=GetSysColor(index);return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f);}
using PromptState=SystemControlPromptState;
struct PanelLifetime { bool alive=true; };
}
struct SystemPanel::Impl
{
    struct Request{StatusBarAction action;HWND owner;RECT anchor;PersonalizationSettings appearance;StatusBarSettings settings;std::shared_ptr<tray::Service> tray;std::shared_ptr<wr::WidgetSystemDataProvider> data;};
    SettingsChanged changed;SystemCalendarActions calendar;std::function<bool(std::string_view,POINT)> dropOutside;Background background;
    TrayDragFeedback dragFeedback;std::function<void(HWND,RECT)> nativeControls;
    UiAnimationScheduler* scheduler=nullptr;UiScheduleToken animationToken=0;quick_navigation_animation_rules::State slide;
    ComPtr<IDCompositionDesktopDevice> composition;ComPtr<IDWriteFactory> text;ComPtr<IDCompositionTarget> target;ComPtr<IDCompositionVisual2> visual;ComPtr<IDCompositionSurface> surface;
    DesktopBackdropCompositor backdrop;HWND window=nullptr;NativeTooltip tooltip;HMONITOR monitor=nullptr;std::optional<Request> current,pending;
    std::unique_ptr<WidgetAccessibilityProviderHost> accessibility;
    std::shared_ptr<SystemPanelModel> model;ui::Input input;std::string hovered;std::function<void()> afterClose;
    std::shared_ptr<PromptState> promptState;
    std::shared_ptr<PanelLifetime> lifetime=std::make_shared<PanelLifetime>();
    bool paintDirty=true;
    bool scrollbarDragging=false;int scrollbarPointerStart=0,scrollbarOffsetStart=0;
    bool trayPreview=false;std::optional<D2D1_RECT_F> dropIndicator;HWND retainedAbove=nullptr;
    std::vector<RECT> cards;float scale=1;int width=0,height=0;bool showing=false,closing=false,modal=false,destroying=false;tray::MenuRetentionSession context;WPARAM closeGeneration=0;
    Impl(SettingsChanged c,SystemCalendarActions dates,std::function<bool(std::string_view,POINT)> drop,UiAnimationScheduler* timing,IDCompositionDesktopDevice* graphics,IDWriteFactory* fonts,Background draw)
        :changed(std::move(c)),calendar(std::move(dates)),dropOutside(std::move(drop)),background(std::move(draw)),scheduler(timing),composition(graphics),text(fonts){}
    ~Impl(){lifetime->alive=false;destroying=true;HideNow();tooltip.Close();if(accessibility)accessibility->DetachWindow(window);backdrop.Reset();surface.Reset();visual.Reset();target.Reset();if(window)DestroyWindow(window);}
    bool Ensure()
    {
        if(window&&target&&visual)return true;if(window){tooltip.Close();if(accessibility)accessibility->DetachWindow(window);DestroyWindow(window);window=nullptr;}if(!composition||!text)return false;WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=Procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"SnowDesktop.NativeSystemPanel";cls.style=CS_DBLCLKS;RegisterClassExW(&cls);
        window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOREDIRECTIONBITMAP,cls.lpszClassName,L"",WS_POPUP,0,0,1,1,nullptr,nullptr,cls.hInstance,this);if(!window)return false;
        accessibility=std::make_unique<WidgetAccessibilityProviderHost>([this]{return Accessible();},[this](const auto&,const auto& id){if(!showing||closing||modal||slide.IsAnimating()||!model||!input.Focus(id))return false;if(model->Reveal(id))Arrange();SetFocus(window);Paint();if(accessibility)accessibility->RefreshEvents();return true;},[this](const auto& request){return AccessibleAction(request);});
        accessibility->AttachWindow(window);
        return SUCCEEDED(composition->CreateTargetForHwnd(window,FALSE,&target))&&SUCCEEDED(composition->CreateVisual(&visual))&&SUCCEEDED(target->SetRoot(visual.Get()));
    }
    bool Glass()const{return current&&current->appearance.glassEnabled&&!HighContrast();}
    void EndDragFeedback()
    {if(std::exchange(trayPreview,false)&&dragFeedback.end)dragFeedback.end();}
    void MoveDragFeedback(POINT screen)
    {
        if(!current||!model||!input.Dragging()||!input.Pressed().starts_with("tray:"))return;
        const auto key=input.Pressed().substr(5);
        if(!trayPreview&&current->tray&&dragFeedback.begin)
            for(const auto& icon:current->tray->Current().icons)if(icon.key==key)
            {trayPreview=true;dragFeedback.begin(window,icon,screen,static_cast<UINT>(std::lround(28*scale)));break;}
        if(dragFeedback.move)dragFeedback.move(key,screen);
    }
    void Tip(const ui::Node* node)
    {
        if(!node||!current||closing||modal){tooltip.Hide();return;}
        POINT origin{};ClientToScreen(window,&origin);const auto& b=node->bounds;
        RECT anchor{origin.x+static_cast<LONG>(b.left*scale),origin.y+static_cast<LONG>(b.top*scale),origin.x+static_cast<LONG>(b.right*scale),origin.y+static_cast<LONG>(b.bottom*scale)};
        tooltip.SetTarget(node->id,node->tooltip,anchor,current->settings.position==DockPosition::Bottom?NativeTooltipPlacement::Above:NativeTooltipPlacement::Below);
    }
    ui::Palette Palette()const
    {
        return SystemPanelPalette(current->appearance,HighContrast());
    }
    std::vector<LuaWidgetAccessibilitySnapshot> Accessible() const
    {
        if(!showing||closing||modal||slide.IsAnimating()||!model)return {};
        LuaWidgetAccessibilitySnapshot snapshot;snapshot.widgetId=L"system-panel";
        snapshot.name=_L(current->action==StatusBarAction::Calendar?"statusBar.clock":current->action==StatusBarAction::Tray?"statusBar.tray":"statusBar.controlCenter");snapshot.bounds={0,0,width,height};
        const auto regions=input.AccessibilityRegions();std::string focus;
        for(const auto& r:regions)if(GetFocus()==window&&input.Identity(r.key)==input.Focused())focus=r.key;
        // The Lua collector's per-frame limit remains unchanged. A native
        // device list includes offscreen items, collected in bounded batches.
        constexpr auto batchSize=wr::WidgetInteractionRegions::kMaximumRegions;
        for(std::size_t first=0;first<regions.size();first+=batchSize)
        {
            const auto last=(std::min)(regions.size(),first+batchSize);
            std::vector<wr::InteractionRegion> batch(regions.begin()+static_cast<std::ptrdiff_t>(first),regions.begin()+static_cast<std::ptrdiff_t>(last));
            std::vector<wr::ViewAccessibilityNode> nodes;
            if(!wr::CollectInteractionAccessibilityNodes(batch,model->View().width,model->View().height,focus,nodes,snapshot.error)){snapshot.nodes.clear();return {std::move(snapshot)};}
            for(auto& node:nodes)snapshot.nodes.push_back(std::move(node));
        }
        for(auto& node:snapshot.nodes)
        {
            node.key=input.Identity(node.key);node.bounds.x*=scale;node.bounds.y*=scale;node.bounds.width*=scale;node.bounds.height*=scale;
            if(node.clip){node.clip->x*=scale;node.clip->y*=scale;node.clip->width*=scale;node.clip->height*=scale;}
        }
        if(model->MaximumScroll()>0)
        {
            const auto clip=model->ScrollViewport();wr::ViewAccessibilityNode scroll;scroll.semanticId=scroll.key="panel.scroll";scroll.role="group";scroll.controlType="pane";scroll.name=snapshot.name;scroll.patterns=wr::ViewAccessibilityPattern::Scroll;
            scroll.bounds={clip.left*scale,clip.top*scale,(clip.right-clip.left)*scale,(clip.bottom-clip.top)*scale};scroll.scrollOffset=model->ScrollOffset();scroll.scrollViewportExtent=clip.bottom-clip.top;scroll.scrollContentExtent=scroll.scrollViewportExtent+model->MaximumScroll();snapshot.nodes.push_back(std::move(scroll));
        }
        return {std::move(snapshot)};
    }
    bool AccessibleAction(const LuaWidgetAccessibilityActionRequest& request)
    {
        if(!showing||closing||modal||slide.IsAnimating()||!model||request.widgetId!=L"system-panel")return false;
        if(request.nodeKey=="panel.scroll"&&request.kind==LuaWidgetAccessibilityActionKind::SetScrollOffset)
        {if(!std::isfinite(request.numericValue))return false;model->Scroll(static_cast<float>(request.numericValue)-model->ScrollOffset());Arrange();Paint();if(accessibility)accessibility->RefreshEvents();return true;}
        const auto* node=model->View().Find(request.nodeKey);if(!node||!node->Interactive())return false;
        if(model->Reveal(request.nodeKey)){Arrange();node=model->View().Find(request.nodeKey);if(!node||!node->Interactive())return false;}
        ui::InputResult result;result.id=node->id;
        switch(request.kind)
        {
        case LuaWidgetAccessibilityActionKind::SetRangeValue:
            if(node->role!=ui::Role::Slider||!std::isfinite(request.numericValue)||request.numericValue<0||request.numericValue>1)return false;
            result.kind=ui::InputResult::Kind::Value;result.value=static_cast<float>(request.numericValue);break;
        case LuaWidgetAccessibilityActionKind::Invoke:
        case LuaWidgetAccessibilityActionKind::Toggle:
        case LuaWidgetAccessibilityActionKind::Select:result.kind=ui::InputResult::Kind::Invoke;break;
        default:return false;
        }
        POINT point{static_cast<LONG>((node->bounds.left+node->bounds.right)*scale/2),static_cast<LONG>((node->bounds.top+node->bounds.bottom)*scale/2)};ClientToScreen(window,&point);Result(std::move(result),point,true);return true;
    }
    void Open(Request request)
    {
        if(modal){pending=std::move(request);return;}
        if(!Ensure())return;current=std::move(request);const auto& r=*current;monitor=MonitorFromRect(&r.anchor,MONITOR_DEFAULTTONEAREST);scale=GetDpiForWindow(r.owner)/96.f;
        auto source=LiveSystemPanelSource(r.data);source.calendar=calendar;source.tray=[service=r.tray]{return service?service->Current():tray::Snapshot{};};
        source.nativeControls=[this] {if(current&&nativeControls){const auto fn=nativeControls;fn(current->owner,current->anchor);}};
        source.trayChanged=[this](const auto& value){if(current)current->settings=value;if(changed)changed(value);};
        source.prompt=[this,life=lifetime](auto& request)
        {
            if(!life->alive||modal||!showing||closing)return false;
            tooltip.Hide();modal=true;const auto activeModel=model;
            const auto state=promptState=std::make_shared<PromptState>();
            const auto wifi=current&&current->data?current->data->Controls()->Current("network.wifi"):std::nullopt;
            const bool result=ConfirmSystemControl(window,request,state,{},wifi?&*wifi:nullptr);
            if(!life->alive)return false;
            promptState.reset();modal=false;
            // A replacement is opened only after the dialog's nested loop has
            // restored its owner, so it cannot steal focus back from the new UI.
            if(!showing&&!destroying&&(pending||afterClose))PostMessageW(window,kOpenPending,++closeGeneration,0);
            return result&&!state->cancelled&&showing&&!closing&&model==activeModel;
        };
        tooltip.Configure(window,composition.Get(),text.Get(),r.appearance,background);input={};scrollbarDragging=false;paintDirty=true;
        model=std::make_shared<SystemPanelModel>(std::move(source),r.settings,r.action);showing=true;closing=false;Arrange();Paint();Animate(true);backdrop.SetPopupWindowPairZOrder(window,HWND_TOPMOST,true);if(Glass())backdrop.ShowPopupWindowPair(window);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);SetFocus(window);SetTimer(window,1,500,nullptr);
    }
    void Arrange()
    {
        if(!model||!current)return;MONITORINFO info{sizeof(info)};if(!GetMonitorInfoW(monitor,&info))return;
        const auto previousScene=model->View();model->Refresh((info.rcWork.bottom-info.rcWork.top)/scale-12,(info.rcWork.right-info.rcWork.left)/scale-12);const auto& scene=model->View();
        const bool contentChanged=!previousScene.SameContent(scene);paintDirty|=contentChanged;input.Sync(scene);if(scrollbarDragging&&!model->ScrollbarGeometry().CanDrag())scrollbarDragging=false;if(input.Pressed().empty()&&!scrollbarDragging&&GetCapture()==window)ReleaseCapture();
        const int w=static_cast<int>(std::ceil(scene.width*scale)),h=static_cast<int>(std::ceil(scene.height*scale));
        const auto& a=current->anchor;const int left=std::clamp<int>(current->action==StatusBarAction::Calendar?(a.left+a.right-w)/2:a.right-w,static_cast<int>(info.rcWork.left),static_cast<int>((std::max)(info.rcWork.left,info.rcWork.right-w)));
        const int top=std::clamp<int>(current->settings.position==DockPosition::Bottom?a.top-h-static_cast<int>(6*scale):a.bottom+static_cast<int>(6*scale),static_cast<int>(info.rcWork.top),static_cast<int>((std::max)(info.rcWork.top,info.rcWork.bottom-h)));
        std::vector<RECT> next;for(const auto& c:scene.cards)next.push_back({static_cast<LONG>(std::lround(c.left*scale)),static_cast<LONG>(std::lround(c.top*scale)),static_cast<LONG>(std::lround(c.right*scale)),static_cast<LONG>(std::lround(c.bottom*scale))});
        const bool shape=cards.size()!=next.size()||!std::equal(cards.begin(),cards.end(),next.begin(),[](const auto& x,const auto& y){return EqualRect(&x,&y);});cards=std::move(next);
        if(w!=width||h!=height){width=w;height=h;surface.Reset();paintDirty=true;}RECT previous{};GetWindowRect(window,&previous);const bool moved=previous.left!=left||previous.top!=top||previous.right!=left+w||previous.bottom!=top+h;
        if(moved)SetWindowPos(window,nullptr,left,top,w,h,SWP_NOACTIVATE|SWP_NOZORDER);
        if(Glass()&&(moved||shape||!backdrop.IsAvailable()))
        {
            if(!backdrop.IsAvailable())backdrop.InitializePopup(window,true,false);
            backdrop.Reattach(window);backdrop.BeginFrame(true);
            for(std::size_t i=0;i<cards.size();++i)backdrop.AddPanel(cards[i],current->appearance.cornerRadius*scale,current->appearance.glassBlurRadius*scale,reinterpret_cast<std::uintptr_t>(this)+i);
            backdrop.EndFrame(false);Pose();
            // A rebuilt helper starts hidden even when the content HWND is
            // already shown. Restore the pair without changing keyboard focus.
            if(showing)SyncTrayMenuLayer(true);
            backdrop.SetVisible(showing);
        }
        else if(!Glass())backdrop.Reset();if(moved||shape)Pose();if(moved||shape||contentChanged)PublishGeometry();
        if(!hovered.empty())Tip(scene.Find(hovered));
        if(accessibility&&(contentChanged||moved||shape))accessibility->RefreshEvents();
    }
    void PublishGeometry()
    {
        if(!current||!current->tray||!model)return;POINT origin{};ClientToScreen(window,&origin);
        for(const auto& n:model->View().nodes)if(n.id.starts_with("tray:")){const auto& r=n.bounds;RECT box{origin.x+static_cast<LONG>(r.left*scale),origin.y+static_cast<LONG>(r.top*scale),origin.x+static_cast<LONG>(r.right*scale),origin.y+static_cast<LONG>(r.bottom*scale)};current->tray->SetGeometry(n.id.substr(5),box);}
    }
    void Paint()
    {
        if(!model||!current||!visual||width<=0||height<=0)return;
        if(!surface&&FAILED(composition->CreateSurface(width,height,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&surface)))return;
        ComPtr<ID2D1DeviceContext> dc;POINT offset{};if(FAILED(surface->BeginDraw(nullptr,IID_PPV_ARGS(&dc),&offset)))return;
        dc->SetDpi(96,96);dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));dc->Clear(D2D1::ColorF(0,0.f));
        for(const auto& card:cards)
        {
            if(HighContrast()){ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(SystemColor(COLOR_WINDOW),&b);dc->FillRoundedRectangle(D2D1::RoundedRect({static_cast<float>(card.left),static_cast<float>(card.top),static_cast<float>(card.right),static_cast<float>(card.bottom)},current->appearance.cornerRadius*scale,current->appearance.cornerRadius*scale),b.Get());}
            else if(background)background(dc.Get(),card,current->appearance,scale);
        }
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        const auto result=ui::Draw(dc.Get(),text.Get(),model->View(),Palette(),hovered,GetFocus()==window?input.VisibleFocus():std::string_view{},input.Pressed());
        if(dropIndicator)
        {
            ComPtr<ID2D1SolidColorBrush> line;if(SUCCEEDED(dc->CreateSolidColorBrush(Palette().accent,&line)))
            {
                if(dropIndicator->right-dropIndicator->left<=3)dc->FillRectangle(*dropIndicator,line.Get());
                else dc->DrawRoundedRectangle(D2D1::RoundedRect(*dropIndicator,4,4),line.Get(),2);
            }
        }
        dc.Reset();const auto end=surface->EndDraw();
        if(SUCCEEDED(result)&&SUCCEEDED(end)){visual->SetContent(surface.Get());composition->Commit();paintDirty=false;}else surface.Reset();
    }
    void Pose()
    {
        if(!visual||!current||!window)return;const float p=quick_navigation_animation_rules::EaseInOutSmooth(slide.GetVisual().progress);const float y=(current->settings.position==DockPosition::Bottom?1.f:-1.f)*(1-p)*height;
        visual->SetOffsetY(y);HRGN region=CreateRectRgn(0,0,0,0);const int dyTop=static_cast<int>(std::floor(y)),dyBottom=static_cast<int>(std::ceil(y));
        for(std::size_t i=0;i<cards.size();++i){const auto& c=cards[i];const int radius=static_cast<int>(std::lround(current->appearance.cornerRadius*2*scale));HRGN part=CreateRoundRectRgn(c.left,c.top+dyTop,c.right+1,c.bottom+dyBottom+1,radius,radius);CombineRgn(region,region,part,RGN_OR);DeleteObject(part);if(Glass()){RECT projected=c;projected.top+=dyTop;projected.bottom+=dyBottom;backdrop.SetPanelTransform(reinterpret_cast<std::uintptr_t>(this)+i,D2D1::Matrix4x4F::Translation(0,y,0),projected);}}
        const auto clip=CreateRectRgn(0,0,width,height);CombineRgn(region,region,clip,RGN_AND);DeleteObject(clip);if(!SetWindowRgn(window,region,FALSE))DeleteObject(region);
        composition->Commit();if(Glass())backdrop.CommitVisualChanges();
    }
    void Animate(bool opening)
    {
        if(!opening){KillTimer(window,kTrayMenuTimer);EndDragFeedback();dropIndicator.reset();if(GetCapture()==window)ReleaseCapture();}
        if(!opening&&modal){if(model)model->Close();CancelPrompt();}
        if(scheduler)scheduler->Cancel(animationToken);animationToken=0;
        if(!scheduler||animation::RuntimePopupEffect()==animation::NoEffect){if(opening){slide.ShowImmediately();Pose();}else FinishClose();return;}
        slide.Configure(quick_navigation_animation_rules::Effect::Fade,animation::RuntimeDurationScale());const auto now=static_cast<std::uint64_t>(UiAnimationScheduler::MonotonicMilliseconds());
        if(opening){slide.ResetHidden();slide.Open(now);}else{closing=true;input.Cancel();Tip(nullptr);slide.Close(now);}Pose();
        animationToken=scheduler->StartAnimation(UiAnimationSurface::Popup,[this](double time){slide.Advance(static_cast<std::uint64_t>(time));Pose();if(slide.IsAnimating())return true;animationToken=0;if(slide.IsHidden())FinishClose();else if(accessibility)accessibility->RefreshEvents();return false;});
    }
    void CancelPrompt(){if(promptState){promptState->cancelled=true;if(promptState->window)EndDialog(promptState->window,IDCANCEL);}}
    void FinishClose(){HideNow();if(!destroying&&!modal&&(pending||afterClose))PostMessageW(window,kOpenPending,++closeGeneration,0);}
    void HideNow()
    {
        EndDragFeedback();dropIndicator.reset();
        if(scheduler)scheduler->Cancel(animationToken);animationToken=0;showing=closing=false;input.Cancel();scrollbarDragging=false;hovered.clear();Tip(nullptr);if(GetCapture()==window)ReleaseCapture();
        if(model)model->Close();CancelPrompt();
        if(window){KillTimer(window,1);KillTimer(window,kTrayMenuTimer);backdrop.HidePopupWindowPair(window);backdrop.SetPopupTopmost(false);ShowWindow(window,SW_HIDE);}
        if(current&&current->tray&&model){current->tray->CancelFocusReturn(current->owner);for(const auto& n:model->View().nodes)if(n.id.starts_with("tray:"))current->tray->SetGeometry(n.id.substr(5),{});}
        model.reset();current.reset();context.Reset();retainedAbove=nullptr;slide.ResetHidden();if(accessibility)accessibility->RefreshEvents();
    }
    void Queue(Request request)
    {
        afterClose={};if(showing&&!closing&&current&&current->action==request.action&&current->owner==request.owner){pending.reset();Animate(false);return;}
        if(showing||closing||modal){pending=std::move(request);if(!closing)Animate(false);}else{pending.reset();++closeGeneration;Open(std::move(request));}
    }
    void ArmContext(const std::string& key)
    {context.Reset();if(current&&current->tray)for(const auto& i:current->tray->Current().icons)if(i.key==key){context.Arm(reinterpret_cast<HWND>(i.identity.window),i.identity.process,window,current->owner);if(context.process)SetTimer(window,kTrayMenuTimer,50,nullptr);break;}}
    void SyncTrayMenuLayer(bool force=false)
    {
        HWND above=nullptr;
        for(const auto& binding:context.popups)if(context.LivePopup(binding)){above=reinterpret_cast<HWND>(binding.window);break;}
        if(!force&&above==retainedAbove)return;
        // Move only our own pair. A non-topmost custom menu otherwise remains
        // covered by the overflow panel even after its focus is retained.
        const bool topmost=!above||(GetWindowLongPtrW(above,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
        backdrop.SetPopupWindowPairZOrder(window,above?above:HWND_TOPMOST,topmost,above);
        retainedAbove=above;
    }
    bool RetainTrayMenu(HWND foreground)
    {
        if(!current||!current->tray)return false;
        const bool wasObserved=context.tracker.ObservedMenu();
        const bool retained=context.Active(foreground,current->tray->MenuPopups(context.target));
        if(retained)SyncTrayMenuLayer();
        if(!wasObserved&&context.tracker.ObservedMenu())WriteDiagnosticLogEntry(L"Tray overflow retained for the gesture's observed menu",DiagnosticLogLevel::Debug);
        if(!context.process||context.tracker.ObservedMenu())KillTimer(window,kTrayMenuTimer);
        return retained;
    }
    void Result(ui::InputResult result,POINT screen,bool keyboard=false)
    {
        if(result.kind==ui::InputResult::Kind::None||!model||!current||modal||closing)return;
        if(result.id.starts_with("tray:")&&current->tray)
        {
            const auto key=result.id.substr(5);if(result.kind==ui::InputResult::Kind::Drag){POINT local=screen;ScreenToClient(window,&local);const auto life=lifetime;auto activeModel=model;const bool dropped=activeModel->Drop(key,{local.x/scale,local.y/scale});if(!life->alive)return;if(!dropped&&model==activeModel&&dropOutside){const auto drop=dropOutside;drop(key,screen);}if(life->alive){Arrange();Paint();}return;}
            const auto life=lifetime;auto service=current->tray;const auto source=window,bar=current->owner;
            ArmContext(key);
            bool accepted=false;
            if(keyboard)accepted=service->Activate(key,result.kind==ui::InputResult::Kind::Context?tray::Activation::ContextKeyboard:tray::Activation::Keyboard,screen,{bar,source});
            else{const bool right=result.kind==ui::InputResult::Kind::Context;const bool down=service->Activate(key,right?tray::Activation::RightDown:tray::Activation::LeftDown,screen,{bar,source});accepted=service->Activate(key,right?tray::Activation::RightUp:tray::Activation::LeftUp,screen,{bar,source})||down;}
            if(life->alive&&!accepted)context.Reset();return;
        }
        const auto life=lifetime;auto activeModel=model;activeModel->Invoke(result.id,result.kind==ui::InputResult::Kind::Value?std::optional(result.value):std::nullopt);if(life->alive&&model){Arrange();Paint();}
    }
    static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        const auto life=self->lifetime;
        try
        {
            if(m==kOpenPending){if(wp!=self->closeGeneration||self->showing||self->modal)return 0;if(self->pending){auto next=std::move(*self->pending);self->pending.reset();self->Open(std::move(next));}else if(auto fn=std::move(self->afterClose)){self->afterClose={};fn();}return 0;}
            if(m==WM_ERASEBKGND)return 1;
            if(m==WM_GETOBJECT&&self->accessibility){LRESULT result=0;if(self->accessibility->TryHandleGetObject(w,wp,lp,result))return result;}
            if(m==WM_PAINT){PAINTSTRUCT p{};BeginPaint(w,&p);self->Paint();EndPaint(w,&p);return 0;}
            if(m==WM_CLOSE){self->pending.reset();self->afterClose={};self->Animate(false);return 0;}
            if(m==WM_DPICHANGED||m==WM_DISPLAYCHANGE){self->pending.reset();self->afterClose={};++self->closeGeneration;self->HideNow();return 0;}
            if(m==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE&&!self->modal&&self->closing&&self->current&&reinterpret_cast<HWND>(lp)!=self->current->owner)
            {self->pending.reset();self->afterClose={};++self->closeGeneration;}
            if(m==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE&&!self->modal&&self->showing&&!self->closing)
            {if(!self->RetainTrayMenu(reinterpret_cast<HWND>(lp)))self->Animate(false);}
            if(!self->model||self->closing||self->modal)return DefWindowProcW(w,m,wp,lp);
            if(m==WM_THEMECHANGED||m==WM_SETTINGCHANGE){self->paintDirty=true;self->Arrange();self->Paint();return 0;}
            if(m==WM_TIMER&&(wp==1||wp==kTrayMenuTimer))
            {
                if(self->context.process&&!self->modal)
                {
                    const auto foreground=GetForegroundWindow();
                    if(!self->RetainTrayMenu(foreground)){self->Animate(false);return 0;}
                }
                else if(wp==kTrayMenuTimer)KillTimer(w,kTrayMenuTimer);
                if(wp==kTrayMenuTimer)return 0;
                if(self->input.Pressed().empty()&&!self->scrollbarDragging&&!self->modal){self->Arrange();if(self->paintDirty)self->Paint();}
                return 0;
            }
            // Opening uses translated visuals; controls become interactive only
            // once their drawn, hit-test and accessibility coordinates coincide.
            if(self->slide.IsAnimating()&&((m>=WM_MOUSEFIRST&&m<=WM_MOUSELAST)||m==WM_KEYDOWN))return 0;
            if(m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN)
            {
                const bool focusChanged=self->input.PointerInput();
                self->context.Reset();KillTimer(w,kTrayMenuTimer);self->SyncTrayMenuLayer();self->tooltip.Hide();const D2D1_POINT_2F p{GET_X_LPARAM(lp)/self->scale,GET_Y_LPARAM(lp)/self->scale};
                const auto axis=self->model->ScrollbarGeometry();const auto viewport=self->model->ScrollViewport();
                if(m==WM_LBUTTONDOWN&&axis.CanDrag()&&p.x>=self->model->View().width-12&&p.x<self->model->View().width&&p.y>=viewport.top&&p.y<viewport.bottom)
                {
                    self->input.Cancel();SetFocus(w);
                    if(p.y>=axis.thumbStart&&p.y<axis.thumbEnd)
                    {self->scrollbarDragging=true;self->scrollbarPointerStart=static_cast<int>(std::lround(p.y));self->scrollbarOffsetStart=static_cast<int>(std::lround(self->model->ScrollOffset()));SetCapture(w);}
                    else{self->model->Scroll((p.y<axis.thumbStart?-1.f:1.f)*(viewport.bottom-viewport.top));self->Arrange();self->Paint();}
                    if(focusChanged)self->Paint();return 0;
                }
                if(self->input.Press(self->model->View(),p,m==WM_RBUTTONDOWN)){SetCapture(w);SetFocus(w);self->Paint();}
                else if(focusChanged)self->Paint();return 0;
            }
            if(m==WM_MOUSEMOVE)
            {
                const D2D1_POINT_2F p{GET_X_LPARAM(lp)/self->scale,GET_Y_LPARAM(lp)/self->scale};POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&screen);
                if(self->scrollbarDragging){self->model->DragScrollbar(self->scrollbarOffsetStart,static_cast<int>(std::lround(p.y))-self->scrollbarPointerStart);self->Arrange();self->Paint();return 0;}
                if(!self->input.Pressed().empty()){self->Result(self->input.Move(self->model->View(),p),screen);if(life->alive&&self->input.Dragging()){self->MoveDragFeedback(screen);self->Tip(nullptr);SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));self->Paint();}return 0;}
                const auto* node=self->model->View().Hit(p,false);self->Tip(node);const auto id=node?node->id:std::string{};if(id!=self->hovered){self->hovered=id;self->Paint();}TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,w,0};TrackMouseEvent(&t);return 0;
            }
            if(m==WM_MOUSELEAVE){self->Tip(nullptr);self->hovered.clear();self->Paint();return 0;}
            if(m==WM_LBUTTONUP||m==WM_RBUTTONUP)
            {
                if(self->scrollbarDragging){if(m==WM_LBUTTONUP){self->model->DragScrollbar(self->scrollbarOffsetStart,static_cast<int>(std::lround(GET_Y_LPARAM(lp)/self->scale))-self->scrollbarPointerStart);self->scrollbarDragging=false;ReleaseCapture();self->Arrange();self->Paint();}return 0;}
                const auto result=self->input.Release(self->model->View(),{GET_X_LPARAM(lp)/self->scale,GET_Y_LPARAM(lp)/self->scale},m==WM_RBUTTONUP);self->EndDragFeedback();ReleaseCapture();POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&screen);self->Result(result,screen);if(life->alive)self->Paint();return 0;
            }
            if(m==WM_LBUTTONDBLCLK)
            {
                if(self->input.PointerInput())self->Paint();
                const auto* node=self->model->View().Hit({GET_X_LPARAM(lp)/self->scale,GET_Y_LPARAM(lp)/self->scale});
                if(node&&node->id.starts_with("tray:")&&self->current->tray)
                {
                    POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&screen);
                    const auto key=node->id.substr(5);const auto service=self->current->tray;const auto owner=self->current->owner;
                    self->ArmContext(key);const bool accepted=service->Activate(key,tray::Activation::DoubleClick,screen,{owner,w});
                    if(life->alive&&!accepted)self->context.Reset();
                }
                return 0;
            }
            if(m==WM_CAPTURECHANGED||m==WM_CANCELMODE)
            {
                const bool wasPressed=!self->input.Pressed().empty();
                self->EndDragFeedback();self->input.Cancel();self->scrollbarDragging=false;
                if(m==WM_CANCELMODE&&GetCapture()==w)ReleaseCapture();
                if(life->alive&&wasPressed)self->Paint();return 0;
            }
            if(m==WM_MOUSEWHEEL){if(self->scrollbarDragging||!self->input.Pressed().empty())return 0;self->input.PointerInput();POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&point);auto activeModel=self->model;activeModel->Wheel({point.x/self->scale,point.y/self->scale},static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp))/WHEEL_DELTA);if(life->alive){self->Arrange();self->Paint();}return 0;}
            if(m==WM_SETFOCUS||m==WM_KILLFOCUS){self->Paint();if(self->accessibility)self->accessibility->RefreshEvents();}
            if(m==WM_KEYDOWN)
            {
                if(wp==VK_ESCAPE){self->pending.reset();self->Animate(false);return 0;}
                self->input.Cancel();self->scrollbarDragging=false;if(GetCapture()==w)ReleaseCapture();
                const auto result=self->model->HandleKey(self->input,static_cast<unsigned>(wp),(GetKeyState(VK_SHIFT)&0x8000)!=0);self->Arrange();POINT p{};
                if(const auto* n=self->model->View().Find(result.id)){p={static_cast<LONG>(n->bounds.left*self->scale),static_cast<LONG>(n->bounds.bottom*self->scale)};ClientToScreen(w,&p);}
                self->Result(result,p,true);if(life->alive){self->Paint();if(self->accessibility)self->accessibility->RefreshEvents();}return 0;
            }
        }
        catch(...){WriteDiagnosticLogEntry(L"Native system panel failed; dismissing its surface");if(life->alive){self->pending.reset();self->afterClose={};++self->closeGeneration;self->HideNow();}}
        return DefWindowProcW(w,m,wp,lp);
    }
};
SystemPanel::SystemPanel(SettingsChanged c,SystemCalendarActions d,std::function<bool(std::string_view,POINT)> drop,UiAnimationScheduler* timer,IDCompositionDesktopDevice* graphics,IDWriteFactory* text,Background background)
    :impl_(std::make_unique<Impl>(std::move(c),std::move(d),std::move(drop),timer,graphics,text,std::move(background))){}
SystemPanel::~SystemPanel()=default;
void SystemPanel::Show(StatusBarAction a,HWND owner,RECT anchor,const PersonalizationSettings& appearance,const StatusBarSettings& settings,std::shared_ptr<tray::Service> tray,std::shared_ptr<wr::WidgetSystemDataProvider> data)
{impl_->Queue({a,owner,anchor,appearance,settings,std::move(tray),std::move(data)});}
void SystemPanel::Hide(){impl_->pending.reset();impl_->afterClose={};if(impl_->showing)impl_->Animate(false);}
void SystemPanel::CloseThen(std::function<void()> next){impl_->pending.reset();impl_->afterClose=std::move(next);if(impl_->showing||impl_->modal)impl_->Animate(false);else if(auto fn=std::move(impl_->afterClose)){impl_->afterClose={};fn();}}
bool SystemPanel::IsOpen()const{return impl_->showing||impl_->modal;}
void SystemPanel::UpdateSettings(const StatusBarSettings& settings)
{if(impl_->current)impl_->current->settings=settings;if(impl_->model){impl_->model->UpdateSettings(settings);impl_->paintDirty=true;}}
void SystemPanel::HideForMonitor(HMONITOR m){if(impl_->monitor==m){impl_->pending.reset();impl_->afterClose={};impl_->HideNow();}}
bool SystemPanel::PreTranslateMessage(MSG*){return false;}
bool SystemPanel::DropTrayIcon(std::string_view key,POINT p){if(!impl_->showing||impl_->closing||impl_->modal||impl_->slide.IsAnimating()||!impl_->model)return false;ScreenToClient(impl_->window,&p);const bool ok=impl_->model->Drop(key,{p.x/impl_->scale,p.y/impl_->scale});if(ok){impl_->Arrange();impl_->Paint();}return ok;}
bool SystemPanel::PreviewTrayDrop(std::string_view key,POINT p)
{
    std::optional<D2D1_RECT_F> next;
    if(!key.empty()&&impl_->showing&&!impl_->closing&&!impl_->modal&&!impl_->slide.IsAnimating()&&impl_->model)
    {ScreenToClient(impl_->window,&p);next=impl_->model->TrayDropIndicator(key,{p.x/impl_->scale,p.y/impl_->scale});}
    const auto& old=impl_->dropIndicator;
    if(next.has_value()!=old.has_value()||(next&&(next->left!=old->left||next->top!=old->top||next->right!=old->right||next->bottom!=old->bottom)))
    {impl_->dropIndicator=next;impl_->paintDirty=true;if(impl_->showing&&!impl_->destroying)impl_->Paint();}
    return next.has_value();
}
void SystemPanel::SetTrayDragFeedback(TrayDragFeedback feedback){impl_->dragFeedback=std::move(feedback);}
void SystemPanel::SetNativeControlsHandler(std::function<void(HWND,RECT)> callback){impl_->nativeControls=std::move(callback);}
}
