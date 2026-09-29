#pragma once

// Internal styling for the existing Win32 form controls. Text, selection, IME,
// undo and accessibility remain owned by EDIT/COMBOBOX; no replacement editor.
#include "personalization.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <new>
#include <string>

namespace snowdesktop::native_form
{
struct Palette
{
    COLORREF background=0,field=0,foreground=0,secondary=0,border=0,accent=0,accentText=0;
    bool highContrast=false;
};
inline int Scale(int dip,UINT dpi) {return MulDiv(dip,static_cast<int>(dpi?dpi:96),96);}
inline Palette ResolvePalette(const PersonalizationSettings& appearance)
{
    const auto color=[](float r,float g,float b){return RGB(static_cast<BYTE>(std::clamp(r,0.f,1.f)*255),
        static_cast<BYTE>(std::clamp(g,0.f,1.f)*255),static_cast<BYTE>(std::clamp(b,0.f,1.f)*255));};
    HIGHCONTRASTW hc{sizeof(hc)};
    const bool high=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
    const bool dark=appearance.contentTheme==0;
    return {high?GetSysColor(COLOR_WINDOW):color(appearance.widgetBgR,appearance.widgetBgG,appearance.widgetBgB),
        high?GetSysColor(COLOR_WINDOW):(dark?RGB(43,45,49):RGB(255,255,255)),
        high?GetSysColor(COLOR_WINDOWTEXT):(dark?RGB(245,245,245):RGB(28,28,30)),
        high?GetSysColor(COLOR_WINDOWTEXT):(dark?RGB(188,190,196):RGB(92,95,101)),
        high?GetSysColor(COLOR_WINDOWTEXT):(dark?RGB(77,80,87):RGB(210,213,220)),
        high?GetSysColor(COLOR_HIGHLIGHT):RGB(0,103,192),
        high?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(255,255,255),high};
}
// The caller owns this font and must release it after its child controls.
inline HFONT CreateFormFont(UINT dpi,int dip=13,int weight=FW_NORMAL)
{
    return CreateFontW(-Scale(dip,dpi),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}
// Ordinary GDI children draw into their top-level parent's redirection bitmap.
// A DirectComposition host can intentionally have no such bitmap. Give only
// those children a native, opaque redirected surface; EDIT still owns painting,
// caret, selection, IME and accessibility. No captured bitmap is presented.
struct EditStyleFailure
{
    const char* operation=nullptr;
    DWORD error=ERROR_SUCCESS;
};
inline bool EditStyleFailed(EditStyleFailure* failure,const char* operation,DWORD error)
{
    if(!error)error=ERROR_INVALID_FUNCTION;
    if(failure)*failure={operation,error};
    SetLastError(error);return false;
}
inline bool EnsureOpaqueChildRedirection(HWND window,EditStyleFailure* failure=nullptr)
{
    if(failure)*failure={};
    if(!window||!IsWindow(window))return EditStyleFailed(failure,"IsWindow",ERROR_INVALID_WINDOW_HANDLE);
    if(!(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD))return true;
    bool required=false;
    for(auto parent=GetParent(window);parent;)
    {
        if(GetWindowLongPtrW(parent,GWL_EXSTYLE)&WS_EX_NOREDIRECTIONBITMAP){required=true;break;}
        if(!(GetWindowLongPtrW(parent,GWL_STYLE)&WS_CHILD))break;
        parent=GetParent(parent);
    }
    if(!required)return true;
    const auto style=GetWindowLongPtrW(window,GWL_EXSTYLE);
    if(!(style&WS_EX_LAYERED))
    {
        SetLastError(ERROR_SUCCESS);
        SetWindowLongPtrW(window,GWL_EXSTYLE,style|WS_EX_LAYERED);
        const auto error=GetLastError();
        // A successful change returns the previous style (often zero).
        // Synchronous style messages can also leave a nonzero last error.
        // Verify the requested bit instead of mistaking that pair for failure.
        if(!(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_LAYERED))
            return EditStyleFailed(failure,"SetWindowLongPtr(GWL_EXSTYLE)",error);
    }
    COLORREF key=0;BYTE opacity=0;DWORD flags=0;
    if(GetLayeredWindowAttributes(window,&key,&opacity,&flags)&&flags==LWA_ALPHA&&opacity==255)return true;
    if(!SetLayeredWindowAttributes(window,0,255,LWA_ALPHA))
        return EditStyleFailed(failure,"SetLayeredWindowAttributes",GetLastError());
    if(!GetLayeredWindowAttributes(window,&key,&opacity,&flags))
        return EditStyleFailed(failure,"GetLayeredWindowAttributes",GetLastError());
    if(flags!=LWA_ALPHA||opacity!=255)
        return EditStyleFailed(failure,"LayeredWindowAttributesReadback",ERROR_INVALID_DATA);
    return true;
}
namespace detail
{
inline void Fill(HDC dc,const RECT& rect,COLORREF color)
{const auto brush=CreateSolidBrush(color);if(brush){FillRect(dc,&rect,brush);DeleteObject(brush);}}
inline void Rounded(HDC dc,const RECT& rect,COLORREF fill,COLORREF edge,int radius,int stroke=1)
{
    const auto brush=CreateSolidBrush(fill);const auto pen=CreatePen(PS_SOLID,stroke,edge);
    const auto oldBrush=SelectObject(dc,brush),oldPen=SelectObject(dc,pen);
    RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,2*radius,2*radius);
    SelectObject(dc,oldPen);SelectObject(dc,oldBrush);if(pen)DeleteObject(pen);if(brush)DeleteObject(brush);
}
inline bool Enabled(HWND window)
{
    for(auto current=window;current;)
    {
        if(!IsWindowEnabled(current))return false;
        // GetParent(top-level popup) returns its owner. DialogBox disables
        // that owner, not the dialog's controls.
        if(!(GetWindowLongPtrW(current,GWL_STYLE)&WS_CHILD))break;
        current=GetParent(current);
    }
    return true;
}
inline int Radius(float dip,UINT dpi)
{return (std::max)(0,static_cast<int>(std::lround(std::clamp(dip,0.f,32.f)*static_cast<float>(dpi?dpi:96)/96.f)));}
inline std::wstring Text(HWND window)
{
    std::wstring text(static_cast<std::size_t>((std::max)(0,GetWindowTextLengthW(window)))+1,L'\0');
    const int copied=GetWindowTextW(window,text.data(),static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>((std::max)(0,copied)));return text;
}
inline std::wstring ComboText(HWND window,LRESULT item)
{
    const auto length=item>=0?SendMessageW(window,CB_GETLBTEXTLEN,static_cast<WPARAM>(item),0):CB_ERR;
    if(length<0||length>65535)return {};
    std::wstring text(static_cast<std::size_t>(length)+1,L'\0');
    const auto copied=SendMessageW(window,CB_GETLBTEXT,static_cast<WPARAM>(item),reinterpret_cast<LPARAM>(text.data()));
    text.resize(copied>=0?static_cast<std::size_t>(copied):0);return text;
}
}
inline void DrawButton(const DRAWITEMSTRUCT& item,const Palette& palette,HFONT font,UINT dpi,
    bool primary=false,float radiusDip=6.f)
{
    const auto text=detail::Text(item.hwndItem);const int saved=SaveDC(item.hDC);
    const bool enabled=detail::Enabled(item.hwndItem)&&(item.itemState&ODS_DISABLED)==0;
    const bool focused=(item.itemState&ODS_FOCUS)!=0&&(item.itemState&ODS_NOFOCUSRECT)==0,pressed=(item.itemState&ODS_SELECTED)!=0;
    const bool accented=enabled&&(primary||pressed);
    detail::Fill(item.hDC,item.rcItem,palette.background);
    detail::Rounded(item.hDC,item.rcItem,accented?palette.accent:palette.field,
        enabled&&(focused||primary)?palette.accent:palette.border,detail::Radius(radiusDip,dpi),(std::max)(1,Scale(1,dpi)));
    SelectObject(item.hDC,font);SetBkMode(item.hDC,TRANSPARENT);
    SetTextColor(item.hDC,!enabled?palette.secondary:accented?palette.accentText:palette.foreground);
    RECT rect=item.rcItem;InflateRect(&rect,-Scale(8,dpi),0);
    DrawTextW(item.hDC,text.c_str(),-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    if(focused&&!(item.itemState&ODS_NOFOCUSRECT)){rect=item.rcItem;InflateRect(&rect,-Scale(3,dpi),-Scale(3,dpi));DrawFocusRect(item.hDC,&rect);}
    RestoreDC(item.hDC,saved);
}
inline void DrawComboFace(HWND window,HDC dc,const Palette& palette,HFONT font,UINT dpi)
{
    const auto text=detail::ComboText(window,SendMessageW(window,CB_GETCURSEL,0,0));
    const int saved=SaveDC(dc);RECT rect{};GetClientRect(window,&rect);
    const bool enabled=detail::Enabled(window),focused=GetFocus()==window&&!(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS);
    detail::Fill(dc,rect,palette.background);
    detail::Rounded(dc,rect,palette.field,enabled&&focused?palette.accent:palette.border,Scale(6,dpi),(std::max)(1,Scale(1,dpi)));
    SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,enabled?palette.foreground:palette.secondary);
    RECT label=rect;label.left+=Scale(8,dpi);label.right-=Scale(32,dpi);
    DrawTextW(dc,text.c_str(),-1,&label,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    const int x=rect.right-Scale(16,dpi),y=(rect.top+rect.bottom)/2;
    const POINT arrow[]{{x-Scale(4,dpi),y-Scale(2,dpi)},{x,y+Scale(2,dpi)},{x+Scale(4,dpi),y-Scale(2,dpi)}};
    const auto pen=CreatePen(PS_SOLID,(std::max)(1,Scale(2,dpi)),enabled?palette.foreground:palette.secondary);
    const auto old=SelectObject(dc,pen);Polyline(dc,arrow,3);SelectObject(dc,old);if(pen)DeleteObject(pen);
    if(focused&&!(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
    {InflateRect(&rect,-Scale(3,dpi),-Scale(3,dpi));DrawFocusRect(dc,&rect);}
    RestoreDC(dc,saved);
}
inline void DrawComboItem(const DRAWITEMSTRUCT& item,const Palette& palette,HFONT font,UINT dpi)
{
    const auto text=detail::ComboText(item.hwndItem,item.itemID==static_cast<UINT>(-1)?-1:static_cast<LRESULT>(item.itemID));
    const int saved=SaveDC(item.hDC);const bool selected=(item.itemState&ODS_SELECTED)!=0;
    detail::Fill(item.hDC,item.rcItem,selected?palette.accent:palette.field);
    SelectObject(item.hDC,font);SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,selected?palette.accentText:palette.foreground);
    RECT rect=item.rcItem;InflateRect(&rect,-Scale(8,dpi),0);
    DrawTextW(item.hDC,text.c_str(),-1,&rect,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    if((item.itemState&ODS_FOCUS)&&!(item.itemState&ODS_NOFOCUSRECT))
    {rect=item.rcItem;InflateRect(&rect,-Scale(3,dpi),-Scale(3,dpi));DrawFocusRect(item.hDC,&rect);}
    RestoreDC(item.hDC,saved);
}

struct EditScrollInfo
{
    bool canScroll=false;
    RECT thumb{}; // Window-local coordinates, including the nonclient frame.
    int firstLine=0,maximum=0;
};
namespace detail
{
constexpr UINT_PTR kEditSubclass=0x4e464544;
struct EditState
{
    HWND window=nullptr;Palette palette;UINT dpi=96;float radius=6;
    HBRUSH brush=nullptr;int lineHeight=16,references=1,wheel=0;
    bool multiline=false,dragging=false,refreshing=false;
    int dragY=0,dragFirst=0,dragMaximum=0,dragTravel=0;
    ~EditState(){if(brush)DeleteObject(brush);}
};
inline LRESULT CALLBACK EditProcedure(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
inline EditState* State(HWND window)
{
    DWORD_PTR value=0;return GetWindowSubclass(window,EditProcedure,kEditSubclass,&value)?reinterpret_cast<EditState*>(value):nullptr;
}
// EN_CHANGE/EN_UPDATE and focus notifications may synchronously destroy EDIT.
// Keep its state until every active subclass stack has returned.
struct EditCall
{
    EditState* state;
    explicit EditCall(EditState* value):state(value){++state->references;}
    ~EditCall(){if(!--state->references)delete state;}
};
inline RECT ClientInWindow(HWND window)
{
    RECT frame{},client{};GetWindowRect(window,&frame);GetClientRect(window,&client);
    POINT origin{};ClientToScreen(window,&origin);OffsetRect(&client,origin.x-frame.left,origin.y-frame.top);return client;
}
struct ScrollGeometry
{
    EditScrollInfo info;RECT bar{};int page=1,arrow=0,trackTop=0,trackBottom=0;
};
inline ScrollGeometry Scroll(HWND window,const EditState& state)
{
    ScrollGeometry result;if(!state.multiline)return result;
    RECT client{};GetClientRect(window,&client);
    result.page=(std::max)(1,static_cast<int>(client.bottom-client.top)/(std::max)(1,state.lineHeight));
    const int lines=static_cast<int>(SendMessageW(window,EM_GETLINECOUNT,0,0));
    result.info.maximum=(std::max)(0,lines-result.page);
    result.info.firstLine=static_cast<int>(SendMessageW(window,EM_GETFIRSTVISIBLELINE,0,0));
    SCROLLBARINFO bar{sizeof(bar)};
    if(result.info.maximum<=0||!GetScrollBarInfo(window,OBJID_VSCROLL,&bar))return result;
    RECT frame{};GetWindowRect(window,&frame);result.bar=bar.rcScrollBar;OffsetRect(&result.bar,-frame.left,-frame.top);
    if(result.bar.right<=result.bar.left||result.bar.bottom<=result.bar.top)return result;
    result.arrow=(std::max)(0,static_cast<int>(bar.dxyLineButton));
    result.trackTop=result.bar.top+result.arrow;result.trackBottom=result.bar.bottom-result.arrow;
    result.info.thumb={result.bar.left,result.bar.top+bar.xyThumbTop,result.bar.right,result.bar.top+bar.xyThumbBottom};
    result.info.canScroll=result.trackBottom>result.trackTop&&result.info.thumb.bottom>result.info.thumb.top;
    return result;
}
inline void PaintFrame(HWND window,HDC dc,const EditState& state)
{
    RECT frame{};GetWindowRect(window,&frame);OffsetRect(&frame,-frame.left,-frame.top);
    const RECT client=ClientInWindow(window);const int saved=SaveDC(dc);
    ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
    Fill(dc,frame,state.palette.background);
    const bool visibleFocus=GetFocus()==window&&Enabled(window)&&!(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS);
    Rounded(dc,frame,state.palette.field,visibleFocus?state.palette.accent:state.palette.border,
        Radius(state.radius,state.dpi),(std::max)(1,Scale(1,state.dpi)));
    const auto scroll=Scroll(window,state);
    if(scroll.info.canScroll)
    {
        // Use the native range and geometry, also exposed by the standard EDIT
        // accessibility provider. A different painted geometry would lie to UIA.
        Fill(dc,scroll.bar,state.palette.field);
        RECT thumb=scroll.info.thumb;const int inset=(std::max)(1,Scale(4,state.dpi));
        InflateRect(&thumb,-(std::min)(inset,static_cast<int>((thumb.right-thumb.left-1)/2)),0);
        Rounded(dc,thumb,state.palette.secondary,state.palette.secondary,(thumb.right-thumb.left)/2);
        const auto pen=CreatePen(PS_SOLID,(std::max)(1,Scale(1,state.dpi)),state.palette.secondary);const auto previous=SelectObject(dc,pen);
        const int x=(scroll.bar.left+scroll.bar.right)/2,half=(std::max)(1,Scale(3,state.dpi));
        const int top=scroll.bar.top+scroll.arrow/2,bottom=scroll.bar.bottom-scroll.arrow/2;
        const POINT up[]{{x-half,top+half/2},{x,top-half/2},{x+half,top+half/2}};
        const POINT down[]{{x-half,bottom-half/2},{x,bottom+half/2},{x+half,bottom-half/2}};
        Polyline(dc,up,3);Polyline(dc,down,3);SelectObject(dc,previous);if(pen)DeleteObject(pen);
    }
    RestoreDC(dc,saved);
}
inline void PaintFrame(HWND window,const EditState& state)
{if(const auto dc=GetWindowDC(window)){PaintFrame(window,dc,state);ReleaseDC(window,dc);}}
inline void Measure(EditState& state)
{
    if(const auto dc=GetDC(state.window))
    {
        const auto font=reinterpret_cast<HFONT>(SendMessageW(state.window,WM_GETFONT,0,0));
        const auto previous=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));TEXTMETRICW metrics{};
        if(GetTextMetricsW(dc,&metrics))state.lineHeight=(std::max)(1,static_cast<int>(metrics.tmHeight));
        SelectObject(dc,previous);ReleaseDC(state.window,dc);
    }
}
inline void ScrollTo(HWND window,EditState& state,int first)
{
    const auto scroll=Scroll(window,state);first=std::clamp(first,0,scroll.info.maximum);
    if(first!=scroll.info.firstLine)SendMessageW(window,EM_LINESCROLL,0,first-scroll.info.firstLine);
    if(state.window)PaintFrame(window,state);
}
inline POINT WindowPoint(HWND window,LPARAM position,bool screen)
{
    POINT point{GET_X_LPARAM(position),GET_Y_LPARAM(position)};if(!screen)ClientToScreen(window,&point);
    RECT frame{};GetWindowRect(window,&frame);point.x-=frame.left;point.y-=frame.top;return point;
}
inline LRESULT CALLBACK EditProcedure(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
{
    auto* state=reinterpret_cast<EditState*>(data);EditCall call(state);
    if(message==WM_NCDESTROY)
    {
        RemoveWindowSubclass(window,EditProcedure,kEditSubclass);state->window=nullptr;
        --state->references;return DefSubclassProc(window,message,wp,lp);
    }
    if(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK||message==WM_NCLBUTTONDOWN||message==WM_NCLBUTTONDBLCLK)
    {
        // Child mouse input does not pass through the host's pointer handler.
        // Hide keyboard cues before focus changes, including padding/scrollbar clicks.
        if(const auto parent=GetParent(window))SendMessageW(parent,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
        if(!state->window)return 0;
    }
    if(message==WM_NCCALCSIZE)
    {
        const auto result=DefSubclassProc(window,message,wp,lp);
        auto& rect=wp?reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]:*reinterpret_cast<RECT*>(lp);
        const int x=(std::min)(Scale(8,state->dpi),(std::max)(0,static_cast<int>(rect.right-rect.left-1)/2));
        const int space=(std::max)(0,static_cast<int>(rect.bottom-rect.top)-state->lineHeight);
        const int y=state->multiline?(std::min)(Scale(6,state->dpi),space/2):space/2;
        rect.left+=x;rect.right-=x;rect.top+=y;rect.bottom-=y;return result;
    }
    if(message==WM_NCPAINT){PaintFrame(window,*state);return 0;}
    if(message==WM_NCMOUSEMOVE||message==WM_NCMOUSELEAVE)
        return 0;
    if(message==WM_PRINT)
    {
        const auto dc=reinterpret_cast<HDC>(wp);
        if((lp&PRF_CHECKVISIBLE)&&!IsWindowVisible(window))return 0;
        if(lp&PRF_NONCLIENT)PaintFrame(window,dc,*state);
        if(lp&PRF_CLIENT)
        {
            const int saved=SaveDC(dc);const auto client=ClientInWindow(window);POINT origin{};GetViewportOrgEx(dc,&origin);
            SetViewportOrgEx(dc,origin.x+client.left,origin.y+client.top,nullptr);
            IntersectClipRect(dc,0,0,client.right-client.left,client.bottom-client.top);
            if(lp&PRF_ERASEBKGND)DefSubclassProc(window,WM_ERASEBKGND,wp,0);
            DefSubclassProc(window,WM_PRINTCLIENT,wp,lp);RestoreDC(dc,saved);
        }
        return 0;
    }
    if(message==WM_NCHITTEST)
    {
        const auto scroll=Scroll(window,*state);const auto point=WindowPoint(window,lp,true);
        if(scroll.info.canScroll&&PtInRect(&scroll.bar,point))return HTVSCROLL;
        const auto client=ClientInWindow(window);if(PtInRect(&client,point))return HTCLIENT;
        return HTBORDER;
    }
    if(message==WM_NCLBUTTONDOWN||message==WM_NCLBUTTONDBLCLK)
    {
        const auto scroll=Scroll(window,*state);const auto point=WindowPoint(window,lp,true);
        SetFocus(window);if(!state->window)return 0;
        if(scroll.info.canScroll&&PtInRect(&scroll.bar,point))
        {
            if(PtInRect(&scroll.info.thumb,point))
            {
                state->dragging=true;state->dragY=point.y;state->dragFirst=scroll.info.firstLine;state->dragMaximum=scroll.info.maximum;
                state->dragTravel=(std::max)(0,scroll.trackBottom-scroll.trackTop-static_cast<int>(scroll.info.thumb.bottom-scroll.info.thumb.top));
                SetCapture(window);
            }
            else ScrollTo(window,*state,scroll.info.firstLine+(point.y<scroll.trackTop?-1:point.y>=scroll.trackBottom?1:point.y<scroll.info.thumb.top?-scroll.page:scroll.page));
            return 0;
        }
        POINT client{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window,&client);RECT bounds{};GetClientRect(window,&bounds);
        client.x=std::clamp<LONG>(client.x,0,(std::max)(0L,bounds.right-1));client.y=std::clamp<LONG>(client.y,0,(std::max)(0L,bounds.bottom-1));
        WPARAM modifiers=MK_LBUTTON;if(GetKeyState(VK_SHIFT)&0x8000)modifiers|=MK_SHIFT;if(GetKeyState(VK_CONTROL)&0x8000)modifiers|=MK_CONTROL;
        return SendMessageW(window,message==WM_NCLBUTTONDBLCLK?WM_LBUTTONDBLCLK:WM_LBUTTONDOWN,modifiers,MAKELPARAM(client.x,client.y));
    }
    if(message==WM_MOUSEMOVE&&state->dragging)
    {
        const auto point=WindowPoint(window,lp,false);
        if(state->dragTravel>0)ScrollTo(window,*state,state->dragFirst+static_cast<int>(std::lround(
            static_cast<double>(point.y-state->dragY)*state->dragMaximum/state->dragTravel)));
        return 0;
    }
    if(message==WM_LBUTTONUP&&state->dragging)
    {state->dragging=false;if(GetCapture()==window)ReleaseCapture();PaintFrame(window,*state);return 0;}
    if(message==WM_CAPTURECHANGED||message==WM_CANCELMODE)
    {const bool dragging=state->dragging;state->dragging=false;if(dragging&&message==WM_CANCELMODE&&GetCapture()==window)ReleaseCapture();}
    if(message==WM_MOUSEWHEEL&&state->multiline)
    {
        const auto scroll=Scroll(window,*state);
        if(scroll.info.canScroll)
        {
            UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
            state->wheel+=GET_WHEEL_DELTA_WPARAM(wp);const int steps=state->wheel/WHEEL_DELTA;state->wheel%=WHEEL_DELTA;
            const int amount=lines==WHEEL_PAGESCROLL?scroll.page:static_cast<int>((std::min)(lines,static_cast<UINT>(scroll.page)));
            ScrollTo(window,*state,scroll.info.firstLine-steps*amount);return 0;
        }
    }
    const auto result=DefSubclassProc(window,message,wp,lp);if(!state->window)return result;
    if(message==WM_SETFONT&&!state->refreshing)
    {
        state->refreshing=true;Measure(*state);
        SetWindowPos(window,nullptr,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
        state->refreshing=false;
    }
    switch(message)
    {
    case WM_PAINT:case WM_SIZE:case WM_SETFOCUS:case WM_KILLFOCUS:case WM_ENABLE:case WM_SETFONT:case WM_UPDATEUISTATE:
    case WM_SETTEXT:case WM_CHAR:case WM_KEYDOWN:case WM_IME_COMPOSITION:case WM_PASTE:case WM_CUT:
    case WM_CLEAR:case WM_UNDO:case EM_REPLACESEL:case EM_SETSEL:case EM_SCROLLCARET:case EM_LINESCROLL:
    case EM_SCROLL:case WM_VSCROLL:case WM_MOUSEWHEEL:case WM_NCMOUSEMOVE:PaintFrame(window,*state);break;
    default:break;
    }
    return result;
}
}
// Attach after setting the native font. No wrapper/reparenting: ids, EN_*
// notifications and the standard accessibility provider remain unchanged.
inline bool AttachEdit(HWND window,const Palette& palette,UINT dpi,float radiusDip=6.f,EditStyleFailure* failure=nullptr)
{
    if(failure)*failure={};
    if(!EnsureOpaqueChildRedirection(window,failure))return false;
    wchar_t className[16]{};
    if(!GetClassNameW(window,className,static_cast<int>(std::size(className))))return EditStyleFailed(failure,"GetClassName",GetLastError());
    if(lstrcmpiW(className,L"EDIT")!=0)return EditStyleFailed(failure,"EditClass",ERROR_INVALID_PARAMETER);
    auto* state=detail::State(window);
    if(!state)
    {
        state=new(std::nothrow) detail::EditState;if(!state)return EditStyleFailed(failure,"AllocateEditState",ERROR_NOT_ENOUGH_MEMORY);
        state->window=window;state->palette=palette;state->dpi=dpi?dpi:96;state->radius=radiusDip;
        state->brush=CreateSolidBrush(palette.field);if(!state->brush){const auto error=GetLastError();delete state;return EditStyleFailed(failure,"CreateSolidBrush",error);}
        state->multiline=(GetWindowLongPtrW(window,GWL_STYLE)&ES_MULTILINE)!=0;
        if(!SetWindowSubclass(window,detail::EditProcedure,detail::kEditSubclass,reinterpret_cast<DWORD_PTR>(state))){const auto error=GetLastError();delete state;return EditStyleFailed(failure,"SetWindowSubclass",error);}
    }
    detail::EditCall call(state);
    const auto brush=CreateSolidBrush(palette.field);if(!brush)return EditStyleFailed(failure,"CreateSolidBrush",GetLastError());
    if(state->brush)DeleteObject(state->brush);state->brush=brush;state->palette=palette;state->dpi=dpi?dpi:96;state->radius=radiusDip;
    auto style=GetWindowLongPtrW(window,GWL_STYLE)&~static_cast<LONG_PTR>(WS_BORDER);
    SetWindowLongPtrW(window,GWL_STYLE,style);
    SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)&~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE|WS_EX_STATICEDGE));
    detail::Measure(*state);SendMessageW(window,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
    SetWindowPos(window,nullptr,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
    if(!state->window)return EditStyleFailed(failure,"EditDestroyed",ERROR_INVALID_WINDOW_HANDLE);
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ERASE);return true;
}
inline void UpdateEdit(HWND window,const Palette& palette,UINT dpi,float radiusDip=6.f)
{AttachEdit(window,palette,dpi,radiusDip);}
// Borrowed brush: the EDIT subclass releases it on WM_NCDESTROY. Call from the
// immediate parent's WM_CTLCOLOREDIT / WM_CTLCOLORSTATIC before its fallback.
inline HBRUSH EditControlColor(HWND window,HDC dc)
{
    const auto* state=detail::State(window);if(!state)return nullptr;
    SetTextColor(dc,detail::Enabled(window)?state->palette.foreground:state->palette.secondary);
    SetBkColor(dc,state->palette.field);return state->brush;
}
inline EditScrollInfo GetEditScrollInfo(HWND window)
{const auto* state=detail::State(window);return state?detail::Scroll(window,*state).info:EditScrollInfo{};}
}
