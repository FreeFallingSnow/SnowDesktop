#pragma once

#include <windows.h>
#include <oleidl.h>
#include <cstddef>
#include <cstdint>

// Internal, opt-in diagnostics for same-thread synchronous OLE calls. This
// does not change routing or make any claim about calls on another thread.
// Each enabled thread allocates a 1024-event buffer (typically 64 KiB on x64,
// plus bookkeeping), retained until thread exit. Disabled hooks still incur a
// TLS check and LastError preservation. `returned` is recorded after the handler returns but
// before the scope destructor and actual ABI return; callers must independently
// observe their Dispatch return and must not infer it from the log alone.
namespace snowdesktop::ole_adapter_observation
{
enum class Kind { enter, over, leave, drop };
enum class Phase { entered, returned, unwound };
struct Event
{
    std::uint64_t session = 0, call = 0, parent = 0;
    DWORD thread = 0;
    std::uintptr_t adapter = 0;
    std::size_t depth = 0;
    Kind kind = Kind::enter;
    Phase phase = Phase::entered;
    HRESULT result = E_UNEXPECTED; // meaningful only for returned
};
struct State
{
    bool active = false, failed = false;
    std::uint64_t session = 0;
    DWORD ownerThread = 0;
    std::size_t count = 0, depth = 0, capacity = 0;
};
// One opt-in allocation on this thread before its first Begin. This bounds
// the event VM allocation alone; it is neither a process budget nor a worker
// completion signal. Failure permanently disables this thread's capture.
struct CaptureConfiguration { bool configured = false, resourceUnavailable = false; std::size_t committedBytes = 0; };
CaptureConfiguration ConfigureBudgetOnce(std::size_t eventCapacity, std::size_t byteBudget) noexcept;
bool Begin() noexcept;
bool End() noexcept;
State ReadState() noexcept;
bool ReadEvent(std::size_t index, Event& event) noexcept;
// Current same-thread adapter scope only; false when disabled or between
// calls. The returned values do not retain or dereference the adapter.
bool Current(Event& event) noexcept;
}

class OleDragDropHandler
{
public:
    virtual ~OleDragDropHandler() = default;

    virtual HRESULT HandleOleDragEnter(IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect) = 0;
    virtual HRESULT HandleOleDragOver(
        DWORD keyState, POINTL point, DWORD* effect) = 0;
    virtual HRESULT HandleOleDragLeave() = 0;
    virtual HRESULT HandleOleDrop(IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect) = 0;
    virtual HRESULT HandleOleQueryContinueDrag(
        BOOL escapePressed, DWORD keyState) = 0;
    virtual HRESULT HandleOleGiveFeedback(DWORD effect) = 0;
};

/**
 * Windows OLE boundary for SnowDesktop drag and drop.
 *
 * The adapter owns COM identity and reference counting. It translates OLE
 * callbacks into application-level handlers without making DesktopApp itself
 * a COM object. Drop handlers retain the initiating mouse button for Shell
 * handoffs even after OLE reports its release, with the latest modifiers.
 */
class OleDragDropAdapter final : public IDropTarget, public IDropSource
{
public:
    explicit OleDragDropAdapter(OleDragDropHandler* handler) noexcept;

    void Detach() noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID interfaceId, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keyState,
        POINTL point, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* dataObject,
        DWORD keyState, POINTL point, DWORD* effect) override;

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(
        BOOL escapePressed, DWORD keyState) override;
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override;

private:
    ~OleDragDropAdapter() = default;

    LONG referenceCount_ = 1;
    OleDragDropHandler* handler_ = nullptr;
    DWORD dragButtons_ = 0;
};
