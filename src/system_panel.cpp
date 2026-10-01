#include "system_panel.h"
#include "system_panel_model.h"
#include "system_calendar_editor.h"
#include "system_panel_placement.h"
#include "system_panel_transition.h"
#include "modern_menu.h"
#include "popup_round_geometry.h"
#include "flat_glass_rim.h"
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
#include <algorithm>
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
constexpr UINT kFormInputChanged=WM_APP+212;
constexpr UINT kPointerChanged=WM_APP+213;
constexpr UINT_PTR kTrayMenuTimer=2;
bool HighContrast(){HIGHCONTRASTW h{sizeof(h)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(h),&h,0);return(h.dwFlags&HCF_HIGHCONTRASTON)!=0;}
D2D1_COLOR_F SystemColor(int index){const auto c=GetSysColor(index);return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f);}
struct PanelLifetime { bool alive=true; };
std::string PanelUtf8(std::wstring_view value)
{
    if(value.empty())return {};
    const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<std::size_t>((std::max)(0,length)),0);
    if(length)WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length,nullptr,nullptr);
    return result;
}
}
struct SystemPanel::Impl
{
    struct Request
    {
        StatusBarAction action;HWND owner;RECT anchor;PersonalizationSettings appearance;StatusBarSettings settings;std::shared_ptr<tray::Service> tray;std::shared_ptr<wr::WidgetSystemDataProvider> data;std::string confirmPower;bool clockAtRight=false;
        bool SameTarget(const Request& other) const
        {return action==other.action&&owner==other.owner&&confirmPower==other.confirmPower;}
        HMONITOR Monitor() const {return MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST);}
    };
    SettingsChanged changed;SystemCalendarActions calendar;std::function<bool(std::string_view,POINT)> dropOutside;Background background;
    TrayDragFeedback dragFeedback;std::function<void(HWND,RECT)> nativeControls;
    std::function<void(HMONITOR,bool)> trayStateChanged;HMONITOR expandedTrayMonitor=nullptr;
    std::function<UINT(POINT,HWND,bool)> calendarMenuHandler;
    std::function<UINT(POINT,HWND,const std::vector<std::wstring>&,std::size_t)> calendarChoiceMenuHandler;
    UiAnimationScheduler* scheduler=nullptr;UiScheduleToken animationToken=0;quick_navigation_animation_rules::State slide;
    ComPtr<IDCompositionDesktopDevice> composition;ComPtr<IDWriteFactory> text;ComPtr<IDCompositionTarget> target;ComPtr<IDCompositionVisual2> visual;ComPtr<IDCompositionSurface> surface;
    DesktopBackdropCompositor backdrop;HWND window=nullptr;NativeTooltip tooltip;HMONITOR monitor=nullptr;std::optional<Request> current;
    SystemPanelTransition<Request> transition;
    std::unique_ptr<WidgetAccessibilityProviderHost> accessibility;
    std::unique_ptr<SystemCalendarInputs> calendarInputs;
    bool calendarMenu=false;
    std::shared_ptr<SystemPanelModel> model;ui::Input input;std::string hovered;std::function<void()> afterClose;HWND afterCloseOwner=nullptr;
    std::shared_ptr<PanelLifetime> lifetime=std::make_shared<PanelLifetime>();
    bool paintDirty=true;
    bool controlInputRefreshPending=false;
    bool pointerRefreshPending=false;
    NativePointerHoverState pointerHover;
    bool scrollbarDragging=false;int scrollbarPointerStart=0,scrollbarOffsetStart=0;
    bool trayPreview=false;std::optional<D2D1_RECT_F> dropIndicator;HWND retainedAbove=nullptr;
    std::vector<RECT> cards;float scale=1;int width=0,height=0;bool showing=false,closing=false,modal=false,destroying=false;tray::MenuRetentionSession context;WPARAM closeGeneration=0;
    Impl(SettingsChanged c,SystemCalendarActions dates,std::function<bool(std::string_view,POINT)> drop,UiAnimationScheduler* timing,IDCompositionDesktopDevice* graphics,IDWriteFactory* fonts,Background draw)
        :changed(std::move(c)),calendar(std::move(dates)),dropOutside(std::move(drop)),background(std::move(draw)),scheduler(timing),composition(graphics),text(fonts){}
    ~Impl(){lifetime->alive=false;destroying=true;HideNow();calendarInputs.reset();tooltip.Close();if(accessibility)accessibility->DetachWindow(window);backdrop.Reset();surface.Reset();visual.Reset();target.Reset();if(window)DestroyWindow(window);}
    bool Ensure()
    {
        if(window&&target&&visual)return true;if(window){calendarInputs.reset();tooltip.Close();if(accessibility)accessibility->DetachWindow(window);DestroyWindow(window);window=nullptr;}if(!composition||!text)return false;WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=Procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"SnowDesktop.NativeSystemPanel";cls.style=CS_DBLCLKS;RegisterClassExW(&cls);
        window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOREDIRECTIONBITMAP,cls.lpszClassName,L"",WS_POPUP|WS_CLIPCHILDREN,0,0,1,1,nullptr,nullptr,cls.hInstance,this);if(!window)return false;
        calendarInputs=std::make_unique<SystemCalendarInputs>(window,[this,life=lifetime](const auto& id,std::wstring value){
            struct Erase{std::wstring& value;bool secret;~Erase(){if(secret&&!value.empty())SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} erase{value,id.starts_with("control.password:")};
            if(life->alive&&model&&!closing&&!modal)
            {
                StopPointerHover();
                const bool controlInput=model->SetControlInput(id,value);
                const bool calendarNotes=id=="calendar.edit.notes";
                if(controlInput||calendarNotes)
                {
                    if(!controlInputRefreshPending&&PostMessageW(window,kFormInputChanged,0,0))controlInputRefreshPending=true;
                }
                if(!controlInput)model->SetCalendarInput(id,std::move(value));
                paintDirty=true;if(accessibility&&!controlInput)accessibility->RefreshEvents();
            }
        },[this,life=lifetime](const auto& id,UINT key,bool shift,bool control){if(life->alive){if(id.starts_with("control."))ControlKey(id,key,shift);else CalendarKey(id,key,shift,control);}},
        [this,life=lifetime]{if(life->alive)QueuePointerRefresh();});
        accessibility=std::make_unique<WidgetAccessibilityProviderHost>([this]{return Accessible();},[this](const auto&,const auto& id){if(!showing||closing||modal||slide.IsAnimating()||!model||!input.Focus(id))return false;StopPointerHover();if(model->Reveal(id)&&!Arrange())return false;if(!calendarInputs||!calendarInputs->Focus(id))SetFocus(window);Paint();if(accessibility)accessibility->RefreshEvents();return true;},[this](const auto& request){return AccessibleAction(request);});
        accessibility->AttachWindow(window);
        return SUCCEEDED(composition->CreateTargetForHwnd(window,FALSE,&target))&&SUCCEEDED(composition->CreateVisual(&visual))&&SUCCEEDED(target->SetRoot(visual.Get()));
    }
    static constexpr int kSurfacePadding = flat_glass_rim::kPanelOverdraw;
    D2D1_POINT_2F ModelPoint(LONG x,LONG y) const
    {return {static_cast<float>(x-kSurfacePadding)/scale,static_cast<float>(y-kSurfacePadding)/scale};}
    bool Glass()const{return current&&current->appearance.glassEnabled&&!HighContrast();}
    float SlideOffset()const
    {return current?(current->settings.position==DockPosition::Bottom?1.f:-1.f)*(1-quick_navigation_animation_rules::EaseInOutSmooth(slide.GetVisual().progress))*height:0;}
    bool ContainsClientPoint(POINT point)const
    {
        if(!current)return false;
        RECT client{};GetClientRect(window,&client);if(!PtInRect(&client,point))return false;
        const D2D1_POINT_2F local{static_cast<float>(point.x),static_cast<float>(point.y)};
        const float offset=SlideOffset();
        return std::any_of(cards.begin(),cards.end(),[&](const auto& card){
            return popup_round_geometry::Contains(popup_round_geometry::Resolve(card,current->appearance.cornerRadius*scale,offset),local);
        });
    }
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
        if(!node||!current||!model||!showing||closing||modal||slide.IsAnimating()||context.process||!pointerHover.Enabled())
        {tooltip.Hide();return;}
        const auto b=model->View().VisibleBounds(*node);
        if(b.right<=b.left||b.bottom<=b.top){tooltip.Hide();return;}
        POINT origin{};ClientToScreen(window,&origin);origin.x+=kSurfacePadding;origin.y+=kSurfacePadding;
        RECT anchor{origin.x+static_cast<LONG>(b.left*scale),origin.y+static_cast<LONG>(b.top*scale),origin.x+static_cast<LONG>(b.right*scale),origin.y+static_cast<LONG>(b.bottom*scale)};
        tooltip.SetTarget(node->id,node->tooltip,anchor,current->settings.position==DockPosition::Bottom?NativeTooltipPlacement::Above:NativeTooltipPlacement::Below);
    }
    void QueuePointerRefresh()
    {
        if(window&&!pointerRefreshPending&&PostMessageW(window,kPointerChanged,0,0))pointerRefreshPending=true;
    }
    void ClearPointerHover()
    {
        tooltip.Hide();
        if(!hovered.empty()){hovered.clear();paintDirty=true;}
    }
    void StopPointerHover()
    {
        POINT point{};if(GetCursorPos(&point))pointerHover.Observe(point);
        pointerHover.Suppress();ClearPointerHover();
    }
    void RefreshPointer(bool repaint=true)
    {
        const ui::Node* node=nullptr;POINT point{};
        const bool located=GetCursorPos(&point)!=FALSE;
        if(located)pointerHover.Observe(point);
        if(showing&&!closing&&!modal&&!slide.IsAnimating()&&!context.process&&pointerHover.Enabled()&&model&&input.Pressed().empty()&&!scrollbarDragging&&located)
        {
            const auto pointerWindow=WindowFromPoint(point);
            if(pointerWindow==window||(calendarInputs&&calendarInputs->Contains(pointerWindow)))
                if(ScreenToClient(window,&point)&&ContainsClientPoint(point))node=model->View().Hit(ModelPoint(point.x,point.y),false);
        }
        Tip(node);const auto id=node?node->id:std::string{};
        if(hovered!=id){hovered=id;paintDirty=true;}
        if(repaint&&paintDirty&&showing)Paint();
    }
    ui::Palette Palette()const
    {
        return SystemPanelPalette(current->appearance,HighContrast());
    }
    std::vector<SystemCalendarInputField> InputFields() const
    {
        if(!model)return {};
        auto fields=model->CalendarInputFields();auto controls=model->ControlInputFields();
        for(auto& field:controls)fields.push_back(std::move(field));
        const float padding=static_cast<float>(kSurfacePadding)/scale;
        for(auto& field:fields)for(auto* rect:{&field.bounds,&field.clip})
        {rect->left+=padding;rect->right+=padding;rect->top+=padding;rect->bottom+=padding;}
        return fields;
    }
    std::vector<LuaWidgetAccessibilitySnapshot> Accessible() const
    {
        if(!showing||closing||modal||slide.IsAnimating()||!model)return {};
        LuaWidgetAccessibilitySnapshot snapshot;snapshot.widgetId=L"system-panel";
        snapshot.name=_L(current->action==StatusBarAction::Calendar?"statusBar.clock":current->action==StatusBarAction::Tray?"statusBar.tray":"statusBar.controlCenter");snapshot.bounds={0,0,width,height};
        const auto regions=input.AccessibilityRegions();std::string focus;
        const auto nativeFocus=calendarInputs?calendarInputs->FieldId(GetFocus()):std::string{};
        for(const auto& r:regions)if((GetFocus()==window&&input.Identity(r.key)==input.Focused())||(!nativeFocus.empty()&&input.Identity(r.key)==nativeFocus))focus=r.key;
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
        const auto fields=InputFields();
        for(auto& node:snapshot.nodes)
        {
            node.key=input.Identity(node.key);node.bounds.x=node.bounds.x*scale+kSurfacePadding;node.bounds.y=node.bounds.y*scale+kSurfacePadding;node.bounds.width*=scale;node.bounds.height*=scale;
            if(node.clip){node.clip->x=node.clip->x*scale+kSurfacePadding;node.clip->y=node.clip->y*scale+kSurfacePadding;node.clip->width*=scale;node.clip->height*=scale;}
            if(const auto field=std::find_if(fields.begin(),fields.end(),[&](const auto& item){return item.id==node.key;});field!=fields.end())
            {
                node.role="textbox";node.controlType="Edit";node.name=PanelUtf8(field->label);node.valueText=PanelUtf8(field->text);
                node.patterns=wr::ViewAccessibilityPattern::Value;node.valueReadOnly=!field->enabled;
            }
        }
        // Real ES_PASSWORD children provide their native IsPassword contract;
        // never shadow them with a synthetic ordinary Value provider.
        std::erase_if(snapshot.nodes,[](const auto& node){return node.key.starts_with("control.password:");});
        if(model->MaximumScroll()>0)
        {
            const auto clip=model->ScrollViewport();wr::ViewAccessibilityNode scroll;scroll.semanticId=scroll.key="panel.scroll";scroll.role="group";scroll.controlType="pane";scroll.name=snapshot.name;scroll.patterns=wr::ViewAccessibilityPattern::Scroll;
            scroll.bounds={clip.left*scale+kSurfacePadding,clip.top*scale+kSurfacePadding,(clip.right-clip.left)*scale,(clip.bottom-clip.top)*scale};scroll.scrollOffset=model->ScrollOffset();scroll.scrollViewportExtent=clip.bottom-clip.top;scroll.scrollContentExtent=scroll.scrollViewportExtent+model->MaximumScroll();snapshot.nodes.push_back(std::move(scroll));
        }
        return {std::move(snapshot)};
    }
    bool AccessibleAction(const LuaWidgetAccessibilityActionRequest& request)
    {
        if(!showing||closing||modal||slide.IsAnimating()||!model||request.widgetId!=L"system-panel")return false;
        StopPointerHover();
        if(request.nodeKey=="panel.scroll"&&request.kind==LuaWidgetAccessibilityActionKind::SetScrollOffset)
        {if(!std::isfinite(request.numericValue))return false;model->Scroll(static_cast<float>(request.numericValue)-model->ScrollOffset());Arrange();Paint();if(accessibility)accessibility->RefreshEvents();return true;}
        const auto* node=model->View().Find(request.nodeKey);if(!node||!node->Interactive())return false;
        if(model->Reveal(request.nodeKey)){if(!Arrange())return false;node=model->View().Find(request.nodeKey);if(!node||!node->Interactive())return false;}
        if(request.kind==LuaWidgetAccessibilityActionKind::SetValue)
            return calendarInputs&&calendarInputs->SetValue(request.nodeKey,request.textValue);
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
        POINT point{static_cast<LONG>((node->bounds.left+node->bounds.right)*scale/2)+kSurfacePadding,static_cast<LONG>((node->bounds.top+node->bounds.bottom)*scale/2)+kSurfacePadding};ClientToScreen(window,&point);Result(std::move(result),point,true);return true;
    }
    void Open(Request request)
    {
        if(modal||transition.Releasing()){transition.Defer(std::move(request));return;}
        if(!Ensure())return;current=std::move(request);const auto& r=*current;monitor=MonitorFromRect(&r.anchor,MONITOR_DEFAULTTONEAREST);scale=GetDpiForWindow(r.owner)/96.f;
        auto source=LiveSystemPanelSource(r.data);source.calendar=calendar;source.tray=[service=r.tray]{return service?service->Current():tray::Snapshot{};};
        if(nativeControls)source.nativeControls=[this] {if(current&&nativeControls){const auto fn=nativeControls;fn(current->owner,current->anchor);}};
        source.trayChanged=[this](const auto& value){if(current)current->settings=value;if(changed)changed(value);};
        tooltip.Configure(window,composition.Get(),text.Get(),r.appearance,background);input={};pointerHover={};scrollbarDragging=false;paintDirty=true;
        model=std::make_shared<SystemPanelModel>(std::move(source),r.settings,r.action,r.clockAtRight);if(!r.confirmPower.empty())model->BeginPowerConfirmation(r.confirmPower,true);showing=true;closing=false;if(!Arrange()){if(showing&&!closing)HideNow();return;}
        // HideNow clears these rectangles. Reopening the same layout need not
        // move or reshape the HWND, so Arrange alone may not publish them.
        PublishGeometry();
        Paint();Animate(true);backdrop.SetPopupWindowPairZOrder(window,HWND_TOPMOST,true);if(Glass())backdrop.ShowPopupWindowPair(window);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);SetFocus(window);if(!r.confirmPower.empty())FocusControlPage(false);SetTimer(window,1,500,nullptr);
        ReportTrayState(showing&&!closing&&current&&current->action==StatusBarAction::Tray&&IsWindowVisible(window)?monitor:nullptr);
    }
    void ReportTrayState(HMONITOR expanded)
    {
        if(expandedTrayMonitor==expanded)return;
        const auto previous=std::exchange(expandedTrayMonitor,expanded);const auto notify=trayStateChanged;
        if(notify){if(previous)notify(previous,false);if(expanded)notify(expanded,true);}
    }
    bool ApplyDismissRequest()
    {
        if(!model||!model->TakeDismissRequest())return false;
        transition.Cancel();afterClose={};++closeGeneration;
        if(showing&&!closing)Animate(false);
        return true;
    }
    bool Arrange()
    {
        if(!model||!current||closing)return false;if(ApplyDismissRequest())return false;
        MONITORINFO info{sizeof(info)};if(!GetMonitorInfoW(monitor,&info))return false;
        const auto previousScene=model->View();model->Refresh((info.rcWork.bottom-info.rcWork.top)/scale-12,(info.rcWork.right-info.rcWork.left)/scale-12);
        // A completed standalone confirmation closes over its existing surface.
        // Do not publish the model's return page before the close animation.
        if(ApplyDismissRequest())return false;const auto& scene=model->View();
        const bool contentChanged=!previousScene.SameContent(scene);paintDirty|=contentChanged;input.Sync(scene);if(scrollbarDragging&&!model->ScrollbarGeometry().CanDrag())scrollbarDragging=false;if(input.Pressed().empty()&&!scrollbarDragging&&GetCapture()==window)ReleaseCapture();
        // The surface and HWND must both include the rim's exterior pixels.
        // Merely enlarging the GDI region still clips straight edges at the
        // render target while retaining the outside half around the corners.
        const int w=static_cast<int>(std::ceil(scene.width*scale))+kSurfacePadding*2,h=static_cast<int>(std::ceil(scene.height*scale))+kSurfacePadding*2;
        const bool calendarPanel=current->action==StatusBarAction::Calendar;
        const bool controls=current->action!=StatusBarAction::Tray&&!calendarPanel&&!IsSystemResourceAction(current->action)&&current->confirmPower.empty();
        const auto alignment=controls||(calendarPanel&&current->clockAtRight)?SystemPanelAlignment::ScreenRight:
            calendarPanel?SystemPanelAlignment::IconCenter:SystemPanelAlignment::IconRight;
        const auto placement=PlaceSystemPanel(current->anchor,{w,h},info.rcWork,current->settings.position,scale,alignment);
        const int left=placement.left,top=placement.top;
        std::vector<RECT> next;for(const auto& c:scene.cards)next.push_back({static_cast<LONG>(std::lround(c.left*scale)),static_cast<LONG>(std::lround(c.top*scale)),static_cast<LONG>(std::lround(c.right*scale)),static_cast<LONG>(std::lround(c.bottom*scale))});
        for(auto& card:next)OffsetRect(&card,kSurfacePadding,kSurfacePadding);
        const bool shape=cards.size()!=next.size()||!std::equal(cards.begin(),cards.end(),next.begin(),[](const auto& x,const auto& y){return EqualRect(&x,&y);});cards=std::move(next);
        if(w!=width||h!=height){width=w;height=h;surface.Reset();paintDirty=true;}RECT previous{};GetWindowRect(window,&previous);const bool moved=previous.left!=left||previous.top!=top||previous.right!=left+w||previous.bottom!=top+h;
        if(moved){auto moving=transition.BeginPlacement();SetWindowPos(window,nullptr,left,top,w,h,SWP_NOACTIVATE|SWP_NOZORDER);}
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
        if(calendarInputs){const bool hadInputFocus=calendarInputs->Contains(GetFocus());calendarInputs->Sync(InputFields(),current->appearance,static_cast<UINT>(std::lround(scale*96)));calendarInputs->Pose(SlideOffset(),showing&&!closing&&!modal&&!slide.IsAnimating());if(hadInputFocus&&!GetFocus()&&showing&&!closing&&!modal)SetFocus(window);}
        // Layout/scrolling can put a different node under a stationary mouse.
        // Resolve it now, without painting recursively from inside Arrange.
        RefreshPointer(false);
        if(accessibility&&(contentChanged||moved||shape))accessibility->RefreshEvents();
        return true;
    }
    void PublishGeometry()
    {
        if(!current||!current->tray||!model)return;POINT origin{};ClientToScreen(window,&origin);origin.x+=kSurfacePadding;origin.y+=kSurfacePadding;
        for(const auto& n:model->View().nodes)if(n.id.starts_with("tray:")){const auto& r=n.bounds;RECT box{origin.x+static_cast<LONG>(r.left*scale),origin.y+static_cast<LONG>(r.top*scale),origin.x+static_cast<LONG>(r.right*scale),origin.y+static_cast<LONG>(r.bottom*scale)};current->tray->SetGeometry(n.id.substr(5),box);}
    }
    void Paint()
    {
        if(!model||!current||!visual||closing||width<=0||height<=0)return;
        if(!surface&&FAILED(composition->CreateSurface(width,height,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_ALPHA_MODE_PREMULTIPLIED,&surface)))return;
        ComPtr<ID2D1DeviceContext> dc;POINT offset{};if(FAILED(surface->BeginDraw(nullptr,IID_PPV_ARGS(&dc),&offset)))return;
        dc->SetDpi(96,96);dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),static_cast<float>(offset.y)));dc->Clear(D2D1::ColorF(0,0.f));
        for(const auto& card:cards)
        {
            if(HighContrast()){ComPtr<ID2D1SolidColorBrush> b;dc->CreateSolidColorBrush(SystemColor(COLOR_WINDOW),&b);dc->FillRoundedRectangle(popup_round_geometry::Resolve(card,current->appearance.cornerRadius*scale),b.Get());}
            else if(background)background(dc.Get(),card,current->appearance,scale);
        }
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x+kSurfacePadding),static_cast<float>(offset.y+kSurfacePadding)));dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
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
        if(!visual||!current||!window)return;const float y=SlideOffset();
        visual->SetOffsetY(y);HRGN region=CreateRectRgn(0,0,0,0);const int dyTop=static_cast<int>(std::floor(y)),dyBottom=static_cast<int>(std::ceil(y));
        for(std::size_t i=0;i<cards.size();++i){const auto& c=cards[i];HRGN part=popup_round_geometry::CreateWindowFence(c,current->appearance.cornerRadius*scale,y,static_cast<float>(flat_glass_rim::kPanelOverdraw));CombineRgn(region,region,part,RGN_OR);DeleteObject(part);if(Glass()){RECT projected=c;projected.top+=dyTop;projected.bottom+=dyBottom;backdrop.SetPanelTransform(reinterpret_cast<std::uintptr_t>(this)+i,D2D1::Matrix4x4F::Translation(0,y,0),projected);}}
        const auto clip=CreateRectRgn(0,0,width,height);CombineRgn(region,region,clip,RGN_AND);DeleteObject(clip);if(!SetWindowRgn(window,region,FALSE))DeleteObject(region);
        if(calendarInputs)calendarInputs->Pose(y,showing&&!closing&&!modal&&!slide.IsAnimating());
        composition->Commit();if(Glass())backdrop.CommitVisualChanges();
    }
    void Animate(bool opening)
    {
        if(!opening&&(closing||transition.Releasing()))return;
        // Mark the close before notifications, capture release or menu
        // dismissal can dispatch another status-bar click.
        if(!opening)closing=true;
        if(!opening)ReportTrayState(nullptr);
        if(!opening&&model){model->CancelControlInput();if(calendarInputs)calendarInputs->Sync(InputFields(),current->appearance,static_cast<UINT>(std::lround(scale*96)));}
        if(!opening){KillTimer(window,kTrayMenuTimer);EndDragFeedback();dropIndicator.reset();if(GetCapture()==window)ReleaseCapture();}
        if(!opening&&modal){if(model)model->Close();CancelPrompt();}
        if(scheduler)scheduler->Cancel(animationToken);animationToken=0;
        if(!scheduler||animation::RuntimePopupEffect()==animation::NoEffect){if(opening){slide.ShowImmediately();Pose();}else FinishClose();return;}
        slide.Configure(quick_navigation_animation_rules::Effect::Fade,animation::RuntimeDurationScale());const auto now=static_cast<std::uint64_t>(UiAnimationScheduler::MonotonicMilliseconds());
        if(opening){slide.ResetHidden();slide.Open(now);}else{closing=true;input.Cancel();Tip(nullptr);slide.Close(now);}Pose();
        animationToken=scheduler->StartAnimation(UiAnimationSurface::Popup,[this](double time){slide.Advance(static_cast<std::uint64_t>(time));Pose();if(slide.IsAnimating())return true;animationToken=0;if(slide.IsHidden())FinishClose();else if(accessibility)accessibility->RefreshEvents();return false;});
    }
    void CancelPrompt(){if(calendarMenu)modern_menu::DismissActive();}
    void PostPendingOpen()
    {
        if(!destroying&&!showing&&!modal&&!transition.Releasing()&&(transition.Pending()||afterClose))
            PostMessageW(window,kOpenPending,++closeGeneration,0);
    }
    void FinishClose(){HideNow();PostPendingOpen();}
    void HideNow()
    {
        auto release=transition.BeginRelease();if(!release)return;
        showing=false;closing=true;
        // Detach the old model/request before callbacks. Reentrant requests
        // stay queued until this guard ends and cannot be cleared below.
        auto oldModel=std::move(model);auto oldRequest=std::move(current);current.reset();
        ReportTrayState(nullptr);
        EndDragFeedback();dropIndicator.reset();
        if(scheduler)scheduler->Cancel(animationToken);animationToken=0;input.Cancel();scrollbarDragging=false;hovered.clear();Tip(nullptr);if(GetCapture()==window)ReleaseCapture();
        if(calendarInputs)calendarInputs->Clear();if(oldModel)oldModel->Close();CancelPrompt();
        if(window){KillTimer(window,1);KillTimer(window,kTrayMenuTimer);backdrop.HidePopupWindowPair(window);backdrop.SetPopupTopmost(false);ShowWindow(window,SW_HIDE);}
        if(oldRequest&&oldRequest->tray&&oldModel){oldRequest->tray->CancelFocusReturn(oldRequest->owner);for(const auto& n:oldModel->View().nodes)if(n.id.starts_with("tray:"))oldRequest->tray->SetGeometry(n.id.substr(5),{});}
        oldModel.reset();oldRequest.reset();context.Reset();retainedAbove=nullptr;slide.ResetHidden();closing=false;if(accessibility)accessibility->RefreshEvents();
    }
    void Queue(Request request)
    {
        if(destroying)return;
        afterClose={};
        const auto next=transition.Queue(std::move(request),current,showing,closing||modal);
        if(next==SystemPanelTransition<Request>::Action::Open)
        {++closeGeneration;if(auto ready=transition.Take())Open(std::move(*ready));}
        else if(next==SystemPanelTransition<Request>::Action::Close&&(showing||modal))Animate(false);
    }
    void ArmContext(const std::string& key)
    {ClearPointerHover();context.Reset();if(current&&current->tray)for(const auto& i:current->tray->Current().icons)if(i.key==key){context.Arm(reinterpret_cast<HWND>(i.identity.window),i.identity.process,window,current->owner);if(context.process)SetTimer(window,kTrayMenuTimer,50,nullptr);break;}}
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
    void ControlKey(const std::string& id,UINT key,bool shift)
    {
        if(!showing||closing||modal||slide.IsAnimating()||!model)return;
        StopPointerHover();input.Focus(id,true);
        if(key==VK_ESCAPE)model->ControlBack();
        else if(key==VK_TAB)model->HandleKey(input,key,shift);
        // Enter in an input moves to the safe cancel action. Authorizing a
        // connection or destructive command requires its explicit button.
        else if(key==VK_RETURN){input.Focus("control.cancel",true);model->Reveal("control.cancel");}
        if(ApplyDismissRequest()||!Arrange())return;if(!calendarInputs||!calendarInputs->Focus(input.Focused(),true))SetFocus(window);
        Paint();if(accessibility)accessibility->RefreshEvents();
    }
    void FocusControlPage(bool keyboard)
    {
        if(!model)return;const auto id=model->ControlFocusTarget();if(id.empty())return;
        if(model->Reveal(id)&&!Arrange())return;input.Focus(id,keyboard);
        if(!calendarInputs||!calendarInputs->Focus(id,keyboard))SetFocus(window);
    }
    void CalendarKey(const std::string& id,UINT key,bool shift,bool control)
    {
        (void)control;
        if(!showing||closing||modal||slide.IsAnimating()||!model||!model->CalendarEditing())return;
        StopPointerHover();const auto life=lifetime;auto active=model;input.Focus(id,true);
        if(key==VK_ESCAPE)active->CalendarBack();
        else if(key==VK_RETURN)active->Invoke(active->View().Find("calendar.edit.confirmDelete")?"calendar.edit.cancelDelete":"calendar.edit.save");
        else if(key==VK_TAB)active->HandleKey(input,key,shift);
        if(!life->alive||!model)return;
        if(!Arrange())return;
        const bool failed=key==VK_RETURN&&model->View().Find("calendar.edit.error")!=nullptr;
        if(failed&&model->Reveal("calendar.edit.error")&&!Arrange())return;
        if(failed||!calendarInputs||!calendarInputs->Focus(input.Focused(),true))SetFocus(window);
        Paint();if(accessibility)accessibility->RefreshEvents();
    }
    void CalendarContext(const std::string& id,POINT anchor)
    {
        if(!model||!current||!calendarMenuHandler||!id.starts_with("event:")||model->CalendarEditing()||!model->View().Find(id))return;
        const auto life=lifetime;auto active=model;
        const bool series=model->CalendarEventIsSeries(id);
        StopPointerHover();input.Cancel();modal=calendarMenu=true;
        UINT command=0;
        try{command=calendarMenuHandler(anchor,window,series);}
        catch(...){if(life->alive)modal=calendarMenu=false;throw;}
        if(!life->alive)return;
        modal=calendarMenu=false;
        PostPendingOpen();
        if(!current||!model)return;
        if(!command&&GetForegroundWindow()!=window&&GetForegroundWindow()!=current->owner)
        {transition.Cancel();afterClose={};if(showing&&!closing)Animate(false);return;}
        if(showing&&!closing&&model==active&&command>=1&&command<=4)
        {active->CalendarEventCommand(id,command==2||command==4,command>=3);Arrange();FocusCalendarPage(false);Paint();}
    }
    void CalendarChoice(const std::string& id,bool keyboard)
    {
        if(!model||!current||!calendarChoiceMenuHandler||!model->CalendarEditing())return;
        const auto* node=model->View().Find(id);
        const auto choices=model->CalendarChoices(id);
        if(!node||!node->enabled||choices.empty())return;
        const auto bounds=model->View().VisibleBounds(*node);
        POINT anchor{static_cast<LONG>(bounds.left*scale)+kSurfacePadding,static_cast<LONG>(bounds.bottom*scale)+kSurfacePadding};
        ClientToScreen(window,&anchor);
        std::vector<std::wstring> labels;labels.reserve(choices.size());
        std::size_t selected=0;
        for(std::size_t index=0;index<choices.size();++index)
        {
            labels.push_back(choices[index].label);
            if(choices[index].selected)selected=index;
        }
        const auto life=lifetime;auto active=model;
        StopPointerHover();input.Cancel();modal=calendarMenu=true;
        UINT command=0;
        try{command=calendarChoiceMenuHandler(anchor,window,labels,selected);}
        catch(...){if(life->alive)modal=calendarMenu=false;throw;}
        if(!life->alive)return;
        modal=calendarMenu=false;
        PostPendingOpen();
        if(!current||!model)return;
        if(!command&&GetForegroundWindow()!=window&&GetForegroundWindow()!=current->owner)
        {transition.Cancel();afterClose={};if(showing&&!closing)Animate(false);return;}
        if(showing&&!closing&&model==active&&command>=1&&command<=choices.size())
        {
            active->SelectCalendarChoice(id,command-1);
            if(Arrange())
            {
                input.Focus(id,keyboard);
                if(!calendarInputs||!calendarInputs->Focus(id,keyboard))SetFocus(window);
                Paint();
                if(accessibility)accessibility->RefreshEvents();
            }
        }
    }
    void FocusCalendarPage(bool keyboard)
    {
        if(!model)return;
        auto id=model->CalendarFocusTarget();
        if(id.empty())return;
        if(!model->View().Find(id)){if(!model->CalendarEditing())return;id="picker.confirm";}
        if(model->Reveal(id)&&!Arrange())return;input.Focus(id,keyboard);
        if(!calendarInputs||!calendarInputs->Focus(id,keyboard))SetFocus(window);
    }
    void Result(ui::InputResult result,POINT screen,bool keyboard=false)
    {
        if(result.kind==ui::InputResult::Kind::None||!model||!current||modal||closing)return;
        if(result.id.starts_with("tray:")&&current->tray)
        {
            const auto key=result.id.substr(5);if(result.kind==ui::InputResult::Kind::Drag){POINT local=screen;ScreenToClient(window,&local);const auto life=lifetime;auto activeModel=model;const bool dropped=activeModel->Drop(key,ModelPoint(local.x,local.y));if(!life->alive)return;if(!dropped&&model==activeModel&&dropOutside){const auto drop=dropOutside;drop(key,screen);}if(life->alive){Arrange();Paint();}return;}
            const auto life=lifetime;auto service=current->tray;const auto source=window,bar=current->owner;
            PublishGeometry();
            ArmContext(key);
            bool accepted=false;
            if(keyboard)accepted=service->Activate(key,result.kind==ui::InputResult::Kind::Context?tray::Activation::ContextKeyboard:tray::Activation::Keyboard,screen,{bar,source});
            else{const bool right=result.kind==ui::InputResult::Kind::Context;const bool down=service->Activate(key,right?tray::Activation::RightDown:tray::Activation::LeftDown,screen,{bar,source});accepted=service->Activate(key,right?tray::Activation::RightUp:tray::Activation::LeftUp,screen,{bar,source})||down;}
            if(life->alive&&!accepted)context.Reset();return;
        }
        if(result.kind==ui::InputResult::Kind::Context){CalendarContext(result.id,screen);return;}
        if(result.kind==ui::InputResult::Kind::Invoke&&current->action==StatusBarAction::Calendar&&
            !model->CalendarChoices(result.id).empty())
        {CalendarChoice(result.id,keyboard);return;}
        if(calendarInputs&&calendarInputs->Focus(result.id,keyboard)){input.Focus(result.id,keyboard);Paint();return;}
        const auto life=lifetime;auto activeModel=model;const auto calendarFocus=activeModel->CalendarFocusTarget();const auto controlFocus=activeModel->ControlFocusTarget();
        activeModel->Invoke(result.id,result.kind==ui::InputResult::Kind::Value?std::optional(result.value):std::nullopt);
        if(!life->alive||!model||ApplyDismissRequest()||!Arrange())return;
        if(calendarFocus!=model->CalendarFocusTarget()||result.id.starts_with("picker.hour:")||result.id.starts_with("picker.minute:"))FocusCalendarPage(keyboard);
        if(!model||closing)return;
        if(controlFocus!=model->ControlFocusTarget()){if(model->ControlFocusTarget().empty())SetFocus(window);else FocusControlPage(keyboard);}
        if(!model||closing)return;
        if(model->View().Find("calendar.edit.error")&&model->Reveal("calendar.edit.error")&&!Arrange())return;
        if(model->View().Find("control.error")&&model->Reveal("control.error")&&!Arrange())return;
        Paint();
    }
    static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(m==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(w,m,wp,lp);
        const auto life=self->lifetime;
        try
        {
            if(m==kPointerChanged){self->pointerRefreshPending=false;self->RefreshPointer();return 0;}
            if(m==kFormInputChanged)
            {
                self->controlInputRefreshPending=false;
                if(self->showing&&!self->closing&&!self->modal&&self->model){self->Arrange();self->Paint();if(self->accessibility)self->accessibility->RefreshEvents();}
                return 0;
            }
            if(m==kOpenPending)
            {
                if(wp!=self->closeGeneration||self->showing||self->modal||self->transition.Releasing())return 0;
                if(auto next=self->transition.Take())self->Open(std::move(*next));
                else if(auto fn=std::move(self->afterClose)){self->afterClose={};fn();}
                return 0;
            }
            if(m==WM_ERASEBKGND)return 1;
            if((m==WM_CTLCOLOREDIT||m==WM_CTLCOLORSTATIC)&&self->calendarInputs)
                if(const auto brush=self->calendarInputs->ControlColor(reinterpret_cast<HWND>(lp),reinterpret_cast<HDC>(wp)))return reinterpret_cast<LRESULT>(brush);
            if(m==WM_COMMAND&&self->calendarInputs&&self->calendarInputs->HandleCommand(wp,lp))return 0;
            if(m==WM_NCHITTEST&&self->current)
            {
                POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&point);
                return self->ContainsClientPoint(point)?HTCLIENT:HTTRANSPARENT;
            }
            if(m==WM_GETOBJECT&&self->accessibility){LRESULT result=0;if(self->accessibility->TryHandleGetObject(w,wp,lp,result))return result;}
            if(m==WM_PAINT){PAINTSTRUCT p{};BeginPaint(w,&p);self->Paint();EndPaint(w,&p);return 0;}
            if(m==WM_CLOSE){self->transition.Cancel();self->afterClose={};self->Animate(false);return 0;}
            if(m==WM_DPICHANGED&&!self->transition.ShouldDismissForDpiChange())return 0;
            if(m==WM_DPICHANGED||m==WM_DISPLAYCHANGE){self->transition.Cancel();self->afterClose={};++self->closeGeneration;self->HideNow();return 0;}
            if(m==WM_ACTIVATE&&LOWORD(wp)==WA_INACTIVE&&!self->modal&&self->closing&&self->current&&
                self->transition.ShouldCancelOnDeactivation(self->current->owner,reinterpret_cast<HWND>(lp))&&
                (!self->afterClose||self->afterCloseOwner!=reinterpret_cast<HWND>(lp)))
            {self->transition.Cancel();self->afterClose={};++self->closeGeneration;}
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
                self->pointerHover.Resume();
                const bool focusChanged=self->input.PointerInput();
                self->context.Reset();KillTimer(w,kTrayMenuTimer);self->SyncTrayMenuLayer();self->ClearPointerHover();const auto p=self->ModelPoint(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));
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
                const auto p=self->ModelPoint(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&screen);
                if(self->scrollbarDragging){self->model->DragScrollbar(self->scrollbarOffsetStart,static_cast<int>(std::lround(p.y))-self->scrollbarPointerStart);self->Arrange();self->Paint();return 0;}
                if(!self->input.Pressed().empty()){self->Result(self->input.Move(self->model->View(),p),screen);if(life->alive&&self->input.Dragging()){self->MoveDragFeedback(screen);self->Tip(nullptr);SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));self->Paint();}return 0;}
                self->RefreshPointer();TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,w,0};TrackMouseEvent(&t);return 0;
            }
            if(m==WM_MOUSELEAVE){self->QueuePointerRefresh();return 0;}
            if(m==WM_LBUTTONUP||m==WM_RBUTTONUP)
            {
                if(self->scrollbarDragging){if(m==WM_LBUTTONUP){self->model->DragScrollbar(self->scrollbarOffsetStart,static_cast<int>(std::lround(self->ModelPoint(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)).y))-self->scrollbarPointerStart);self->scrollbarDragging=false;ReleaseCapture();self->Arrange();self->Paint();}return 0;}
                const auto result=self->input.Release(self->model->View(),self->ModelPoint(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)),m==WM_RBUTTONUP);self->EndDragFeedback();ReleaseCapture();POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&screen);self->Result(result,screen);if(life->alive)self->Paint();return 0;
            }
            if(m==WM_LBUTTONDBLCLK)
            {
                if(self->input.PointerInput())self->Paint();
                const auto* node=self->model->View().Hit(self->ModelPoint(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)));
                if(node&&node->id.starts_with("tray:")&&self->current->tray)
                {
                    POINT screen{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ClientToScreen(w,&screen);
                    const auto key=node->id.substr(5);const auto service=self->current->tray;const auto owner=self->current->owner;
                    self->PublishGeometry();self->ArmContext(key);const bool accepted=service->Activate(key,tray::Activation::DoubleClick,screen,{owner,w});
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
            if(m==WM_MOUSEWHEEL){if(self->scrollbarDragging||!self->input.Pressed().empty())return 0;self->pointerHover.Resume();self->input.PointerInput();POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&point);auto activeModel=self->model;activeModel->Wheel(self->ModelPoint(point.x,point.y),static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp))/WHEEL_DELTA);if(life->alive){self->Arrange();self->Paint();}return 0;}
            if(m==WM_SETFOCUS||m==WM_KILLFOCUS){self->Paint();if(self->accessibility)self->accessibility->RefreshEvents();}
            if(m==WM_KEYDOWN)
            {
                self->StopPointerHover();
                if(wp==VK_ESCAPE){if(self->model->ControlBack()){if(self->ApplyDismissRequest()||!self->Arrange())return 0;SetFocus(w);self->Paint();}else if(self->model->CalendarBack()){if(!self->Arrange())return 0;self->FocusCalendarPage(true);self->Paint();}else{self->transition.Cancel();self->Animate(false);}return 0;}
                self->input.Cancel();self->scrollbarDragging=false;if(GetCapture()==w)ReleaseCapture();
                const auto result=self->model->HandleKey(self->input,static_cast<unsigned>(wp),(GetKeyState(VK_SHIFT)&0x8000)!=0);if(!self->Arrange())return 0;POINT p{};
                if(const auto* n=self->model->View().Find(result.id)){p={static_cast<LONG>(n->bounds.left*self->scale)+kSurfacePadding,static_cast<LONG>(n->bounds.bottom*self->scale)+kSurfacePadding};ClientToScreen(w,&p);}
                self->Result(result,p,true);if(life->alive){if(wp==VK_TAB&&self->calendarInputs)self->calendarInputs->Focus(self->input.Focused(),true);self->Paint();if(self->accessibility)self->accessibility->RefreshEvents();}return 0;
            }
        }
        catch(...){WriteDiagnosticLogEntry(L"Native system panel failed; dismissing its surface");if(life->alive){self->transition.Cancel();self->afterClose={};++self->closeGeneration;self->HideNow();}}
        return DefWindowProcW(w,m,wp,lp);
    }
};
SystemPanel::SystemPanel(SettingsChanged c,SystemCalendarActions d,std::function<bool(std::string_view,POINT)> drop,UiAnimationScheduler* timer,IDCompositionDesktopDevice* graphics,IDWriteFactory* text,Background background)
    :impl_(std::make_unique<Impl>(std::move(c),std::move(d),std::move(drop),timer,graphics,text,std::move(background))){}
