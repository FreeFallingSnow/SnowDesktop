#include "system_control_prompt.h"
#include "l10n.h"
#include <commctrl.h>
#include <algorithm>

namespace snowdesktop
{
namespace
{
std::wstring DisplayIdentity(std::string_view value)
{
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(length>0?static_cast<std::size_t>(length):0,L' ');
    if(length>0)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),length);
    for(auto& ch:result)if(ch<32||ch==127)ch=L' ';
    return result;
}
bool PromptValid(const std::shared_ptr<SystemControlPromptState>& state)
{
    if(state->cancelled)return false;
    if(std::chrono::steady_clock::now()>=state->deadline){state->error="timeout";state->cancelled=true;return false;}
    try{if(state->valid&&!state->valid()){state->error="canceled";state->cancelled=true;return false;}}
    catch(...){state->error="canceled";state->cancelled=true;return false;}
    return true;
}
struct Prompt
{
    system_control::Request& request;std::shared_ptr<SystemControlPromptState> state;HWND password=nullptr,ssid=nullptr,security=nullptr;bool hidden=false,passwordForm=false;std::wstring actor,target;
    static INT_PTR CALLBACK Procedure(HWND w,UINT m,WPARAM wp,LPARAM lp)
    {
        auto* self=reinterpret_cast<Prompt*>(GetWindowLongPtrW(w,DWLP_USER));
        if(m==WM_INITDIALOG)
        {
            self=reinterpret_cast<Prompt*>(lp);SetWindowLongPtrW(w,DWLP_USER,lp);
            self->state->window=w;
            if(!PromptValid(self->state)){EndDialog(w,IDCANCEL);return TRUE;}
            if(!SetTimer(w,1,100,nullptr)){self->state->error="confirmationUnavailable";self->state->cancelled=true;EndDialog(w,IDCANCEL);return TRUE;}
            SetWindowTextW(w,_LW(self->hidden?"controlCenter.hiddenNetwork":"statusBar.controlCenter"));
            const auto font=reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            auto child=[&](DWORD ex,const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int width,int height,int id){const auto h=CreateWindowExW(ex,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,width,height,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return h;};
            int y=16;
            if(!self->actor.empty()){child(0,L"STATIC",self->actor.c_str(),SS_NOPREFIX,16,y,330,48,0);y+=56;}
            if(!self->target.empty()){child(0,L"STATIC",self->target.c_str(),SS_NOPREFIX,16,y,330,40,0);y+=48;}
            if(self->hidden)
            {
                child(0,L"STATIC",_LW("controlCenter.ssid"),0,16,y,330,20,0);y+=24;self->ssid=child(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,16,y,330,28,101);SendMessageW(self->ssid,EM_SETLIMITTEXT,32,0);y+=40;
                child(0,L"STATIC",_LW("controlCenter.security"),0,16,y,330,20,0);y+=24;self->security=child(0,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,16,y,330,120,102);
                for(const auto* s:{_LW("controlCenter.openNetwork"),L"WPA2-Personal",L"WPA3-Personal"})SendMessageW(self->security,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));SendMessageW(self->security,CB_SETCURSEL,1,0);y+=40;
            }
            if(self->passwordForm)
            {
                child(0,L"STATIC",_LW("controlCenter.passwordHint"),0,16,y,330,36,0);y+=40;self->password=child(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_TABSTOP|ES_PASSWORD|ES_AUTOHSCROLL,16,y,330,28,103);SendMessageW(self->password,EM_SETLIMITTEXT,63,0);y+=44;
            }
            else if(system_control::RequiresConfirmation(self->request.name))
            {
                const auto& task=self->request.name;
                const char* key=task=="network.wifi.forget"?"controlCenter.confirmForget":task=="system.power.sleep"?"controlCenter.confirmSleep":task=="system.power.restart"?"controlCenter.confirmRestart":"controlCenter.confirmShutdown";
                child(0,L"STATIC",_LW(key),0,16,y,330,64,0);y+=76;
            }
            child(0,L"BUTTON",_LW("settings.dialog.cancel"),WS_TABSTOP|BS_DEFPUSHBUTTON,142,y,96,32,IDCANCEL);child(0,L"BUTTON",_LW("settings.dialog.confirm"),WS_TABSTOP|BS_PUSHBUTTON,250,y,96,32,IDOK);
            SendMessageW(w,DM_SETDEFID,IDCANCEL,0);
            RECT r{0,0,362,y+48};AdjustWindowRectEx(&r,static_cast<DWORD>(GetWindowLongPtrW(w,GWL_STYLE)),FALSE,0);SetWindowPos(w,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);return TRUE;
        }
        if(!self)return FALSE;
        if(m==WM_TIMER&&wp==1&&!PromptValid(self->state)){EndDialog(w,IDCANCEL);return TRUE;}
        if(m==WM_COMMAND&&LOWORD(wp)==102&&HIWORD(wp)==CBN_SELCHANGE&&self->password)
        {
            const bool secured=SendMessageW(self->security,CB_GETCURSEL,0,0)!=0;
            EnableWindow(self->password,secured);if(!secured)SetWindowTextW(self->password,L"");return TRUE;
        }
        if(m==WM_COMMAND&&(LOWORD(wp)==IDOK||LOWORD(wp)==IDCANCEL))
        {
            if(LOWORD(wp)==IDOK&&PromptValid(self->state))
            {
                if(self->hidden){wchar_t name[64]{};GetWindowTextW(self->ssid,name,64);char utf8[256]{};const int count=WideCharToMultiByte(CP_UTF8,0,name,-1,utf8,256,nullptr,nullptr);if(count<=1||count>33){SetFocus(self->ssid);return TRUE;}self->request.arguments["ssid"]=utf8;const auto selected=SendMessageW(self->security,CB_GETCURSEL,0,0);self->request.arguments["security"]=selected==0?"open":selected==2?"wpa3":"wpa2";}
                if(self->password&&IsWindowEnabled(self->password)){wchar_t secret[64]{};GetWindowTextW(self->password,secret,64);self->request.password=system_control::Secret(secret);SecureZeroMemory(secret,sizeof(secret));SetWindowTextW(self->password,L"");}self->request.hostConfirmed=true;
            }
            EndDialog(w,self->state->cancelled?IDCANCEL:LOWORD(wp));return TRUE;
        }
        if(m==WM_NCDESTROY){KillTimer(w,1);if(self->state->window==w)self->state->window=nullptr;}
        if(m==WM_CLOSE){EndDialog(w,IDCANCEL);return TRUE;}return FALSE;
    }
};
}
bool ConfirmSystemControl(HWND owner,system_control::Request& request,const std::shared_ptr<SystemControlPromptState>& state,
    const std::wstring& actor,const system_control::Snapshot* wifi)
{
    if(!state||!PromptValid(state))return false;
    std::wstring target;
    const auto argument=[&](const char* key){const auto found=request.arguments.find(key);return found==request.arguments.end()?std::string{}:found->second;};
    const bool connect=request.name=="network.wifi.connect";
    const bool hidden=connect&&argument("hidden")=="1"&&argument("ssid").empty()&&argument("networkId").empty()&&argument("profileName").empty();
    if(connect&&!argument("networkId").empty())
    {
        const JsonValue* network=nullptr;
        const auto* interfaces=wifi&&wifi->available?wifi->value.Find("interfaces"):nullptr;
        if(interfaces&&interfaces->IsArray())for(const auto& adapter:interfaces->array)
            if(system_control::json::String(adapter,"id")==argument("interfaceId"))
                if(const auto* networks=adapter.Find("networks");networks&&networks->IsArray())for(const auto& item:networks->array)
                    if(system_control::json::String(item,"id")==argument("networkId")){network=&item;break;}
        if(!network){state->error="networkGone";return false;}
        target=DisplayIdentity(system_control::json::String(*network,"ssid"));
        request.arguments["security"]=system_control::json::String(*network,"security");
    }
    else if(connect)target=DisplayIdentity(argument("ssid"));
    else if(request.name=="network.wifi.forget")target=DisplayIdentity(argument("profileName"));
    const auto security=argument("security");
    if(connect&&!hidden&&argument("profileName").empty()&&security!="open"&&security!="wpa2"&&security!="wpa3")
    {state->error="systemSettingsRequired";return false;}
    const bool password=hidden||(system_control::RequiresPasswordPrompt(request)&&security!="open");
    if(!password&&!system_control::RequiresConfirmation(request.name))return true;
    struct Template{DLGTEMPLATE dialog;WORD menu=0,cls=0,title=0;} form{};
    form.dialog.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;form.dialog.cx=260;form.dialog.cy=hidden?250:130;
    Prompt prompt{request,state,nullptr,nullptr,nullptr,hidden,password,actor,target};
    const auto result=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),&form.dialog,owner,Prompt::Procedure,reinterpret_cast<LPARAM>(&prompt));
    state->window=nullptr;const bool confirmed=result==IDOK&&PromptValid(state);
    if(!confirmed){request.password.Clear();request.hostConfirmed=false;if(state->error.empty())state->error=result==-1?"confirmationUnavailable":"userCanceled";}
    return confirmed;
}
}
