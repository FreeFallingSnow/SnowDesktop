#pragma once

#include "drag_drop/external_slot_transfer.h"
#include "drag_drop/external_drop_resources.h"
#include "drag_drop/file_backed_drop_request.h"
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace async_slot_transfer_cases
{
using Microsoft::WRL::ComPtr;
using namespace snowdesktop;
inline constexpr UINT completedMessage = WM_APP + 73;

inline void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

// Explorer is the external boundary. It remains apartment-bound: GetData and
// EndOperation must reach this original STA through real COM marshaling.
class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IDataObject, IDataObjectAsyncCapability>
{
public:
    ComPtr<IDataObject> shell;
    DWORD owner = GetCurrentThreadId();
    int reads = 0, starts = 0, ends = 0, performed = 0;
    HRESULT outcome = E_PENDING;
    DWORD endEffect = 0xffffffff, performedEffect = 0xffffffff;
    bool wrongApartment = false, readBeforeStart = false, failRead = false, failStart = false;
    bool async = true;

    void CheckThread() { wrongApartment |= GetCurrentThreadId() != owner; }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override
    {
        CheckThread(); ++reads; readBeforeStart |= async && starts == 0;
        return failRead ? E_FAIL : shell->GetData(f, m);
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* f, STGMEDIUM* m) override
    { CheckThread(); return shell->GetDataHere(f, m); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override
    { CheckThread(); return shell->QueryGetData(f); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* f, FORMATETC* o) override
    { CheckThread(); return shell->GetCanonicalFormatEtc(f, o); }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) override
    {
        CheckThread();
        if (f && m && f->cfFormat == RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT) &&
            m->tymed == TYMED_HGLOBAL && GlobalSize(m->hGlobal) >= sizeof(DWORD))
        {
            const auto* storedEffect = static_cast<const DWORD*>(GlobalLock(m->hGlobal));
            if (storedEffect) { ++performed; performedEffect = *storedEffect; GlobalUnlock(m->hGlobal); }
        }
        return shell->SetData(f, m, release);
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override
    { CheckThread(); return shell->EnumFormatEtc(direction, out); }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override
    { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override { async = enabled != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override
    { if (!enabled) return E_POINTER; *enabled = async; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override
    { CheckThread(); ++starts; return failStart ? E_FAIL : S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override
    { if (!active) return E_POINTER; *active = starts > ends; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD effect) override
    { CheckThread(); ++ends; outcome = result; endEffect = effect; return S_OK; }
};

inline ComPtr<Source> MakeSource(const std::vector<std::filesystem::path>& paths)
{
    auto source = Microsoft::WRL::Make<Source>();
    std::vector<PIDLIST_ABSOLUTE> absolute;
    std::vector<PCUITEMID_CHILD> children;
    auto parent = ILCreateFromPathW(paths.front().parent_path().c_str());
    Require(parent != nullptr, "source parent PIDL exists");
    for (const auto& path : paths)
    {
        const auto item = ILCreateFromPathW(path.c_str());
        Require(item != nullptr, "selected source PIDL exists");
        absolute.push_back(item); children.push_back(ILFindLastID(item));
    }
    const auto status = SHCreateDataObject(parent, static_cast<UINT>(children.size()),
        children.data(), nullptr, IID_PPV_ARGS(&source->shell));
    ILFree(parent);
    for (auto item : absolute) ILFree(item);
    Require(SUCCEEDED(status), "real Shell data object is available");
    return source;
}

class Window
{
public:
    HWND hwnd = nullptr;
    bool hold = false;
    std::vector<std::unique_ptr<ShellFileOperationUiCompletion>> pending;
    Window()
    {
        WNDCLASSW cls{};
        cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"SnowDesktopAsyncTransferTest";
        Require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
            "register message-only completion window");
        hwnd = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, cls.hInstance, this);
        Require(hwnd != nullptr, "create message-only completion window");
    }
    ~Window()
    {
        Drain();
        if (hwnd) DestroyWindow(hwnd);
    }
    void Drain()
    {
        MSG msg{};
        while (PeekMessageW(&msg, hwnd, completedMessage, completedMessage, PM_REMOVE))
            TakeShellFileOperationCompletion(msg.lParam);
        pending.clear();
    }
    void Deliver(size_t index)
    {
        auto value = std::move(pending.at(index));
        pending.erase(pending.begin() + static_cast<ptrdiff_t>(index));
        value->callback(value->succeeded);
    }
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == completedMessage && self)
        {
            auto result = TakeShellFileOperationCompletion(l);
            if (self->hold) self->pending.push_back(std::move(result));
            else if (result->callback) result->callback(result->succeeded);
            return 0;
        }
        return DefWindowProcW(hwnd, message, w, l);
    }
};

template<class Predicate>
inline void PumpUntil(Predicate ready)
{
    const auto deadline = GetTickCount64() + 10000;
    while (!ready())
    {
        Require(GetTickCount64() < deadline, "bounded async completion reached its observable condition");
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        { TranslateMessage(&message); DispatchMessageW(&message); }
    }
}

struct Batch
{
    ComPtr<Source> source;
    std::filesystem::path destination;
    std::shared_ptr<ShellFileOperationResult> result = std::make_shared<ShellFileOperationResult>();
    DWORD effect = DROPEFFECT_COPY;
    int commits = 0, completed = 0;
    bool success = false, reject = false, notifyRejection = false;
    bool correctWorker = false, correctCommitThread = false;
    bool ownedContent = false;
    std::function<void(const ShellFileOperationResult&)> reconcile;
};

struct Harness
{
    Window window;
    ShellFileOperationWorker reader, files;
    std::stop_source stop;
    DWORD owner = GetCurrentThreadId();
    DWORD Submit(Batch& batch, bool async = true, HWND target = nullptr)
    {
        external_slot_transfer::Callbacks callbacks;
        callbacks.read = [&, async](IDataObject* source, bool download, const auto&) {
            APTTYPE apt{}; APTTYPEQUALIFIER qualifier{};
            batch.correctWorker = !async || (download && GetCurrentThreadId() != owner &&
                SUCCEEDED(CoGetApartmentType(&apt, &qualifier)) && apt == APTTYPE_STA);
            return external_drop_content::Content{
                external_drop_content::ReadFilePaths(source), {}, batch.ownedContent, true};
        };
        callbacks.commit = [&](const auto& content, auto finished, bool synchronously) {
            ++batch.commits; batch.correctCommitThread = GetCurrentThreadId() == owner;
            if (batch.reject)
            {
                if (batch.notifyRejection) finished(false);
                return false;
            }
            auto request = file_backed_drop::BuildRequest(content.paths,
                batch.destination.wstring(), batch.effect, batch.result);
            if (!request) return false;
            if (synchronously)
            {
                const bool success = ShellFileOperationWorker::Execute(*request);
                finished(success);
                return success;
            }
            return files.Enqueue(std::move(*request), [&, finished](bool success) {
                auto completion = std::make_unique<ShellFileOperationUiCompletion>();
                completion->callback = finished;
                PostShellFileOperationCompletion(window.hwnd, completedMessage, std::move(completion), success);
            });
        };
        callbacks.completed = [&](bool success) {
            ++batch.completed; batch.success = success;
            if (batch.reconcile) batch.reconcile(*batch.result);
        };
        return external_slot_transfer::Transfer(batch.source.Get(), batch.effect, async, {},
            external_drop_content::MakeContent(batch.result), reader, stop.get_token(),
            target ? target : window.hwnd, completedMessage, std::move(callbacks));
    }
};

inline std::string Bytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

inline std::vector<std::filesystem::path> Seed(const std::filesystem::path& root, size_t count)
{
    std::filesystem::create_directories(root / L"source");
    std::filesystem::create_directory(root / L"target");
    const auto file = root / L"source" / L"a.txt";
    std::ofstream(file) << "alpha";
    std::vector<std::filesystem::path> result{file};
    if (count == 2)
    {
        const auto folder = root / L"source" / L"folder";
        std::filesystem::create_directory(folder);
        std::ofstream(folder / L"b.txt") << "beta";
        result.push_back(folder);
    }
    return result;
}

inline void AssertSuccess(const Batch& batch, const std::vector<std::filesystem::path>& paths, bool asynchronous = true)
{
    Require(batch.commits == 1 && batch.completed == 1 && batch.success,
        "one accepted transfer commits and completes exactly once");
    Require(batch.correctWorker && batch.correctCommitThread && !batch.source->wrongApartment &&
        !batch.source->readBeforeStart, "decoder runs on worker STA, provider on source STA, commit on destination STA");
    if (asynchronous)
    {
        Require(batch.source->starts == 1 && batch.source->ends == 1 && batch.source->outcome == S_OK,
            "source async operation ends successfully exactly once after commit");
        Require(batch.source->endEffect == (batch.effect == DROPEFFECT_MOVE ? 0 : batch.effect),
            "optimized move reports NONE; copy and link report their selected effect");
    }
    else
        Require(batch.source->starts == 0 && batch.source->ends == 0,
            "synchronous transfer never starts an asynchronous source lease");
    Require(batch.result->outputs.size() == paths.size(), "real Shell outputs cover exactly the selection");
    if (asynchronous && batch.effect == DROPEFFECT_MOVE)
        Require(batch.source->performed == 1 && batch.source->performedEffect == DROPEFFECT_NONE,
            "target-side move prevents the original provider deleting sources twice");
    for (size_t index = 0; index < paths.size(); ++index)
    {
        Require(std::filesystem::exists(paths[index]) == (batch.effect != DROPEFFECT_MOVE),
            "move removes each original; copy and link preserve each original");
        const auto found = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
            [&](const auto& output) { return _wcsicmp(output.source.c_str(), paths[index].c_str()) == 0; });
        Require(found != batch.result->outputs.end(), "each original identity has an actual output record");
        const auto& out = *found;
        Require(std::filesystem::equivalent(std::filesystem::path(out.destination).parent_path(), batch.destination),
            "each output belongs to the captured target directory");
        if (batch.effect == DROPEFFECT_LINK)
        {
            ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
            Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link))) && SUCCEEDED(link.As(&persist)) &&
                SUCCEEDED(persist->Load(out.destination.c_str(), STGM_READ)), "load actual shortcut output");
            std::array<wchar_t, 32768> target{};
            Require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) &&
                std::filesystem::equivalent(target.data(), paths[index]), "shortcut points to the original selected item");
        }
        else
            Require(Bytes(index ? std::filesystem::path(out.destination) / L"b.txt" :
                std::filesystem::path(out.destination)) == (index ? "beta" : "alpha"),
                "asynchronous copy/move preserves file and nested directory bytes");
    }
}

