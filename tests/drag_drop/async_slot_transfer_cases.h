#pragma once

#include "drag_drop/external_slot_transfer.h"
#include "drag_drop/external_drop_resources.h"
#include "drag_drop/external_file_destination.h"
#include "drag_drop/desktop_file_materialization.h"
#include "drag_drop/file_drop_execution.h"
#include "drag_drop/drop_pipeline.h"
#include "drag_drop/file_backed_drop_request.h"
#include "dock/dock_drop_model.h"
#include "data/dock_layout_codec.h"
#include "common/json_string.h"
#include <wrl/implements.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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

inline std::wstring Normalize(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    return value;
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
    bool copyOnly = false;
    std::optional<external_file_destination::Destination> captured;
    std::function<DesktopWidget*(const std::wstring&, bool)> resolveTarget;
    std::function<void(const DragSourceList&, const DropPreviewList&)> capturePlan;
    int executions = 0, pinSaves = 0;
    bool existingDesktopPaths = false, dockCapacity = true, pinAccepted = true;
    size_t pinnedIndex = static_cast<size_t>(-1);
    std::vector<std::wstring> pinnedPaths;
    std::shared_ptr<pending_drop::Queue> placements = std::make_shared<pending_drop::Queue>();
    std::unordered_set<std::wstring> existingDesktopKeys;
    bool reverseOutputs = false;
    int refreshes = 0;
    std::vector<std::wstring> forgottenKeys;
    std::vector<DesktopWidget> widgets;
    std::vector<DockEntry> dock;
    std::vector<DesktopItem> items;
    bool programsEnabled = false, confirmPrograms = false, confirmConversion = false;
    bool programSaveAccepted = true, exiting = false, conversionPrograms = false;
    int programPrompts = 0, conversionPrompts = 0, programSaveAttempts = 0;
    int layoutSaves = 0, convertedLayouts = 0, viewsCreated = 0, viewsReleased = 0;
    int dockChanges = 0, collectedRefreshes = 0;
    std::function<void(bool)> onConfirmation;
    std::function<void()> savePrograms;
    std::function<bool(const DragSourceList&, const DropPreviewList&)> commitInternal;
};

