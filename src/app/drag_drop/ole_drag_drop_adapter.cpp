#include "ole_drag_drop_adapter.h"

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
    dragButtons_ = keyState & kDragButtonMask;
    return handler_ ? handler_->HandleOleDragEnter(
        dataObject, keyState, point, effect) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::DragOver(
    DWORD keyState, POINTL point, DWORD* effect)
{
    if (dragButtons_ == 0)
        dragButtons_ = keyState & kDragButtonMask;
    return handler_ ? handler_->HandleOleDragOver(
        keyState, point, effect) : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::DragLeave()
{
    dragButtons_ = 0;
    return handler_ ? handler_->HandleOleDragLeave() : E_UNEXPECTED;
}

HRESULT STDMETHODCALLTYPE OleDragDropAdapter::Drop(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
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
    return handler_ ? handler_->HandleOleDrop(
        dataObject, keyState, point, effect) : E_UNEXPECTED;
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
