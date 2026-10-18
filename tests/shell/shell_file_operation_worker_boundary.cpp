#include "shell_file_operation_worker_boundary.h"
#include "shell/shell_file_operation_worker.h"
#include "drag_drop/clipboard_file_operation.h"
#include <shobjidl.h>
#include <sherrors.h>
#include <wrl/implements.h>
#include <atomic>

namespace shell_worker_boundary
{
namespace
{
std::atomic<Cancellation> cancellation{Cancellation::None};
std::atomic<unsigned> calls{0};
std::wstring cancelledSource;

class CancelledOperation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperation>
{
public:
    Cancellation outcome = Cancellation::None;
    Microsoft::WRL::ComPtr<IFileOperationProgressSink> sink;
    Microsoft::WRL::ComPtr<IShellItem> source;
    bool move = false;
    IFACEMETHODIMP Advise(IFileOperationProgressSink*, DWORD*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Unadvise(DWORD) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetOperationFlags(DWORD) override { return S_OK; }
    IFACEMETHODIMP SetProgressMessage(LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP SetProgressDialog(IOperationsProgressDialog*) override { return S_OK; }
    IFACEMETHODIMP SetProperties(IPropertyChangeArray*) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetOwnerWindow(HWND) override { return S_OK; }
    IFACEMETHODIMP ApplyPropertiesToItem(IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP ApplyPropertiesToItems(IUnknown*) override { return E_NOTIMPL; }
    IFACEMETHODIMP RenameItem(IShellItem*, LPCWSTR, IFileOperationProgressSink*) override { return E_NOTIMPL; }
    IFACEMETHODIMP RenameItems(IUnknown*, LPCWSTR) override { return E_NOTIMPL; }
    IFACEMETHODIMP MoveItem(IShellItem* item, IShellItem*, LPCWSTR, IFileOperationProgressSink* progress) override
    { source = item; sink = progress; move = true; return S_OK; }
    IFACEMETHODIMP MoveItems(IUnknown*, IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP CopyItem(IShellItem* item, IShellItem*, LPCWSTR, IFileOperationProgressSink* progress) override
    { source = item; sink = progress; return S_OK; }
    IFACEMETHODIMP CopyItems(IUnknown*, IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP DeleteItem(IShellItem*, IFileOperationProgressSink*) override { return E_NOTIMPL; }
    IFACEMETHODIMP DeleteItems(IUnknown*) override { return E_NOTIMPL; }
    IFACEMETHODIMP NewItem(IShellItem*, DWORD, LPCWSTR, LPCWSTR, IFileOperationProgressSink*) override { return E_NOTIMPL; }
    IFACEMETHODIMP PerformOperations() override
    {
        if (outcome == Cancellation::TrackedSkipped && sink)
        {
            if (move) sink->PostMoveItem(0, source.Get(), nullptr, nullptr, COPYENGINE_S_USER_IGNORED, nullptr);
            else sink->PostCopyItem(0, source.Get(), nullptr, nullptr, COPYENGINE_S_USER_IGNORED, nullptr);
        }
        if (outcome == Cancellation::TrackedAbortResult) return E_ABORT;
        if (outcome == Cancellation::TrackedUserCancelled) return COPYENGINE_E_USER_CANCELLED;
        return S_OK;
    }
    IFACEMETHODIMP GetAnyOperationsAborted(BOOL* aborted) override
    {
        if (!aborted) return E_POINTER;
        *aborted = outcome == Cancellation::TrackedAborted;
        return S_OK;
    }
};
}

void Arm(Cancellation mode, const std::wstring& source)
{
    cancelledSource = source;
    calls = 0;
    cancellation = mode;
}
void Disarm() { cancellation = Cancellation::None; cancelledSource.clear(); }
unsigned Calls() { return calls.load(); }

HRESULT WINAPI CreateInstance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* object)
{
    const auto mode = cancellation.load();
    if (mode >= Cancellation::TrackedAborted && clsid == CLSID_FileOperation)
    {
        const auto ordinal = ++calls;
        if (ordinal > 1)
        {
            auto operation = Microsoft::WRL::Make<CancelledOperation>();
            if (!operation) return E_OUTOFMEMORY;
            operation->outcome = mode;
            return operation.CopyTo(iid, object);
        }
    }
    return ::CoCreateInstance(clsid, outer, context, iid, object);
}

int WINAPI FileOperation(LPSHFILEOPSTRUCTW operation)
{
    const auto mode = cancellation.load();
    if (mode == Cancellation::LegacyAborted || mode == Cancellation::LegacyCode)
    {
        ++calls;
        if (operation && operation->pFrom &&
            _wcsicmp(operation->pFrom, cancelledSource.c_str()) == 0)
        {
            operation->fAnyOperationsAborted = mode == Cancellation::LegacyAborted;
            return mode == Cancellation::LegacyCode ? ERROR_CANCELLED : 0;
        }
    }
    return ::SHFileOperationW(operation);
}
}

// Compile the production worker once in this test target. No test hook or ABI
// is added to the host. Rename the inline executor too, avoiding an ODR clash
// with the normal progress header included by the test driver.
#define ExecuteTrackedDropStep ExecuteTrackedDropStepAtControlledBoundary
#define CoCreateInstance shell_worker_boundary::CreateInstance
#define SHFileOperationW shell_worker_boundary::FileOperation
#include "../../src/shell/shell_file_operation_worker.cpp"
#undef SHFileOperationW
#undef CoCreateInstance
#undef ExecuteTrackedDropStep
