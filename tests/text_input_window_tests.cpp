#include "text_input_window.h"
#include <wrl/client.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;
namespace input = snowdesktop::text_input;
namespace
{
int failures=0,changes=0;
bool destroyOnChange=false;
void Check(bool value,const char* message)
{if(!value){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
LRESULT CALLBACK Owner(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
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
}
int main()
{
    const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSW cls{};cls.lpfnWndProc=Owner;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"SnowDesktop.TextInputTestOwner";RegisterClassW(&cls);
    const auto owner=CreateWindowExW(0,cls.lpszClassName,L"",WS_OVERLAPPED,0,0,400,300,nullptr,nullptr,cls.hInstance,nullptr);
    if(!owner)Check(false,"create isolated hidden test owner");else{Ordinary(owner);Accessible(owner);Password(owner);Reentrant(owner);DestroyWindow(owner);}
    if(SUCCEEDED(apartment))CoUninitialize();
    if(failures)return 1;std::cout<<"shared text input production checks passed\n";return 0;
}
