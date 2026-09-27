#include "system_calendar_editor.h"
#include "l10n.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace snowdesktop
{
namespace
{
constexpr int kTitle=101,kDate=102,kAllDay=103,kStart=104,kEnd=105,kReminder=106,kNotes=107,kDelete=108;
constexpr UINT kReveal=WM_APP+41;
constexpr int kContentHeight=394;
std::wstring Wide(const std::string& value)
{
    const int size=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(static_cast<std::size_t>((std::max)(0,size)),L' ');
    if(size)MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size);
    return result;
}
std::string Utf8(const std::wstring& value)
{
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<std::size_t>((std::max)(0,size)),' ');
    if(size)WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);
    return result;
}
std::wstring Text(HWND control)
{
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(control))+1,L'\0');
    const int length=GetWindowTextW(control,value.data(),static_cast<int>(value.size()));
    value.resize(static_cast<std::size_t>((std::max)(0,length)));return value;
}
std::wstring Time(int minutes)
{wchar_t text[8]{};swprintf_s(text,L"%02d:%02d",minutes/60,minutes%60);return text;}
COLORREF Color(float r,float g,float b)
{return RGB(static_cast<BYTE>(std::clamp(r,0.f,1.f)*255),static_cast<BYTE>(std::clamp(g,0.f,1.f)*255),static_cast<BYTE>(std::clamp(b,0.f,1.f)*255));}
struct DialogTemplate {DLGTEMPLATE dialog{};WORD menu=0,cls=0,title=0;};
struct Editor
{
    SystemCalendarEditorState session;
    PersonalizationSettings appearance;
    std::shared_ptr<SystemControlPromptState> lifetime;
    HWND window=nullptr,viewport=nullptr;
    HFONT font=nullptr,headingFont=nullptr;
    HBRUSH backgroundBrush=nullptr,fieldBrush=nullptr;
    COLORREF background=0,field=0,foreground=0,secondary=0,border=0,accent=0,accentText=0;
    struct ControlLayout {HWND window;int x,y,width,height;};
    std::vector<ControlLayout> children;
    int width=472,height=556,layoutWidth=472,scroll=0,viewportHeight=394;
    unsigned dpi=96;
    bool preview=false,confirmDelete=false,highContrast=false;
    std::wstring message;
    ~Editor(){if(font)DeleteObject(font);if(headingFont)DeleteObject(headingFont);if(backgroundBrush)DeleteObject(backgroundBrush);if(fieldBrush)DeleteObject(fieldBrush);}
    int Px(int value) const {return MulDiv(value,static_cast<int>(dpi),96);}
    HRGN Shape(int w,int h) const
    {
        const int radius=(std::min)((std::min)(w,h)/2,(std::max)(0,static_cast<int>(std::lround(appearance.cornerRadius*dpi/96.f))));
        return radius?CreateRoundRectRgn(0,0,w+1,h+1,2*radius,2*radius):CreateRectRgn(0,0,w,h);
    }
    HWND Control(int id) const {return GetDlgItem(viewport,id);}
    bool Valid() const
    {
        if(!lifetime||lifetime->cancelled)return false;
        try{return !lifetime->valid||lifetime->valid();}catch(...){return false;}
    }
    void End(int result)
    {if(preview)DestroyWindow(window);else EndDialog(window,result);}
    static LRESULT CALLBACK ChildProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        if(m==WM_SETFOCUS)SendMessageW(reinterpret_cast<HWND>(data),kReveal,reinterpret_cast<WPARAM>(w),0);
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,ChildProcedure,1);
        return DefSubclassProc(w,m,wp,lp);
    }
    static LRESULT CALLBACK ButtonProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR)
    {
        // Dialog navigation changes the default button style as focus moves.
        // Keep the same owner-drawn controls used by the preview.
        if(m==BM_SETSTYLE)wp=(wp&~static_cast<WPARAM>(BS_TYPEMASK))|BS_OWNERDRAW;
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,ButtonProcedure,1);
        return DefSubclassProc(w,m,wp,lp);
    }
    static LRESULT CALLBACK NativeFaceProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        auto* self=reinterpret_cast<Editor*>(data);
        // Keep the native checkbox/combo styles, state, keyboard handling and
        // accessibility providers. Only their visible surface is theme-owned.
        if(m==WM_PAINT)
        {
            PAINTSTRUCT paint{};const auto dc=BeginPaint(w,&paint);
            try{self->DrawControlFace(w,dc);}catch(...){}
            EndPaint(w,&paint);return 0;
        }
        if(m==WM_PRINT||m==WM_PRINTCLIENT)
        {try{self->DrawControlFace(w,reinterpret_cast<HDC>(wp));}catch(...){}return 0;}
        if(m==WM_ERASEBKGND)
        {RECT r{};GetClientRect(w,&r);FillRect(reinterpret_cast<HDC>(wp),&r,self->backgroundBrush);return 1;}
        if(m==WM_NCDESTROY)RemoveWindowSubclass(w,NativeFaceProcedure,2);
        const auto result=DefSubclassProc(w,m,wp,lp);
        if(m==WM_SETFOCUS||m==WM_KILLFOCUS||m==WM_UPDATEUISTATE||m==WM_ENABLE||m==BM_SETCHECK||m==CB_SETCURSEL||m==WM_KEYUP||m==WM_LBUTTONUP)
            InvalidateRect(w,nullptr,FALSE);
        return result;
    }
    static LRESULT CALLBACK ViewportProcedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        if(m==WM_ERASEBKGND){auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,GWLP_USERDATA));if(e){RECT r{};GetClientRect(w,&r);FillRect(reinterpret_cast<HDC>(wp),&r,e->backgroundBrush);return 1;}}
        // Native children send owner-draw messages to this immediate parent.
        // A dialog's TRUE means "handled", not the LRESULT to forward; preserve
        // the control's actual result instead of returning a stale DWLP_MSGRESULT.
        if(m==WM_DRAWITEM||m==WM_MEASUREITEM)
        {
            auto* self=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,GWLP_USERDATA));
            if(self)try
            {
                if(m==WM_DRAWITEM)self->DrawItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
                else reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=self->Px(28);
                return TRUE;
            }
            catch(...){return FALSE;}
        }
        if(m==WM_COMMAND||m==WM_CTLCOLORBTN||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX||m==WM_VSCROLL||m==WM_MOUSEWHEEL)
            return SendMessageW(GetParent(w),m,wp,lp);
        return DefWindowProcW(w,m,wp,lp);
    }
    void SetColors()
    {
        HIGHCONTRASTW hc{sizeof(hc)};highContrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0)&&(hc.dwFlags&HCF_HIGHCONTRASTON);
        const bool dark=appearance.contentTheme==0;
        background=highContrast?GetSysColor(COLOR_WINDOW):Color(appearance.widgetBgR,appearance.widgetBgG,appearance.widgetBgB);
        field=highContrast?GetSysColor(COLOR_WINDOW):(dark?RGB(43,45,49):RGB(255,255,255));
        foreground=highContrast?GetSysColor(COLOR_WINDOWTEXT):(dark?RGB(245,245,245):RGB(28,28,30));
        secondary=highContrast?foreground:(dark?RGB(188,190,196):RGB(92,95,101));
        border=highContrast?foreground:(dark?RGB(77,80,87):RGB(210,213,220));
        accent=highContrast?GetSysColor(COLOR_HIGHLIGHT):RGB(0,103,192);
        accentText=highContrast?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(255,255,255);
        backgroundBrush=CreateSolidBrush(background);fieldBrush=CreateSolidBrush(field);
    }
    HWND Child(const wchar_t* cls,const std::wstring& text,DWORD style,int x,int y,int w,int h,int id)
    {
        const auto child=CreateWindowExW(0,cls,text.c_str(),WS_CHILD|WS_VISIBLE|style,0,0,0,0,viewport,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!child)throw std::runtime_error("calendar editor control unavailable");
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        SetWindowSubclass(child,ChildProcedure,1,reinterpret_cast<DWORD_PTR>(window));
        children.push_back({child,x,y,w,h});return child;
    }
    void Label(const char* key,int x,int y,int w)
    {Child(L"STATIC",_LW(key),SS_NOPREFIX,x,y,w,22,-1);}
    void Field(int id,const std::wstring& text,int x,int y,int w,int h=32,bool multiline=false)
    {
        const auto control=Child(L"EDIT",text,WS_TABSTOP|WS_BORDER|ES_LEFT|ES_NOHIDESEL|
            (multiline?(ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL):ES_AUTOHSCROLL),x,y,w,h,id);
        SendMessageW(control,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(Px(8),Px(8)));
        SendMessageW(control,EM_SETLIMITTEXT,id==kNotes?8192:id==kTitle?512:id==kDate?10:5,0);
    }
    void Button(int id,const char* label)
    {
        const auto child=CreateWindowExW(0,L"BUTTON",_LW(label),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
            0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        if(!child)throw std::runtime_error("calendar editor button unavailable");
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        SetWindowSubclass(child,ButtonProcedure,1,0);
    }
    void CreateControls()
    {
        SetColors();
        font=CreateFontW(-Px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        headingFont=CreateFontW(-Px(18),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        WNDCLASSW cls{};cls.lpfnWndProc=ViewportProcedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"SnowDesktopCalendarEditorViewport";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("calendar editor viewport unavailable");
        viewport=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|WS_VSCROLL,0,0,0,0,window,nullptr,cls.hInstance,nullptr);
        if(!viewport)throw std::runtime_error("calendar editor viewport unavailable");
        SetWindowLongPtrW(viewport,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        layoutWidth=width;const int full=width-40,half=(full-12)/2;
        Label("settings.calendar.title",0,0,full);Field(kTitle,Wide(session.draft.title),0,24,full);
        Label("settings.calendar.date",0,70,half);Field(kDate,Wide(session.draft.date),0,94,half);
        const auto allDay=Child(L"BUTTON",_LW("settings.calendar.allDay"),WS_TABSTOP|BS_AUTOCHECKBOX,half+12,94,half,32,kAllDay);
        SetWindowSubclass(allDay,NativeFaceProcedure,2,reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(allDay,BM_SETCHECK,session.draft.allDay?BST_CHECKED:BST_UNCHECKED,0);
        Label("settings.calendar.start",0,140,half);Field(kStart,Time(session.draft.startMinutes),0,164,half);
        Label("settings.calendar.end",half+12,140,half);Field(kEnd,Time(session.draft.endMinutes),half+12,164,half);
        Label("settings.calendar.reminder",0,210,full);
        const auto reminder=Child(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,0,234,full,240,kReminder);
        SetWindowSubclass(reminder,NativeFaceProcedure,2,reinterpret_cast<DWORD_PTR>(this));
        for(std::size_t i=0;i<SystemCalendarReminderMinutes.size();++i)
        {
            const auto key="settings.calendar.reminder."+std::to_string(SystemCalendarReminderMinutes[i]);
            SendMessageW(reminder,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(_LW(key.c_str())));
            if(SystemCalendarReminderMinutes[i]==session.draft.reminderMinutes)SendMessageW(reminder,CB_SETCURSEL,i,0);
        }
        SendMessageW(reminder,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),Px(28));SendMessageW(reminder,CB_SETITEMHEIGHT,0,Px(28));
        Label("settings.calendar.notes",0,280,full);Field(kNotes,Wide(session.draft.notes),0,304,full,82,true);
        if(!session.original.id.empty())Button(kDelete,"app.settings.delete");
        Button(IDCANCEL,"settings.dialog.cancel");Button(IDOK,"settings.calendar.save");
        EnableWindow(Control(kStart),!session.draft.allDay);EnableWindow(Control(kEnd),!session.draft.allDay);
    }
    void Layout()
    {
        viewportHeight=(std::max)(40,height-162);
        scroll=std::clamp(scroll,0,(std::max)(0,kContentHeight-viewportHeight));
        MoveWindow(viewport,Px(20),Px(56),Px(width-40),Px(viewportHeight),TRUE);
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};info.nMin=0;info.nMax=kContentHeight-1;info.nPage=static_cast<UINT>(viewportHeight);info.nPos=scroll;SetScrollInfo(viewport,SB_VERT,&info,TRUE);
        RECT client{};GetClientRect(viewport,&client);const int missing=(layoutWidth-40)-MulDiv(client.right,96,static_cast<int>(dpi));
        for(const auto& child:children)
        {
            const int adjustment=child.x?missing/2:0;
            const int childWidth=child.width>layoutWidth/2?child.width-missing:child.width-missing/2;
            MoveWindow(child.window,Px(child.x-adjustment),Px(child.y-scroll),Px((std::max)(24,childWidth)),Px(child.height),TRUE);
        }
        const int buttonWidth=(std::min)(120,(width-64)/3);
        if(const auto remove=GetDlgItem(window,kDelete))MoveWindow(remove,Px(20),Px(height-52),Px(buttonWidth),Px(34),TRUE);
        MoveWindow(GetDlgItem(window,IDCANCEL),Px(width-32-2*buttonWidth),Px(height-52),Px(buttonWidth),Px(34),TRUE);
        MoveWindow(GetDlgItem(window,IDOK),Px(width-20-buttonWidth),Px(height-52),Px(buttonWidth),Px(34),TRUE);
        RECT frame{};GetWindowRect(window,&frame);
        const auto shape=Shape(frame.right-frame.left,frame.bottom-frame.top);
        if(shape&&!SetWindowRgn(window,shape,TRUE))DeleteObject(shape);
        InvalidateRect(window,nullptr,TRUE);
    }
    void Confirmation(bool enabled)
    {
        confirmDelete=enabled;message=enabled?_LW("settings.calendar.confirmDelete"):L"";
        EnableWindow(viewport,!enabled);ShowWindow(GetDlgItem(window,IDOK),enabled?SW_HIDE:SW_SHOW);
        SendMessageW(window,DM_SETDEFID,enabled?IDCANCEL:IDOK,0);
        if(!preview)SetFocus(GetDlgItem(window,IDCANCEL));
        InvalidateRect(window,nullptr,TRUE);
    }
    void Paint(HDC dc)
    {
        RECT client{};GetClientRect(window,&client);FillRect(dc,&client,backgroundBrush);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,foreground);
        const auto edge=CreatePen(PS_SOLID,(std::max)(1,Px(1)),border);const auto oldEdge=SelectObject(dc,edge),oldFill=SelectObject(dc,GetStockObject(NULL_BRUSH));
        const int corner=(std::max)(0,static_cast<int>(std::lround(appearance.cornerRadius*dpi/96.f)));
        RoundRect(dc,0,0,client.right,client.bottom,2*corner,2*corner);SelectObject(dc,oldFill);SelectObject(dc,oldEdge);DeleteObject(edge);
        const auto previous=SelectObject(dc,headingFont);
        RECT heading{Px(20),Px(14),Px(width-20),Px(44)};
        const auto caption=_LW(session.original.id.empty()?"settings.calendar.add":"settings.calendar.events");
        DrawTextW(dc,caption,-1,&heading,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        SelectObject(dc,font);SetTextColor(dc,confirmDelete?foreground:secondary);
        RECT error{Px(20),Px(height-100),Px(width-20),Px(height-56)};
        DrawTextW(dc,message.c_str(),-1,&error,DT_WORDBREAK|DT_NOPREFIX);SelectObject(dc,previous);
    }
    void DrawControlFace(HWND control,HDC dc)
    {
        const bool combo=GetDlgCtrlID(control)==kReminder;
        std::wstring label;
        if(combo)
        {
            const auto selected=SendMessageW(control,CB_GETCURSEL,0,0);
            const auto length=selected>=0?SendMessageW(control,CB_GETLBTEXTLEN,selected,0):CB_ERR;
            if(length>=0&&length<4096)
            {
                label.resize(static_cast<std::size_t>(length)+1);
                const auto copied=SendMessageW(control,CB_GETLBTEXT,selected,reinterpret_cast<LPARAM>(label.data()));
                label.resize(copied>=0?static_cast<std::size_t>(copied):0);
            }
        }
        else label=Text(control);
        RECT client{};GetClientRect(control,&client);const int saved=SaveDC(dc);
        FillRect(dc,&client,backgroundBrush);SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
        const bool enabled=IsWindowEnabled(control)&&IsWindowEnabled(viewport);
        const bool checked=!combo&&SendMessageW(control,BM_GETCHECK,0,0)==BST_CHECKED;
        const COLORREF ink=enabled?foreground:secondary;
        const auto fill=CreateSolidBrush(checked?accent:field);
        const auto edge=CreatePen(PS_SOLID,(std::max)(1,Px(1)),border);
        SelectObject(dc,fill);SelectObject(dc,edge);
        RECT box=client,text=client;
        if(combo)
        {
            RoundRect(dc,box.left,box.top,box.right,box.bottom,Px(8),Px(8));
            text.left+=Px(8);text.right-=Px(32);
        }
        else
        {
            box.left=Px(1);box.right=box.left+Px(16);box.top=(client.bottom-Px(16))/2;box.bottom=box.top+Px(16);
            RoundRect(dc,box.left,box.top,box.right,box.bottom,Px(4),Px(4));text.left=box.right+Px(9);
        }
        const auto stroke=CreatePen(PS_SOLID,(std::max)(1,Px(2)),checked?accentText:ink);SelectObject(dc,stroke);
        if(combo)
        {
            const int x=client.right-Px(16),y=client.bottom/2;
            const POINT arrow[]{{x-Px(4),y-Px(2)},{x,y+Px(2)},{x+Px(4),y-Px(2)}};Polyline(dc,arrow,3);
        }
        else if(checked)
        {
            const POINT check[]{{box.left+Px(3),box.top+Px(8)},{box.left+Px(7),box.top+Px(12)},{box.left+Px(13),box.top+Px(4)}};Polyline(dc,check,3);
        }
        SetTextColor(dc,ink);DrawTextW(dc,label.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        if(GetFocus()==control&&!(SendMessageW(control,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS))
        {RECT focus=client;InflateRect(&focus,-Px(3),-Px(3));DrawFocusRect(dc,&focus);}
        RestoreDC(dc,saved);DeleteObject(stroke);DeleteObject(edge);DeleteObject(fill);
    }
    void DrawItem(const DRAWITEMSTRUCT& item)
    {
        const bool combo=item.CtlID==kReminder;
        const bool selected=(item.itemState&ODS_SELECTED)!=0;
        const bool primary=!combo&&item.CtlID==IDOK;
        const bool highlight=(combo&&selected)||primary;
        std::wstring label;
        if(combo&&item.itemID<SystemCalendarReminderMinutes.size())label=_LW(("settings.calendar.reminder."+std::to_string(SystemCalendarReminderMinutes[item.itemID])).c_str());
        else if(!combo)label=Text(item.hwndItem);
        const int saved=SaveDC(item.hDC);
        const auto brush=CreateSolidBrush(highlight?accent:field);
        const auto pen=CreatePen(PS_SOLID,Px(1),selected?accent:border);
        // The native BUTTON may have erased with COLOR_BTNFACE. Cover its whole
        // client rect before the rounded face so no system-colored corners remain.
        FillRect(item.hDC,&item.rcItem,combo?fieldBrush:backgroundBrush);
        const auto oldBrush=SelectObject(item.hDC,brush),oldPen=SelectObject(item.hDC,pen),oldFont=SelectObject(item.hDC,font);
        if(combo)Rectangle(item.hDC,item.rcItem.left,item.rcItem.top,item.rcItem.right,item.rcItem.bottom);
        else RoundRect(item.hDC,item.rcItem.left,item.rcItem.top,item.rcItem.right,item.rcItem.bottom,Px(8),Px(8));
        SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,highlight?accentText:foreground);
        RECT text=item.rcItem;InflateRect(&text,-Px(8),0);DrawTextW(item.hDC,label.c_str(),-1,&text,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX|(combo?DT_LEFT:DT_CENTER));
        if((item.itemState&ODS_FOCUS)&&!(item.itemState&ODS_NOFOCUSRECT)){RECT focus=item.rcItem;InflateRect(&focus,-Px(3),-Px(3));DrawFocusRect(item.hDC,&focus);}
        SelectObject(item.hDC,oldFont);SelectObject(item.hDC,oldPen);SelectObject(item.hDC,oldBrush);RestoreDC(item.hDC,saved);DeleteObject(pen);DeleteObject(brush);
    }
    void Error()
    {
        const auto& code=session.error;
        const char* key=code=="conflict"||code=="not_found"?"settings.calendar.conflict":
            code=="invalid_date"||code=="invalid_time"||code=="invalid_reminder"||code=="title_required"||code=="text_too_long"?"settings.calendar.invalid":"settings.calendar.failed";
        message=_LW(key);InvalidateRect(window,nullptr,TRUE);
    }
    void Submit()
    {
        if(!Valid()){End(IDCANCEL);return;}
        auto candidate=session.draft;candidate.title=Utf8(Text(Control(kTitle)));candidate.date=Utf8(Text(Control(kDate)));
        candidate.notes=Utf8(Text(Control(kNotes)));candidate.allDay=SendMessageW(Control(kAllDay),BM_GETCHECK,0,0)==BST_CHECKED;
        const auto start=ParseCalendarEditorTime(Text(Control(kStart))),end=ParseCalendarEditorTime(Text(Control(kEnd)));
        if(!candidate.allDay&&(!start||!end||*end<*start)){session.error="invalid_time";Error();return;}
        candidate.startMinutes=candidate.allDay?0:*start;candidate.endMinutes=candidate.allDay?1439:*end;
        const auto reminder=SendMessageW(Control(kReminder),CB_GETCURSEL,0,0);
        if(reminder<0||static_cast<std::size_t>(reminder)>=SystemCalendarReminderMinutes.size()){session.error="invalid_reminder";Error();return;}
        candidate.reminderMinutes=SystemCalendarReminderMinutes[static_cast<std::size_t>(reminder)];
        if(session.Save(std::move(candidate)))End(IDOK);else Error();
    }
    static INT_PTR CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Editor*>(GetWindowLongPtrW(w,DWLP_USER));
        try
        {
            if(m==WM_INITDIALOG)
            {
                self=reinterpret_cast<Editor*>(lp);self->window=w;SetWindowLongPtrW(w,DWLP_USER,lp);self->lifetime->window=w;
                if(!self->Valid()){self->End(IDCANCEL);return TRUE;}
                SetWindowTextW(w,_LW("settings.calendar.events"));self->CreateControls();
                RECT size{0,0,self->Px(self->width),self->Px(self->height)};
                AdjustWindowRectExForDpi(&size,static_cast<DWORD>(GetWindowLongPtrW(w,GWL_STYLE)),FALSE,0,self->dpi);
                const auto ownerWindow=GetWindow(w,GW_OWNER);RECT owner{};GetWindowRect(ownerWindow,&owner);MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromWindow(ownerWindow,MONITOR_DEFAULTTONEAREST),&monitor);
                const int ww=size.right-size.left,hh=size.bottom-size.top;
                const int x=std::clamp((owner.left+owner.right-ww)/2,monitor.rcWork.left,(std::max)(monitor.rcWork.left,monitor.rcWork.right-ww));
                const int y=std::clamp((owner.top+owner.bottom-hh)/2,monitor.rcWork.top,(std::max)(monitor.rcWork.top,monitor.rcWork.bottom-hh));
                SetWindowPos(w,nullptr,x,y,ww,hh,SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();
                self->Confirmation(self->confirmDelete);
                if(!self->preview&&!SetTimer(w,1,100,nullptr)){self->End(IDCANCEL);return TRUE;}
                if(!self->preview)SetFocus(self->confirmDelete?GetDlgItem(w,IDCANCEL):self->Control(kTitle));
                return FALSE;
            }
            if(!self)return FALSE;
            if(m==WM_PAINT){PAINTSTRUCT paint{};const auto dc=BeginPaint(w,&paint);self->Paint(dc);EndPaint(w,&paint);return TRUE;}
            if(m==WM_PRINTCLIENT){self->Paint(reinterpret_cast<HDC>(wp));return TRUE;}
            if(m==WM_ERASEBKGND)return TRUE;
            if(m==WM_CTLCOLOREDIT||m==WM_CTLCOLORSTATIC||m==WM_CTLCOLORLISTBOX||m==WM_CTLCOLORBTN)
            {
                const auto dc=reinterpret_cast<HDC>(wp);const bool input=m==WM_CTLCOLOREDIT||m==WM_CTLCOLORLISTBOX;
                SetTextColor(dc,self->foreground);SetBkColor(dc,input?self->field:self->background);
                return reinterpret_cast<INT_PTR>(input?self->fieldBrush:self->backgroundBrush);
            }
            if(m==WM_MEASUREITEM){reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=self->Px(28);SetWindowLongPtrW(w,DWLP_MSGRESULT,TRUE);return TRUE;}
            if(m==WM_DRAWITEM){self->DrawItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));SetWindowLongPtrW(w,DWLP_MSGRESULT,TRUE);return TRUE;}
            if(m==WM_TIMER&&wp==1&&!self->Valid()){self->End(IDCANCEL);return TRUE;}
            if(m==WM_DPICHANGED)
            {
                self->dpi=HIWORD(wp);if(!self->dpi)self->dpi=96;
                const auto oldFont=self->font,oldHeading=self->headingFont;
                self->font=CreateFontW(-self->Px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
                self->headingFont=CreateFontW(-self->Px(18),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
                for(const auto& child:self->children)
                {
                    SendMessageW(child.window,WM_SETFONT,reinterpret_cast<WPARAM>(self->font),TRUE);
                    const int id=GetDlgCtrlID(child.window);
                    if(id==kTitle||id==kDate||id==kStart||id==kEnd||id==kNotes)
                        SendMessageW(child.window,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(self->Px(8),self->Px(8)));
                }
                for(const int id:{kDelete,IDCANCEL,IDOK})if(const auto control=GetDlgItem(w,id))SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(self->font),TRUE);
                if(oldFont)DeleteObject(oldFont);if(oldHeading)DeleteObject(oldHeading);
                const auto suggested=*reinterpret_cast<RECT*>(lp);MONITORINFO monitor{sizeof(monitor)};
                const bool monitored=GetMonitorInfoW(MonitorFromRect(&suggested,MONITOR_DEFAULTTONEAREST),&monitor)!=FALSE;
                if(monitored)
                {
                    self->width=(std::min)(self->width,MulDiv(monitor.rcWork.right-monitor.rcWork.left-24,96,static_cast<int>(self->dpi)));
                    self->height=(std::min)(556,MulDiv(monitor.rcWork.bottom-monitor.rcWork.top-24,96,static_cast<int>(self->dpi)));
                }
                RECT size{0,0,self->Px(self->width),self->Px(self->height)};AdjustWindowRectExForDpi(&size,static_cast<DWORD>(GetWindowLongPtrW(w,GWL_STYLE)),FALSE,0,self->dpi);
                const int ww=size.right-size.left,hh=size.bottom-size.top;
                const int x=monitored?std::clamp(suggested.left,monitor.rcWork.left,(std::max)(monitor.rcWork.left,monitor.rcWork.right-ww)):suggested.left;
                const int y=monitored?std::clamp(suggested.top,monitor.rcWork.top,(std::max)(monitor.rcWork.top,monitor.rcWork.bottom-hh)):suggested.top;
                SetWindowPos(w,nullptr,x,y,ww,hh,SWP_NOZORDER|SWP_NOACTIVATE);
                SendMessageW(self->Control(kReminder),CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),self->Px(28));
                SendMessageW(self->Control(kReminder),CB_SETITEMHEIGHT,0,self->Px(28));self->Layout();
                if(const auto focused=GetFocus();focused&&IsChild(self->viewport,focused))SendMessageW(w,kReveal,reinterpret_cast<WPARAM>(focused),0);
                return TRUE;
            }
            if(m==kReveal)
            {
                for(const auto& child:self->children)if(child.window==reinterpret_cast<HWND>(wp))
                {
                    const int h=GetDlgCtrlID(child.window)==kReminder?32:child.height;
                    if(child.y<self->scroll)self->scroll=child.y;
                    else if(child.y+h>self->scroll+self->viewportHeight)self->scroll=child.y+h-self->viewportHeight;
                    self->Layout();break;
                }
                return TRUE;
            }
            if(m==WM_VSCROLL||m==WM_MOUSEWHEEL)
            {
                if(m==WM_MOUSEWHEEL)self->scroll-=GET_WHEEL_DELTA_WPARAM(wp)/3;
                else switch(LOWORD(wp))
                {
                case SB_LINEUP:self->scroll-=24;break;case SB_LINEDOWN:self->scroll+=24;break;
                case SB_PAGEUP:self->scroll-=self->viewportHeight;break;case SB_PAGEDOWN:self->scroll+=self->viewportHeight;break;
                case SB_THUMBTRACK:{SCROLLINFO info{sizeof(info),SIF_TRACKPOS};GetScrollInfo(self->viewport,SB_VERT,&info);self->scroll=info.nTrackPos;break;}
                default:break;
                }
                self->Layout();return TRUE;
            }
            if(m==WM_COMMAND)
            {
                const int id=LOWORD(wp);
                if(id==IDCANCEL){if(self->confirmDelete)self->Confirmation(false);else self->End(IDCANCEL);return TRUE;}
                if(id==IDOK){if(!self->confirmDelete)self->Submit();return TRUE;}
                if(id==kDelete)
                {
                    if(!self->Valid()){self->End(IDCANCEL);return TRUE;}
                    if(!self->confirmDelete)self->Confirmation(true);
                    else if(self->session.Remove())self->End(IDOK);else self->Error();return TRUE;
                }
                if(id==kAllDay&&HIWORD(wp)==BN_CLICKED){const bool enabled=SendMessageW(self->Control(kAllDay),BM_GETCHECK,0,0)!=BST_CHECKED;EnableWindow(self->Control(kStart),enabled);EnableWindow(self->Control(kEnd),enabled);return TRUE;}
                if(id==kReminder&&HIWORD(wp)==CBN_SELCHANGE){InvalidateRect(self->Control(kReminder),nullptr,FALSE);return TRUE;}
            }
            if(m==WM_CLOSE){self->End(IDCANCEL);return TRUE;}
            if(m==WM_NCHITTEST)
            {
                POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(w,&point);
                if(point.y<self->Px(48)){SetWindowLongPtrW(w,DWLP_MSGRESULT,HTCAPTION);return TRUE;}
            }
            if(m==WM_NCDESTROY){KillTimer(w,1);if(self->lifetime->window==w)self->lifetime->window=nullptr;self->window=nullptr;}
        }
        catch(...){if(self){self->session.error="unavailable";if(m==WM_INITDIALOG)self->End(IDCANCEL);else self->Error();}return TRUE;}
        return FALSE;
    }
};
DialogTemplate Template()
{
    DialogTemplate form;form.dialog.style=WS_POPUP|WS_CLIPCHILDREN|DS_MODALFRAME;
    form.dialog.dwExtendedStyle=WS_EX_CONTROLPARENT;form.dialog.cx=300;form.dialog.cy=350;return form;
}
}
bool ShowSystemCalendarEditor(HWND owner,calendar::CalendarEvent& event,const PersonalizationSettings& appearance,
    const std::shared_ptr<SystemControlPromptState>& state,SystemCalendarEditorActions actions)
{
    Editor editor;editor.session.original=editor.session.draft=event;editor.session.actions=std::move(actions);
    editor.appearance=appearance;editor.lifetime=state;editor.dpi=owner?GetDpiForWindow(owner):96;
    if(!editor.dpi)editor.dpi=96;
    editor.session.valid=[&editor]{return editor.Valid();};if(!editor.Valid())return false;
    MONITORINFO monitor{sizeof(monitor)};
    if(GetMonitorInfoW(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor))
    {
        editor.width=(std::min)(472,MulDiv(monitor.rcWork.right-monitor.rcWork.left-24,96,static_cast<int>(editor.dpi)));
        editor.height=(std::min)(556,MulDiv(monitor.rcWork.bottom-monitor.rcWork.top-24,96,static_cast<int>(editor.dpi)));
    }
    if(editor.width<260||editor.height<240)return false;
    auto form=Template();const auto result=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&form.dialog,owner,Editor::Procedure,reinterpret_cast<LPARAM>(&editor));
    state->window=nullptr;
    if(result!=IDOK||!editor.session.committed||!editor.Valid())return false;
    if(!editor.session.deleted)event=std::move(editor.session.draft);return true;
}
SystemCalendarEditorPreview RenderSystemCalendarEditorPreview(const calendar::CalendarEvent& event,const PersonalizationSettings& appearance,bool confirmDelete,unsigned dpi)
{
    if(dpi<96||dpi>480)throw std::invalid_argument("calendar editor preview DPI");
    Editor editor;editor.session.original=editor.session.draft=event;editor.appearance=appearance;
    editor.lifetime=std::make_shared<SystemControlPromptState>();editor.preview=true;editor.confirmDelete=confirmDelete;editor.dpi=dpi;
    auto form=Template();const auto window=CreateDialogIndirectParamW(GetModuleHandleW(nullptr),&form.dialog,nullptr,Editor::Procedure,reinterpret_cast<LPARAM>(&editor));
    if(!window||!IsWindow(window))throw std::runtime_error("calendar editor preview form unavailable");
    struct WindowGuard{HWND value;~WindowGuard(){DestroyWindow(value);}} guard{window};
    SystemCalendarEditorPreview result;result.width=editor.Px(editor.width);result.height=editor.Px(editor.height);
    const auto selected=SendMessageW(editor.Control(kReminder),CB_GETCURSEL,0,0);
    if(selected>=0)
    {
        const auto length=SendMessageW(editor.Control(kReminder),CB_GETLBTEXTLEN,selected,0);
        if(length>=0&&length<4096)
        {
            result.reminderSelection.resize(static_cast<std::size_t>(length)+1);
            const auto copied=SendMessageW(editor.Control(kReminder),CB_GETLBTEXT,selected,reinterpret_cast<LPARAM>(result.reminderSelection.data()));
            result.reminderSelection.resize(copied>=0?static_cast<std::size_t>(copied):0);
        }
    }
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=result.width;info.bmiHeader.biHeight=-result.height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    struct BitmapGuard
    {
        HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ previous=nullptr;
        ~BitmapGuard(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
    } dib;
    dib.dc=CreateCompatibleDC(nullptr);void* pixels=nullptr;dib.bitmap=CreateDIBSection(dib.dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!dib.dc||!dib.bitmap||!pixels)throw std::runtime_error("calendar editor preview bitmap unavailable");
    const auto dc=dib.dc;dib.previous=SelectObject(dc,dib.bitmap);editor.Paint(dc);
    // Print the actual native edit/checkbox/combo/button controls. Never expose
    // this fixture form or activate an existing desktop window.
    const auto print=[&](HWND child,POINT origin,const RECT* clip)
    {
        const int saved=SaveDC(dc);if(clip)IntersectClipRect(dc,clip->left,clip->top,clip->right,clip->bottom);
        SetViewportOrgEx(dc,origin.x,origin.y,nullptr);SendMessageW(child,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(dc,saved);
    };
    RECT clip{editor.Px(20),editor.Px(56),editor.Px(editor.width-20),editor.Px(56+editor.viewportHeight)};
    for(const auto& child:editor.children)
    {
        POINT origin{};MapWindowPoints(child.window,window,&origin,1);print(child.window,origin,&clip);
    }
    for(const int id:{kDelete,IDCANCEL,IDOK})if(const auto child=GetDlgItem(window,id);child&&(id!=IDOK||!confirmDelete))
    {POINT origin{};MapWindowPoints(child,window,&origin,1);print(child,origin,nullptr);}
    GdiFlush();const auto count=static_cast<std::size_t>(result.width)*result.height*4;
    result.pixels.resize(count/4);std::memcpy(result.pixels.data(),pixels,count);
    const auto shape=editor.Shape(result.width,result.height);
    const int corner=(std::max)(0,static_cast<int>(std::ceil(appearance.cornerRadius*dpi/96.f)))+1;
    for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x)
    {
        const auto i=static_cast<std::size_t>(y)*result.width+x;
        if(shape&&(y<corner||y>=result.height-corner)&&(x<corner||x>=result.width-corner)&&!PtInRegion(shape,x,y))
            result.pixels[i]=0;
        else result.pixels[i]|=0xff000000u;
    }
    if(shape)DeleteObject(shape);
    // Exercise the same immediate-parent callback used by the real dropdown
    // list without showing a popup or targeting any existing desktop window.
    DRAWITEMSTRUCT dropdown{};dropdown.CtlType=ODT_COMBOBOX;dropdown.CtlID=kReminder;
    dropdown.itemID=static_cast<UINT>(selected);dropdown.itemAction=ODA_DRAWENTIRE;
    dropdown.hwndItem=editor.Control(kReminder);dropdown.hDC=dc;dropdown.rcItem={0,0,editor.Px(240),editor.Px(28)};
    if(SendMessageW(editor.viewport,WM_DRAWITEM,kReminder,reinterpret_cast<LPARAM>(&dropdown))!=TRUE)
        throw std::runtime_error("calendar reminder parent lost its owner-draw result");
    // Exercise the real dialog's default-key route after capturing the image.
    // The only effect callback here is an isolated, always-failing fixture.
    const auto defaultButton=SendMessageW(window,DM_GETDEFID,0,0);
    if(HIWORD(defaultButton)!=DC_HASDEFID||LOWORD(defaultButton)!=(confirmDelete?IDCANCEL:IDOK))
        throw std::runtime_error("calendar editor default button lost safe edit/confirmation semantics");
    int writes=0;editor.session.actions.save=[&](const auto&){++writes;return calendar::MutationResult{false,{},0,"write_failed"};};
    MSG enter{};enter.hwnd=confirmDelete?GetDlgItem(window,IDCANCEL):editor.Control(kTitle);enter.message=WM_KEYDOWN;enter.wParam=VK_RETURN;
    if(!IsDialogMessageW(window,&enter)||!IsWindow(window)||writes!=(confirmDelete?0:1)||editor.confirmDelete)
        throw std::runtime_error("calendar editor Enter discarded a draft or confirmed deletion");
    if(!confirmDelete&&(editor.session.error!="write_failed"||Text(editor.Control(kTitle))!=Wide(event.title)))
        throw std::runtime_error("calendar editor failed write discarded the native input draft");
    return result;
}
}