inline void Run(const std::filesystem::path& root)
{
    size_t serial = 0;
    for (DWORD effect : {DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK})
    for (size_t count : {size_t{1}, size_t{2}})
    for (bool asynchronous : {true, false})
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, count);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target"; batch.effect = effect;
        batch.source->async = asynchronous;
        Harness harness;
        const DWORD expectedReturn = !asynchronous && effect == DROPEFFECT_MOVE ? DROPEFFECT_NONE : effect;
        Require(harness.Submit(batch, asynchronous) == expectedReturn,
            "transfer preserves the selected effect and optimized synchronous move reporting");
        if (asynchronous)
        {
            Require(batch.source->reads == 0 && batch.commits == 0 && batch.source->starts == 1,
                "Drop starts the source lease but does not read paths or commit on the caller");
            PumpUntil([&] { return batch.source->ends == 1; });
        }
        AssertSuccess(batch, selected, asynchronous);
    }
    // Failure sequences use unchanged source bytes and an empty destination as
    // their independent oracle, never a production return value as expected IO.
    for (int failure = 0; failure < 6; ++failure)
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, 1);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        Harness harness;
        batch.source->failRead = failure == 0;
        batch.reject = failure == 1 || failure == 5;
        batch.notifyRejection = failure == 5;
        if (failure == 2) harness.reader.Stop();
        if (failure == 3) harness.stop.request_stop();
        batch.source->failStart = failure == 4;
        const DWORD accepted = harness.Submit(batch);
        if (failure == 2 || failure == 4)
            Require(accepted == DROPEFFECT_NONE, "failed start/enqueue rejects the transfer");
        else
            Require(accepted == DROPEFFECT_COPY, "accepted read reports failure through completion");
        if (failure != 4) PumpUntil([&] { return batch.source->ends == 1; });
        Require(batch.source->starts == 1 && batch.source->ends == (failure == 4 ? 0 : 1),
            "failed StartOperation is never ended; each started lease is cancelled once");
        Require(failure == 4 || (batch.source->outcome == E_ABORT && batch.source->endEffect == DROPEFFECT_NONE),
            "read, destination, enqueue and cancellation failures cancel the source operation");
        Require(batch.completed == (failure == 2 || failure == 4 ? 0 : 1),
            "destination failure completes once even when commit both notifies failure and returns rejection");
        Require(batch.commits == (failure == 1 || failure == 5 ? 1 : 0) && std::filesystem::is_empty(batch.destination) &&
            Bytes(selected.front()) == "alpha", "failure performs no unselected file operation");
    }
    // Reverse destination completions for two captured transfers. Reading is
    // serial in production; posted UI completions can be held independently.
    {
        const auto aPath = root / L"reverse-a", bPath = root / L"reverse-b";
        const auto aFiles = Seed(aPath, 1), bFiles = Seed(bPath, 1);
        std::ofstream(bFiles.front()) << "second source";
        Batch a, b; a.source = MakeSource(aFiles); b.source = MakeSource(bFiles);
        a.destination = aPath / L"target"; b.destination = bPath / L"target";
        Harness harness; harness.window.hold = true;
        Require(harness.Submit(a) == DROPEFFECT_COPY && harness.Submit(b) == DROPEFFECT_COPY,
            "two async requests retain independent destination snapshots");
        PumpUntil([&] { return harness.window.pending.size() == 2; });
        harness.window.Deliver(1);
        PumpUntil([&] { return harness.window.pending.size() == 2; });
        harness.window.Deliver(1);
        Require(b.source->ends == 1 && a.source->ends == 0 &&
            Bytes(b.destination / L"a.txt") == "second source" && std::filesystem::is_empty(a.destination),
            "B completes at its own destination while A remains pending");
        harness.window.Deliver(0);
        PumpUntil([&] { return harness.window.pending.size() == 1; });
        harness.window.Deliver(0);
        Require(a.source->ends == 1 && Bytes(a.destination / L"a.txt") == "alpha" &&
            a.commits == 1 && b.commits == 1, "reverse completion never swaps same-name file ownership");
    }
    // The window may disappear after admission. Real PostMessage failure and
    // shutdown draining must release the source lease without committing.
    for (bool destroy : {false, true})
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, 1);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        Harness harness; harness.window.hold = true;
        Require(harness.Submit(batch) == DROPEFFECT_COPY, "window-loss sequence begins with an accepted transfer");
        if (destroy)
        {
            DestroyWindow(harness.window.hwnd);
            harness.window.hwnd = nullptr;
            PumpUntil([&] { return batch.source->ends == 1; });
        }
        else
        {
            PumpUntil([&] { return harness.window.pending.size() == 1; });
            harness.window.Drain();
        }
        Require(batch.source->ends == 1 && batch.source->outcome == E_ABORT && batch.commits == 0 &&
            Bytes(selected.front()) == "alpha" && std::filesystem::is_empty(batch.destination),
            "failed post or discarded completion cancels once and preserves both source and destination");
    }
    for (int outcome = 0; outcome < 3; ++outcome)
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, 1);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        batch.ownedContent = true; batch.reject = outcome == 1;
        {
            Harness harness; harness.window.hold = outcome == 2;
            Require(harness.Submit(batch) == DROPEFFECT_COPY, "owned rendered content starts a tracked transfer");
            if (outcome == 2)
            {
                PumpUntil([&] { return harness.window.pending.size() == 1; });
                harness.window.Drain();
            }
            else
                PumpUntil([&] { return batch.source->ends == 1; });
        }
        Require(!std::filesystem::exists(selected.front()),
            "owned rendered content is released after successful copy, rejection and discarded completion");
        Require(batch.source->ends == 1 && batch.source->outcome == (outcome == 0 ? S_OK : E_ABORT),
            "owned-content lifetime and source completion agree with the actual outcome");
        Require(outcome == 0 ? Bytes(batch.destination / L"a.txt") == "alpha" : std::filesystem::is_empty(batch.destination),
            "rendered-file cleanup preserves successful outputs and creates nothing for cancellation");
    }
    std::cout << "slot transfer protocol, IO, failure, ownership and reverse-completion cases passed\n";
}
}
