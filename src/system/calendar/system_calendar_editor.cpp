#include "system_calendar_editor.h"
#include "system/panel/system_panel_model.h"
#include "ui/input/text_input_window.h"
#include "ui/render/native_form_style.h"
#include <commctrl.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT_PTR kInputSubclass=0x43414c49;
RECT Pixels(const D2D1_RECT_F& r,float scale,int dy=0)
{return {static_cast<LONG>(std::floor(r.left*scale)),static_cast<LONG>(std::floor(r.top*scale))+dy,
    static_cast<LONG>(std::ceil(r.right*scale)),static_cast<LONG>(std::ceil(r.bottom*scale))+dy};}
bool Finite(const D2D1_RECT_F& r)
{return std::isfinite(r.left)&&std::isfinite(r.top)&&std::isfinite(r.right)&&std::isfinite(r.bottom);}
std::wstring InputText(HWND window)
{
    const int count=GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>((std::max)(0,count))+1,L'\0');
    value.resize(static_cast<std::size_t>((std::max)(0,GetWindowTextW(window,value.data(),count+1))));return value;
}
text_input::Colors InputColors(const native_form::Palette& p)
{return {p.field,p.foreground,p.border,p.accent,p.accentText,p.secondary};}
}
struct SystemCalendarInputs::Impl:std::enable_shared_from_this<Impl>
{
    struct Entry
    {
        std::weak_ptr<Impl> owner;
        HWND window=nullptr;
        SystemCalendarInputField field;
        std::wstring modelText;
        bool muting=false,removing=false;
        UINT suppressChar=0;
        std::uint64_t changes=0;
    };
    HWND parent;Change change;Key key;PointerChanged pointerChanged;
    UINT dpi=96,nextControl=0x6000;
    HFONT font=nullptr;
    native_form::Palette palette;
    float offset=0;
    bool alive=true,interactive=true;
    std::map<std::string,std::shared_ptr<Entry>> entries;
    Impl(HWND p,Change c,Key k,PointerChanged pointer):parent(p),change(std::move(c)),key(std::move(k)),pointerChanged(std::move(pointer)){}
    ~Impl(){Dispose();if(font)DeleteObject(font);}
    static bool HandlesKey(const Entry& entry,UINT value,bool ctrl)
    {return value==VK_TAB||value==VK_ESCAPE||(value==VK_RETURN&&(!entry.field.multiline||ctrl));}
    static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        auto* holder=reinterpret_cast<std::shared_ptr<Entry>*>(data);const auto entry=*holder;
        const auto owner=entry->owner.lock();
        if(m==WM_NCDESTROY)
        {
            const auto pointer=owner&&owner->alive?owner->pointerChanged:PointerChanged{};
            RemoveWindowSubclass(w,Procedure,kInputSubclass);entry->window=nullptr;delete holder;
            const auto result=DefSubclassProc(w,m,wp,lp);if(pointer)pointer();return result;
        }
        if(!owner||!owner->alive)return DefSubclassProc(w,m,wp,lp);
        if(m==WM_MOUSEWHEEL&&entry->field.multiline)return SendMessageW(owner->parent,m,wp,lp);
        const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(m==WM_GETDLGCODE)
        {
            const auto* event=reinterpret_cast<const MSG*>(lp);
            const auto result=DefSubclassProc(w,m,wp,lp);
            return result|DLGC_WANTTAB|(!text_input::IsComposing(w)&&event&&
                event->message==WM_KEYDOWN&&HandlesKey(*entry,static_cast<UINT>(event->wParam),ctrl)?DLGC_WANTMESSAGE:0);
        }
        if(m==WM_KEYDOWN&&!text_input::IsComposing(w)&&HandlesKey(*entry,static_cast<UINT>(wp),ctrl))
        {
            entry->suppressChar=static_cast<UINT>(wp);const auto callback=owner->key;const auto id=entry->field.id;
            if(callback)callback(id,static_cast<UINT>(wp),shift,ctrl);return 0;
        }
        if(m==WM_CHAR&&entry->suppressChar&&
            (entry->suppressChar==wp||(entry->suppressChar==VK_RETURN&&wp==L'\n')))
        {entry->suppressChar=0;return 0;}
        if(m==WM_KEYUP&&entry->suppressChar==wp)entry->suppressChar=0;
        const auto result=DefSubclassProc(w,m,wp,lp);
        if(owner->alive&&entry->window&&(m==WM_MOUSEMOVE||m==WM_MOUSELEAVE||
            m==WM_NCMOUSEMOVE||m==WM_NCMOUSELEAVE))
        {const auto callback=owner->pointerChanged;if(callback)callback();}
        return result;
    }
    std::shared_ptr<Entry> Find(HWND w) const
    {for(const auto& [id,entry]:entries){(void)id;if(entry->window==w)return entry;}return {};}
    void Destroy(const std::shared_ptr<Entry>& entry)
    {entry->removing=true;if(entry->window)DestroyWindow(entry->window);}
    void Clear()
    {auto old=std::move(entries);entries.clear();for(const auto& [id,entry]:old){(void)id;Destroy(entry);}}
    void Dispose(){if(!alive)return;alive=false;change={};key={};pointerChanged={};Clear();}
    void Place(const std::shared_ptr<Entry>& entry)
    {
        if(!entry->window||entry->removing)return;
        const float scale=static_cast<float>(dpi)/96.f;const int dy=static_cast<int>(std::lround(offset));
        const auto bounds=Pixels(entry->field.bounds,scale,dy),clip=Pixels(entry->field.clip,scale,dy);
        RECT visible{};const bool shown=IntersectRect(&visible,&bounds,&clip)!=FALSE;
        text_input::SetEmbeddedPose(entry->window,bounds,clip,shown,interactive&&entry->field.enabled&&shown);
    }
    std::shared_ptr<Entry> Create(const SystemCalendarInputField& field)
    {
        auto entry=std::make_shared<Entry>();entry->owner=shared_from_this();entry->field=field;
        if(field.password)entry->field.text.clear();else entry->modelText=field.text;
        const DWORD style=WS_CHILD|WS_TABSTOP|ES_LEFT|ES_NOHIDESEL|(field.password?ES_PASSWORD:0)|
            (field.multiline?ES_MULTILINE|ES_WANTRETURN:ES_AUTOHSCROLL);
        if(nextControl>=0x7fff)nextControl=0x6000;
        entry->window=CreateWindowExW(0,text_input::WindowClass(),L"",style,-32000,-32000,1,1,parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(nextControl++)),GetModuleHandleW(nullptr),nullptr);
        if(!entry->window)throw std::runtime_error("calendar input host unavailable");
        try
        {
            auto holder=std::make_unique<std::shared_ptr<Entry>>(entry);
            if(!SetWindowSubclass(entry->window,Procedure,kInputSubclass,reinterpret_cast<DWORD_PTR>(holder.get())))
                throw std::runtime_error("calendar input handler unavailable");
            holder.release();
            SendMessageW(entry->window,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
            text_input::SetPadding(entry->window,12.f*static_cast<float>(dpi)/96.f,4.f*static_cast<float>(dpi)/96.f);
            text_input::SetColors(entry->window,InputColors(palette),6.f*static_cast<float>(dpi)/96.f);
            SendMessageW(entry->window,EM_SETLIMITTEXT,static_cast<WPARAM>((std::max)(0,field.limit)),0);
            entry->muting=true;SetWindowTextW(entry->window,field.password?L"":field.text.c_str());entry->muting=false;
            text_input::SetAccessibleName(entry->window,field.label);
        }
        catch(...){Destroy(entry);throw;}
        return entry;
    }
    void Sync(const std::vector<SystemCalendarInputField>& fields,const PersonalizationSettings& appearance,UINT nextDpi)
    {
        if(!alive)return;
        nextDpi=nextDpi?nextDpi:96;
        const auto nextPalette=native_form::ResolvePalette(appearance);
        LOGFONTW previousFont{};if(font)GetObjectW(font,sizeof(previousFont),&previousFont);
        const bool fontChanged=!font||dpi!=nextDpi||app_fonts::GdiFamily()!=previousFont.lfFaceName;
        const bool colorsChanged=fontChanged||palette.field!=nextPalette.field||palette.foreground!=nextPalette.foreground||
            palette.border!=nextPalette.border||palette.accent!=nextPalette.accent||palette.secondary!=nextPalette.secondary;
        HFONT old=nullptr;
        if(fontChanged){const auto next=native_form::CreateFormFont(nextDpi);if(!next)throw std::runtime_error("calendar input font unavailable");old=font;font=next;}
        dpi=nextDpi;palette=nextPalette;
        for(const auto& [id,entry]:entries)
        {
            (void)id;
            if(fontChanged)SendMessageW(entry->window,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
            if(fontChanged)text_input::SetPadding(entry->window,12.f*static_cast<float>(dpi)/96.f,4.f*static_cast<float>(dpi)/96.f);
            if(colorsChanged)text_input::SetColors(entry->window,InputColors(palette),6.f*static_cast<float>(dpi)/96.f);
        }
        if(old)DeleteObject(old);
        std::set<std::string> wanted;
        for(const auto& field:fields)
        {
            if(field.id.empty()||!Finite(field.bounds)||!Finite(field.clip)||!wanted.insert(field.id).second)continue;
            auto found=entries.find(field.id);
            if(found!=entries.end()&&(found->second->field.multiline!=field.multiline||found->second->field.password!=field.password))
            {const auto previous=found->second;entries.erase(found);Destroy(previous);found=entries.end();}
            std::shared_ptr<Entry> entry;
            if(found==entries.end()){entry=Create(field);if(!alive){Destroy(entry);return;}entries.emplace(field.id,entry);}
            else
            {
                entry=found->second;const bool changed=entry->modelText!=field.text;
                if(entry->field.limit!=field.limit)SendMessageW(entry->window,EM_SETLIMITTEXT,static_cast<WPARAM>((std::max)(0,field.limit)),0);
                entry->field=field;
                text_input::SetAccessibleName(entry->window,field.label);
                if(field.password){entry->field.text.clear();entry->modelText.clear();}
                else
                {
                    entry->modelText=field.text;
                    if(changed){entry->muting=true;SetWindowTextW(entry->window,field.text.c_str());entry->muting=false;}
                }
            }
            if(!alive)return;Place(entry);
        }
        for(auto found=entries.begin();found!=entries.end();)
        {if(wanted.contains(found->first)){++found;continue;}const auto previous=found->second;found=entries.erase(found);Destroy(previous);}
    }
    bool Changed(HWND w)
    {
        const auto entry=Find(w);if(!entry)return false;
        if(!alive||entry->muting||entry->removing)return true;
        ++entry->changes;const auto callback=change;const auto id=entry->field.id;
        if(entry->field.password)
        {
            struct Buffer{wchar_t value[64]{};~Buffer(){SecureZeroMemory(value,sizeof(value));}} buffer;
            text_input::CopySecret(w,buffer.value,64);
            std::wstring value;value.reserve(64);value.assign(buffer.value);
            struct Erase{std::wstring& value;~Erase(){if(!value.empty())SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}} erase{value};
            if(callback)callback(id,std::move(value));
        }
        else{auto value=InputText(w);entry->modelText=value;if(callback)callback(id,std::move(value));}
        return true;
    }
    void Draw(ID2D1RenderTarget* target) const
    {
        if(!alive||!target)return;
        const float scale=static_cast<float>(dpi)/96.f;
        for(const auto& [id,entry]:entries)
        {
            (void)id;if(!entry->window||!(GetWindowLongPtrW(entry->window,GWL_STYLE)&WS_VISIBLE))continue;
            target->PushAxisAlignedClip(entry->field.clip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            text_input::Draw(entry->window,target,entry->field.bounds,scale);
            target->PopAxisAlignedClip();
        }
    }
};
SystemCalendarInputs::SystemCalendarInputs(HWND p,Change c,Key k,PointerChanged pointer)
    :impl_(std::make_shared<Impl>(p,std::move(c),std::move(k),std::move(pointer))){}
SystemCalendarInputs::~SystemCalendarInputs(){impl_->Dispose();}
void SystemCalendarInputs::Sync(const std::vector<SystemCalendarInputField>& f,const PersonalizationSettings& a,UINT dpi)
{const auto impl=impl_;impl->Sync(f,a,dpi);}
void SystemCalendarInputs::Pose(float offset,bool interactive)
{
    const auto impl=impl_;if(!impl->alive||!std::isfinite(offset))return;
    impl->offset=offset;impl->interactive=interactive;
    for(const auto& [id,entry]:impl->entries){(void)id;impl->Place(entry);}
}
bool SystemCalendarInputs::Focus(std::string_view id,bool keyboard)
{
    const auto impl=impl_;const auto found=impl->entries.find(std::string(id));
    if(!impl->alive||found==impl->entries.end())return false;const auto entry=found->second;
    if(!entry->window||!IsWindowEnabled(entry->window)||!(GetWindowLongPtrW(entry->window,GWL_STYLE)&WS_VISIBLE))return false;
    SendMessageW(impl->parent,WM_CHANGEUISTATE,MAKEWPARAM(keyboard?UIS_CLEAR:UIS_SET,UISF_HIDEFOCUS),0);
    SetFocus(entry->window);return entry->window&&GetFocus()==entry->window;
}
bool SystemCalendarInputs::SetValue(std::string_view id,const std::wstring& value)
{
    const auto impl=impl_;const auto found=impl->entries.find(std::string(id));
    if(!impl->alive||found==impl->entries.end())return false;const auto entry=found->second;
    if(!entry->window||!impl->interactive||!entry->field.enabled||entry->field.password||text_input::IsComposing(entry->window)||
        (entry->field.limit>0&&value.size()>static_cast<std::size_t>(entry->field.limit)))return false;
    return SetWindowTextW(entry->window,value.c_str())!=FALSE;
}
bool SystemCalendarInputs::Contains(HWND w) const {return impl_->alive&&static_cast<bool>(impl_->Find(w));}
std::string SystemCalendarInputs::FieldId(HWND w) const
{const auto impl=impl_;const auto entry=impl->Find(w);return impl->alive&&entry?entry->field.id:std::string{};}
bool SystemCalendarInputs::HandleCommand(WPARAM wp,LPARAM lp)
{const auto impl=impl_;return HIWORD(wp)==EN_CHANGE&&impl->Changed(reinterpret_cast<HWND>(lp));}
HBRUSH SystemCalendarInputs::ControlColor(HWND,HDC) const{return nullptr;}
void SystemCalendarInputs::Clear(){const auto impl=impl_;impl->Clear();}
void SystemCalendarInputs::Draw(ID2D1RenderTarget* target) const{const auto impl=impl_;impl->Draw(target);}
std::shared_ptr<text_input::TextAccess> SystemCalendarInputs::Accessibility(std::string_view id) const
{
    const auto impl=impl_;const auto found=impl->entries.find(std::string(id));
    return impl->alive&&found!=impl->entries.end()?text_input::Accessibility(found->second->window):std::shared_ptr<text_input::TextAccess>{};
}
bool SystemCalendarInputs::HandlePointer(UINT m,WPARAM wp,POINT p)
{
    const auto impl=impl_;if(!impl->alive)return false;
    for(const auto& [id,entry]:impl->entries)
    {(void)id;if(text_input::RoutePointer(entry->window,m,wp,p))return true;}
    return false;
}
void SystemCalendarInputs::Print(HDC dc) const
{
    if(!dc)return;const auto impl=impl_;RECT rect{};GetClientRect(impl->parent,&rect);
    ComPtr<ID2D1Factory> factory;ComPtr<ID2D1DCRenderTarget> target;
    if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf())))return;
    const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    if(FAILED(factory->CreateDCRenderTarget(&props,&target))||FAILED(target->BindDC(dc,&rect)))return;
    target->BeginDraw();const float scale=static_cast<float>(impl->dpi)/96.f;
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));impl->Draw(target.Get());target->EndDraw();
}
float MeasureSystemCalendarNotesHeight(std::wstring_view value,float width)
{
    ComPtr<IDWriteFactory> factory;ComPtr<IDWriteTextFormat> format;ComPtr<IDWriteTextLayout> layout;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))return 72.f;
    const auto font=native_form::CreateFormFont(96);LOGFONTW info{};if(font){GetObjectW(font,sizeof(info),&info);DeleteObject(font);}
    if(FAILED(factory->CreateTextFormat(info.lfFaceName[0]?info.lfFaceName:L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,static_cast<float>((std::max)(12L,std::abs(info.lfHeight))),L"",&format)))return 72.f;
    if(FAILED(factory->CreateTextLayout(value.data(),static_cast<UINT32>(value.size()),format.Get(),(std::max)(1.f,width-24.f),100000.f,&layout)))return 72.f;
    DWRITE_TEXT_METRICS metrics{};layout->GetMetrics(&metrics);return (std::max)(72.f,metrics.height+40.f);
}
namespace
{
struct Apartment{HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);~Apartment(){if(SUCCEEDED(result))CoUninitialize();}};
struct PreviewParent
{
    HWND window=nullptr;SystemCalendarInputs* inputs=nullptr;int wheels=0;
    PreviewParent(int width,int height)
    {
        WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Procedure;cls.lpszClassName=L"SnowDesktop.DrawnInputsPreview";
        RegisterClassW(&cls);window=CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP,cls.lpszClassName,L"",WS_POPUP,0,0,width,height,nullptr,nullptr,cls.hInstance,this);
        if(!window)throw std::runtime_error("drawn input preview unavailable");
    }
    ~PreviewParent(){inputs=nullptr;if(window)DestroyWindow(window);}
    static LRESULT CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<PreviewParent*>(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<PreviewParent*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(self&&self->inputs)
        {if(m==WM_COMMAND&&self->inputs->HandleCommand(wp,lp))return 0;if(m==WM_MOUSEWHEEL){++self->wheels;return 0;}}
        return DefWindowProcW(w,m,wp,lp);
    }
};
HWND FindInput(const PreviewParent& parent,const SystemCalendarInputs& inputs,std::string_view id)
{for(auto w=GetWindow(parent.window,GW_CHILD);w;w=GetWindow(w,GW_HWNDNEXT))if(inputs.FieldId(w)==id)return w;return nullptr;}
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
}
void OverlaySystemCalendarInputs(const std::vector<SystemCalendarInputField>& fields,const PersonalizationSettings& appearance,
    UINT dpi,int width,int height,std::vector<std::uint32_t>& pixels)
{
    if(width<=0||height<=0||pixels.size()!=static_cast<std::size_t>(width)*height)throw std::invalid_argument("calendar input overlay extent");
    Apartment apartment;PreviewParent parent(width,height);SystemCalendarInputs inputs(parent.window,{},{});parent.inputs=&inputs;
    inputs.Sync(fields,appearance,dpi);ComPtr<IWICImagingFactory> imaging;ComPtr<IWICBitmap> bitmap;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging))),"input preview imaging unavailable");
    Require(SUCCEEDED(imaging->CreateBitmapFromMemory(static_cast<UINT>(width),static_cast<UINT>(height),GUID_WICPixelFormat32bppPBGRA,
        static_cast<UINT>(width*4),static_cast<UINT>(pixels.size()*4),reinterpret_cast<BYTE*>(pixels.data()),&bitmap)),"input preview bitmap unavailable");
    ComPtr<ID2D1Factory> factory;ComPtr<ID2D1RenderTarget> target;
    Require(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf())),"input preview drawing unavailable");
    const auto props=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    Require(SUCCEEDED(factory->CreateWicBitmapRenderTarget(bitmap.Get(),props,&target)),"input preview surface unavailable");
    const float scale=static_cast<float>(dpi)/96.f;target->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale));
    target->BeginDraw();inputs.Draw(target.Get());Require(SUCCEEDED(target->EndDraw()),"input preview draw failed");target.Reset();
    Require(SUCCEEDED(bitmap->CopyPixels(nullptr,static_cast<UINT>(width*4),static_cast<UINT>(pixels.size()*4),reinterpret_cast<BYTE*>(pixels.data()))),"input preview read failed");
}
void CheckSystemCalendarInputs()
{
    Apartment apartment;PreviewParent parent(360,240);PersonalizationSettings appearance;
    std::wstring observed;int changes=0,keys=0;SystemCalendarInputs* current=nullptr;
    SystemCalendarInputs inputs(parent.window,[&](std::string,std::wstring value){++changes;observed=std::move(value);},
        [&](std::string,UINT key,bool,bool){++keys;if(key==VK_ESCAPE)current->Clear();});
    current=&inputs;parent.inputs=&inputs;
    std::vector<SystemCalendarInputField> fields{
        {"calendar.edit.title",L"Title",L"Draft",{16,16,344,50},{0,0,360,240},false,true,512},
        {"calendar.edit.notes",L"Notes",L"Short notes",{16,70,344,166},{0,0,360,240},true,true,8192}};
    inputs.Sync(fields,appearance,96);const auto title=FindInput(parent,inputs,fields[0].id),notes=FindInput(parent,inputs,fields[1].id);
    wchar_t name[64]{};GetClassNameW(title,name,64);
    Require(title&&notes&&lstrcmpW(name,text_input::WindowClass())==0,"calendar did not use the shared drawn input");
    RECT host{};GetWindowRect(title,&host);MapWindowPoints(nullptr,parent.window,reinterpret_cast<POINT*>(&host),2);
    Require(host.left==-32000&&host.right-host.left==1&&!(GetWindowLongPtrW(title,GWL_EXSTYLE)&WS_EX_LAYERED),
        "calendar retained an independent visible input surface");
    Require(inputs.SetValue(fields[0].id,L"Changed")&&changes==1&&observed==L"Changed","calendar value did not deliver exactly one production callback");
    fields[0].text=observed;SendMessageW(title,EM_SETSEL,2,5);inputs.Sync(fields,appearance,96);
    DWORD start=0,end=0;SendMessageW(title,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
    Require(start==2&&end==5,"calendar echo changed selection");
    SendMessageW(title,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"x"));fields[0].text=observed;inputs.Sync(fields,appearance,96);
    Require(SendMessageW(title,EM_CANUNDO,0,0)!=0,"calendar echo cleared undo");
    SendMessageW(title,WM_UNDO,0,0);Require(InputText(title)==L"Changed","calendar undo did not restore the edited range");
    SendMessageW(title,WM_IME_STARTCOMPOSITION,0,0);const auto composing=InputText(title);
    fields[0].text=L"External update";inputs.Sync(fields,appearance,96);
    SendMessageW(title,WM_KEYDOWN,VK_RETURN,0);Require(InputText(title)==composing&&keys==0,"calendar Enter or external write interrupted composition");
    SendMessageW(title,WM_IME_ENDCOMPOSITION,0,0);Require(InputText(title)==fields[0].text,"calendar lost its deferred external value");
    SendMessageW(notes,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),0);Require(parent.wheels==1,"notes did not route wheel to the panel");
    inputs.Pose(12,false);Require(!IsWindowEnabled(title)&&!IsWindowEnabled(notes),"animation retained active input");
    inputs.Pose(0,true);fields[0].enabled=false;inputs.Sync(fields,appearance,96);Require(!inputs.SetValue(fields[0].id,L"blocked"),"disabled input accepted value");
    fields[0].enabled=true;inputs.Sync(fields,appearance,96);SendMessageW(title,WM_KEYDOWN,VK_ESCAPE,0);
    Require(keys==1&&!GetWindow(parent.window,GW_CHILD),"calendar callback could not synchronously retire inputs");
    for(const UINT dpi:{96u,144u,192u})for(const int theme:{0,1})
    {
        PersonalizationSettings appearanceSample;appearanceSample.contentTheme=theme;
        const int width=static_cast<int>(120*dpi/96),height=static_cast<int>(80*dpi/96);
        const std::uint32_t untouched=0x40101010u;std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width)*height,untouched);
        const std::vector<SystemCalendarInputField> sample{{"title",L"Title",L"",{16,16,100,50},{20,20,96,46},false,true,512}};
        OverlaySystemCalendarInputs(sample,appearanceSample,dpi,width,height,pixels);
        Require(pixels[static_cast<std::size_t>(height/2)*width+width/2]!=untouched&&pixels[0]==untouched,
            "drawn input lost its field or overwrote the surrounding surface");
        // Exercise the real embedded calendar paint path with the reported
        // title, scaled form font and field height rather than an empty proxy.
        std::vector<std::uint32_t> titlePixels(static_cast<std::size_t>(width)*height,untouched);
        const std::vector<SystemCalendarInputField> titleSample{
            {"title",L"Title",L"测试",{16,16,100,52},{0,0,120,80},false,true,512}}; // l10n-allow: user-reported text in an isolated rendering regression, never application copy
        OverlaySystemCalendarInputs(titleSample,appearanceSample,dpi,width,height,titlePixels);
        const int top=static_cast<int>(16*dpi/96),bottom=static_cast<int>(52*dpi/96);
        int first=bottom,last=-1;
        for(int y=top+4;y<bottom-4;++y)
            for(int x=static_cast<int>(28*dpi/96);x<static_cast<int>(88*dpi/96);++x)
            {
                const auto pixel=titlePixels[static_cast<std::size_t>(y)*width+x];
                const auto r=(pixel>>16)&255,g=(pixel>>8)&255,b=pixel&255;
                if(theme==0?(r>180&&g>180&&b>180):(r<80&&g<80&&b<80))
                {first=(std::min)(first,y);last=(std::max)(last,y);}
            }
        Require(last>=first&&std::abs(first+last-top-bottom+1)<=2,
            "actual embedded calendar title glyphs are not vertically centered");
    }
}
void CheckSystemControlPasswordInput()
{
    Apartment apartment;PreviewParent parent(360,140);PersonalizationSettings appearance;
    system_control::Secret captured;unsigned changes=0;
    SystemCalendarInputs inputs(parent.window,[&](std::string,std::wstring value){
        captured=system_control::Secret(value);if(!value.empty())SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));++changes;
    },{});parent.inputs=&inputs;
    const std::vector<SystemCalendarInputField> fields{{"password",L"Password",L"",{16,16,344,52},{0,0,360,140},false,true,63,true}};
    inputs.Sync(fields,appearance,96);const auto password=FindInput(parent,inputs,"password");
    SetWindowTextW(password,L"offline-only-password");
    Require(changes==1&&captured.View()==L"offline-only-password","password failed its protected production callback");
    inputs.Sync(fields,appearance,96);
    Require(GetWindowTextLengthW(password)==21&&InputText(password).empty()&&!inputs.SetValue("password",L"value"),
        "password refresh cleared the secret or exposed its ordinary text path");
    const auto clipboard=GetClipboardSequenceNumber();SendMessageW(password,EM_SETSEL,0,-1);
    SendMessageW(password,WM_COPY,0,0);SendMessageW(password,WM_CUT,0,0);SendMessageW(password,WM_UNDO,0,0);
    Require(GetClipboardSequenceNumber()==clipboard&&GetWindowTextLengthW(password)==21&&!SendMessageW(password,EM_CANUNDO,0,0),
        "password entered clipboard or ordinary undo history");
    inputs.Clear();captured.Clear();Require(!IsWindow(password),"password cancel retained its host");
}
}
