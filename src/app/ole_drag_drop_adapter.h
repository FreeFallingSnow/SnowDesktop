#pragma once

#include <windows.h>
#include <oleidl.h>

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
 * a COM object.
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
};