SystemPanel::~SystemPanel()=default;
void SystemPanel::Show(StatusBarAction a,HWND owner,RECT anchor,const PersonalizationSettings& appearance,const StatusBarSettings& settings,std::shared_ptr<tray::Service> tray,std::shared_ptr<wr::WidgetSystemDataProvider> data,bool clockAtRight)
{impl_->Queue({a,owner,anchor,appearance,settings,std::move(tray),std::move(data),{},clockAtRight});}
void SystemPanel::ShowPowerConfirmation(std::string task,HWND owner,RECT anchor,const PersonalizationSettings& appearance,const StatusBarSettings& settings,std::shared_ptr<wr::WidgetSystemDataProvider> data)
{
    if(task!="system.power.sleep"&&task!="system.power.restart"&&task!="system.power.shutdown")return;
    impl_->Queue({StatusBarAction::ControlCenter,owner,anchor,appearance,settings,{},std::move(data),std::move(task)});
}
void SystemPanel::Hide(){impl_->transition.Cancel();impl_->afterClose={};++impl_->closeGeneration;if(impl_->showing)impl_->Animate(false);}
void SystemPanel::CloseThen(std::function<void()> next, HWND destinationOwner)
{
    impl_->transition.Cancel();impl_->afterClose=std::move(next);
    impl_->afterCloseOwner=destinationOwner?destinationOwner:impl_->current?impl_->current->owner:nullptr;
    if(impl_->closing||impl_->transition.Releasing())return;
    if(impl_->showing||impl_->modal)impl_->Animate(false);
    else if(auto fn=std::move(impl_->afterClose)){impl_->afterClose={};fn();}
}
bool SystemPanel::IsOpen()const{return impl_->showing||impl_->modal||impl_->transition.Releasing()||impl_->transition.Pending()||impl_->afterClose;}
bool SystemPanel::IsOpenForMonitor(HMONITOR monitor)const
{
    if(!monitor)return false;
    if((impl_->showing||impl_->modal||impl_->transition.Releasing())&&impl_->monitor==monitor)return true;
    if(impl_->afterClose&&MonitorFromWindow(impl_->afterCloseOwner,MONITOR_DEFAULTTONULL)==monitor)return true;
    // A queued replacement owns the same interaction hold between the close
    // endpoint and kOpenPending; that message gap must not hide the merged bar.
    const auto& pending=impl_->transition.Pending();
    return pending&&MonitorFromRect(&pending->anchor,MONITOR_DEFAULTTONEAREST)==monitor;
}
bool SystemPanel::ContainsPoint(POINT screen)const
{
    if(!IsOpen())return false;
    const HWND menu=modern_menu::ActiveRootWindow();
    if(impl_->calendarMenu&&menu&&GetWindow(menu,GW_OWNER)==impl_->window)
    {
        HWND target=GetAncestor(WindowFromPoint(screen),GA_ROOT);
        for(unsigned depth=0;target&&depth<16;++depth,target=GetWindow(target,GW_OWNER))
            if(target==menu)return true;
    }
    if(impl_->context.ContainsPoint(screen))return true;
    if(!impl_->window||!IsWindowVisible(impl_->window))return false;
    POINT client=screen;if(!ScreenToClient(impl_->window,&client))return false;
    return impl_->ContainsClientPoint(client);
}
void SystemPanel::UpdateSettings(const StatusBarSettings& settings)
{if(impl_->current)impl_->current->settings=settings;if(impl_->model){impl_->model->UpdateSettings(settings);impl_->paintDirty=true;}}
void SystemPanel::HideForMonitor(HMONITOR m, bool animate)
{
    const bool canceled=impl_->transition.CancelForMonitor(m);
    const bool continuation=impl_->afterClose&&MonitorFromWindow(impl_->afterCloseOwner,MONITOR_DEFAULTTONULL)==m;
    if(continuation)impl_->afterClose={};
    if(canceled||continuation)++impl_->closeGeneration;
    // Hiding the old bar closes only its current surface. A replacement (or
    // external continuation) on another monitor retains its request and hold.
    if(impl_->monitor==m&&(impl_->showing||impl_->modal||impl_->transition.Releasing()))
    {
        if(animate)impl_->Animate(false);
        else {++impl_->closeGeneration;impl_->FinishClose();}
    }
}
bool SystemPanel::PreTranslateMessage(MSG*){return false;}
bool SystemPanel::DropTrayIcon(std::string_view key,POINT p){if(!impl_->showing||impl_->closing||impl_->modal||impl_->slide.IsAnimating()||!impl_->model)return false;ScreenToClient(impl_->window,&p);const bool ok=impl_->model->Drop(key,impl_->ModelPoint(p.x,p.y));if(ok){impl_->Arrange();impl_->Paint();}return ok;}
bool SystemPanel::PreviewTrayDrop(std::string_view key,POINT p)
{
    std::optional<D2D1_RECT_F> next;
    if(!key.empty()&&impl_->showing&&!impl_->closing&&!impl_->modal&&!impl_->slide.IsAnimating()&&impl_->model)
    {ScreenToClient(impl_->window,&p);next=impl_->model->TrayDropIndicator(key,impl_->ModelPoint(p.x,p.y));}
    const auto& old=impl_->dropIndicator;
    if(next.has_value()!=old.has_value()||(next&&(next->left!=old->left||next->top!=old->top||next->right!=old->right||next->bottom!=old->bottom)))
    {impl_->dropIndicator=next;impl_->paintDirty=true;if(impl_->showing&&!impl_->destroying)impl_->Paint();}
    return next.has_value();
}
void SystemPanel::SetTrayDragFeedback(TrayDragFeedback feedback){impl_->dragFeedback=std::move(feedback);}
void SystemPanel::SetTrayStateChanged(std::function<void(HMONITOR,bool)> callback){impl_->trayStateChanged=std::move(callback);}
void SystemPanel::SetNativeControlsHandler(std::function<void(HWND,RECT)> callback){impl_->nativeControls=std::move(callback);}
void SystemPanel::SetCalendarMenuHandler(std::function<UINT(POINT,HWND,bool)> callback){impl_->calendarMenuHandler=std::move(callback);}
void SystemPanel::SetCalendarChoiceMenuHandler(std::function<UINT(POINT,HWND,const std::vector<std::wstring>&,std::size_t)> callback)
{impl_->calendarChoiceMenuHandler=std::move(callback);}
}