struct Harness
{
    Window window;
    ShellFileOperationWorker reader, files;
    dock_drop_rules::MaterializedPathReservations reservations;
    std::stop_source stop;
    DWORD owner = GetCurrentThreadId();
    drop_pipeline::Services PipelineServices(Batch& batch, decltype(drop_pipeline::Services::files) executeFiles)
    {
        drop_pipeline::Services pipeline;
        pipeline.containsPrograms = [](const DragSourceList& list) {
            return drop_pipeline::ContainsPrograms(list, {}, [](const auto&) -> DesktopItem* { return nullptr; }, Normalize);
        };
        pipeline.confirmConversion = [&](bool programs) {
            ++batch.conversionPrompts; batch.conversionPrograms = programs;
            if (batch.onConfirmation) batch.onConfirmation(true);
            return batch.confirmConversion;
        };
        pipeline.confirmPrograms = [&] {
            ++batch.programPrompts;
            if (batch.onConfirmation) batch.onConfirmation(false);
            return batch.confirmPrograms;
        };
        pipeline.exiting = [&] { return batch.exiting; };
        pipeline.runtimeLive = [](Container*) { return true; };
        pipeline.createConvertedView = [&](DesktopWidget*) {
            ++batch.viewsCreated;
            std::shared_ptr<int> lifetime(new int(1), [&batch](int* value) { ++batch.viewsReleased; delete value; });
            return drop_pipeline::ConvertedView{lifetime, nullptr};
        };
        pipeline.saveLayout = [&] { ++batch.layoutSaves; };
        pipeline.convertedLayout = [&] { ++batch.convertedLayouts; ++batch.layoutSaves; };
        pipeline.programsEnabled = [&] { return batch.programsEnabled; };
        pipeline.enablePrograms = [&] {
            ++batch.programSaveAttempts;
            if (!batch.programSaveAccepted) return false;
            if (batch.savePrograms) batch.savePrograms();
            batch.programsEnabled = true;
            return true;
        };
        pipeline.autoCollectSource = [&](const auto& list) { return drop_pipeline::AutoCollectSource(list, batch.widgets); };
        pipeline.normalize = Normalize;
        pipeline.refreshCollectedKeys = [&] { ++batch.collectedRefreshes; };
        pipeline.dockChanged = [&] { ++batch.dockChanges; };
        pipeline.files = executeFiles;
        pipeline.internal = [&](const auto& list, const auto& preview) -> bool {
            if (batch.commitInternal) return batch.commitInternal(list, preview);
            throw std::runtime_error("external files cannot execute the internal-only model route");
        };
        pipeline.collectionTitle = L"Collection"; pipeline.filesTitle = L"Desktop files";
        return pipeline;
    }
    decltype(drop_pipeline::Services::files) FileExecutor(Batch& batch,
        decltype(file_drop_execution::Services::fileName) fileName = {},
        decltype(file_drop_execution::Services::pin) pin = {},
        decltype(file_drop_execution::Services::savePins) afterPin = {})
    {
        if (!fileName) fileName = [](const auto& path) { return std::filesystem::path(path).filename().wstring(); };
        return [this, &batch, fileName = std::move(fileName), pin = std::move(pin), afterPin = std::move(afterPin)](const DragSourceList& sources, const DropPreviewList& preview,
            auto done, bool sync, auto outputs) {
            ++batch.executions;
            if (batch.capturePlan) batch.capturePlan(sources, preview);
            desktop_file_materialization::Services materialization;
            materialization.enqueue = [&](auto request, auto finishedRequest) {
                return files.Enqueue(std::move(request), [&, finishedRequest](bool success) {
                    if (batch.reverseOutputs) std::reverse(batch.result->outputs.begin(), batch.result->outputs.end());
                    auto completion = std::make_unique<ShellFileOperationUiCompletion>();
                    completion->callback = finishedRequest;
                    PostShellFileOperationCompletion(window.hwnd, completedMessage, std::move(completion), success);
                });
            };
            materialization.isStaging = [](const auto&) { return false; };
            materialization.pathsEqual = [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; };
            materialization.normalize = Normalize;
            materialization.copyName = [](const auto& stem, const auto& extension, int index) {
                return stem + L" copy " + std::to_wstring(index) + extension;
            };
            materialization.shortcutName = L"Shortcut";
            file_drop_execution::Services execution;
            execution.snapshotDesktopKeys = [&] { return batch.existingDesktopKeys; };
            execution.folderPlacement = [normalize = materialization.normalize](const auto& widget, size_t index, const DragSourceList* list) {
                return pending_drop::BuildFolder(widget, index, list, widget.id, {},
                    normalize,
                    [](const auto& path) { return std::filesystem::path(path).filename().wstring(); });
            };
            execution.fileName = fileName;
            execution.pathsEqual = materialization.pathsEqual;
            execution.desktop = [&, materialization](const auto& list, DropAction action, bool duplicate,
                auto finishedRequest, bool synchronous, auto result) {
                return desktop_file_materialization::Materialize(list, action, duplicate, batch.destination.wstring(),
                    reservations, materialization, nullptr, std::move(finishedRequest), synchronous, std::move(result));
            };
            execution.folder = [materialization](const auto& list, const auto& directory, DropAction action,
                auto finishedRequest, bool synchronous, auto result) {
                return desktop_file_materialization::MaterializeFolder(list, directory, action, materialization,
                    std::move(finishedRequest), synchronous, std::move(result));
            };
            execution.pin = pin;
            execution.savePins = afterPin;
            execution.forgetMovedKeys = [&](const auto& keys) { batch.forgottenKeys = keys; };
            execution.refresh = [&] { ++batch.refreshes; };
            return file_drop_execution::Execute(sources, preview, *batch.placements,
                std::move(execution), std::move(done), sync, std::move(outputs));
        };
    }

