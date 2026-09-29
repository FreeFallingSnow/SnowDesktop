#include "system_calendar_editor.h"
// Oleacc's property-service GUIDs are declared by the SDK but are not supplied
// by uuid.lib. Instantiate the SDK definitions in this translation unit.
#include <initguid.h>
#include <oleacc.h>
#include "system_panel_model.h"
#include "native_form_style.h"
#include <dcomp.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <exception>
#include <thread>
#include <utility>

namespace snowdesktop
{
namespace
{
constexpr UINT_PTR kInputSubclass=0x43414c49;
constexpr float kInputRadius=6.f;
struct InputRegion
{
    HRGN value=nullptr;
    explicit InputRegion(HRGN region):value(region)
    {if(!value)throw std::runtime_error("native input region unavailable");}
    explicit InputRegion(HWND window):InputRegion(CreateRectRgn(0,0,0,0))
    {if(GetWindowRgn(window,value)==ERROR)throw std::runtime_error("native input window region unavailable");}
    ~InputRegion(){if(value)DeleteObject(value);}
    InputRegion(const InputRegion&)=delete;
    InputRegion& operator=(const InputRegion&)=delete;
};
HRGN CreateInputFrameRegion(LONG width,LONG height,UINT dpi)
{
    // GDI's stroked RoundRect does not have CreateRoundRectRgn's raster
    // footprint, even with matching endpoints. Derive both the fill and pen
    // footprint from the same GDI path used by the native EDIT frame instead.
    struct PathDc
    {
        HDC dc=CreateCompatibleDC(nullptr);HPEN pen=nullptr;HGDIOBJ oldPen=nullptr;
        ~PathDc(){if(oldPen)SelectObject(dc,oldPen);if(pen)DeleteObject(pen);if(dc)DeleteDC(dc);}
    } path;
    const int diameter=2*native_form::detail::Radius(kInputRadius,dpi);
    path.pen=CreatePen(PS_SOLID,(std::max)(1,native_form::Scale(1,dpi)),0);
    if(!path.dc||!path.pen)throw std::runtime_error("native input frame path unavailable");
    path.oldPen=SelectObject(path.dc,path.pen);
    const auto record=[&]{
        if(!BeginPath(path.dc)||!RoundRect(path.dc,0,0,width,height,diameter,diameter)||!EndPath(path.dc))
            throw std::runtime_error("native input frame path recording failed");
    };
    record();InputRegion fill(PathToRegion(path.dc));
    record();if(!WidenPath(path.dc))throw std::runtime_error("native input frame stroke path unavailable");
    InputRegion stroke(PathToRegion(path.dc));
    if(CombineRgn(fill.value,fill.value,stroke.value,RGN_OR)==ERROR)
        throw std::runtime_error("native input frame region unavailable");
    return std::exchange(fill.value,nullptr);
}
std::wstring InputText(HWND window)
{
    std::wstring text(static_cast<std::size_t>((std::max)(0,GetWindowTextLengthW(window)))+1,L'\0');
    const int length=GetWindowTextW(window,text.data(),static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>((std::max)(0,length)));return text;
}
bool SamePalette(const native_form::Palette& a,const native_form::Palette& b)
{
    return a.background==b.background&&a.field==b.field&&a.foreground==b.foreground&&a.secondary==b.secondary&&
        a.border==b.border&&a.accent==b.accent&&a.accentText==b.accentText&&a.highContrast==b.highContrast;
}
bool Finite(const D2D1_RECT_F& rect)
{return std::isfinite(rect.left)&&std::isfinite(rect.top)&&std::isfinite(rect.right)&&std::isfinite(rect.bottom);}
RECT Pixels(const D2D1_RECT_F& rect,float scale,int offset)
{
    return {static_cast<LONG>(std::floor(rect.left*scale)),static_cast<LONG>(std::floor(rect.top*scale))+offset,
        static_cast<LONG>(std::ceil(rect.right*scale)),static_cast<LONG>(std::ceil(rect.bottom*scale))+offset};
}
}
struct SystemCalendarInputs::Impl:std::enable_shared_from_this<Impl>
{
    struct Entry
    {
        std::weak_ptr<Impl> owner;
        HWND window=nullptr;
        SystemCalendarInputField field;
        std::wstring modelText,pendingText;
        bool muting=false,composing=false,pending=false,removing=false;
        UINT suppressChar=0;
        std::uint64_t changes=0;
        RECT placed{},region{};
        bool positioned=false;UINT regionDpi=0;
    };
    HWND parent=nullptr;Change change;Key key;PointerChanged pointerChanged;
    bool alive=true,interactive=true,styled=false;
    UINT dpi=96,nextControl=0x6000;
    float offset=0;
    HFONT font=nullptr;
    native_form::Palette palette;
    std::map<std::string,std::shared_ptr<Entry>> entries;
    Microsoft::WRL::ComPtr<IAccPropServices> accessibility;

