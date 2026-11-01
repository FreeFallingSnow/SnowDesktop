#include "ole_drag_drop_adapter.h"
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace snowdesktop::ole_adapter_observation
{
namespace
{
constexpr std::size_t kCapacity = 1024;
struct Log
{
    State state;
    std::array<Event, kCapacity> events{};
    Event* configuredEvents = nullptr;
    std::size_t configuredCapacity = 0;
    std::uint64_t nextSession = 0, nextCall = 0, currentCall = 0;
    Event currentEvent;
    ~Log() { if (configuredEvents) VirtualFree(configuredEvents, 0, MEM_RELEASE); }
    Event* Data() noexcept { return configuredEvents ? configuredEvents : events.data(); }
    std::size_t Capacity() const noexcept { return configuredEvents ? configuredCapacity : kCapacity; }
};
// Allocate the bounded buffer only for an explicitly enabled observer.
thread_local std::unique_ptr<Log> log;
thread_local bool configurationAttempted = false, configurationFailed = false, beginAttempted = false;

void Append(const Event& event) noexcept
{
    if (log->state.count >= log->Capacity()) { log->state.failed = true; return; }
    log->Data()[log->state.count++] = event;
}
}

CaptureConfiguration ConfigureBudgetOnce(std::size_t eventCapacity, std::size_t byteBudget) noexcept
{
    CaptureConfiguration out;
    const DWORD saved = GetLastError();
    struct ErrorGuard { DWORD value; ~ErrorGuard() { SetLastError(value); } } restore{saved};
    if (configurationAttempted || beginAttempted || log) return out;
    configurationAttempted = true;
    const auto fail = [&]() noexcept { configurationFailed = true; out.resourceUnavailable = true; };
    if (!eventCapacity || !byteBudget || eventCapacity > (std::numeric_limits<std::size_t>::max)() / sizeof(Event))
    { fail(); return out; }
    const auto used = eventCapacity * sizeof(Event);
    SYSTEM_INFO info{}; GetSystemInfo(&info); const auto page = static_cast<std::size_t>(info.dwPageSize);
    if (!page || used > (std::numeric_limits<std::size_t>::max)() - (page - 1)) { fail(); return out; }
    const auto committed = ((used + page - 1) / page) * page;
    if (!committed || committed > byteBudget) { fail(); return out; }
    auto candidate = std::unique_ptr<Log>(new (std::nothrow) Log);
    if (!candidate) { fail(); return out; }
    auto* block = static_cast<Event*>(VirtualAlloc(nullptr, committed, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block) { fail(); return out; }
    static_assert(std::is_nothrow_default_constructible_v<Event> && std::is_trivially_destructible_v<Event>);
    for (std::size_t i = 0; i < eventCapacity; ++i) new (block + i) Event{};
    candidate->configuredEvents = block; candidate->configuredCapacity = eventCapacity;
    log = std::move(candidate); out.configured = true; out.committedBytes = committed; return out;
}
bool Begin() noexcept
{
    const DWORD saved = GetLastError();
    beginAttempted = true;
    if (!log && !configurationFailed) log.reset(new (std::nothrow) Log);
    const bool ok = log && !log->state.active && log->state.depth == 0 &&
        log->nextSession != std::numeric_limits<std::uint64_t>::max();
    if (ok)
    {
        log->state = {};
        log->state.active = true;
        log->state.ownerThread = GetCurrentThreadId();
        log->state.capacity = log->Capacity();
        log->state.session = ++log->nextSession;
        log->nextCall = 0;
        log->currentCall = 0;
        log->currentEvent = {};
    }
    SetLastError(saved);
    return ok;
}
bool End() noexcept
{
    const DWORD saved = GetLastError();
    const bool ok = log && log->state.active && log->state.depth == 0 && !log->state.failed;
    if (log && log->state.active && log->state.depth == 0) log->state.active = false;
    SetLastError(saved);
    return ok;
}
State ReadState() noexcept
{
    const DWORD saved = GetLastError();
    const auto result = log ? log->state : State{};
    SetLastError(saved);
    return result;
}
bool ReadEvent(std::size_t index, Event& event) noexcept
{
    const DWORD saved = GetLastError();
    const bool ok = log && index < log->state.count;
    if (ok) event = log->Data()[index];
    SetLastError(saved);
    return ok;
}
bool Current(Event& event) noexcept
{
    const DWORD saved = GetLastError();
    const bool ok = log && log->state.active && !log->state.failed && log->state.depth != 0 && log->currentCall != 0;
    if (ok) event = log->currentEvent;
    SetLastError(saved);
    return ok;
}

class Scope final
{
public:
    Scope(const void* adapter, Kind kind) noexcept
    {
        const DWORD saved = GetLastError();
        if (!log || !log->state.active) { SetLastError(saved); return; }
        active_ = true;
        parent_ = log->currentCall;
        previousEvent_ = log->currentEvent;
        if (log->nextCall == std::numeric_limits<std::uint64_t>::max() ||
            log->state.depth == std::numeric_limits<std::size_t>::max())
        {
            log->state.failed = true;
            active_ = false;
            SetLastError(saved);
            return;
        }
        event_.session = log->state.session;
        event_.thread = log->state.ownerThread;
        event_.adapter = reinterpret_cast<std::uintptr_t>(adapter);
        event_.call = ++log->nextCall;
        event_.parent = parent_;
        event_.depth = log->state.depth++;
        event_.kind = kind;
        event_.phase = Phase::entered;
        log->currentCall = event_.call;
        log->currentEvent = event_;
        Append(event_);
        SetLastError(saved);
    }
    ~Scope() noexcept
    {
        if (!active_) return;
        const DWORD saved = GetLastError();
        if (!returned_) { event_.phase = Phase::unwound; Append(event_); }
        --log->state.depth;
        log->currentCall = parent_;
        log->currentEvent = previousEvent_;
        SetLastError(saved);
    }
    HRESULT Return(HRESULT value) noexcept
    {
        if (!active_) return value;
        const DWORD saved = GetLastError();
        event_.phase = Phase::returned;
        event_.result = value;
        Append(event_);
        returned_ = true;
        SetLastError(saved);
        return value;
    }
private:
    bool active_ = false, returned_ = false;
    std::uint64_t parent_ = 0;
    Event event_;
    Event previousEvent_;
};
}

namespace
{
constexpr DWORD kDragButtonMask = MK_LBUTTON | MK_RBUTTON | MK_MBUTTON;
}

OleDragDropAdapter::OleDragDropAdapter(
    OleDragDropHandler* handler) noexcept
    : handler_(handler)
{
}

void OleDragDropAdapter::Detach() noexcept
{
    handler_ = nullptr;
    dragButtons_ = 0;
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::QueryInterface(
    REFIID interfaceId, void** object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    if (interfaceId == IID_IUnknown || interfaceId == IID_IDropTarget)
        *object = static_cast<IDropTarget*>(this);
    else if (interfaceId == IID_IDropSource)
        *object = static_cast<IDropSource*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE OleDragDropAdapter::AddRef()
{
    return static_cast<ULONG>(
        InterlockedIncrement(&referenceCount_));
}

ULONG STDMETHODCALLTYPE OleDragDropAdapter::Release()
{
    const LONG remaining = InterlockedDecrement(&referenceCount_);
    if (remaining == 0)
        delete this;
    return static_cast<ULONG>(remaining);
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::DragEnter(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    snowdesktop::ole_adapter_observation::Scope observation(this, snowdesktop::ole_adapter_observation::Kind::enter);
    dragButtons_ = keyState & kDragButtonMask;
    return observation.Return(handler_ ? handler_->HandleOleDragEnter(
        dataObject, keyState, point, effect) : E_UNEXPECTED);
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::DragOver(
    DWORD keyState, POINTL point, DWORD* effect)
{
    snowdesktop::ole_adapter_observation::Scope observation(this, snowdesktop::ole_adapter_observation::Kind::over);
    if (dragButtons_ == 0)
        dragButtons_ = keyState & kDragButtonMask;
    return observation.Return(handler_ ? handler_->HandleOleDragOver(
        keyState, point, effect) : E_UNEXPECTED);
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::DragLeave()
{
    snowdesktop::ole_adapter_observation::Scope observation(this, snowdesktop::ole_adapter_observation::Kind::leave);
    dragButtons_ = 0;
    return observation.Return(handler_ ? handler_->HandleOleDragLeave() : E_UNEXPECTED);
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::Drop(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    snowdesktop::ole_adapter_observation::Scope observation(this, snowdesktop::ole_adapter_observation::Kind::drop);
    // Shell targets receive a synthetic DragEnter/DragOver/Drop sequence from
    // the handler. Entering with no mouse button can show the drag-action menu
    // ("Open with / Cancel") instead of performing a normal left-button drop.
    // Only modifiers are live at release. Other buttons pressed later must not
    // change the gesture that the Shell target receives.
    if (dragButtons_ != 0)
        keyState = (keyState & ~kDragButtonMask) | dragButtons_;
    // Consume before dispatch: a Shell message pump can reenter this adapter
    // with a new drag, whose state must survive the outer handler's return.
    dragButtons_ = 0;
    return observation.Return(handler_ ? handler_->HandleOleDrop(
        dataObject, keyState, point, effect) : E_UNEXPECTED);
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::QueryContinueDrag(
    BOOL escapePressed, DWORD keyState)
{
    return handler_ ? handler_->HandleOleQueryContinueDrag(
        escapePressed, keyState) : DRAGDROP_S_CANCEL;
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::GiveFeedback(DWORD effect)
{
    return handler_ ? handler_->HandleOleGiveFeedback(effect) :
        DRAGDROP_S_USEDEFAULTCURSORS;
}