    DWORD Submit(Batch& batch, bool async = true, HWND target = nullptr)
    {
        external_slot_transfer::Callbacks callbacks;
        callbacks.read = [&, async](IDataObject* source, bool download, const auto&) {
            APTTYPE apt{}; APTTYPEQUALIFIER qualifier{};
            batch.correctWorker = !async || (download && GetCurrentThreadId() != owner &&
                SUCCEEDED(CoGetApartmentType(&apt, &qualifier)) && apt == APTTYPE_STA);
            return external_drop_content::Content{
                external_drop_content::ReadFilePaths(source), {}, batch.ownedContent,
                batch.ownedContent || batch.copyOnly};
        };
        callbacks.commit = [&](const auto& content, auto finished, bool synchronously) {
            ++batch.commits; batch.correctCommitThread = GetCurrentThreadId() == owner;
            if (batch.reject)
            {
                if (batch.notifyRejection) finished(false);
                return false;
            }
            external_file_destination::Services services;
            services.resolveTarget = [&](const auto& id, bool popup) {
                return batch.resolveTarget ? batch.resolveTarget(id, popup) : nullptr;
            };
            services.desktopPreview = [](const GridCell& anchor, size_t count) {
                DropPreviewList preview;
                preview.targetKind = DropTargetKind::Desktop;
                preview.anchorCell = anchor;
                for (size_t index = 0; index < count; ++index)
                {
                    DropLanding landing; landing.kind = DropLandingKind::DesktopCell;
                    landing.sourceIndex = index; landing.cell = anchor;
                    landing.cell.row += static_cast<int>(index);
                    preview.landings.push_back(landing);
                }
                return preview;
            };
            services.canPinExisting = [&](const auto&) { return batch.existingDesktopPaths; };
            services.pinExisting = [&](const auto& paths, size_t index) {
                dock_drop_model::Services model;
                model.normalize = Normalize;
                model.recycle = [](const auto& entry) { return entry.reference == kDesktopIconClsidRecycleBin; };
                model.folder = [](const auto& entry) { return entry.type == DockEntryType::FolderMapping; };
                model.capacity = [&](size_t) { return batch.pinAccepted; };
                model.item = [&](const auto& key) -> DesktopItem* {
                    for (auto& item : batch.items) if (Normalize(item.layoutKey) == key) return &item;
                    return nullptr;
                };
                if (!dock_drop_model::AddMaterialized({batch.dock, batch.widgets}, paths, index, false, model)) return false;
                batch.pinnedPaths = paths; batch.pinnedIndex = index;
                return true;
            };
            services.dockHasCapacity = [&](size_t) { return batch.dockCapacity; };
            services.afterPin = [&] {
                ++batch.pinSaves;
                std::ostringstream stream;
                stream << "{\"schemaVersion\":1,\"dockEntries\":[";
                const auto ordered = dock_folder_rules::EntriesInUserOrder(batch.dock);
                for (size_t index = 0; index < ordered.size(); ++index)
                {
                    if (index) stream << ',';
                    dock_layout_codec::Write(stream, *ordered[index], [](const auto& key) {
                        const auto utf8 = std::filesystem::path(key).u8string();
                        return json::EscapeString(std::string(utf8.begin(), utf8.end()));
                    });
                }
                stream << "]}";
                Require(layout_storage::SaveDocument(batch.destination.parent_path() / L"dock-layout.json", stream.str()),
                    "accepted Dock pin persists the actual committed entries atomically");
            };
            services.fileName = [](const auto& path) { return std::filesystem::path(path).filename().wstring(); };
            auto executeFiles = FileExecutor(batch, services.fileName, services.pinExisting, services.afterPin);
            services.execute = [&, executeFiles](const auto& sources, const auto& preview, auto done, bool sync, auto outputs) {
                auto pipeline = PipelineServices(batch, executeFiles);
                return drop_pipeline::Execute(sources, preview, batch.widgets, batch.dock, pipeline,
                    std::move(done), sync, std::move(outputs));
            };
            external_file_destination::Destination destination;
            if (batch.captured) destination = *batch.captured;
            else
            {
                destination.preview.targetKind = DropTargetKind::Desktop;
                destination.preview.anchorCell = {L"test-page", 2, 1};
                destination.preview.action = batch.effect == DROPEFFECT_MOVE ? DropAction::Move :
                    batch.effect == DROPEFFECT_LINK ? DropAction::Link : DropAction::Copy;
            }
            return external_file_destination::Commit(destination, content.paths, content.owned, content.copyOnly,
                finished, synchronously, batch.result, services);
        };
        callbacks.completed = [&](bool success) {
            ++batch.completed; batch.success = success;
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

inline void SeedDock(Batch& batch)
{
    batch.dock = {{DockEntryType::DesktopItem, L"EXISTING-0", true},
        {DockEntryType::DesktopItem, L"EXISTING-1", true}};
}

inline void CheckDock(const Batch& batch, const std::vector<std::wstring>& paths, bool accepted)
{
    std::vector<std::wstring> expected{L"EXISTING-0", L"EXISTING-1"};
    if (accepted) for (const auto& path : paths) expected.push_back(Normalize(path));
    std::vector<std::wstring> actual;
    for (const auto& entry : batch.dock) actual.push_back(entry.reference);
    Require(actual == expected, "real Dock model contains exactly the selected output identities at the captured insertion boundary");
    const auto documentPath = batch.destination.parent_path() / L"dock-layout.json";
    if (!accepted)
    {
        Require(!std::filesystem::exists(documentPath), "rejected Dock model does not publish a successful layout save");
        return;
    }
    layout_storage::Document document;
    Require(layout_storage::LoadDocument(documentPath, document).status == layout_storage::LoadStatus::LoadedPrimary &&
        document.dockEntries.size() == expected.size(), "actual Dock identities survive the shared atomic layout storage");
    for (size_t index = 0; index < expected.size(); ++index)
    {
        const auto entry = dock_layout_codec::Restore(document.dockEntries[index], batch.widgets,
            [](const auto& text) { return std::filesystem::path(std::u8string(text.begin(), text.end())).wstring(); }, Normalize);
        Require(entry.reference == expected[index] && entry.type == DockEntryType::DesktopItem &&
            entry.keepOnDesktop == (index < 2), "Dock reload preserves original entry order and each mapping versus exclusive identity");
    }
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
        batch.capturePlan = [&](const DragSourceList& sources, const DropPreviewList& preview) {
            Require(sources.entries.size() == count && preview.landings.size() == count &&
                preview.anchorCell.pageId == L"test-page" && preview.anchorCell.column == 2 &&
                preview.anchorCell.row == 1 && preview.fileBacked,
                "delayed batch expansion preserves the captured desktop anchor and all selected files");
        };
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
    // S06: the original destination changes while its external source is
    // reading. Resume the real posted UI commit only after that mutation.
    for (int mutation = 0; mutation < 5; ++mutation)
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, 2);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        batch.effect = DROPEFFECT_MOVE;
        DesktopWidget widget; widget.id = L"captured-target"; widget.type = DesktopWidgetType::FolderMapping;
        widget.sourceFolderPath = batch.destination.wstring(); widget.itemKeys = {L"existing-member"};
        external_file_destination::Destination destination;
        destination.widgetId = widget.id; destination.widgetType = widget.type;
        destination.folderPath = widget.sourceFolderPath; destination.dockFolderPopup = mutation == 3;
        destination.preview.targetKind = DropTargetKind::FolderMapping;
        destination.preview.action = DropAction::Move;
        batch.captured = destination;
        bool exists = true, popupOpen = true;
        batch.resolveTarget = [&](const auto& id, bool popup) -> DesktopWidget* {
            return exists && (!popup || popupOpen) && id == L"captured-target" ? &widget : nullptr;
        };
        Harness harness; harness.window.hold = true;
        Require(harness.Submit(batch) == DROPEFFECT_MOVE, "target mutation begins after admitting a real move");
        PumpUntil([&] { return harness.window.pending.size() == 1; });
        if (mutation == 0) exists = false;
        if (mutation == 1) widget.type = DesktopWidgetType::Collection;
        if (mutation == 2)
        {
            widget.sourceFolderPath = (path / L"different-folder").wstring();
            std::filesystem::create_directory(widget.sourceFolderPath);
        }
        if (mutation == 3) popupOpen = false;
        if (mutation == 4) widget.id = L"replacement-target";
        harness.window.Deliver(0);
        Require(batch.source->ends == 1 && batch.source->outcome == E_ABORT &&
            batch.completed == 1 && !batch.success && batch.executions == 0 &&
            batch.result->outputs.empty() && std::filesystem::is_empty(batch.destination) &&
            Bytes(selected[0]) == "alpha" && Bytes(selected[1] / L"b.txt") == "beta" &&
            widget.itemKeys == std::vector<std::wstring>{L"existing-member"},
            "changed destination cancels before file execution and preserves both files and membership");
    }
    // Pinning an existing desktop identity must not produce another physical
    // copy or shortcut. Keep the actual Dock model commit and layout codec.
    for (bool accepted : {true, false})
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, 2);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        batch.effect = DROPEFFECT_LINK; batch.existingDesktopPaths = true; batch.pinAccepted = accepted;
        SeedDock(batch);
        external_file_destination::Destination destination;
        destination.preview.pinMaterializedItemsToDock = true;
        destination.preview.dockInsertIndex = 2; destination.preview.action = DropAction::Link;
        batch.captured = destination;
        Harness harness;
        Require(harness.Submit(batch) == DROPEFFECT_LINK, "existing desktop identity reaches captured Dock admission");
        PumpUntil([&] { return batch.source->ends == 1; });
        Require(batch.success == accepted && batch.completed == 1 && batch.executions == 0 &&
            batch.pinSaves == (accepted ? 1 : 0) && batch.result->outputs.empty() &&
            std::filesystem::is_empty(batch.destination) && Bytes(selected[0]) == "alpha" &&
            Bytes(selected[1] / L"b.txt") == "beta",
            "Dock identity pinning preserves original files and never performs a physical file operation");
        if (accepted)
            Require(batch.pinnedPaths == std::vector<std::wstring>{selected[0].wstring(), selected[1].wstring()} &&
                batch.pinnedIndex == 2, "Dock identity pinning preserves the original selection and insertion boundary");
        CheckDock(batch, {selected[0].wstring(), selected[1].wstring()}, accepted);
    }
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, 2);
        Batch batch; batch.source = MakeSource(selected); batch.destination = path / L"target";
        batch.effect = DROPEFFECT_LINK; batch.dockCapacity = false;
        external_file_destination::Destination destination;
        destination.preview.pinMaterializedItemsToDock = true;
        destination.preview.action = DropAction::Link;
        batch.captured = destination;
        Harness harness;
        Require(harness.Submit(batch) == DROPEFFECT_LINK, "Dock capacity is rechecked after delayed reading");
        PumpUntil([&] { return batch.source->ends == 1; });
        Require(!batch.success && batch.completed == 1 && batch.executions == 0 &&
            batch.source->outcome == E_ABORT && std::filesystem::is_empty(batch.destination) &&
            Bytes(selected[0]) == "alpha" && Bytes(selected[1] / L"b.txt") == "beta",
            "insufficient Dock capacity rejects before materializing files or modifying originals");
    }
    // Files can be materialized successfully while the final Dock commit
    // rejects (for example after capacity changes). Physical success must not
    // masquerade as a successful Dock drop, and generated files must survive.
    for (bool asynchronous : {true, false})
    for (bool accepted : {true, false})
    for (size_t count : {size_t{1}, size_t{2}})
    {
        const auto path = root / std::to_wstring(serial++);
        const auto selected = Seed(path, count);
        Batch batch; batch.source = MakeSource(selected); batch.source->async = asynchronous;
        batch.destination = path / L"target"; batch.effect = DROPEFFECT_LINK;
        batch.pinAccepted = accepted; batch.reverseOutputs = true;
        SeedDock(batch);
        external_file_destination::Destination destination;
        destination.preview.targetKind = DropTargetKind::Desktop;
        destination.preview.pinMaterializedItemsToDock = true;
        destination.preview.dockInsertIndex = 2; destination.preview.action = DropAction::Link;
        batch.captured = destination;
        Harness harness;
        const auto returned = harness.Submit(batch, asynchronous);
        const DWORD expectedReturn = !asynchronous && !accepted ? DROPEFFECT_NONE : DROPEFFECT_LINK;
        Require(returned == expectedReturn,
            "synchronous Dock completion includes final pin success, while async completion remains deferred");
        if (asynchronous) PumpUntil([&] { return batch.source->ends == 1; });
        Require(batch.completed == 1 && batch.success == accepted && batch.executions == 1 &&
            batch.pinSaves == (accepted ? 1 : 0) && batch.refreshes == 1 && batch.placements->empty(),
            "materialized Dock rejection is reported once without publishing success or losing real output refresh");
        Require(batch.result->outputs.size() == count && Bytes(selected[0]) == "alpha" &&
            (count == 1 || Bytes(selected[1] / L"b.txt") == "beta"),
            "materialized Dock success and rejection both preserve all original files");
        std::vector<std::wstring> expectedPins;
        for (const auto& file : selected)
        {
            const auto output = std::find_if(batch.result->outputs.begin(), batch.result->outputs.end(),
                [&](const auto& value) { return value.source == file.wstring(); });
            Require(output != batch.result->outputs.end() && std::filesystem::exists(output->destination) &&
                std::filesystem::equivalent(item_location::ResolveRevealPath(output->destination), file),
                "successful physical output remains a real usable shortcut even when the final Dock pin rejects");
            expectedPins.push_back(output->destination);
        }
        Require(accepted ? batch.pinnedPaths == expectedPins && batch.pinnedIndex == 2 : batch.pinnedPaths.empty(),
            "materialized Dock commit preserves captured source order and insertion boundary despite reverse receipts");
        CheckDock(batch, expectedPins, accepted);
        if (asynchronous)
            Require(batch.source->outcome == (accepted ? S_OK : E_ABORT),
                "OLE final status reflects the complete Dock transaction rather than only file materialization");
    }
    std::cout << "slot transfer protocol, IO, failure, ownership and reverse-completion cases passed\n";
}
}