    Impl(HWND p,Change changed,Key keyed,PointerChanged pointer):parent(p),change(std::move(changed)),key(std::move(keyed)),pointerChanged(std::move(pointer))
    {CoCreateInstance(CLSID_AccPropServices,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(accessibility.GetAddressOf()));}
    ~Impl(){Dispose();if(font)DeleteObject(font);}
    static bool HandlesKey(const Entry& entry,UINT value,bool ctrl)
    {return value==VK_TAB||value==VK_ESCAPE||(value==VK_RETURN&&(!entry.field.multiline||ctrl));}
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        // HWND owns one shared entry. A callback can synchronously clear the
        // model, all inputs, and their manager; this invocation retains no raw
        // object dependency after calling it.
        auto* holder=reinterpret_cast<std::shared_ptr<Entry>*>(data);const auto entry=*holder;
        if(message==WM_NCDESTROY)
        {
            const auto owner=entry->owner.lock();const auto pointer=owner&&owner->alive?owner->pointerChanged:PointerChanged{};
            RemoveWindowSubclass(window,Procedure,kInputSubclass);entry->window=nullptr;delete holder;
            const auto result=DefSubclassProc(window,message,wp,lp);if(pointer)pointer();return result;
        }
        const auto owner=entry->owner.lock();
        if(!owner||!owner->alive)return DefSubclassProc(window,message,wp,lp);
        if(message==WM_MOUSEWHEEL&&entry->field.multiline)
            return SendMessageW(owner->parent,message,wp,lp);
        const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        // Keep copying forbidden without EDIT's extra password balloon. This
        // applies only to the in-panel protected field, not external dialogs.
        if(entry->field.password&&(message==WM_COPY||message==WM_CUT||
            (message==WM_KEYDOWN&&((ctrl&&(wp=='C'||wp=='X'||wp==VK_INSERT))||(shift&&wp==VK_DELETE)))||
            (message==WM_CHAR&&(wp==3||wp==24))))return 0;
        if(message==WM_IME_STARTCOMPOSITION)entry->composing=true;
        if(message==WM_GETDLGCODE)
        {
            const auto result=DefSubclassProc(window,message,wp,lp);
            const auto* event=reinterpret_cast<const MSG*>(lp);
            if(!entry->composing&&event&&event->message==WM_KEYDOWN&&HandlesKey(*entry,static_cast<UINT>(event->wParam),ctrl))
                return result|DLGC_WANTMESSAGE;
            return result|DLGC_WANTTAB;
        }
        if(message==WM_KEYDOWN&&!entry->composing&&HandlesKey(*entry,static_cast<UINT>(wp),ctrl))
        {
            entry->suppressChar=static_cast<UINT>(wp);const auto callback=owner->key;const auto id=entry->field.id;
            if(callback)callback(id,static_cast<UINT>(wp),shift,ctrl);
            return 0;
        }
        if(message==WM_CHAR&&entry->suppressChar&&
            (entry->suppressChar==static_cast<UINT>(wp)||(entry->suppressChar==VK_RETURN&&wp==L'\n')))
        {entry->suppressChar=0;return 0;}
        if(message==WM_KEYUP&&entry->suppressChar==static_cast<UINT>(wp))entry->suppressChar=0;
        const bool pointerMove=message==WM_MOUSEMOVE||message==WM_NCMOUSEMOVE;
        if(pointerMove&&owner->pointerChanged)
        {
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE|(message==WM_NCMOUSEMOVE?TME_NONCLIENT:0u),window,0};
            TrackMouseEvent(&tracking);
        }
        const auto result=DefSubclassProc(window,message,wp,lp);
        if((pointerMove||message==WM_MOUSELEAVE||message==WM_NCMOUSELEAVE)&&owner->alive&&entry->window)
        {
            // Notify only. EDIT keeps its own selection, capture, cursor and
            // default message processing; the panel coalesces a visual refresh.
            const auto pointer=owner->pointerChanged;if(pointer)pointer();
        }
        if(message==WM_IME_ENDCOMPOSITION&&owner->alive&&entry->window)
        {
            entry->composing=false;
            if(entry->pending)
            {
                auto text=std::move(entry->pendingText);entry->pending=false;
                owner->ExternalText(entry,text);
            }
        }
        return result;
    }
    void Name(const std::shared_ptr<Entry>& entry)
    {
        if(accessibility&&entry->window)
        {
            accessibility->SetHwndPropStr(entry->window,static_cast<DWORD>(OBJID_CLIENT),CHILDID_SELF,PROPID_ACC_NAME,entry->field.label.c_str());
            NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,entry->window,OBJID_CLIENT,CHILDID_SELF);
        }
    }
    void Destroy(const std::shared_ptr<Entry>& entry)
    {
        entry->removing=true;
        if(const auto window=entry->window)
        {
            if(entry->field.password){SetWindowTextW(window,L"");SendMessageW(window,EM_EMPTYUNDOBUFFER,0,0);}
            if(accessibility){const MSAAPROPID properties[]{PROPID_ACC_NAME};accessibility->ClearHwndProps(window,static_cast<DWORD>(OBJID_CLIENT),CHILDID_SELF,properties,1);}
            DestroyWindow(window);
        }
    }
    void Clear()
    {
        auto old=std::move(entries);entries.clear();
        for(const auto& [id,entry]:old){(void)id;Destroy(entry);}
    }
    void Dispose(){if(!alive)return;alive=false;change={};key={};pointerChanged={};Clear();}
    std::shared_ptr<Entry> Find(HWND window) const
    {
        if(!window)return {};
        for(const auto& [id,entry]:entries){(void)id;if(entry->window==window)return entry;}
        return {};
    }
    void ExternalText(const std::shared_ptr<Entry>& entry,const std::wstring& text)
    {
        if(!alive||!entry->window||entry->removing)return;
        const bool different=InputText(entry->window)!=text;
        if(entry->composing){entry->pending=different;entry->pendingText=different?text:std::wstring{};return;}
        if(!different)return;
        DWORD start=0,end=0;SendMessageW(entry->window,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        entry->muting=true;SetWindowTextW(entry->window,text.c_str());
        if(entry->window)
        {
            const auto limit=static_cast<DWORD>((std::min)(text.size(),static_cast<std::size_t>(MAXDWORD)));
            SendMessageW(entry->window,EM_SETSEL,(std::min)(start,limit),(std::min)(end,limit));
        }
        entry->muting=false;
    }
    std::shared_ptr<Entry> Create(const SystemCalendarInputField& field)
    {
        auto entry=std::make_shared<Entry>();entry->owner=shared_from_this();entry->field=field;
        if(field.password)entry->field.text.clear();else entry->modelText=field.text;
        const DWORD style=WS_CHILD|WS_TABSTOP|ES_LEFT|ES_NOHIDESEL|(field.password?ES_PASSWORD:0)|
            (field.multiline?(ES_MULTILINE|ES_WANTRETURN):ES_AUTOHSCROLL);
        auto holder=std::make_unique<std::shared_ptr<Entry>>(entry);
        if(nextControl>=0x7fff)nextControl=0x6000;
        entry->window=CreateWindowExW(0,L"EDIT",L"",style,0,0,1,1,parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextControl++)),GetModuleHandleW(nullptr),nullptr);
        if(!entry->window)throw std::runtime_error("calendar input unavailable");
        try
        {
            SendMessageW(entry->window,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
            native_form::EditStyleFailure failure;
            if(!native_form::AttachEdit(entry->window,palette,dpi,kInputRadius,&failure))
                throw std::runtime_error(std::string("calendar input style unavailable: ")+
                    (failure.operation?failure.operation:"unknown")+"; win32="+std::to_string(failure.error));
            // Observe before the styling subclass consumes nonclient hover or
            // scrollbar messages. EDIT and styling still receive them normally.
            if(!SetWindowSubclass(entry->window,Procedure,kInputSubclass,reinterpret_cast<DWORD_PTR>(holder.get())))
                throw std::runtime_error("calendar input handler unavailable");
            holder.release();
            SendMessageW(entry->window,EM_SETLIMITTEXT,static_cast<WPARAM>((std::max)(0,field.limit)),0);
            entry->muting=true;SetWindowTextW(entry->window,field.password?L"":field.text.c_str());entry->muting=false;Name(entry);
        }
        catch(...){Destroy(entry);throw;}
        return entry;
    }
    void Place(const std::shared_ptr<Entry>& entry)
    {
        if(!alive||!entry->window||entry->removing)return;
        const float scale=static_cast<float>(dpi)/96.f;const int dy=static_cast<int>(std::lround(offset));
        const RECT bounds=Pixels(entry->field.bounds,scale,dy),clip=Pixels(entry->field.clip,scale,dy);
        RECT visible{};const bool shown=IntersectRect(&visible,&bounds,&clip)!=FALSE;
        const bool enabled=interactive&&entry->field.enabled&&shown;
        if((IsWindowEnabled(entry->window)!=FALSE)!=enabled)EnableWindow(entry->window,enabled);
        const bool resized=bounds.right-bounds.left!=entry->placed.right-entry->placed.left||
            bounds.bottom-bounds.top!=entry->placed.bottom-entry->placed.top;
        if(!entry->positioned||!EqualRect(&bounds,&entry->placed))
        {
            SetWindowPos(entry->window,nullptr,bounds.left,bounds.top,(std::max)(1L,bounds.right-bounds.left),(std::max)(1L,bounds.bottom-bounds.top),SWP_NOACTIVATE|SWP_NOZORDER);
            entry->placed=bounds;
        }
        RECT region=visible;OffsetRect(&region,-bounds.left,-bounds.top);
        if(!entry->positioned||resized||entry->regionDpi!=dpi||!EqualRect(&region,&entry->region))
        {
            InputRegion shape(CreateInputFrameRegion(bounds.right-bounds.left,bounds.bottom-bounds.top,dpi));
            InputRegion clipRegion(CreateRectRgn(region.left,region.top,region.right,region.bottom));
            if(CombineRgn(shape.value,shape.value,clipRegion.value,RGN_AND)==ERROR||!SetWindowRgn(entry->window,shape.value,TRUE))
                throw std::runtime_error("native input clipped region unavailable");
            shape.value=nullptr;entry->region=region;entry->regionDpi=dpi;
        }
        entry->positioned=true;
        const bool currentlyShown=(GetWindowLongPtrW(entry->window,GWL_STYLE)&WS_VISIBLE)!=0;
        if(shown!=currentlyShown)ShowWindow(entry->window,shown?SW_SHOWNA:SW_HIDE);
    }
    void Sync(const std::vector<SystemCalendarInputField>& fields,const PersonalizationSettings& appearance,UINT nextDpi)
    {
        if(!alive)return;nextDpi=nextDpi?nextDpi:96;const auto nextPalette=native_form::ResolvePalette(appearance);
        const bool fontChanged=!font||dpi!=nextDpi,styleChanged=!styled||fontChanged||!SamePalette(palette,nextPalette);
        struct RetiredFont{HFONT value=nullptr;~RetiredFont(){if(value)DeleteObject(value);}} oldFont;
        if(fontChanged)
        {
            const auto next=native_form::CreateFormFont(nextDpi);if(!next)throw std::runtime_error("calendar input font unavailable");
            oldFont.value=font;font=next;
        }
        dpi=nextDpi;palette=nextPalette;styled=true;
        // Existing controls stop referring to the old font before it is freed,
        // even if creation of a later field fails.
        if(styleChanged)
        {
            std::vector<std::shared_ptr<Entry>> existing;existing.reserve(entries.size());
            for(const auto& [id,entry]:entries){(void)id;existing.push_back(entry);}
            for(const auto& entry:existing)
            {
                if(!alive)return;if(!entry->window||entry->removing)continue;
                if(fontChanged)SendMessageW(entry->window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
                if(!alive)return;if(entry->window&&!entry->removing)native_form::UpdateEdit(entry->window,palette,dpi);
            }
        }
        if(!alive)return;
        std::set<std::string> wanted;
        for(const auto& field:fields)
        {
            if(field.id.empty()||!Finite(field.bounds)||!Finite(field.clip)||!wanted.insert(field.id).second)continue;
            auto found=entries.find(field.id);
            if(found!=entries.end()&&(found->second->field.multiline!=field.multiline||found->second->field.password!=field.password))
            {const auto previous=found->second;entries.erase(found);Destroy(previous);found=entries.end();}
            std::shared_ptr<Entry> entry;
            if(found==entries.end())
            {
                entry=Create(field);if(!alive){Destroy(entry);return;}
                entries.emplace(field.id,entry);
            }
            else
            {
                entry=found->second;const bool labelChanged=entry->field.label!=field.label;
                const bool textChanged=entry->modelText!=field.text;
                if(entry->field.limit!=field.limit)SendMessageW(entry->window,EM_SETLIMITTEXT,static_cast<WPARAM>((std::max)(0,field.limit)),0);
                entry->field=field;if(field.password){entry->field.text.clear();entry->modelText.clear();}else entry->modelText=field.text;if(labelChanged)Name(entry);
                if(textChanged&&!field.password)ExternalText(entry,field.text);
            }
            if(!alive)return;Place(entry);
        }
        for(auto found=entries.begin();found!=entries.end();)
        {
            if(wanted.contains(found->first)){++found;continue;}
            const auto previous=found->second;found=entries.erase(found);Destroy(previous);
        }
    }
    bool Changed(HWND window)
    {
        const auto entry=Find(window);if(!entry)return false;
        if(!alive||entry->muting||entry->removing||(entry->composing&&entry->pending))return true;
        ++entry->changes;const auto callback=change;const auto id=entry->field.id;
        if(entry->field.password)
        {
            // The only copy outside EDIT is a bounded, explicitly erased bridge
            // buffer. The receiving panel immediately moves it into Secret.
            struct Buffer{wchar_t text[64]{};~Buffer(){SecureZeroMemory(text,sizeof(text));}} buffer;
            GetWindowTextW(window,buffer.text,64);
            // Keep even a short draft off small-string inline move copies.
            std::wstring value;value.reserve(64);value.assign(buffer.text);
            struct Erase{std::wstring& value;~Erase(){if(!value.empty())SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} erase{value};
            if(callback)callback(id,std::move(value));
        }
        else if(callback)callback(id,InputText(window));
        return true;
    }
};
SystemCalendarInputs::SystemCalendarInputs(HWND parent,Change change,Key key,PointerChanged pointer)
    :impl_(std::make_shared<Impl>(parent,std::move(change),std::move(key),std::move(pointer))){}
SystemCalendarInputs::~SystemCalendarInputs(){const auto impl=impl_;impl->Dispose();}
void SystemCalendarInputs::Sync(const std::vector<SystemCalendarInputField>& fields,const PersonalizationSettings& appearance,UINT dpi)
{const auto impl=impl_;impl->Sync(fields,appearance,dpi);}
void SystemCalendarInputs::Pose(float offset,bool interactive)
{
    const auto impl=impl_;if(!impl->alive||!std::isfinite(offset))return;impl->offset=offset;impl->interactive=interactive;
    for(const auto& [id,entry]:impl->entries){(void)id;impl->Place(entry);}
}
bool SystemCalendarInputs::Focus(std::string_view id,bool keyboard)
{
    const auto impl=impl_;const auto found=impl->entries.find(std::string(id));if(!impl->alive||found==impl->entries.end())return false;
    const auto entry=found->second;if(!entry->window||!native_form::detail::Enabled(entry->window)||!(GetWindowLongPtrW(entry->window,GWL_STYLE)&WS_VISIBLE))return false;
    SendMessageW(impl->parent,WM_CHANGEUISTATE,MAKEWPARAM(keyboard?UIS_CLEAR:UIS_SET,UISF_HIDEFOCUS),0);
    SetFocus(entry->window);return entry->window&&GetFocus()==entry->window;
}
bool SystemCalendarInputs::SetValue(std::string_view id,const std::wstring& text)
{
    const auto impl=impl_;const auto found=impl->entries.find(std::string(id));if(!impl->alive||found==impl->entries.end())return false;
    const auto entry=found->second;
    if(!entry->window||!impl->interactive||!entry->field.enabled||entry->field.password||entry->composing||(entry->field.limit>0&&text.size()>static_cast<std::size_t>(entry->field.limit)))return false;
    if(InputText(entry->window)==text)return true;
    const auto revision=entry->changes;if(!SetWindowTextW(entry->window,text.c_str()))return false;
    if(!impl->alive||!entry->window)return true;
    // Standard multiline EDIT does not promise EN_CHANGE for WM_SETTEXT.
    // Deliver the same native command path exactly once in that case.
    if(entry->changes==revision)
        SendMessageW(impl->parent,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(entry->window),EN_CHANGE),reinterpret_cast<LPARAM>(entry->window));
    return true;
}
bool SystemCalendarInputs::Contains(HWND window) const
{const auto impl=impl_;return impl->alive&&static_cast<bool>(impl->Find(window));}
std::string SystemCalendarInputs::FieldId(HWND window) const
{const auto impl=impl_;const auto entry=impl->Find(window);return impl->alive&&entry?entry->field.id:std::string{};}
bool SystemCalendarInputs::HandleCommand(WPARAM wp,LPARAM lp)
{const auto impl=impl_;return HIWORD(wp)==EN_CHANGE&&impl->Changed(reinterpret_cast<HWND>(lp));}
HBRUSH SystemCalendarInputs::ControlColor(HWND window,HDC dc) const
{return Contains(window)?native_form::EditControlColor(window,dc):nullptr;}
void SystemCalendarInputs::Clear(){const auto impl=impl_;impl->Clear();}
void SystemCalendarInputs::Print(HDC dc) const
{
    const auto impl=impl_;if(!impl->alive)return;
    for(const auto& [id,entry]:impl->entries)
    {
        (void)id;if(!entry->window||!(GetWindowLongPtrW(entry->window,GWL_STYLE)&WS_VISIBLE))continue;
        InputRegion region(entry->window);
        const int saved=SaveDC(dc);POINT origin{};GetViewportOrgEx(dc,&origin);const auto& frame=entry->placed;
        OffsetRgn(region.value,origin.x+frame.left,origin.y+frame.top);ExtSelectClipRgn(dc,region.value,RGN_AND);
        SetViewportOrgEx(dc,origin.x+frame.left,origin.y+frame.top,nullptr);
        SendMessageW(entry->window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);RestoreDC(dc,saved);
    }
}
float MeasureSystemCalendarNotesHeight(std::wstring_view text,float width)
{
    const auto dc=GetDC(nullptr);
    if(!dc)return 72.f;
    const auto font=native_form::CreateFormFont(96);
    const auto previous=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
    RECT bounds{0,0,(std::max)(1,static_cast<int>(std::lround(width))-24),0};
    const auto length=static_cast<int>((std::min)(text.size(),static_cast<std::size_t>(8192)));
    DrawTextW(dc,text.data(),length,&bounds,DT_CALCRECT|DT_EDITCONTROL|DT_WORDBREAK|DT_NOPREFIX);
    TEXTMETRICW metrics{};GetTextMetricsW(dc,&metrics);
    SelectObject(dc,previous);if(font)DeleteObject(font);ReleaseDC(nullptr,dc);
    return static_cast<float>((std::max)(72,static_cast<int>(bounds.bottom)+
        (std::max)(static_cast<int>(metrics.tmHeight),16)*2+16));
}

