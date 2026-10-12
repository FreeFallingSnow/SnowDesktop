#include "system_control_prompt.h"
#include "ui/input/text_input_window.h"
#include "ui/menu/modern_menu_appearance_rules.h"
#include "ui/render/native_form_style.h"
#include "widget/view/widget_scroll_rules.h"
#include "common/l10n.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace snowdesktop
{
namespace
{
constexpr int kSsid=101,kSecurity=102,kPassword=103;
constexpr UINT kReveal=WM_APP+43;
std::wstring DisplayIdentity(std::string_view value)
{
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(length>0?static_cast<std::size_t>(length):0,L' ');
    if(length>0)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),length);
    for(auto& ch:result)if(ch<32||ch==127)ch=L' ';return result;
}
bool PromptValid(const std::shared_ptr<SystemControlPromptState>& state)
{
    if(!state||state->cancelled)return false;
    if(std::chrono::steady_clock::now()>=state->deadline){state->error="timeout";state->cancelled=true;return false;}
    try{if(state->valid&&!state->valid()){state->error="canceled";state->cancelled=true;return false;}}
    catch(...){state->error="canceled";state->cancelled=true;return false;}
    return true;
}
struct DialogTemplate{DLGTEMPLATE dialog{};WORD menu=0,cls=0,title=0;};
DialogTemplate FormTemplate()
{
    DialogTemplate form;form.dialog.style=WS_POPUP|WS_CLIPCHILDREN|DS_MODALFRAME;
    form.dialog.dwExtendedStyle=WS_EX_CONTROLPARENT;form.dialog.cx=300;form.dialog.cy=260;return form;
}
struct Prompt
{
    explicit Prompt(system_control::Request& value):request(value){}
    system_control::Request& request;
    std::shared_ptr<SystemControlPromptState> state;
    PersonalizationSettings appearance;
    native_form::Palette palette;
    HWND window=nullptr,viewport=nullptr,password=nullptr,ssid=nullptr,security=nullptr;
    HFONT font=nullptr,headingFont=nullptr;
    HBRUSH backgroundBrush=nullptr,fieldBrush=nullptr;
    bool hidden=false,passwordForm=false,preview=false,dragging=false;
    std::wstring actor,target;
    unsigned dpi=96;
    int width=440,height=320,maximumHeight=700,bodyHeight=0,viewportHeight=180,scroll=0,dragStart=0,dragOffset=0,previewResult=0;
    int securityChoice=1;
    struct Item{HWND window;std::wstring label;int height=0,gap=0,top=0;bool paragraph=false;};
    std::vector<Item> items;
    ~Prompt(){if(font)DeleteObject(font);if(headingFont)DeleteObject(headingFont);if(backgroundBrush)DeleteObject(backgroundBrush);if(fieldBrush)DeleteObject(fieldBrush);}
    int Px(int dip) const{return native_form::Scale(dip,dpi);}
    int Dip(int pixels) const{return MulDiv(pixels,96,static_cast<int>(dpi));}
    HRGN Shape(int w,int h) const
    {
        const int radius=(std::min)((std::min)(w,h)/2,native_form::detail::Radius(appearance.cornerRadius,dpi));
        return radius?CreateRoundRectRgn(0,0,w+1,h+1,2*radius,2*radius):CreateRectRgn(0,0,w,h);
    }
    void End(int result)
    {
        if(password)SetWindowTextW(password,L"");
        if(preview){previewResult=result;DestroyWindow(window);}else EndDialog(window,result);
    }
    void Colors()
    {
        palette=native_form::ResolvePalette(appearance);
        const auto background=CreateSolidBrush(palette.background),field=CreateSolidBrush(palette.field);
        if(!background||!field){if(background)DeleteObject(background);if(field)DeleteObject(field);throw std::runtime_error("prompt brush unavailable");}
        if(backgroundBrush)DeleteObject(backgroundBrush);if(fieldBrush)DeleteObject(fieldBrush);
        backgroundBrush=background;fieldBrush=field;
    }
    void Fonts()
    {
        const auto next=native_form::CreateFormFont(dpi),heading=native_form::CreateFormFont(dpi,18,FW_SEMIBOLD);
        if(!next||!heading){if(next)DeleteObject(next);if(heading)DeleteObject(heading);throw std::runtime_error("prompt font unavailable");}
        const auto old=font,oldHeading=headingFont;font=next;headingFont=heading;
        for(const auto& item:items)SendMessageW(item.window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        for(const int id:{IDCANCEL,IDOK})if(const auto button=GetDlgItem(window,id))SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        if(old)DeleteObject(old);if(oldHeading)DeleteObject(oldHeading);
        for(const auto edit:{ssid,password})if(edit)
        {
            text_input::SetColors(edit,{palette.field,palette.foreground,palette.border,palette.accent,palette.accentText,palette.secondary},6.f*static_cast<float>(dpi)/96.f);
            text_input::SetPadding(edit,12.f*static_cast<float>(dpi)/96.f,4.f*static_cast<float>(dpi)/96.f);
        }
    }
    static LRESULT CALLBACK ChildProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        if(m==WM_SETFOCUS)SendMessageW(reinterpret_cast<HWND>(data),kReveal,reinterpret_cast<WPARAM>(w),0);
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,ChildProcedure,1);
        return DefSubclassProc(w,m,wp,lp);
    }
    static LRESULT CALLBACK ButtonProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR)
    {
        if(m==BM_SETSTYLE)wp=(wp&~static_cast<WPARAM>(BS_TYPEMASK))|BS_OWNERDRAW;
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,ButtonProcedure,2);
        return DefSubclassProc(w,m,wp,lp);
    }
    void PickSecurity()
    {
        std::vector<modern_menu::Item> choices;
        const wchar_t* labels[]{_LW("controlCenter.openNetwork"),L"WPA2-Personal",L"WPA3-Personal"};
        for(int i=0;i<3;++i){modern_menu::Item item;item.command=static_cast<UINT>(i+1);item.label=labels[i];item.checked=i==securityChoice;choices.push_back(std::move(item));}
        modern_menu::Options options;options.owner=security;options.zOrderOwner=window;options.dpi=dpi;
        options.appearance=static_cast<modern_menu::Appearance>(std::clamp(appearance.contextMenuStyle,0,6));
        options.lightTheme=modern_menu::appearance_rules::IsLightThemeForCurrentWindows(options.appearance);
        GetWindowRect(security,&options.anchorRect);options.anchor={options.anchorRect.left,options.anchorRect.bottom};
        options.rootPlacement=modern_menu::RootPlacement::BelowAnchorRect;
        const auto result=modern_menu::Show(choices,options);
        if(!window||!PromptValid(state))return;
        if(result.command>=1&&result.command<=3){securityChoice=static_cast<int>(result.command)-1;SecurityChanged();}
    }
    widget_scroll_rules::ScrollbarAxisGeometry Scrollbar() const
    {return widget_scroll_rules::ResolveScrollbarAxisGeometry(0,viewportHeight,bodyHeight,viewportHeight,scroll);}
    void PaintViewport(HDC dc)
    {
        RECT rect{};GetClientRect(viewport,&rect);FillRect(dc,&rect,backgroundBrush);
        const auto axis=Scrollbar();if(!axis.CanDrag())return;
        RECT thumb{Px(width-40-8),Px(axis.thumbStart),Px(width-40-3),Px(axis.thumbEnd)};
        native_form::detail::Rounded(dc,thumb,palette.secondary,palette.secondary,Px(3));
    }
    static LRESULT CALLBACK ViewportProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Prompt*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_PAINT&&self){PAINTSTRUCT paint{};const auto dc=BeginPaint(w,&paint);self->PaintViewport(dc);EndPaint(w,&paint);return 0;}
        if((m==WM_PRINT||m==WM_PRINTCLIENT)&&self){self->PaintViewport(reinterpret_cast<HDC>(wp));return 0;}
        if(m==WM_ERASEBKGND)return 1;
        if(self&&(m==WM_COMMAND||m==WM_DRAWITEM))return SendMessageW(self->window,m,wp,lp);
        if(self&&(m==WM_LBUTTONDOWN||m==WM_MOUSEMOVE||m==WM_LBUTTONUP))
        {
            const int x=self->Dip(GET_X_LPARAM(lp)),y=self->Dip(GET_Y_LPARAM(lp));const auto axis=self->Scrollbar();
            if(m==WM_LBUTTONDOWN&&axis.CanDrag()&&x>=self->width-52)
            {
                if(y>=axis.thumbStart&&y<axis.thumbEnd){self->dragging=true;self->dragStart=y;self->dragOffset=self->scroll;SetCapture(w);}
                else{self->scroll+=(y<axis.thumbStart?-1:1)*self->viewportHeight;self->Layout();}return 0;
            }
            if(m==WM_MOUSEMOVE&&self->dragging){self->scroll=widget_scroll_rules::ApplyScrollbarThumbDrag(self->dragOffset,y-self->dragStart,axis);self->Layout();return 0;}
            if(m==WM_LBUTTONUP&&self->dragging){self->dragging=false;ReleaseCapture();return 0;}
        }
        if(self&&(m==WM_CAPTURECHANGED||m==WM_CANCELMODE)){self->dragging=false;if(m==WM_CANCELMODE&&GetCapture()==w)ReleaseCapture();}
        if(m==WM_COMMAND||m==WM_DRAWITEM||m==WM_MEASUREITEM||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX||m==WM_MOUSEWHEEL)
            return SendMessageW(GetParent(w),m,wp,lp);
        return DefWindowProcW(w,m,wp,lp);
    }
    HWND Child(const wchar_t* cls,const std::wstring& text,DWORD style,int id,int itemHeight,int gap,bool paragraph=false)
    {
        const auto child=CreateWindowExW(0,cls,text.c_str(),WS_CHILD|WS_VISIBLE|style,0,0,0,0,viewport,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!child)throw std::runtime_error("prompt control unavailable");
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        if(!SetWindowSubclass(child,ChildProcedure,1,reinterpret_cast<DWORD_PTR>(window)))throw std::runtime_error("prompt focus handler unavailable");
        items.push_back({child,paragraph?text:std::wstring{},itemHeight,gap,0,paragraph});return child;
    }
    void Label(const std::wstring& text,int gap=6){Child(L"STATIC",text,SS_NOPREFIX,-1,20,gap,true);}
    HWND Edit(int id,DWORD style)
    {
        const auto edit=Child(text_input::WindowClass(),L"",WS_TABSTOP|ES_AUTOHSCROLL|style,id,34,16);
        text_input::SetColors(edit,{palette.field,palette.foreground,palette.border,palette.accent,palette.accentText,palette.secondary},6.f*static_cast<float>(dpi)/96.f);
        text_input::SetPadding(edit,12.f*static_cast<float>(dpi)/96.f,4.f*static_cast<float>(dpi)/96.f);
        text_input::SetAccessibleName(edit,_LW(id==kPassword?"controlCenter.password":"controlCenter.ssid"));
        return edit;
    }
    void Build()
    {
        Colors();Fonts();
        WNDCLASSW cls{};cls.lpfnWndProc=ViewportProcedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"SnowDesktopControlPromptViewport";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("prompt viewport unavailable");
        viewport=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,0,0,window,nullptr,cls.hInstance,nullptr);
        if(!viewport)throw std::runtime_error("prompt viewport unavailable");SetWindowLongPtrW(viewport,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        if(!actor.empty())Label(actor,12);if(!target.empty())Label(target,16);
        if(hidden)
        {
            Label(_LW("controlCenter.ssid"));ssid=Edit(kSsid,0);SendMessageW(ssid,EM_SETLIMITTEXT,32,0);
            Label(_LW("controlCenter.security"));security=Child(L"BUTTON",L"WPA2-Personal",WS_TABSTOP|BS_OWNERDRAW,kSecurity,34,16);
            if(!SetWindowSubclass(security,ButtonProcedure,2,0))throw std::runtime_error("prompt selection handler unavailable");
        }
        if(passwordForm)
        {
            Label(_LW("controlCenter.passwordHint"),12);Label(_LW("controlCenter.password"));password=Edit(kPassword,ES_PASSWORD);SendMessageW(password,EM_SETLIMITTEXT,63,0);
        }
        else if(system_control::RequiresConfirmation(request.name))
        {
            const auto& task=request.name;
            Label(_LW(task=="network.wifi.forget"?"controlCenter.confirmForget":task=="system.power.sleep"?"controlCenter.confirmSleep":task=="system.power.restart"?"controlCenter.confirmRestart":"controlCenter.confirmShutdown"),8);
        }
        for(const int id:{IDCANCEL,IDOK})
        {
            const auto button=CreateWindowExW(0,L"BUTTON",_LW(id==IDCANCEL?"settings.dialog.cancel":"settings.dialog.confirm"),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
                0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            if(!button||!SetWindowSubclass(button,ButtonProcedure,2,0))throw std::runtime_error("prompt button unavailable");
            SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        }
        SendMessageW(window,DM_SETDEFID,IDCANCEL,0);
    }
    void Reflow()
    {
        const auto dc=GetDC(window);if(!dc)throw std::runtime_error("prompt measurement unavailable");const auto previous=SelectObject(dc,font);
        int top=0;for(auto& item:items)
        {
            if(item.paragraph){RECT rect{0,0,Px(width-56),0};DrawTextW(dc,item.label.c_str(),-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);item.height=(std::max)(20,Dip(rect.bottom+Px(1)));}
            item.top=top;top+=item.height+item.gap;
        }
        SelectObject(dc,previous);ReleaseDC(window,dc);bodyHeight=top;
        height=(std::min)(maximumHeight,(std::max)(208,bodyHeight+128));viewportHeight=(std::max)(40,height-128);
    }
    void Layout()
    {
        scroll=std::clamp(scroll,0,(std::max)(0,bodyHeight-viewportHeight));
        MoveWindow(viewport,Px(20),Px(56),Px(width-40),Px(viewportHeight),TRUE);
        for(const auto& item:items)MoveWindow(item.window,0,Px(item.top-scroll),Px(width-56),Px(item.height),TRUE);
        const int buttonWidth=(std::min)(132,(width-52)/2);
        MoveWindow(GetDlgItem(window,IDCANCEL),Px(width-32-2*buttonWidth),Px(height-52),Px(buttonWidth),Px(34),TRUE);
        MoveWindow(GetDlgItem(window,IDOK),Px(width-20-buttonWidth),Px(height-52),Px(buttonWidth),Px(34),TRUE);
        const auto shape=Shape(Px(width),Px(height));if(shape&&!SetWindowRgn(window,shape,TRUE))DeleteObject(shape);
        InvalidateRect(viewport,nullptr,TRUE);InvalidateRect(window,nullptr,TRUE);
    }
    void Place(const RECT* suggested=nullptr)
    {
        const auto owner=GetWindow(window,GW_OWNER);MONITORINFO monitor{sizeof(monitor)};
        const bool found=GetMonitorInfoW(suggested?MonitorFromRect(suggested,MONITOR_DEFAULTTONEAREST):MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE;
        if(!preview&&found){width=(std::min)(440,Dip(monitor.rcWork.right-monitor.rcWork.left-Px(24)));maximumHeight=Dip(monitor.rcWork.bottom-monitor.rcWork.top-Px(24));}
        if(width<260||maximumHeight<168)throw std::runtime_error("prompt work area unavailable");Reflow();
        RECT anchor{};if(owner)GetWindowRect(owner,&anchor);else if(found)anchor=monitor.rcWork;
        int x=suggested?suggested->left:(anchor.left+anchor.right-Px(width))/2,y=suggested?suggested->top:(anchor.top+anchor.bottom-Px(height))/2;
        if(found){x=std::clamp(x,static_cast<int>(monitor.rcWork.left),(std::max)(static_cast<int>(monitor.rcWork.left),static_cast<int>(monitor.rcWork.right)-Px(width)));y=std::clamp(y,static_cast<int>(monitor.rcWork.top),(std::max)(static_cast<int>(monitor.rcWork.top),static_cast<int>(monitor.rcWork.bottom)-Px(height)));}
        SetWindowPos(window,nullptr,x,y,Px(width),Px(height),SWP_NOZORDER|SWP_NOACTIVATE);Layout();
    }
    void Reveal(HWND child)
    {
        for(const auto& item:items)if(item.window==child)
        {if(item.top<scroll)scroll=item.top;else if(item.top+item.height>scroll+viewportHeight)scroll=item.top+item.height-viewportHeight;Layout();break;}
    }
    void Paint(HDC dc)
    {
        RECT client{};GetClientRect(window,&client);FillRect(dc,&client,backgroundBrush);
        native_form::detail::Rounded(dc,client,palette.background,palette.border,native_form::detail::Radius(appearance.cornerRadius,dpi),(std::max)(1,Px(1)));
        const auto previous=SelectObject(dc,headingFont);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,palette.foreground);
        RECT title{Px(20),Px(14),Px(width-20),Px(44)};
        DrawTextW(dc,_LW(hidden?"controlCenter.hiddenNetwork":"statusBar.controlCenter"),-1,&title,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);SelectObject(dc,previous);
    }
    void SecurityChanged()
    {
        const wchar_t* labels[]{_LW("controlCenter.openNetwork"),L"WPA2-Personal",L"WPA3-Personal"};
        if(security)SetWindowTextW(security,labels[securityChoice]);
        if(!password)return;const bool secured=securityChoice!=0;
        EnableWindow(password,secured);if(!secured){SetWindowTextW(password,L"");request.password.Clear();}
        InvalidateRect(security,nullptr,FALSE);
    }
    void Confirm()
    {
        if(!PromptValid(state)){End(IDCANCEL);return;}
        if(hidden)
        {
            wchar_t name[64]{};GetWindowTextW(ssid,name,64);char utf8[256]{};
            const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name,-1,utf8,256,nullptr,nullptr);
            if(count<=1||count>33){SetFocus(ssid);return;}
            const auto selected=securityChoice;if(selected<0||selected>2){SetFocus(security);return;}
            request.arguments["ssid"]=utf8;request.arguments["security"]=selected==0?"open":selected==2?"wpa3":"wpa2";
        }
        if(password&&IsWindowEnabled(password))
        {
            struct SecretBuffer{wchar_t value[64]{};~SecretBuffer(){SecureZeroMemory(value,sizeof(value));}} secret;
            text_input::CopySecret(password,secret.value,64);request.password=system_control::Secret(secret.value);SetWindowTextW(password,L"");
        }
        request.hostConfirmed=true;End(IDOK);
    }
    static INT_PTR CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Prompt*>(GetWindowLongPtrW(w,DWLP_USER));
        try
        {
            if(m==WM_INITDIALOG)
            {
                self=reinterpret_cast<Prompt*>(lp);self->window=w;SetWindowLongPtrW(w,DWLP_USER,lp);self->state->window=w;
                if(!PromptValid(self->state)){self->End(IDCANCEL);return TRUE;}
                SetWindowTextW(w,_LW(self->hidden?"controlCenter.hiddenNetwork":"statusBar.controlCenter"));self->Build();self->Place();
                if(!self->preview&&!SetTimer(w,1,100,nullptr))throw std::runtime_error("prompt timer unavailable");
                if(!self->preview)SetFocus(self->ssid?self->ssid:self->password?self->password:GetDlgItem(w,IDCANCEL));return FALSE;
            }
            if(!self)return FALSE;
            if(m==WM_PAINT){PAINTSTRUCT paint{};const auto dc=BeginPaint(w,&paint);self->Paint(dc);EndPaint(w,&paint);return TRUE;}
            if(m==WM_PRINTCLIENT){self->Paint(reinterpret_cast<HDC>(wp));return TRUE;}
            if(m==WM_ERASEBKGND)return TRUE;
            if(m==WM_CTLCOLOREDIT||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLORLISTBOX)
            {
                const auto dc=reinterpret_cast<HDC>(wp);if(const auto brush=native_form::EditControlColor(reinterpret_cast<HWND>(lp),dc))return reinterpret_cast<INT_PTR>(brush);
                const bool field=m==WM_CTLCOLORLISTBOX;SetTextColor(dc,self->palette.foreground);SetBkColor(dc,field?self->palette.field:self->palette.background);
                return reinterpret_cast<INT_PTR>(field?self->fieldBrush:self->backgroundBrush);
            }
            if(m==WM_DRAWITEM){const auto& item=*reinterpret_cast<DRAWITEMSTRUCT*>(lp);native_form::DrawButton(item,self->palette,self->font,self->dpi,item.CtlID==IDOK);return TRUE;}
            if(m==kReveal){self->Reveal(reinterpret_cast<HWND>(wp));return TRUE;}
            if(m==WM_MOUSEWHEEL){self->scroll=widget_scroll_rules::ApplyWheelDelta(self->scroll,(std::max)(0,self->bodyHeight-self->viewportHeight),GET_WHEEL_DELTA_WPARAM(wp)).offset;self->Layout();return TRUE;}
            if(m==WM_TIMER&&wp==1&&!PromptValid(self->state)){self->End(IDCANCEL);return TRUE;}
            if(m==WM_DPICHANGED)
            {self->dpi=HIWORD(wp);if(!self->dpi)self->dpi=96;self->Fonts();self->Place(reinterpret_cast<const RECT*>(lp));if(const auto focus=GetFocus();focus&&IsChild(self->viewport,focus))self->Reveal(focus);return TRUE;}
            if(m==WM_SETTINGCHANGE||m==WM_THEMECHANGED)
            {self->Colors();for(const auto edit:{self->ssid,self->password})if(edit)text_input::SetColors(edit,{self->palette.field,self->palette.foreground,self->palette.border,self->palette.accent,self->palette.accentText,self->palette.secondary},6.f*static_cast<float>(self->dpi)/96.f);RedrawWindow(w,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_ERASE);return TRUE;}
            if(m==WM_COMMAND)
            {
                if(LOWORD(wp)==kSecurity&&HIWORD(wp)==BN_CLICKED){self->PickSecurity();return TRUE;}
                if(LOWORD(wp)==IDCANCEL){self->End(IDCANCEL);return TRUE;}
                if(LOWORD(wp)==IDOK){self->Confirm();return TRUE;}
            }
            if(m==WM_CLOSE){self->End(IDCANCEL);return TRUE;}
            if(m==WM_DESTROY){if(self->password&&IsWindow(self->password))SetWindowTextW(self->password,L"");}
            if(m==WM_NCHITTEST){POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&point);if(point.y<self->Px(48)){SetWindowLongPtrW(w,DWLP_MSGRESULT,HTCAPTION);return TRUE;}}
            if(m==WM_NCDESTROY){KillTimer(w,1);if(self->state->window==w)self->state->window=nullptr;self->window=nullptr;}
        }
        catch(...){if(self){self->state->error="confirmationUnavailable";self->state->cancelled=true;self->End(IDCANCEL);}return TRUE;}
        return FALSE;
    }
};
SystemControlPromptPreview CapturePrompt(Prompt& prompt,const char* name)
{
    auto form=FormTemplate();const auto window=CreateDialogIndirectParamW(GetModuleHandleW(nullptr),&form.dialog,nullptr,Prompt::Procedure,reinterpret_cast<LPARAM>(&prompt));
    if(!window||!IsWindow(window))throw std::runtime_error("prompt preview form unavailable");
    struct WindowGuard{HWND value;~WindowGuard(){if(IsWindow(value))DestroyWindow(value);}} guard{window};
    if(prompt.ssid)SetWindowTextW(prompt.ssid,L"SnowDesktop preview");
    if(prompt.password){SetWindowTextW(prompt.password,L"preview-only");if(!(GetWindowLongPtrW(prompt.password,GWL_STYLE)&ES_PASSWORD))throw std::runtime_error("prompt password lost masking");}
    SystemControlPromptPreview result;result.name=name;result.width=prompt.Px(prompt.width);result.height=prompt.Px(prompt.height);
    struct BitmapGuard{HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;~BitmapGuard(){if(old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}} dib;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=result.width;info.bmiHeader.biHeight=-result.height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    dib.dc=CreateCompatibleDC(nullptr);void* pixels=nullptr;dib.bitmap=CreateDIBSection(dib.dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!dib.dc||!dib.bitmap||!pixels)throw std::runtime_error("prompt preview bitmap unavailable");dib.old=SelectObject(dib.dc,dib.bitmap);prompt.Paint(dib.dc);
    const auto print=[&](HWND child,const RECT* clip)
    {
        RECT frame{};GetWindowRect(child,&frame);POINT origin{frame.left,frame.top};ScreenToClient(window,&origin);
        const int saved=SaveDC(dib.dc);if(clip)IntersectClipRect(dib.dc,clip->left,clip->top,clip->right,clip->bottom);
        SetViewportOrgEx(dib.dc,origin.x,origin.y,nullptr);SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dib.dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(dib.dc,saved);
    };
    RECT clip{prompt.Px(20),prompt.Px(56),prompt.Px(prompt.width-20),prompt.Px(56+prompt.viewportHeight)};
    print(prompt.viewport,&clip);for(const auto& item:prompt.items)print(item.window,&clip);
    for(const int id:{IDCANCEL,IDOK})print(GetDlgItem(window,id),nullptr);
    GdiFlush();result.pixels.resize(static_cast<std::size_t>(result.width)*result.height);std::memcpy(result.pixels.data(),pixels,result.pixels.size()*sizeof(std::uint32_t));
    const auto shape=prompt.Shape(result.width,result.height);
    for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x)
    {auto& pixel=result.pixels[static_cast<std::size_t>(y)*result.width+x];if(shape&&!PtInRegion(shape,x,y))pixel=0;else pixel|=0xff000000u;}
    if(shape)DeleteObject(shape);
    if(prompt.hidden)
    {
        prompt.securityChoice=0;prompt.SecurityChanged();
        if(IsWindowEnabled(prompt.password)||GetWindowTextLengthW(prompt.password)!=0||!prompt.request.password.View().empty())throw std::runtime_error("open network retained credentials");
    }
    const auto defaultButton=SendMessageW(window,DM_GETDEFID,0,0);
    if(HIWORD(defaultButton)!=DC_HASDEFID||LOWORD(defaultButton)!=IDCANCEL)throw std::runtime_error("prompt lost safe default button");
    MSG enter{};enter.hwnd=prompt.ssid?prompt.ssid:prompt.password?prompt.password:GetDlgItem(window,IDCANCEL);enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
    if(!IsDialogMessageW(window,&enter)||IsWindow(window)||prompt.previewResult!=IDCANCEL||prompt.request.hostConfirmed||!prompt.request.password.View().empty())
        throw std::runtime_error("prompt Enter authorized an unconfirmed request");
    return result;
}
}
bool ConfirmSystemControl(HWND owner,system_control::Request& request,const std::shared_ptr<SystemControlPromptState>& state,
    const PersonalizationSettings& appearance,const std::wstring& actor,const system_control::Snapshot* wifi)
{
    if(!state||!PromptValid(state))return false;
    std::wstring target;const auto argument=[&](const char* key){const auto found=request.arguments.find(key);return found==request.arguments.end()?std::string{}:found->second;};
    const bool connect=request.name=="network.wifi.connect";
    const bool hidden=connect&&argument("hidden")=="1"&&argument("ssid").empty()&&argument("networkId").empty()&&argument("profileName").empty();
    if(connect&&!argument("networkId").empty())
    {
        const JsonValue* network=nullptr;const auto* interfaces=wifi&&wifi->available?wifi->value.Find("interfaces"):nullptr;
        if(interfaces&&interfaces->IsArray())for(const auto& adapter:interfaces->array)
            if(system_control::json::String(adapter,"id")==argument("interfaceId"))
                if(const auto* networks=adapter.Find("networks");networks&&networks->IsArray())for(const auto& item:networks->array)
                    if(system_control::json::String(item,"id")==argument("networkId")){network=&item;break;}
        if(!network){state->error="networkGone";return false;}
        target=DisplayIdentity(system_control::json::String(*network,"ssid"));request.arguments["security"]=system_control::json::String(*network,"security");
    }
    else if(connect)target=DisplayIdentity(argument("ssid"));else if(request.name=="network.wifi.forget")target=DisplayIdentity(argument("profileName"));
    const auto security=argument("security");
    if(connect&&!hidden&&argument("profileName").empty()&&security!="open"&&security!="wpa2"&&security!="wpa3")
    {state->error="systemSettingsRequired";return false;}
    const bool password=hidden||(system_control::RequiresPasswordPrompt(request)&&security!="open");
    if(!password&&!system_control::RequiresConfirmation(request.name))return true;
    Prompt prompt(request);prompt.state=state;prompt.appearance=appearance;prompt.hidden=hidden;prompt.passwordForm=password;prompt.actor=actor;prompt.target=target;prompt.dpi=owner?GetDpiForWindow(owner):96;if(!prompt.dpi)prompt.dpi=96;
    auto form=FormTemplate();const auto result=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&form.dialog,owner,Prompt::Procedure,reinterpret_cast<LPARAM>(&prompt));
    state->window=nullptr;const bool confirmed=result==IDOK&&PromptValid(state);
    if(!confirmed){request.password.Clear();request.hostConfirmed=false;if(state->error.empty())state->error=result==-1?"confirmationUnavailable":"userCanceled";}
    return confirmed;
}
std::vector<SystemControlPromptPreview> RenderSystemControlPromptPreviews(const PersonalizationSettings& appearance,unsigned dpi)
{
    if(dpi<96||dpi>480)throw std::invalid_argument("prompt preview DPI");
    std::vector<SystemControlPromptPreview> result;
    for(int index=0;index<3;++index)
    {
        system_control::Request request;request.name=index==2?"system.power.restart":"network.wifi.connect";
        Prompt prompt(request);prompt.state=std::make_shared<SystemControlPromptState>();prompt.appearance=appearance;prompt.preview=true;prompt.dpi=dpi;
        prompt.hidden=index==1;prompt.passwordForm=index!=2;prompt.actor=L"SnowDesktop";if(index==0)prompt.target=L"SnowDesktop Wi-Fi";
        result.push_back(CapturePrompt(prompt,index==0?"password":index==1?"hidden-network":"confirm-power"));
    }
    return result;
}
}
