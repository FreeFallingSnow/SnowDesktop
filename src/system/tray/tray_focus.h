#pragma once
#include "taskbar_hook/tray_protocol.h"
#include "tray_menu_placement.h"
#include <algorithm>
#include <cwchar>
#include <iterator>
#include <optional>
#include <utility>

namespace snowdesktop::tray
{
enum class MenuForeground { Transition, Origin, TargetProcess, Unrelated };
// A focus transition is not proof that an asynchronous tray callback has
// finished opening its menu. Discovery is bounded; an observed live menu has
// no arbitrary expiry while the user is interacting with it.
class MenuRetentionTracker
{
public:
    static constexpr DWORD kDiscoveryMs = 1500;
    void Arm(DWORD now) { started_ = lastMenu_ = now; armed_ = true; observed_ = false; }
    void Reset() { armed_ = observed_ = false; }
    bool Armed() const { return armed_; }
    bool ObservedMenu() const { return observed_; }
    bool Retain(DWORD now, bool targetAlive, MenuForeground foreground, bool liveMenu)
    {
        if (!armed_) return false;
        if (!targetAlive || foreground == MenuForeground::Unrelated) { Reset(); return false; }
        if (liveMenu) { observed_ = true; lastMenu_ = now; return true; }
        if (foreground == MenuForeground::Origin)
        {
            if (observed_ || static_cast<DWORD>(now - started_) >= kDiscoveryMs) Reset();
            return true;
        }
        if (!observed_ && static_cast<DWORD>(now - started_) < kDiscoveryMs) return true;
        // Native submenu replacement can briefly clear the active HWND/menu
        // owner. It must neither start a closing animation nor renew discovery.
        if (observed_ && foreground == MenuForeground::Transition &&
            static_cast<DWORD>(now - lastMenu_) < 200) return true;
        Reset(); return false;
    }
private:
    DWORD started_ = 0, lastMenu_ = 0;
    bool armed_ = false, observed_ = false;
};
// This state is armed before the first tray callback. WM_ACTIVATE may arrive
// with no next HWND, or with a main window on a different thread from the menu.
// Sample the concrete callback/menu threads and this gesture's SHOW evidence.
struct MenuRetentionSession
{
    DWORD process=0,targetThread=0,menuThread=0;
    HWND target=nullptr,menuOwner=nullptr,origin=nullptr,bar=nullptr;
    MenuRetentionTracker tracker;
    MenuPopupBindings popups{};
    void Reset(){*this={};}
    bool BelongsToTarget(HWND w)const
    {DWORD pid=0;return w&&GetWindowThreadProcessId(w,&pid)&&pid==process;}
    void Arm(HWND owner,DWORD pid,HWND source,HWND statusBar)
    {
        Reset();process=pid;
        if(!BelongsToTarget(owner)){Reset();return;}
        target=owner;targetThread=GetWindowThreadProcessId(owner,nullptr);origin=source;bar=statusBar;tracker.Arm(GetTickCount());
    }
    bool Related(HWND w)const
    {
        if(!BelongsToTarget(w))return false;
        const auto root=GetAncestor(target,GA_ROOTOWNER);
        for(unsigned depth=0;w&&depth<8;++depth)
        {
            if(!BelongsToTarget(w))break;
            if(w==target||w==root||GetWindowThreadProcessId(w,nullptr)==targetThread)return true;
            const auto next=GetWindow(w,GW_OWNER);if(next==w)break;w=next;
        }
        return false;
    }
    bool LivePopup(const MenuPopupBinding& binding)const
    {
        const auto w=reinterpret_cast<HWND>(binding.window);DWORD pid=0;
        if(!w||binding.process!=process||GetWindowThreadProcessId(w,&pid)!=binding.thread||pid!=process||
            !IsWindowVisible(w)||GetWindow(w,GW_OWNER)!=reinterpret_cast<HWND>(binding.owner)||
            GetWindowLongPtrW(w,GWL_STYLE)!=binding.style||GetWindowLongPtrW(w,GWL_EXSTYLE)!=binding.extendedStyle)return false;
        if(binding.owner&&!BelongsToTarget(reinterpret_cast<HWND>(binding.owner)))return false;
        wchar_t name[64]{};GetClassNameW(w,name,static_cast<int>(std::size(name)));
        return binding.standardMenu==(wcscmp(name,L"#32768")==0);
    }
    bool NativeMenu(DWORD thread,bool observedMenuThread=false,HWND expectedOwner=nullptr)
    {
        if(!thread)return false;
        GUITHREADINFO info{sizeof(info)};
        if(!GetGUIThreadInfo(thread,&info)||!(info.flags&(GUI_INMENUMODE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE))||
            !BelongsToTarget(info.hwndMenuOwner)||GetWindowThreadProcessId(info.hwndMenuOwner,nullptr)!=thread||
            (expectedOwner&&info.hwndMenuOwner!=expectedOwner)||
            (!observedMenuThread&&!Related(info.hwndMenuOwner)))return false;
        menuOwner=info.hwndMenuOwner;menuThread=thread;return true;
    }
    bool ContainsPoint(POINT screen)const
    {
        if(!tracker.Armed()||!BelongsToTarget(target)||
            GetWindowThreadProcessId(target,nullptr)!=targetThread)return false;
        const auto contains=[&](HWND window){RECT bounds{};return window&&IsWindowVisible(window)&&
            GetWindowRect(window,&bounds)&&PtInRect(&bounds,screen);};
        for(const auto& popup:popups)
            if(LivePopup(popup)&&contains(reinterpret_cast<HWND>(popup.window)))return true;
        // Placement discovery is deliberately short-lived. A submenu opened
        // later still belongs to the already-confirmed native menu session.
        if(!menuOwner||!menuThread)return false;
        const HWND hit=WindowFromPoint(screen);
        const HWND window=hit?GetAncestor(hit,GA_ROOT):nullptr;
        DWORD processId=0;
        if(!window||GetWindowThreadProcessId(window,&processId)!=menuThread||
            processId!=process||!contains(window))return false;
        wchar_t name[64]{};GetClassNameW(window,name,static_cast<int>(std::size(name)));
        if(wcscmp(name,L"#32768")!=0)return false;
        GUITHREADINFO info{sizeof(info)};
        return GetGUIThreadInfo(menuThread,&info)&&
            (info.flags&(GUI_INMENUMODE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE))&&
            info.hwndMenuOwner==menuOwner&&BelongsToTarget(menuOwner)&&
            GetWindowThreadProcessId(menuOwner,nullptr)==menuThread;
    }
    bool Active(HWND foreground,const MenuPopupBindings& observed)
    {
        if(!tracker.Armed())return false;
        const bool targetAlive=BelongsToTarget(target)&&GetWindowThreadProcessId(target,nullptr)==targetThread;
        const auto kind=!foreground?MenuForeground::Transition:
            foreground==origin||foreground==bar?MenuForeground::Origin:
            BelongsToTarget(foreground)?MenuForeground::TargetProcess:MenuForeground::Unrelated;
        if(!targetAlive||kind==MenuForeground::Unrelated){Reset();return false;}
        for(auto& binding:popups)if(binding.window&&!LivePopup(binding))binding={};
        for(const auto& binding:observed)
        {
            if(!LivePopup(binding)||std::any_of(popups.begin(),popups.end(),[&](const auto& p){return p.window==binding.window;}))continue;
            const auto slot=std::find_if(popups.begin(),popups.end(),[](const auto& p){return !p.window;});
            if(slot!=popups.end())*slot=binding;
        }
        bool liveMenu=menuOwner&&BelongsToTarget(menuOwner)&&NativeMenu(menuThread,true,menuOwner);
        if(!liveMenu){menuOwner=nullptr;menuThread=0;liveMenu=NativeMenu(targetThread);}
        if(!liveMenu&&kind==MenuForeground::TargetProcess)liveMenu=NativeMenu(GetWindowThreadProcessId(foreground,nullptr));
        for(const auto& binding:popups)if(binding.window)
        {liveMenu=true;if(binding.standardMenu)NativeMenu(binding.thread,true);}
        const bool retain=tracker.Retain(GetTickCount(),targetAlive,kind,liveMenu);
        if(!tracker.Armed())Reset();
        return retain;
    }
};
struct FocusDelivery
{
    FocusTicket ticket;
    std::string key;
    std::uint64_t foreground = 0;
};
// One outstanding user operation, never a queue of stale focus requests.
class FocusReturnTracker
{
public:
    void Arm(FocusTicket ticket, std::string key)
    { current_ = FocusDelivery{ticket, std::move(key), 0}; }
    void Cancel() { current_.reset(); }
    const FocusDelivery* Current() const { return current_ ? &*current_ : nullptr; }
    bool ObserveForeground(std::uint64_t window, DWORD process, DWORD time, bool nativeReturn)
    {
        if (!current_ || !window || static_cast<LONG>(time - current_->ticket.started) <= 0) return false;
        if (nativeReturn || FocusForegroundAllowed(current_->ticket, window, process)) return false;
        Cancel(); return true;
    }
    bool Arrive(const Notification& event, std::uint64_t epoch, std::uint64_t foreground)
    {
        if (!current_ || current_->foreground || event.operation != NIM_SETFOCUS ||
            event.epoch != epoch || event.epoch != current_->ticket.epoch ||
            event.focusSerial != current_->ticket.serial || !event.focusForeground ||
            event.focusForeground != foreground || !SameFocusIdentity(event.identity, current_->ticket.identity)) return false;
        current_->foreground = foreground;
        return true;
    }
    std::optional<FocusDelivery> Take(std::uint64_t origin, std::uint64_t serial,
        std::uint64_t epoch, std::uint64_t foreground)
    {
        if (!current_ || current_->ticket.origin != origin || current_->ticket.serial != serial) return {};
        auto value = std::move(current_);
        current_.reset();
        if (!value->foreground || value->foreground != foreground || value->ticket.epoch != epoch) return {};
        return value;
    }
private:
    std::optional<FocusDelivery> current_;
};
inline UINT FocusReturnMessage()
{ static const UINT message = RegisterWindowMessageW(L"SnowDesktop.Tray.FocusReturn.v2"); return message; }
// Called only on the origin's UI thread after consuming its single-use reply.
// Recheck at the Windows boundary: a delayed reply cannot replace a new app.
inline bool RestoreFocus(const FocusDelivery& delivery)
{
    const auto origin = reinterpret_cast<HWND>(delivery.ticket.origin);
    DWORD process = 0;
    if (!origin || !delivery.foreground || GetWindowThreadProcessId(origin, &process) != GetCurrentThreadId() ||
        process != GetCurrentProcessId() || !IsWindowVisible(origin) || !IsWindowEnabled(origin) ||
        reinterpret_cast<std::uint64_t>(GetForegroundWindow()) != delivery.foreground) return false;
    if (GetForegroundWindow() != origin && !SetForegroundWindow(origin)) return false;
    if (GetForegroundWindow() != origin) return false;
    SetFocus(origin);
    return GetFocus() == origin;
}
}
