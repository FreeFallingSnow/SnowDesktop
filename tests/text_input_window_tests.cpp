#include "ui/input/text_input_window.h"
#include "ui/render/app_font.h"
#include <commctrl.h>
#include <wrl/client.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>
#include <iostream>
#include <string>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

using Microsoft::WRL::ComPtr;
namespace input = snowdesktop::text_input;
namespace
{
int failures=0,changes=0,ownerPaints=0;
bool destroyOnChange=false;
void Check(bool value,const char* message)
{if(!value){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
LRESULT CALLBACK Owner(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
    if(message==WM_PAINT)++ownerPaints;
    if(message==WM_COMMAND&&HIWORD(wp)==EN_CHANGE)
    {++changes;if(destroyOnChange)DestroyWindow(reinterpret_cast<HWND>(lp));return 0;}
    return DefWindowProcW(window,message,wp,lp);
}
std::wstring Value(HWND window)
{const int count=GetWindowTextLengthW(window);std::wstring value(static_cast<std::size_t>(count)+1,L'\0');value.resize(static_cast<std::size_t>(GetWindowTextW(window,value.data(),count+1)));return value;}
HWND Create(HWND parent,DWORD extra=0,const wchar_t* initial=L"alpha")
{return CreateWindowExW(0,input::WindowClass(),initial,WS_CHILD|WS_VISIBLE|extra,0,0,120,60,parent,reinterpret_cast<HMENU>(101),GetModuleHandleW(nullptr),nullptr);}
void Selection(HWND window,DWORD expectedStart,DWORD expectedEnd,const char* message)
{DWORD start=0,end=0;SendMessageW(window,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));Check(start==expectedStart&&end==expectedEnd,message);}
void Ordinary(HWND owner)
{
    const auto window=Create(owner);Check(window!=nullptr,"create the production custom window");if(!window)return;
    SendMessageW(window,EM_SETSEL,1,4);
    const int before=changes;SendMessageW(window,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"中文"));
    Check(Value(window)==L"a中文a"&&changes==before+1,"selection replacement notifies exactly once");
    SetWindowTextW(window,L"a中文a");Selection(window,3,3,"model echo preserves the caret");
    Check(SendMessageW(window,EM_CANUNDO,0,0)!=0,"model echo preserves history");
    SendMessageW(window,WM_UNDO,0,0);Check(Value(window)==L"alpha","undo restores the original value");Selection(window,1,4,"undo restores the original selected range");
    SendMessageW(window,EM_SETSEL,2,2);SendMessageW(window,WM_KEYDOWN,VK_BACK,0);
    SendMessageW(window,WM_UNDO,0,0);Selection(window,2,2,"undo of implicit deletion restores the prior caret");
    SetWindowTextW(window,L"a\U0001F469\x200d\U0001F4BB\x65\x301z");
    SendMessageW(window,EM_SETSEL,6,6);SendMessageW(window,WM_KEYDOWN,VK_BACK,0);
    Check(Value(window)==L"ae\x301z","backspace deletes a complete emoji sequence");
    SendMessageW(window,EM_SETREADONLY,TRUE,0);const auto readonly=Value(window);
    SendMessageW(window,WM_CHAR,L'x',0);SendMessageW(window,WM_KEYDOWN,VK_BACK,0);SendMessageW(window,WM_IME_STARTCOMPOSITION,0,0);
    Check(Value(window)==readonly&&!input::IsComposing(window),"readonly refuses mutation and composition");SendMessageW(window,EM_SETREADONLY,FALSE,0);
    SetWindowTextW(window,L"draft");SendMessageW(window,WM_IME_STARTCOMPOSITION,0,0);
    SetWindowTextW(window,L"model");SendMessageW(window,EM_SETSEL,1,3);Check(Value(window)==L"draft","model change waits for composition end");
    const int deferred=changes;SendMessageW(window,WM_IME_ENDCOMPOSITION,0,0);
    Check(Value(window)==L"model"&&changes==deferred,"deferred model change does not emit user change");Selection(window,1,3,"external selection waits with the model");
    SendMessageW(window,EM_SETLIMITTEXT,5,0);SendMessageW(window,EM_SETSEL,5,5);SendMessageW(window,WM_CHAR,L'x',0);Check(Value(window)==L"model","limit rejects oversized edits");
    DestroyWindow(window);
}
void StableEmbeddedPresentation()
{
    // Hidden owners discard their update region even when InvalidateRect succeeds.
    // Keep this isolated window shown offscreen without activating it so the
    // unchanged/changed setter checks can actually observe paint requests.
    const auto owner=CreateWindowExW(WS_EX_NOACTIVATE,L"SnowDesktop.TextInputTestOwner",L"",WS_OVERLAPPED,
        -32000,-32000,400,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(owner!=nullptr,"create isolated offscreen presentation owner");if(!owner)return;
    ShowWindow(owner,SW_SHOWNOACTIVATE);
    InvalidateRect(owner,nullptr,FALSE);
    Check(GetUpdateRect(owner,nullptr,FALSE)!=FALSE,"presentation fixture exposes a requested parent paint");
    const auto window=Create(owner,ES_AUTOHSCROLL,L"search text");
    Check(window!=nullptr,"create embedded presentation regression");if(!window){DestroyWindow(owner);return;}
    input::Colors colors;const RECT frame{20,30,340,72};
    input::SetEmbeddedPose(window,frame,frame,true,true);
    input::SetColors(window,colors,0.f);input::SetPadding(window,4.f,0.f);input::SetCaretHeight(window,19.f);
    SendMessageW(window,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search applications"));
    SendMessageW(window,EM_SETSEL,1,4);
    ValidateRect(owner,nullptr);
    const int stablePaints=ownerPaints;
    for(int i=0;i<100;++i)
    {
        input::SetEmbeddedPose(window,frame,frame,true,true);input::SetColors(window,colors,0.f);
        input::SetPadding(window,4.f,0.f);input::SetCaretHeight(window,19.f);
        SendMessageW(window,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search applications"));
    }
    Check(!GetUpdateRect(owner,nullptr,FALSE)&&ownerPaints==stablePaints,"unchanged presentation never schedules or dispatches another parent paint");
    Check(Value(window)==L"search text","presentation updates preserve committed text");
    Selection(window,1,4,"presentation updates preserve the selected range");
    // UIA notifications may dispatch WM_PAINT synchronously. Observe both a
    // pending region and completed paints instead of requiring it to stay dirty.
    const auto repaint=[&](auto update,const char* message){
        const int before=ownerPaints;update();
        Check(GetUpdateRect(owner,nullptr,FALSE)!=FALSE||ownerPaints>before,message);
        ValidateRect(owner,nullptr);
    };
    auto changed=colors;changed.foreground=RGB(40,45,50);
    repaint([&]{input::SetColors(window,changed,0.f);},"changed colors repaint the embedded owner");
    repaint([&]{SendMessageW(window,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search commands"));},"changed placeholder repaints the embedded owner");
    repaint([&]{input::SetCaretHeight(window,20.f);},"changed caret height requests a paint");
    repaint([&]{input::SetPadding(window,6.f,1.f);},"changed padding requests a paint");
    DestroyWindow(window);DestroyWindow(owner);
}
void Accessible(HWND owner)
{
    const auto window=Create(owner,ES_MULTILINE,L"abc אבג\n第二行用于折行的长名称");if(!window){Check(false,"create accessibility fixture");return;}
    input::SetAccessibleName(window,L"日程备注");input::SetEmbeddedPose(window,{20,30,160,90},{20,30,130,90},true,true);
    RECT real{};GetWindowRect(window,&real);Check(real.right-real.left==1&&real.bottom-real.top==1,"embedded input retains only a tiny message HWND");
    ComPtr<IRawElementProviderSimple> provider;Check(SUCCEEDED(input::CreateWindowProvider(window,&provider)),"create production UIA provider");
    if(!provider){DestroyWindow(window);return;}
    VARIANT property{};provider->GetPropertyValue(UIA_BoundingRectanglePropertyId,&property);Check(property.vt==(VT_ARRAY|VT_R8),"UIA publishes logical bounds instead of the proxy bounds");VariantClear(&property);
    ComPtr<IUnknown> pattern;provider->GetPatternProvider(UIA_TextPatternId,&pattern);ComPtr<ITextProvider> text;
    Check(pattern&&SUCCEEDED(pattern.As(&text)),"ordinary input exposes Text pattern");
    ComPtr<ITextRangeProvider> range;
    if(text)
    {
        Check(SUCCEEDED(text->get_DocumentRange(&range)),"document range is available");
        if(range)
        {
            BSTR value=nullptr;range->GetText(-1,&value);Check(value&&std::wstring(value)==Value(window),"Text range returns committed text");SysFreeString(value);
            SAFEARRAY* rectangles=nullptr;Check(SUCCEEDED(range->GetBoundingRectangles(&rectangles)),"multiline bidi ranges use DirectWrite geometry");
            LONG upper=-1;if(rectangles)SafeArrayGetUBound(rectangles,1,&upper);Check(upper>=7,"multiline ranges return multiple rectangles");if(rectangles)SafeArrayDestroy(rectangles);
            Check(SUCCEEDED(range->Select()),"Text range selection changes the production input");Selection(window,0,static_cast<DWORD>(Value(window).size()),"UIA selection covers the requested text");
            int moved=0;range->MoveEndpointByUnit(TextPatternRangeEndpoint_End,TextUnit_Character,-1,&moved);Check(moved==-1,"Text range movement reports the actual count");
        }
    }
    const auto access=input::Accessibility(window);DestroyWindow(window);
    Check(!access->document(),"stale access cannot read a destroyed window");
    if(range){BSTR value=nullptr;Check(range->GetText(-1,&value)==UIA_E_ELEMENTNOTAVAILABLE&&value==nullptr,"retained UIA ranges become unavailable after destruction");SysFreeString(value);}
    const auto replacement=Create(owner);Check(!access->select(0,1),"stale selection cannot affect a replacement input");if(replacement)DestroyWindow(replacement);
}
void Password(HWND owner)
{
    const auto window=Create(owner,ES_PASSWORD,L"test-secret");Check(window!=nullptr,"create protected input");if(!window)return;
    wchar_t cache[32]{};DefWindowProcW(window,WM_GETTEXT,32,reinterpret_cast<LPARAM>(cache));
    Check(cache[0]==0&&Value(window).empty()&&input::DisplayText(window).empty(),"password is absent from caption and ordinary text queries");
    const auto document=input::Accessibility(window)->document();Check(document&&document->password&&document->text.empty(),"password accessibility snapshots contain no plaintext");
    const auto clipboard=GetClipboardSequenceNumber();SendMessageW(window,EM_SETSEL,0,-1);SendMessageW(window,WM_COPY,0,0);SendMessageW(window,WM_CUT,0,0);
    Check(GetClipboardSequenceNumber()==clipboard&&GetWindowTextLengthW(window)==11,"password copy and cut leave clipboard and value unchanged");
    SendMessageW(window,WM_CHAR,L'x',0);Check(SendMessageW(window,EM_CANUNDO,0,0)==0,"password edits never create ordinary history");
    ComPtr<IRawElementProviderSimple> provider;input::CreateWindowProvider(window,&provider);ComPtr<IValueProvider> value;
    if(provider)
    {
        provider.As(&value);ComPtr<IUnknown> pattern;provider->GetPatternProvider(UIA_TextPatternId,&pattern);Check(!pattern,"password has no Text pattern");
        if(value){BSTR secret=nullptr;Check(value->get_Value(&secret)==E_ACCESSDENIED&&secret==nullptr,"password Value pattern denies reads");Check(value->SetValue(L"another")==E_ACCESSDENIED,"password cannot receive an ordinary UIA value snapshot");}
    }
    SetWindowTextW(window,L"");wchar_t secret[32]{};input::CopySecret(window,secret,32);Check(secret[0]==0,"clearing the protected value clears trusted submission data");SecureZeroMemory(secret,sizeof(secret));DestroyWindow(window);
}
void Reentrant(HWND owner)
{
    const auto window=Create(owner);const auto access=input::Accessibility(window);destroyOnChange=true;
    SendMessageW(window,WM_CHAR,L'x',0);destroyOnChange=false;Check(access&&!access->document(),"change callback may destroy its input safely");
}
std::pair<int,int> InkBand(HWND window)
{
    RECT client{};GetClientRect(window,&client);
    const int width=client.right,height=client.bottom;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;
    const auto dc=CreateCompatibleDC(nullptr);
    const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return {-1,-1};}
    const auto previous=SelectObject(dc,bitmap);
    SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);
    GdiFlush();
    int first=height,last=-1;
    const auto* data=static_cast<const DWORD*>(pixels);
    for(int y=4;y<height-4;++y)for(int x=8;x<width-8;++x)
        if((data[y*width+x]&0xffffff)!=0xffffff){first=std::min(first,y);last=std::max(last,y);}
    SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return {first,last};
}
void VerticalAlignment(HWND owner,const wchar_t* selectedFamily=nullptr)
{
    const auto window=Create(owner,ES_AUTOHSCROLL,L"");if(!window)return;
    input::Colors colors;colors.foreground=colors.secondary=RGB(0,0,0);
    colors.background=colors.border=colors.accent=RGB(255,255,255);input::SetColors(window,colors);
    const auto access=input::Accessibility(window);
    const std::vector<const wchar_t*> families=selectedFamily
        ? std::vector<const wchar_t*>{selectedFamily}
        : std::vector<const wchar_t*>{L"Segoe UI",L"Microsoft YaHei UI"};
    for(const auto* family:families)
    // Representative grid, calendar, search and scaled input sizes avoid an
    // expensive Cartesian product while retaining the reported failure cases.
    for(const auto sample:{std::pair{13,34},std::pair{20,54},std::pair{23,48},std::pair{26,55}})
    {
        const int fontSize=sample.first,height=sample.second;
        const auto font=CreateFontW(-fontSize,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,family);
        SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
        {
            SetWindowPos(window,nullptr,0,0,800,height,SWP_NOZORDER|SWP_NOACTIVATE);
            for(const std::wstring value:{L"测试",L"在桌面、应用、Everything中搜索",L"Everything",L"日程 Event",L"gjpq"})
            {
                SetWindowTextW(window,L"");
                SendMessageW(window,EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(value.c_str()));
                const auto cue=InkBand(window);SetWindowTextW(window,value.c_str());const auto band=InkBand(window);
                Check(cue.second>=cue.first&&band==cue,"actual search/calendar cue and value share their rendered band");
                // Check the visible glyphs, including Latin descenders, rather
                // than accepting a centered line box around elevated text.
                {
                    const int error=band.first+band.second-(height-1);
                    if(std::abs(error)>2)
                        std::cerr<<"alignment: font="<<fontSize<<" height="<<height<<" glyphs="<<value.size()<<" band="<<band.first<<','<<band.second<<'\n';
                    Check(std::abs(error)<=2,"actual search/calendar glyphs are centered within one pixel");
                }
                const auto boxes=access->rectangles(0,value.size());
                Check(!boxes.empty(),"centered input exposes actual text geometry");
                if(!boxes.empty())
                {
                    // Fallback glyph ranges may have different heights. The
                    // hit test must still agree with each actual range.
                    Check(access->hit({boxes[0].left+1,boxes[0].top+boxes[0].height/2})==0,
                        "pointer and accessibility hit testing use the centered text origin");
                }
            }
        }
        // Release the selected font before deleting it.
        SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);
        DeleteObject(font);
    }
    DestroyWindow(window);
}
void ScrolledGlyphs(HWND owner)
{
    const auto window=Create(owner,ES_AUTOHSCROLL,L"");if(!window)return;
    input::Colors colors;colors.background=colors.border=RGB(255,255,255);
    colors.foreground=colors.secondary=RGB(0,0,0);colors.accent=RGB(0,103,192);
    colors.selectionText=RGB(0,0,0);input::SetColors(window,colors);
    const auto font=CreateFontW(-20,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    SetWindowPos(window,nullptr,0,0,80,34,SWP_NOZORDER|SWP_NOACTIVATE);
    const std::wstring value=std::wstring(60,L'W')+L"测试";SetWindowTextW(window,value.c_str());
    SendMessageW(window,EM_SETSEL,value.size(),value.size());
    const auto glyphCount=[&]()
    {
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=80;info.bmiHeader.biHeight=-34;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;const auto dc=CreateCompatibleDC(nullptr);
        const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!dc||!bitmap){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return 0;}
        const auto previous=SelectObject(dc,bitmap);
        SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);GdiFlush();
        int count=0;const auto* data=static_cast<const DWORD*>(pixels);
        // White background and blue selection cannot satisfy this black-ink
        // check. A geometry-only test would miss invisible scrolled glyphs.
        for(int y=4;y<30;++y)for(int x=8;x<72;++x)
        {
            const auto pixel=data[y*80+x];
            if((pixel&255)<32&&((pixel>>8)&255)<32&&((pixel>>16)&255)<32)++count;
        }
        SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);return count;
    };
    Check(glyphCount()>20,"horizontal scrolling draws the last glyphs beyond the original layout width");
    SendMessageW(window,EM_SETSEL,0,-1);
    Check(glyphCount()>20,"a selected overflowing name draws glyphs as well as its blue selection");
    SendMessageW(window,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),FALSE);
    DeleteObject(font);DestroyWindow(window);
}
void RoundedBorder(HWND owner,const std::filesystem::path& output={})
{
    // Render the real custom window and apply its actual HWND region. Region
    // existence alone cannot detect the missing half-stroke at the corners.
    for(const bool multiline:{false,true})for(const int radius:{6,9})for(const bool focused:{false,true})
    {
        const int width=20*radius,height=(multiline?13:6)*radius;
        const auto window=Create(owner,multiline?ES_MULTILINE:ES_AUTOHSCROLL,L"");
        Check(window!=nullptr,"create rounded rename border regression");if(!window)continue;
        input::Colors colors;colors.background=RGB(255,255,255);
        colors.border=colors.accent=RGB(0,103,192);input::SetColors(window,colors,static_cast<float>(radius));
        SetWindowPos(window,nullptr,0,0,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        const auto previousFocus=GetFocus();
        if(focused){SetFocus(window);Check(GetFocus()==window,"focus the isolated hidden rename input");}
        const auto region=CreateRectRgn(0,0,0,0);
        Check(region&&GetWindowRgn(window,region)!=ERROR,"rounded border uses the production window clip");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void* pixels=nullptr;const auto dc=CreateCompatibleDC(nullptr);
        const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        Check(dc&&bitmap,"create isolated rounded-border render surface");
        if(dc&&bitmap)
        {
            const auto previous=SelectObject(dc,bitmap);
            SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);GdiFlush();
            const auto* data=static_cast<const DWORD*>(pixels);
            const auto red=[&](int x,int y){return (data[y*width+x]>>16)&255;};
            Check(red(width/2,0)<=24&&red(width/2,height-1)<=24&&
                    red(0,height/2)<=24&&red(width-1,height/2)<=24,
                "all four rename edges retain a solid stroke inside the client area");
            if(focused)
                Check(red(width/2,1)<=24&&red(width/2,height-2)<=24&&
                        red(1,height/2)<=24&&red(width-2,height/2)<=24,
                    "focused blue outline remains visibly stronger on all four edges");
            for(const bool right:{false,true})for(const bool bottom:{false,true})
            {
                int ink=0;
                for(int y=0;y<radius;++y)for(int x=0;x<radius;++x)
                {
                    const int px=right?width-1-x:x,py=bottom?height-1-y:y;
                    if(PtInRegion(region,px,py)&&red(px,py)<96)++ink;
                }
                Check(ink>=(focused?radius:3),"every rounded corner retains solid outline after the actual window clip");
            }
            if(!output.empty())
            {
                std::error_code error;std::filesystem::create_directories(output,error);
                Check(!error,"create isolated border-preview directory");
                std::vector<DWORD> shown(data,data+width*height);
                for(int y=0;y<height;++y)for(int x=0;x<width;++x)
                    shown[y*width+x]=PtInRegion(region,x,y)?shown[y*width+x]|0xff000000:0xff363c46;
                BITMAPFILEHEADER header{};header.bfType=0x4d42;
                header.bfOffBits=sizeof(header)+sizeof(info.bmiHeader);
                header.bfSize=header.bfOffBits+static_cast<DWORD>(shown.size()*sizeof(DWORD));
                const std::string name=std::string(multiline?"multiline-":"single-")+std::to_string(radius)+
                    (focused?"-focused.bmp":"-unfocused.bmp");
                std::ofstream file(output/name,std::ios::binary);
                file.write(reinterpret_cast<const char*>(&header),sizeof(header));
                file.write(reinterpret_cast<const char*>(&info.bmiHeader),sizeof(info.bmiHeader));
                file.write(reinterpret_cast<const char*>(shown.data()),static_cast<std::streamsize>(shown.size()*sizeof(DWORD)));
                Check(static_cast<bool>(file),"save actual clipped border rendering for inspection");
            }
            SelectObject(dc,previous);
        }
        if(focused)SetFocus(previousFocus);
        if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);if(region)DeleteObject(region);DestroyWindow(window);
    }
}
void BundledFonts(HWND owner,const std::filesystem::path& root)
{
    namespace fonts=snowdesktop::app_fonts;
    for(const auto* relative:{L"assets/fonts/MiSans/MiSans-Regular.otf",
            L"assets/fonts/HarmonyOS-Sans/HarmonyOS_Sans_SC_Regular.ttf"})
    {
        auto resource=std::make_shared<fonts::Resource>();
        ComPtr<IDWriteFontFile> file;ComPtr<IDWriteFontSetBuilder1> builder;
        ComPtr<IDWriteFontSet> set;ComPtr<IDWriteFontCollection2> collection;
        ComPtr<IDWriteFontFamily2> family;ComPtr<IDWriteLocalizedStrings> names;
        const auto path=root/relative;
        const bool loaded=SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED,
            __uuidof(IDWriteFactory6),reinterpret_cast<IUnknown**>(resource->factory.GetAddressOf())))&&
            SUCCEEDED(resource->factory->CreateFontFileReference(path.c_str(),nullptr,&file))&&
            SUCCEEDED(resource->factory->CreateFontSetBuilder(&builder))&&
            SUCCEEDED(builder->AddFontFile(file.Get()))&&SUCCEEDED(builder->CreateFontSet(&set))&&
            SUCCEEDED(resource->factory->CreateFontCollectionFromFontSet(set.Get(),
                DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC,&collection))&&collection->GetFontFamilyCount()>0&&
            SUCCEEDED(collection->GetFontFamily(0,&family))&&SUCCEEDED(family->GetFamilyNames(&names));
        Check(loaded,"load the actual bundled application font collection");
        if(!loaded)continue;
        UINT32 length=0;names->GetStringLength(0,&length);
        resource->family.resize(static_cast<std::size_t>(length)+1);
        names->GetString(0,resource->family.data(),length+1);resource->family.resize(length);
        resource->collection=collection;
        fonts::current.store(resource);
        VerticalAlignment(owner,resource->family.c_str());
        fonts::current.store(nullptr);
    }
}
}
int main(int argc,char** argv)
{
    const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSW cls{};cls.lpfnWndProc=Owner;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"SnowDesktop.TextInputTestOwner";RegisterClassW(&cls);
    const auto owner=CreateWindowExW(0,cls.lpszClassName,L"",WS_OVERLAPPED,0,0,400,300,nullptr,nullptr,cls.hInstance,nullptr);
    if(!owner)Check(false,"create isolated hidden test owner");
    else if(argc>3&&std::string(argv[2])=="--border-preview")
    {RoundedBorder(owner,std::filesystem::path(argv[3]));DestroyWindow(owner);}
    else{Ordinary(owner);StableEmbeddedPresentation();Accessible(owner);Password(owner);Reentrant(owner);VerticalAlignment(owner);ScrolledGlyphs(owner);RoundedBorder(owner);
        Check(argc>1,"the bundled-font regression receives the repository root");
        if(argc>1)BundledFonts(owner,std::filesystem::path(argv[1]));DestroyWindow(owner);}
    if(SUCCEEDED(apartment))CoUninitialize();
    if(failures)return 1;std::cout<<"shared text input production checks passed\n";return 0;
}