namespace
{
struct PreviewApartment
{
    HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~PreviewApartment(){if(SUCCEEDED(result))CoUninitialize();}
};
struct PreviewParent
{
    HWND window=nullptr;SystemCalendarInputs* inputs=nullptr;int colorRequests=0,wheelRequests=0;
    PreviewParent(int width,int height,DWORD extendedStyle=WS_EX_NOREDIRECTIONBITMAP)
    {
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Procedure;cls.lpszClassName=L"SnowDesktopCalendarInputsPreview";
        if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("calendar input preview class unavailable");
        window=CreateWindowExW(extendedStyle,cls.lpszClassName,L"",WS_POPUP|WS_CLIPCHILDREN,0,0,width,height,nullptr,nullptr,cls.hInstance,this);
        if(!window)throw std::runtime_error("calendar input preview parent unavailable");
    }
    ~PreviewParent(){inputs=nullptr;if(window)DestroyWindow(window);}
    static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<PreviewParent*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<PreviewParent*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self&&self->inputs)
        {
            if(m==WM_MOUSEWHEEL){++self->wheelRequests;return 0;}
            if(m==WM_COMMAND&&self->inputs->HandleCommand(wp,lp))return 0;
            if(m==WM_CTLCOLOREDIT||m==WM_CTLCOLORSTATIC)
                if(const auto brush=self->inputs->ControlColor(reinterpret_cast<HWND>(lp),reinterpret_cast<HDC>(wp)))
                {++self->colorRequests;return reinterpret_cast<LRESULT>(brush);}
        }
        return DefWindowProcW(w,m,wp,lp);
    }
};
struct PreviewBinding
{
    PreviewParent& parent;
    PreviewBinding(PreviewParent& p,SystemCalendarInputs& inputs):parent(p){parent.inputs=&inputs;}
    ~PreviewBinding(){parent.inputs=nullptr;}
};
struct PreviewMutations
{
    HWND window;int count=0;
    explicit PreviewMutations(HWND w):window(w)
    {if(!SetWindowSubclass(window,Procedure,kInputSubclass+1,reinterpret_cast<DWORD_PTR>(this)))throw std::runtime_error("calendar input mutation probe unavailable");}
    ~PreviewMutations(){RemoveWindowSubclass(window,Procedure,kInputSubclass+1);}
    static LRESULT CALLBACK Procedure(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        if(message==WM_WINDOWPOSCHANGING||message==WM_STYLECHANGING||message==WM_SETTEXT||message==WM_SETFONT||message==WM_NCPAINT)
            ++reinterpret_cast<PreviewMutations*>(data)->count;
        return DefSubclassProc(w,message,wp,lp);
    }
};
HWND PreviewInput(const PreviewParent& parent,const SystemCalendarInputs& inputs,std::string_view id)
{for(auto w=GetWindow(parent.window,GW_CHILD);w;w=GetWindow(w,GW_HWNDNEXT))if(inputs.FieldId(w)==id)return w;return nullptr;}
void Require(bool result,const char* message){if(!result)throw std::runtime_error(message);}
void CheckChildRedirectionErrors()
{
    PreviewParent parent(160,80);
    for(const bool reject:{false,true})
    {
        struct Window
        {
            HWND value=nullptr;
            ~Window(){if(value)DestroyWindow(value);}
        } control{CreateWindowExW(0,L"EDIT",L"",WS_CHILD,0,0,120,36,parent.window,nullptr,GetModuleHandleW(nullptr),nullptr)};
        Require(control.value!=nullptr,"calendar redirection fixture unavailable");
        struct StyleMessages
        {
            HWND window;bool reject;
            StyleMessages(HWND w,bool denied):window(w),reject(denied)
            {if(!SetWindowSubclass(window,Procedure,kInputSubclass+3,reinterpret_cast<DWORD_PTR>(this)))throw std::runtime_error("calendar style observer unavailable");}
            ~StyleMessages(){RemoveWindowSubclass(window,Procedure,kInputSubclass+3);}
            static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
            {
                if(m==WM_STYLECHANGING&&static_cast<int>(wp)==GWL_EXSTYLE&&reinterpret_cast<StyleMessages*>(data)->reject)
                    reinterpret_cast<STYLESTRUCT*>(lp)->styleNew&=~static_cast<DWORD>(WS_EX_LAYERED);
                const auto result=DefSubclassProc(w,m,wp,lp);
                if(m==WM_STYLECHANGED)SetLastError(ERROR_INVALID_HANDLE);
                return result;
            }
        } messages(control.value,reject);
        native_form::EditStyleFailure failure;
        const bool applied=native_form::EnsureOpaqueChildRedirection(control.value,&failure);
        if(reject)
            Require(!applied&&failure.operation&&std::string_view(failure.operation)=="SetWindowLongPtr(GWL_EXSTYLE)"&&
                !(GetWindowLongPtrW(control.value,GWL_EXSTYLE)&WS_EX_LAYERED),
                "calendar redirection accepted a rejected layered style change");
        else
        {
            COLORREF key=0;BYTE opacity=0;DWORD flags=0;
            Require(applied&&!failure.operation&&GetLayeredWindowAttributes(control.value,&key,&opacity,&flags)&&
                opacity==255&&flags==LWA_ALPHA,
                "calendar redirection rejected a successful zero-old-style change after a style message set last error");
        }
    }
    native_form::EditStyleFailure invalid;
    Require(!native_form::EnsureOpaqueChildRedirection(nullptr,&invalid)&&invalid.error==ERROR_INVALID_WINDOW_HANDLE,
        "calendar redirection discarded the invalid HWND diagnostic");
}
void CheckRedirectedInputPaint()
{
    // The ordinary hidden-parent WM_PRINT preview cannot expose a missing GDI
    // redirection surface. Exercise normal WM_PAINT on our own non-input
    // desktop instead. It is never switched onto the user's screen.
    std::exception_ptr failure;
    std::thread worker([&]{try{
        struct Desktop
        {
            HDESK original=GetThreadDesktop(GetCurrentThreadId()),isolated=nullptr;
            Desktop()
            {
                const auto name=L"SnowDesktop.CalendarPaint."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId());
                isolated=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
                if(!isolated)throw std::runtime_error("calendar paint isolated desktop unavailable");
                if(!SetThreadDesktop(isolated)){CloseDesktop(isolated);isolated=nullptr;throw std::runtime_error("calendar paint desktop attachment failed");}
            }
            ~Desktop(){SetThreadDesktop(original);if(isolated)CloseDesktop(isolated);}
        } desktop;
        PreviewApartment apartment;
        for(const int theme:{0,1})
        {
            PreviewParent parent(360,240);PersonalizationSettings appearance;appearance.contentTheme=theme;
            SystemCalendarInputs inputs(parent.window,{},{});PreviewBinding binding(parent,inputs);
            const std::vector<SystemCalendarInputField> fields{
                {"calendar.edit.title",L"Title",L"Painted title",{16,16,344,52},{0,0,360,240},false,true,512},
                {"calendar.edit.notes",L"Notes",L"Painted notes\r\nSecond line",{16,70,344,166},{0,0,360,240},true,true,8192}};
            inputs.Sync(fields,appearance,96);
            Microsoft::WRL::ComPtr<IDCompositionDevice> composition;
            Require(SUCCEEDED(DCompositionCreateDevice(nullptr,IID_PPV_ARGS(&composition))),"calendar paint composition unavailable");
            Microsoft::WRL::ComPtr<IDCompositionTarget> target;
            Require(SUCCEEDED(composition->CreateTargetForHwnd(parent.window,FALSE,&target)),"calendar paint target unavailable");
            Microsoft::WRL::ComPtr<IDCompositionVisual> root;
            Require(SUCCEEDED(composition->CreateVisual(&root))&&SUCCEEDED(target->SetRoot(root.Get()))&&
                SUCCEEDED(composition->Commit()),"calendar paint composition tree unavailable");
            ShowWindow(parent.window,SW_SHOWNOACTIVATE);
            const auto palette=native_form::ResolvePalette(appearance);
            for(const auto& field:fields)
            {
                const auto control=PreviewInput(parent,inputs,field.id);
                Require(control!=nullptr,"calendar paint native input missing");
                struct PaintProbe
                {
                    HWND window;unsigned paints=0;
                    explicit PaintProbe(HWND value):window(value)
                    {if(!SetWindowSubclass(window,Procedure,kInputSubclass+2,reinterpret_cast<DWORD_PTR>(this)))throw std::runtime_error("calendar paint observer unavailable");}
                    ~PaintProbe(){RemoveWindowSubclass(window,Procedure,kInputSubclass+2);}
                    static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
                    {
                        if(m==WM_PAINT&&wp==0)++reinterpret_cast<PaintProbe*>(data)->paints;
                        return DefSubclassProc(w,m,wp,lp);
                    }
                } probe(control);
                const auto colors=parent.colorRequests;
                RedrawWindow(control,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_UPDATENOW);
                GdiFlush();
                Require(probe.paints>0&&parent.colorRequests>colors,"native calendar input skipped WM_PAINT or its theme callback");
                struct WindowDc{HWND window;HDC dc;~WindowDc(){if(dc)ReleaseDC(window,dc);}} pixels{control,GetDC(control)};
                Require(pixels.dc!=nullptr,"calendar paint native surface unreadable");
                RECT client{};GetClientRect(control,&client);
                Require(GetPixel(pixels.dc,client.right-12,client.bottom-8)==palette.field,
                    "normal calendar EDIT painting did not retain its theme background");
                RECT textBounds{};SendMessageW(control,EM_GETRECT,0,reinterpret_cast<LPARAM>(&textBounds));
                bool text=false;
                // Inspect only the native text formatting area. A themed
                // border must not be mistaken for successfully painted text.
                for(int y=textBounds.top;y<(std::min)(textBounds.bottom,textBounds.top+30)&&!text;++y)
                    for(int x=textBounds.left;x<(std::min)(textBounds.right,textBounds.left+180);++x)
                    {
                        const auto color=GetPixel(pixels.dc,x,y);
                        if(color!=CLR_INVALID&&color!=palette.field){text=true;break;}
                    }
                Require(text,"normal calendar EDIT painting lost its native text");
            }
        }
    }catch(...){failure=std::current_exception();}});
    worker.join();if(failure)std::rethrow_exception(failure);
}
void CheckRoundedInputFootprint()
{
    PreviewApartment apartment;
    for(const UINT dpi:{96u,120u,144u,192u,288u})for(const int theme:{0,1})
    {
        const float scale=static_cast<float>(dpi)/96.f;
        const int width=static_cast<int>(120*scale),height=static_cast<int>(80*scale);
        PreviewParent parent(width,height);PersonalizationSettings appearance;appearance.contentTheme=theme;
        SystemCalendarInputs inputs(parent.window,{},{});PreviewBinding binding(parent,inputs);
        const std::vector<SystemCalendarInputField> fields{
            {"control.password:footprint",L"Password",L"",{16,16,104,52},{0,0,120,80},false,true,63,true}};
        inputs.Sync(fields,appearance,dpi);const auto control=PreviewInput(parent,inputs,fields.front().id);
        Require(control!=nullptr,"rounded input footprint lost its native child");
        InputRegion region(control);const auto bounds=Pixels(fields.front().bounds,scale,0);
        const LONG fieldWidth=bounds.right-bounds.left,fieldHeight=bounds.bottom-bounds.top;
        Require(!PtInRegion(region.value,0,0)&&!PtInRegion(region.value,fieldWidth-1,0)&&
            !PtInRegion(region.value,0,fieldHeight-1)&&!PtInRegion(region.value,fieldWidth-1,fieldHeight-1),
            "rounded native input still covers its card with rectangular corners");
        struct Dib
        {
            HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;void* pixels=nullptr;
            Dib(LONG w,LONG h)
            {
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
                info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
                bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
                if(dc&&bitmap)old=SelectObject(dc,bitmap);
            }
            ~Dib(){if(old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
        } dib(fieldWidth,fieldHeight);
        Require(dib.dc&&dib.bitmap&&dib.pixels,"native input footprint surface unavailable");
        // WM_PRINT directly into an unclipped DC is the independent native
        // frame oracle. The window region must contain every painted border
        // and field pixel, including the right/bottom physical edge.
        SendMessageW(control,WM_PRINT,reinterpret_cast<WPARAM>(dib.dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);GdiFlush();
        const auto palette=native_form::ResolvePalette(appearance);
        const auto rgb=[](COLORREF c){return(static_cast<std::uint32_t>(GetRValue(c))<<16)|
            (static_cast<std::uint32_t>(GetGValue(c))<<8)|GetBValue(c);};
        const auto* native=static_cast<const std::uint32_t*>(dib.pixels);bool right=false,bottom=false;
        const auto pixelFailure=[&](const char* message,LONG x,LONG y,std::uint32_t color){
            std::ostringstream detail;detail<<message<<"; dpi="<<dpi<<" theme="<<theme<<" size="<<fieldWidth<<'x'<<fieldHeight
                <<" pixel=("<<x<<','<<y<<") argb=0x"<<std::hex<<color;
            throw std::runtime_error(detail.str());
        };
        for(LONG y=0;y<fieldHeight;++y)for(LONG x=0;x<fieldWidth;++x)
        {
            const auto color=native[static_cast<std::size_t>(y)*fieldWidth+x]&0xffffffu;
            if(palette.background!=palette.field&&palette.background!=palette.border&&
                color==rgb(palette.background)&&PtInRegion(region.value,x,y))
                pixelFailure("rounded native input region retained background outside its actual GDI frame",x,y,color);
            if(color!=rgb(palette.field)&&color!=rgb(palette.border))continue;
            if(!PtInRegion(region.value,x,y))pixelFailure("rounded native input region clipped its actual GDI field or border",x,y,color);
            right=right||x==fieldWidth-1;bottom=bottom||y==fieldHeight-1;
        }
        Require(right&&bottom,"native input footprint oracle did not inspect its physical right/bottom edges");
        // Different idle/hover backdrops must survive unchanged outside the
        // actual HWND region. The preview must not invent a second round mask.
        for(const std::uint32_t backdrop:{0x40102030u,0xc0907060u})
        {
            std::vector<std::uint32_t> image(static_cast<std::size_t>(width)*height,backdrop);
            OverlaySystemCalendarInputs(fields,appearance,dpi,width,height,image);
            for(LONG y=0;y<fieldHeight;++y)for(LONG x=0;x<fieldWidth;++x)
            {
                const auto pixel=image[static_cast<std::size_t>(y+bounds.top)*width+x+bounds.left];
                if(PtInRegion(region.value,x,y))
                {
                    if(pixel!=(native[static_cast<std::size_t>(y)*fieldWidth+x]|0xff000000u))
                        pixelFailure("native input preview differs from its real child frame",x,y,pixel);
                }
                else if(pixel!=backdrop)pixelFailure("native input preview overwrote the card/hover behind a clipped corner",x,y,pixel);
            }
        }
    }
}
}
void OverlaySystemCalendarInputs(const std::vector<SystemCalendarInputField>& fields,const PersonalizationSettings& appearance,
    UINT dpi,int width,int height,std::vector<std::uint32_t>& pixels)
{
    if(width<=0||height<=0||pixels.size()!=static_cast<std::size_t>(width)*height)throw std::invalid_argument("calendar input overlay extent");
    PreviewApartment apartment;PreviewParent parent(width,height);SystemCalendarInputs inputs(parent.window,{},{});PreviewBinding binding(parent,inputs);
    inputs.Sync(fields,appearance,dpi);
    struct Dib{HDC dc=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ old=nullptr;~Dib(){if(old)SelectObject(dc,old);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}} dib;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    dib.dc=CreateCompatibleDC(nullptr);void* data=nullptr;dib.bitmap=CreateDIBSection(dib.dc,&info,DIB_RGB_COLORS,&data,nullptr,0);
    if(!dib.dc||!dib.bitmap||!data)throw std::runtime_error("calendar input overlay bitmap unavailable");dib.old=SelectObject(dib.dc,dib.bitmap);
    const auto bytes=pixels.size()*sizeof(std::uint32_t);std::memcpy(data,pixels.data(),bytes);inputs.Print(dib.dc);GdiFlush();
    // EDIT is an opaque child HWND. GDI produces straight RGB, so applying the
    // translucent scene's alpha would brighten it when PNG unpremultiplies.
    // Only replace the real clipped child footprint; the surrounding glass
    // and rounded panel corners keep their original premultiplied pixels.
    const auto* output=static_cast<const std::uint32_t*>(data);
    const RECT canvas{0,0,width,height};
    for(auto window=GetWindow(parent.window,GW_CHILD);window;window=GetWindow(window,GW_HWNDNEXT))
    {
        if(!inputs.Contains(window)||!(GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE))continue;
        InputRegion region(window);RECT bounds{};GetWindowRect(window,&bounds);
        MapWindowPoints(nullptr,parent.window,reinterpret_cast<POINT*>(&bounds),2);
        RECT cropped{};if(!IntersectRect(&cropped,&bounds,&canvas))continue;
        for(LONG y=cropped.top;y<cropped.bottom;++y)for(LONG x=cropped.left;x<cropped.right;++x)
        {
            if(!PtInRegion(region.value,x-bounds.left,y-bounds.top))continue;
            const auto index=static_cast<std::size_t>(y)*width+x;
            pixels[index]=output[index]|0xff000000u;
        }
    }
}
void CheckSystemControlPasswordInput()
{
    PreviewApartment apartment;PreviewParent parent(360,140);PersonalizationSettings appearance;
    system_control::Secret captured;unsigned changes=0,pointerChanges=0;
    SystemCalendarInputs inputs(parent.window,[&](std::string,std::wstring value){
        captured=system_control::Secret(value);if(!value.empty())SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));++changes;
    },{},[&]{++pointerChanges;});PreviewBinding binding(parent,inputs);
    const std::vector<SystemCalendarInputField> fields{
        {"control.password:offline",L"Password",L"",{16,16,344,52},{0,0,360,140},false,true,63,true}};
    inputs.Sync(fields,appearance,96);const auto password=PreviewInput(parent,inputs,fields.front().id);
    Require(password&&GetParent(password)==parent.window&&(GetWindowLongPtrW(password,GWL_STYLE)&ES_PASSWORD)&&
        SendMessageW(password,EM_GETPASSWORDCHAR,0,0)!=0,"embedded password lost its real protected EDIT or created a separate window");
    Microsoft::WRL::ComPtr<IAccessible> accessible;
    Require(SUCCEEDED(AccessibleObjectFromWindow(password,static_cast<DWORD>(OBJID_CLIENT),IID_PPV_ARGS(&accessible)))&&accessible,
        "embedded password has no native accessibility provider");
    VARIANT child{};child.vt=VT_I4;child.lVal=CHILDID_SELF;VARIANT state{};VariantInit(&state);
    const auto status=accessible->get_accState(child,&state);const bool protectedState=SUCCEEDED(status)&&state.vt==VT_I4&&(state.lVal&STATE_SYSTEM_PROTECTED);VariantClear(&state);
    Require(protectedState,"embedded password did not expose native protected accessibility semantics");
    SetWindowTextW(password,L"offline-only-password");
    Require(changes>0&&captured.View()==L"offline-only-password","protected EDIT did not deliver its production input callback");
    inputs.Sync(fields,appearance,96);
    Require(GetWindowTextLengthW(password)==21&&!inputs.SetValue(fields.front().id,L"Synthetic UIA value"),
        "refresh cleared the password or synthetic Value exposed its ordinary textbox path");
    const auto clipboard=GetClipboardSequenceNumber();SendMessageW(password,EM_SETSEL,0,-1);
    SendMessageW(password,WM_COPY,0,0);SendMessageW(password,WM_CUT,0,0);SendMessageW(password,WM_CHAR,3,0);SendMessageW(password,WM_CHAR,24,0);
    Require(GetClipboardSequenceNumber()==clipboard&&GetWindowTextLengthW(password)==21,
        "protected copy/cut changed the clipboard or deleted the in-panel secret");
    BSTR value=nullptr;const auto read=accessible->get_accValue(child,&value);const bool leaked=SUCCEEDED(read)&&value&&std::wstring_view(value)==L"offline-only-password";if(value)SysFreeString(value);
    Require(!leaked,"native protected accessibility exposed a password value");
    // Deliver the actual client and styled nonclient messages to a hidden
    // EDIT. This catches a styling subclass swallowing the hover bridge.
    const auto capture=GetCapture();const auto changesBefore=changes;
    SendMessageW(password,EM_SETSEL,2,5);
    for(const UINT message:{WM_MOUSEMOVE,WM_NCMOUSEMOVE,WM_MOUSELEAVE,WM_NCMOUSELEAVE})
    {
        const auto before=pointerChanges;SendMessageW(password,message,0,0);
        Require(pointerChanges==before+1,"native EDIT client/nonclient pointer change did not reach its panel bridge");
    }
    DWORD start=0,end=0;SendMessageW(password,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
    Require(GetCapture()==capture&&changes==changesBefore&&start==2&&end==5&&GetWindowTextLengthW(password)==21,
        "native EDIT hover bridge changed capture, selection or protected input");
    const auto beforeClose=pointerChanges;
    inputs.Clear();captured.Clear();
    Require(!IsWindow(password)&&!GetWindow(parent.window,GW_CHILD)&&pointerChanges>beforeClose,
        "cancel retained the protected child/undo buffer or omitted the pointer refresh after removal");
}
void CheckSystemCalendarInputs()
{
    CheckChildRedirectionErrors();
    CheckRedirectedInputPaint();
    CheckRoundedInputFootprint();
    {
        PersonalizationSettings glass;glass.contentTheme=0;glass.widgetAlpha=.25f;
        const std::uint32_t untouched=0x40101010u;
        std::vector<std::uint32_t> image(120*80,untouched);
        const std::vector<SystemCalendarInputField> sample{
            {"calendar.edit.title",L"Title",L"",{16,16,100,50},{20,20,96,46},false,true,512}};
        OverlaySystemCalendarInputs(sample,glass,96,120,80,image);
        const auto field=native_form::ResolvePalette(glass).field;
        const auto opaque=0xff000000u|(static_cast<std::uint32_t>(GetRValue(field))<<16)|
            (static_cast<std::uint32_t>(GetGValue(field))<<8)|GetBValue(field);
        Require(image[30*120+60]==opaque,"native input overlay must preserve its opaque theme color over translucent glass");
        Require(image[10*120+10]==untouched&&image[30*120+18]==untouched,
            "native input overlay must preserve glass outside the clipped child footprint");
    }
    PreviewApartment apartment;PreviewParent parent(360,240);PersonalizationSettings appearance;
    std::wstring observed;int changes=0,keys=0;SystemCalendarInputs* current=nullptr;
    SystemCalendarInputs inputs(parent.window,[&](std::string,std::wstring text){++changes;observed=std::move(text);},
        [&](std::string,UINT key,bool,bool){++keys;if(key==VK_ESCAPE)current->Clear();});current=&inputs;PreviewBinding binding(parent,inputs);
    std::vector<SystemCalendarInputField> fields{
        {"calendar.edit.title",L"Title",L"Draft",{16,16,344,50},{0,0,360,240},false,true,512},
        {"calendar.edit.notes",L"Notes",L"Short notes",{16,70,344,166},{0,0,360,240},true,true,8192}};
    inputs.Sync(fields,appearance,96);const auto title=PreviewInput(parent,inputs,fields[0].id),notes=PreviewInput(parent,inputs,fields[1].id);
    Require(title&&notes&&GetParent(title)==parent.window&&(GetWindowLongPtrW(title,GWL_STYLE)&WS_CHILD)&&!(GetWindowLongPtrW(title,GWL_STYLE)&WS_POPUP),"calendar input created a separate top-level window");
    // WM_PRINT can succeed even when a NOREDIRECTIONBITMAP parent has no GDI
    // surface to present. Exercise the native composition-surface contract too.
    // This keeps the ordinary HWND/EDIT rendering path, not a preview bitmap.
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition;
    Require(SUCCEEDED(DCompositionCreateDevice(nullptr,IID_PPV_ARGS(&composition))),"calendar input composition probe unavailable");
    Microsoft::WRL::ComPtr<IDCompositionTarget> target;
    Require(SUCCEEDED(composition->CreateTargetForHwnd(parent.window,FALSE,&target)),"calendar input child-clipping target unavailable");
    for(const auto control:{title,notes})
    {
        COLORREF color=0;BYTE opacity=0;DWORD flags=0;
        Require((GetWindowLongPtrW(control,GWL_EXSTYLE)&WS_EX_LAYERED)&&
                GetLayeredWindowAttributes(control,&color,&opacity,&flags)&&flags==LWA_ALPHA&&opacity==255,
            "native calendar EDIT has no opaque GDI surface under its composition-only parent");
        Microsoft::WRL::ComPtr<IUnknown> redirected;
        Require(SUCCEEDED(composition->CreateSurfaceFromHwnd(control,&redirected))&&redirected,
            "native calendar EDIT cannot provide its actual redirected window surface");
    }
    {
        PreviewParent ordinaryParent(120,80,0);SystemCalendarInputs ordinary(ordinaryParent.window,{},{});PreviewBinding ordinaryBinding(ordinaryParent,ordinary);
        ordinary.Sync({fields.front()},appearance,96);const auto control=PreviewInput(ordinaryParent,ordinary,fields.front().id);
        Require(control&&!(GetWindowLongPtrW(control,GWL_EXSTYLE)&WS_EX_LAYERED),
            "ordinary native forms must keep their existing GDI redirection path");
    }
    {
        PreviewMutations titleChanges(title),notesChanges(notes);inputs.Sync(fields,appearance,96);inputs.Pose(0,true);
        Require(titleChanges.count==0&&notesChanges.count==0,"unchanged calendar refresh reset or repainted native inputs");
    }
    Require(inputs.SetValue(fields[0].id,L"Changed")&&changes==1&&observed==L"Changed","calendar UIA value did not follow native EN_CHANGE");
    fields[0].text=observed;SendMessageW(title,EM_SETSEL,2,5);inputs.Sync(fields,appearance,96);
    DWORD start=0,end=0;SendMessageW(title,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
    Require(PreviewInput(parent,inputs,fields[0].id)==title&&start==2&&end==5,"calendar refresh replaced the active EDIT or selection");
    SendMessageW(title,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"x"));fields[0].text=observed;inputs.Sync(fields,appearance,96);
    Require(SendMessageW(title,EM_CANUNDO,0,0)!=0,"calendar model echo cleared native undo");
    SendMessageW(title,WM_IME_STARTCOMPOSITION,0,0);const auto composing=InputText(title);fields[0].text=L"External update";inputs.Sync(fields,appearance,96);
    SendMessageW(title,WM_KEYDOWN,VK_RETURN,0);Require(InputText(title)==composing&&keys==0,"calendar refresh or Enter interrupted composition");
    SendMessageW(title,WM_IME_ENDCOMPOSITION,0,0);Require(InputText(title)==fields[0].text,"calendar deferred external value was lost");
    Require(!native_form::GetEditScrollInfo(notes).canScroll&&
        !(GetWindowLongPtrW(notes,GWL_STYLE)&(WS_VSCROLL|ES_AUTOVSCROLL)),
        "calendar notes retained their inner scrolling style");
    fields[1].text.clear();for(int i=0;i<30;++i)fields[1].text+=L"A wrapped calendar note line.\r\n";
    fields[1].bounds.bottom=fields[1].bounds.top+MeasureSystemCalendarNotesHeight(fields[1].text,328);
    inputs.Sync(fields,appearance,96);
    const auto before=native_form::GetEditScrollInfo(notes);
    SendMessageW(notes,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(20,100));
    const auto after=native_form::GetEditScrollInfo(notes);
    Require(!before.canScroll&&after.firstLine==before.firstLine&&parent.wheelRequests==1&&
        InputText(notes)==fields[1].text,"calendar notes blocked the panel wheel or clipped their content");
    fields[1].clip={16,90,344,140};inputs.Sync(fields,appearance,96);const auto region=CreateRectRgn(0,0,0,0);GetWindowRgn(notes,region);RECT clip{};GetRgnBox(region,&clip);DeleteObject(region);
    Require(clip.top==20&&clip.bottom==70,"calendar input escaped the model clip");
    inputs.Pose(12,false);Require(!IsWindowEnabled(title)&&!IsWindowEnabled(notes),"calendar animation left native input interactive");
    inputs.Pose(0,true);fields[0].enabled=false;inputs.Sync(fields,appearance,96);Require(!inputs.SetValue(fields[0].id,L"Blocked"),"calendar disabled field accepted UIA value");
    fields[0].enabled=true;inputs.Sync(fields,appearance,96);SendMessageW(title,WM_KEYDOWN,VK_ESCAPE,0);
    Require(keys==1&&!GetWindow(parent.window,GW_CHILD),"calendar key callback could not synchronously clear its inputs");
    std::unique_ptr<SystemCalendarInputs> retiring;
    retiring=std::make_unique<SystemCalendarInputs>(parent.window,SystemCalendarInputs::Change{},
        [&](std::string,UINT,bool,bool){parent.inputs=nullptr;retiring.reset();});parent.inputs=retiring.get();
    retiring->Sync(fields,appearance,96);const auto last=PreviewInput(parent,*retiring,fields[0].id);
    SendMessageW(last,WM_KEYDOWN,VK_ESCAPE,0);
    Require(!retiring&&!GetWindow(parent.window,GW_CHILD),"calendar key callback could not destroy its manager");
}
}
