#pragma once

#include <restartmanager.h>

#include "drag_drop/drop_staging.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincodec.h>
#include <map>
#include <set>
#include <thread>

namespace native_owned_content_cases
{
using namespace native_ole_file_cases;

// The external producer is the only substitute. Standard HGLOBAL/stream data,
// COM marshaling, source leases, host routing, decoders, file submission and
// persistence remain production paths.
class Source final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject, IDataObjectAsyncCapability>
{
public:
    std::map<CLIPFORMAT, std::vector<std::byte>> formats;
    std::vector<std::string> contents;
    bool asynchronous = false, wrongApartment = false, readBeforeStart = false;
    int failedContent = -1, starts = 0, ends = 0, reads = 0, contentReads = 0;
    DWORD owner = GetCurrentThreadId(), endEffect = 0xffffffff;
    HRESULT outcome = E_PENDING;
    void CheckThread() { wrongApartment |= owner != GetCurrentThreadId(); }
    static CLIPFORMAT Named(const wchar_t* name) { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)); }
    void Text(CLIPFORMAT format, std::wstring_view text)
    {
        auto& bytes = formats[format]; bytes.resize((text.size() + 1) * sizeof(wchar_t));
        memcpy(bytes.data(), text.data(), text.size() * sizeof(wchar_t));
    }
    void Image()
    {
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header); header.biWidth = header.biHeight = 1;
        header.biPlanes = 1; header.biBitCount = 24; header.biCompression = BI_RGB; header.biSizeImage = 4;
        auto& bytes = formats[CF_DIB]; bytes.resize(sizeof(header) + 4);
        memcpy(bytes.data(), &header, sizeof(header));
        bytes[sizeof(header)] = std::byte{0x10}; bytes[sizeof(header) + 1] = std::byte{0x20};
        bytes[sizeof(header) + 2] = std::byte{0x30};
    }
    void Virtual(std::size_t count, bool incomplete)
    {
        auto& bytes = formats[Named(CFSTR_FILEDESCRIPTORW)];
        bytes.resize(offsetof(FILEGROUPDESCRIPTORW, fgd) + count * sizeof(FILEDESCRIPTORW));
        auto* group = reinterpret_cast<FILEGROUPDESCRIPTORW*>(bytes.data()); group->cItems = static_cast<UINT>(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            contents.push_back("virtual-bytes-" + std::to_string(index));
            auto& descriptor = group->fgd[index]; descriptor.dwFlags = FD_FILESIZE | FD_ATTRIBUTES;
            descriptor.dwFileAttributes = FILE_ATTRIBUTE_NORMAL; descriptor.nFileSizeLow = static_cast<DWORD>(contents.back().size());
            const auto name = L"Virtual-" + std::to_wstring(index) + L".dat";
            wcscpy_s(descriptor.cFileName, name.c_str());
        }
        if (incomplete) failedContent = static_cast<int>(count) - 1;
    }
    void Files(const std::vector<fs::path>& paths)
    {
        std::wstring names;
        for (const auto& path : paths) { names += path.wstring(); names.push_back(L'\0'); }
        names.push_back(L'\0');
        auto& bytes = formats[CF_HDROP]; bytes.resize(sizeof(DROPFILES) + names.size() * sizeof(wchar_t));
        auto* drop = reinterpret_cast<DROPFILES*>(bytes.data()); drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
        memcpy(bytes.data() + sizeof(DROPFILES), names.data(), names.size() * sizeof(wchar_t));
    }
    void ShellNamespaceAlternative(const fs::path& file = {})
    {
        const auto parsing = file.empty() ? std::wstring(L"::") + kDesktopIconClsidRecycleBin : file.wstring();
        PIDLIST_ABSOLUTE absolute = nullptr;
        Require(SUCCEEDED(SHParseDisplayName(parsing.c_str(), nullptr, &absolute, 0, nullptr)) && absolute,
            "resolve a known Shell namespace representation without enumerating the desktop");
        Pidl item(absolute), parent(ILClone(item.get()));
        Require(parent.get() && ILRemoveLastID(parent.get()), "retain the real CIDA parent identity");
        PCUITEMID_CHILD child = ILFindLastID(item.get());
        ComPtr<IDataObject> shell;
        Require(SUCCEEDED(SHCreateDataObject(parent.get(), 1, &child, nullptr, IID_PPV_ARGS(&shell))),
            "obtain the actual Shell CIDA representation for the complete virtual batch");
        FORMATETC format{Named(CFSTR_SHELLIDLIST), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        struct Medium
        {
            STGMEDIUM data{};
            ~Medium() { if (data.tymed != TYMED_NULL) ReleaseStgMedium(&data); }
        } medium;
        Require(shell->QueryGetData(&format) == S_OK && SUCCEEDED(shell->GetData(&format, &medium.data)) &&
            medium.data.tymed == TYMED_HGLOBAL && medium.data.hGlobal,
            "the auxiliary CIDA must be a real queryable and readable Shell payload");
        const auto size = GlobalSize(medium.data.hGlobal);
        Require(size >= offsetof(CIDA, aoffset) + 2 * sizeof(UINT), "actual CIDA carries a parent and one child PIDL");
        const void* bytes = GlobalLock(medium.data.hGlobal);
        Require(bytes != nullptr, "lock the actual auxiliary Shell representation for copying");
        auto& output = formats[format.cfFormat];
        try { output.assign(static_cast<const std::byte*>(bytes), static_cast<const std::byte*>(bytes) + size); }
        catch (...) { GlobalUnlock(medium.data.hGlobal); throw; }
        GlobalUnlock(medium.data.hGlobal);
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
    {
        CheckThread(); ++reads;
        if (!format || !medium) return E_POINTER;
        *medium = {};
        const std::byte* bytes = nullptr; std::size_t size = 0; bool stream = false;
        if (format->cfFormat == Named(CFSTR_FILECONTENTS))
        {
            ++contentReads;
            if (format->lindex < 0 || static_cast<std::size_t>(format->lindex) >= contents.size() || format->lindex == failedContent)
                return DV_E_LINDEX;
            const auto& content = contents[static_cast<std::size_t>(format->lindex)];
            bytes = reinterpret_cast<const std::byte*>(content.data()); size = content.size();
            stream = (format->tymed & TYMED_ISTREAM) != 0;
        }
        else
        {
            const auto found = formats.find(format->cfFormat);
            if (found == formats.end() || !(format->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
            bytes = found->second.data(); size = found->second.size();
        }
        // Native synchronous Shell binding may inspect a CIDA during Drop.
        // Delayed file bytes on the asynchronous decoder path still require
        // its source lease before extraction; asynchronous hover forbids reads.
        readBeforeStart |= asynchronous && format->cfFormat == Named(CFSTR_FILECONTENTS) && starts == 0;
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, std::max<std::size_t>(size, 1));
        if (!memory) return E_OUTOFMEMORY;
        void* destination = GlobalLock(memory);
        if (!destination) { GlobalFree(memory); return E_OUTOFMEMORY; }
        if (size) memcpy(destination, bytes, size);
        GlobalUnlock(memory);
        if (stream)
        {
            IStream* result = nullptr;
            const HRESULT status = CreateStreamOnHGlobal(memory, TRUE, &result);
            if (FAILED(status)) { GlobalFree(memory); return status; }
            ULARGE_INTEGER length{}; length.QuadPart = size;
            if (FAILED(result->SetSize(length))) { result->Release(); return E_FAIL; }
            medium->tymed = TYMED_ISTREAM; medium->pstm = result;
        }
        else { medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
    {
        CheckThread();
        if (!format) return E_POINTER;
        if (format->cfFormat == Named(CFSTR_FILECONTENTS))
            return (format->tymed & (TYMED_HGLOBAL | TYMED_ISTREAM)) && !contents.empty() ? S_OK : DV_E_FORMATETC;
        return (format->tymed & TYMED_HGLOBAL) && formats.contains(format->cfFormat) ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { CheckThread(); return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override
    { if (output) output->ptd = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM* medium, BOOL release) override
    { CheckThread(); if (release && medium) ReleaseStgMedium(medium); return S_OK; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override
    {
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> offered;
        for (const auto& [format, bytes] : formats) { (void)bytes; offered.push_back({format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}); }
        if (!contents.empty()) offered.push_back({Named(CFSTR_FILECONTENTS), nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM | TYMED_HGLOBAL});
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(offered.size()), offered.data(), output);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL enabled) override { asynchronous = enabled != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* enabled) override { if (!enabled) return E_POINTER; *enabled = asynchronous; return S_OK; }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override { CheckThread(); ++starts; return S_OK; }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* active) override { if (!active) return E_POINTER; *active = starts > ends; return S_OK; }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD effect) override
    { CheckThread(); ++ends; outcome = result; endEffect = effect; return S_OK; }
};

// The loopback origin supplies only HTTP response bytes. The real URL worker
// interprets response type, downloads or creates the persistent .url output.
class Origin
{
    bool started_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    unsigned short port_ = 0;
    std::jthread worker_;
public:
    Origin()
    {
        WSADATA data{}; Require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "initialize isolated content HTTP origin"); started_ = true;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(listener_ != INVALID_SOCKET, "create content loopback listener");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener_, SOMAXCONN) == 0,
            "bind an ephemeral content loopback port");
        int size = sizeof(address); Require(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size) == 0, "read isolated HTTP port");
        port_ = ntohs(address.sin_port);
        worker_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested())
            {
                fd_set readable; FD_ZERO(&readable); FD_SET(listener_, &readable); timeval timeout{0, 50000};
                if (select(0, &readable, nullptr, nullptr, &timeout) <= 0) continue;
                const SOCKET client = accept(listener_, nullptr, nullptr); if (client == INVALID_SOCKET) continue;
                DWORD ioTimeout = 1500;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ioTimeout), sizeof(ioTimeout));
                std::string request; char buffer[2048];
                while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
                { const int received = recv(client, buffer, sizeof(buffer), 0); if (received <= 0) break; request.append(buffer, static_cast<std::size_t>(received)); }
                const bool asset = request.find(" /asset ") != std::string::npos;
                const std::string body = asset ? "downloaded-resource-bytes" : "<html><body>local page</body></html>";
                const std::string headers = asset ? "Content-Type: application/octet-stream\r\nContent-Disposition: attachment; filename=payload.dat\r\n"
                    : "Content-Type: text/html; charset=utf-8\r\n";
                const std::string response = "HTTP/1.1 200 OK\r\n" + headers + "Content-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                for (std::size_t offset = 0; offset < response.size();)
                { const int sent = send(client, response.data() + offset, static_cast<int>(response.size() - offset), 0); if (sent <= 0) break; offset += static_cast<std::size_t>(sent); }
                closesocket(client);
            }
        });
    }
    ~Origin() { worker_.request_stop(); if (worker_.joinable()) worker_.join(); if (listener_ != INVALID_SOCKET) closesocket(listener_); if (started_) WSACleanup(); }
    std::wstring Url(bool asset) const { return L"http://127.0.0.1:" + std::to_wstring(port_) + (asset ? L"/asset" : L"/page"); }
};

inline void CheckImage(const fs::path& path)
{
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICBitmapDecoder> decoder; ComPtr<IWICBitmapFrameDecode> frame; ComPtr<IWICFormatConverter> converter;
    Require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)),
        "D08: accepted image must be a complete independently decodable file");
    GUID container{}; UINT width = 0, height = 0;
    Require(SUCCEEDED(decoder->GetContainerFormat(&container)) && container == GUID_ContainerFormatPng &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width == 1 && height == 1,
        "D08: image materialization must preserve dimensions in a real PNG container");
    Require(SUCCEEDED(factory->CreateFormatConverter(&converter)) && SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)), "decode actual accepted image into independent RGBA pixels");
    std::array<std::uint8_t, 4> actual{};
    Require(SUCCEEDED(converter->CopyPixels(nullptr, 4, 4, actual.data())) && actual == std::array<std::uint8_t, 4>{0x30, 0x20, 0x10, 0xff},
        "D08: actual PNG pixels must retain the independently specified source color");
}
inline std::set<fs::path> Staged()
{
    std::set<fs::path> result;
    for (const auto& entry : fs::directory_iterator(snowdesktop::drop_staging::Directory()))
        if (entry.path().filename() != L".owner.lock") result.insert(entry.path());
    return result;
}

inline std::string LockOwners(const fs::path& path)
{
    const HMODULE module = LoadLibraryW(L"rstrtmgr.dll");
    if (!module) return "RestartManager unavailable";
    const auto start = reinterpret_cast<decltype(&RmStartSession)>(GetProcAddress(module, "RmStartSession"));
    const auto add = reinterpret_cast<decltype(&RmRegisterResources)>(GetProcAddress(module, "RmRegisterResources"));
    const auto list = reinterpret_cast<decltype(&RmGetList)>(GetProcAddress(module, "RmGetList"));
    const auto end = reinterpret_cast<decltype(&RmEndSession)>(GetProcAddress(module, "RmEndSession"));
    std::string result = "current=" + std::to_string(GetCurrentProcessId()) + " holders=";
    DWORD session = 0; wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    if (start && add && list && end && start(&session, 0, key) == ERROR_SUCCESS)
    {
        const wchar_t* resources[]{path.c_str()};
        if (add(session, 1, resources, 0, nullptr, 0, nullptr) == ERROR_SUCCESS)
        {
            UINT needed = 0, count = 0; DWORD reasons = 0;
            if (list(session, &needed, &count, nullptr, &reasons) == ERROR_MORE_DATA)
            {
                std::vector<RM_PROCESS_INFO> processes(needed); count = needed;
                if (list(session, &needed, &count, processes.data(), &reasons) == ERROR_SUCCESS)
                    for (UINT index = 0; index < count; ++index) result += std::to_string(processes[index].Process.dwProcessId) + ",";
            }
        }
        end(session);
    }
    FreeLibrary(module);
    return result;
}

inline void RemoveOwnedOutput(const fs::path& desktop, const fs::path& path)
{
    Require(fs::canonical(path).parent_path() == fs::canonical(desktop), "content cleanup requires one owned direct desktop child");
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool reported = false;
    // Stop cancels BackgroundWork delivery without joining its Shell provider.
    // Await actual resource retirement, then issue one delete by that handle.
    Wait([&] {
        handle = CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE) return true;
        const auto error = GetLastError();
        Require(error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION,
            "owned output cleanup may wait only for an existing reader's sharing lifetime");
        if (!reported) { std::cerr << "A26 cleanup sharing " << error << ' ' << LockOwners(path) << '\n'; reported = true; }
        return false;
    }, "actual Shell readers must retire before owned content cleanup");
    FILE_DISPOSITION_INFO disposition{TRUE};
    const bool removed = SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)) != FALSE;
    CloseHandle(handle);
    Require(removed, "delete owned content exactly once through its acquired resource handle");
    Wait([&] { return !fs::exists(path); }, "owned output deletion must complete after Shell readers retire");
}

struct Scenario { const char* name; int kind; std::size_t count; };
inline constexpr Scenario scenarios[]{
    {"incomplete-virtual-preview", 6, 0}, // First: the known desktop fallback risk.
    {"plain-text", 0, 1}, {"image", 1, 1}, {"inline-data", 2, 1},
    {"single-virtual", 3, 1}, {"virtual-batch", 4, 2}, {"virtual-preview", 5, 2},
    {"image-before-url", 7, 1}, {"page-url", 8, 1}, {"resource-url", 9, 1}, {"files-before-image", 10, 1},
};

inline void Run(std::string_view targetName, bool cancel = false, bool rightCopy = false)
{
    const bool keyed = targetName == "collection", mapped = targetName == "mapping", dock = targetName == "dock";
    Require(targetName == "desktop" || keyed || mapped || dock, "D08: independently declared physical destination required");
    native_drag_commit_cases::Profile profile; Origin origin;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop; fs::create_directories(desktop);
    std::size_t scenes = 0;
    for (const auto& scenario : scenarios)
    for (const bool asynchronous : {false, true})
    {
        if (cancel && scenario.kind != 0 && scenario.kind != 1 && scenario.kind != 4 && scenario.kind != 8) continue;
        // Menu dispatch is common across owned formats; a complete multi-file
        // virtual source checks the accepted decision through the real reader
        // and final commit without repeating every format by every gesture.
        if (rightCopy && scenario.kind != 4) continue;
        native_drag_commit_cases::context = "A26 owned-content target=" + std::string(targetName) + " format=" + scenario.name +
            " async=" + std::to_string(asynchronous) + " cancel=" + std::to_string(cancel) + " rightCopy=" + std::to_string(rightCopy);
        const auto directory = profile.root / (L"content-" + std::to_wstring(scenes++)); fs::create_directories(directory / L"mapped");
        const auto original = (mapped ? directory / L"mapped" : desktop) / L"Existing-Target.txt";
        { std::ofstream stream(original, std::ios::binary); stream << "existing-target"; }
        const auto borrowed = directory / L"Borrowed.txt"; { std::ofstream stream(borrowed, std::ios::binary); stream << "borrowed-original"; }
        const auto borrowedIdentity = native_dock_return_cases::Identity(borrowed);
        auto& app = profile.Model(false, directory / L"layout");
        if (keyed || mapped) Access::SeedExternalStorage(app, mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection,
            mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection, false, original);
        else { Access::PopulateModel(app, {original}, 0); if (dock) Access::SeedDock(app); }
        Access::PrepareReload(app); Access::BindAsync(app); Require(Access::Save(app), "save actual content ingress baseline");
        const auto saved = Read(Access::LayoutPath(app)); const auto staged = Staged();
        auto source = Microsoft::WRL::Make<Source>(); source->asynchronous = asynchronous;
        const int kind = scenario.kind;
        if (kind == 0) source->Text(CF_UNICODETEXT, L"独立文本 Ω\r\nsecond line");
        if (kind == 1 || kind == 5 || kind == 6 || kind == 7 || kind == 10) source->Image();
        if (kind == 2) source->Text(CF_UNICODETEXT, L"data:application/octet-stream;base64,YWJjAAE=");
        if (kind >= 3 && kind <= 6) source->Virtual(kind == 3 ? 1 : 2, kind == 6);
        // Retain the synchronous pure-virtual control; the same complete
        // asynchronous batch also offers a genuine lower-priority Shell CIDA.
        if (kind == 4 && asynchronous) source->ShellNamespaceAlternative();
        const auto url = origin.Url(kind == 9);
        if (kind >= 7 && kind <= 9) { source->Text(Source::Named(CFSTR_INETURLW), url); source->Text(CF_UNICODETEXT, L"caption must not replace resource"); }
        if (kind == 10) source->Files({borrowed});
        int menus = 0;
        Access::RightMenuBridge bridge(app, [&](HMENU menu, POINT, HWND owner) {
            ++menus;
            Require((cancel || rightCopy) && owner == Access::completionWindow && Access::RightMenuRetained(app) &&
                source->starts == 0 && (!asynchronous || source->reads == 0) && Read(Access::LayoutPath(app)) == saved && Staged() == staged,
                "B17: content menu must not materialize, lease or save before the user decision");
            native_right_drag_cases::Menu(menu, 1); return cancel ? 4u : 2u;
        });
        const bool showMenu = cancel || rightCopy;
        auto adapter = showMenu ? ComPtr<OleDragDropAdapter>{} : Access::ExternalAdapter(app);
        if (showMenu) adapter.Attach(new OleDragDropAdapter(&bridge));
        const POINT point = dock ? Access::DockPoint(app) : keyed || mapped ? POINT{350, 250} : POINT{150, 150};
        const DWORD keys = showMenu ? MK_RBUTTON : MK_LBUTTON | MK_CONTROL;
        DWORD feedback = 1; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), keys, Access::ExternalPoint(point), &feedback)), "actual adapter must route owned content Enter");
        feedback = 1; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragOver(keys, Access::ExternalPoint(point), &feedback)), "actual adapter must route owned content Over");
        Require(source->starts == 0 && (!asynchronous || source->reads == 0) && Staged() == staged,
            "D08: owned content hover cannot lease or render delayed data");
        DWORD effect = 1; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->Drop(source.Get(), showMenu ? MK_RBUTTON | MK_CONTROL : MK_CONTROL, Access::ExternalPoint(point), &effect)),
            "actual adapter must dispatch owned content through native host Drop");
        Require(menus == static_cast<int>(showMenu), "D08/O02: only the right-button content gesture must present exactly one decision menu");
        if (cancel) Require(effect == 0 && menus == 1 && source->starts == 0 && source->ends == 0,
            "B17: cancelling owned content cannot acquire or submit a source");
        else if (scenario.count) Require(effect == 1, "D08: explicit owned-content Copy must acknowledge Copy");
        const auto destination = mapped ? original.parent_path() : desktop;
        Wait([&] { return (mapped ? Access::FolderDropReadIdle(app) : Access::FileDropIdle(app)) &&
            (!asynchronous || source->starts == source->ends) &&
            ((cancel || !scenario.count) || Outputs(destination, {original}).size() >= scenario.count); },
            "actual owned content read/materialization/file operation and published output must finish");
        const auto outputs = Outputs(destination, {original});
        const auto count = cancel ? 0u : scenario.count;
        Require(outputs.size() == count, "D08: incomplete virtual batches cannot fall back to a preview image; successful content must produce exactly its complete selected representation");
        Require(Staged() == staged && Read(original) == "existing-target" && Read(borrowed) == "borrowed-original" &&
            !source->wrongApartment && !source->readBeforeStart,
            "D08: completion must clean owned staging, preserve borrowed sources and honor COM source lifetime");
        native_dock_return_cases::OriginalFiles({{borrowed, "borrowed-original", borrowedIdentity}});
        if (cancel || !scenario.count) Require(Read(Access::LayoutPath(app)) == saved, "B17: rejected or cancelled content cannot change saved layout");
        for (const auto& output : outputs)
        {
            Require(!snowdesktop::drop_staging::Contains(output.wstring()), "D08: published layout and Dock paths must never retain staging files");
            if (kind == 1 || kind == 7) CheckImage(output);
            else if (kind == 0)
            {
                auto bytes = Read(output);
                if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
                Require(bytes == "独立文本 Ω\r\nsecond line", "D08: text bytes must retain Unicode and line breaks in UTF-8");
            }
            else if (kind == 2) Require(Read(output) == std::string("abc\0\1", 5), "D08: inline URL data must retain decoded bytes");
            else if (kind >= 3 && kind <= 5) Require(std::count(source->contents.begin(), source->contents.end(), Read(output)) == 1,
                "D08: every materialized virtual file must retain an original descriptor's exact bytes");
            else if (kind == 8)
            {
                Require(_wcsicmp(output.extension().c_str(), L".url") == 0, "D08: a web page must persist as an Internet Shortcut");
                std::array<wchar_t, 4096> resolved{};
                Require(GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", resolved.data(),
                    static_cast<DWORD>(resolved.size()), output.c_str()) > 0 && resolved.data() == url,
                    "D08: persistent web shortcut must reference the original page URL");
            }
            else if (kind == 9) Require(Read(output) == "downloaded-resource-bytes", "D08: direct resource URL must download actual resource bytes");
            else if (kind == 10)
            {
                // B06 allows a persistent Dock reference to the original real
                // file; its bridge format is not the owned-content contract.
                // Both a copied file and a real Shell-link bridge must refer
                // to the original payload, never the offered preview bitmap.
                if (dock && _wcsicmp(output.extension().c_str(), L".lnk") == 0)
                    Require(fs::equivalent(LinkTarget(output), borrowed), "D08/B06: the Dock bridge must resolve to the complete original file rather than its preview or staging");
                else Require(Read(output) == "borrowed-original", "D08: complete real files must outrank image previews");
            }
        }
        const auto checkModel = [&] {
            if (dock) native_ole_dock_cases::Check(app, outputs);
            else if (keyed || mapped)
            {
                const auto& widget = native_ole_widget_cases::Widget(app, L"ingress-root");
                std::vector<fs::path> members;
                if (mapped) for (const auto& entry : widget.folderEntries) members.emplace_back(entry.fullPath);
                else for (const auto& key : widget.itemKeys) members.emplace_back(key);
                auto expected = outputs; expected.push_back(original);
                Require(members.size() == expected.size() && std::all_of(expected.begin(), expected.end(), [&](const auto& path) {
                    return std::count_if(members.begin(), members.end(), [&](const auto& member) { return fs::equivalent(member, path); }) == 1;
                }), "D08: all content outputs must retain exactly one intended native owner");
            }
            else for (const auto& output : outputs)
            {
                const auto& item = Find(app, ToUpperInvariant(output.wstring()));
                Require(Access::HasVisibleIcon(app, item) && Access::HasMatchingRuntimeSlot(app, item), "D08: actual desktop output must have one accessible runtime location");
            }
        };
        checkModel();
        if (count)
        {
            Require(Access::Save(app), "persist actual content outputs after native submission");
            Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); checkModel();
            for (const auto& output : outputs) Require(fs::exists(output), "D08: accepted content must remain after staging cleanup and actual disk reload");
        }
        Access::StopModel(app); source.Reset();
        if (!mapped)
        {
            for (const auto& output : outputs) RemoveOwnedOutput(desktop, output);
            native_dock_return_cases::RemoveOwnedEntry(desktop, original);
        }
    }
    std::cout << scenes << " native owned-content decoding, routing, source lifetime and persistent output scenes passed\n";
}

inline void RunShellFiles(std::string_view targetName)
{
    if (targetName == "dock") return;
    const bool mapped = targetName == "mapping", keyed = targetName == "collection";
    native_drag_commit_cases::Profile profile;
    const auto desktop = snowdesktop::debug_profile::Current().configuration.desktop;
    fs::create_directories(desktop);
    for (const bool asynchronous : {false, true})
    {
        native_drag_commit_cases::context = "phone Shell file target=" + std::string(targetName) +
            " async=" + std::to_string(asynchronous);
        const auto directory = profile.root / (asynchronous ? L"shell-async" : L"shell-sync");
        fs::create_directories(directory / L"mapped");
        const auto original = (mapped ? directory / L"mapped" : desktop) / L"Existing-Target.txt";
        const auto borrowed = directory / L"Large-Shell-File.bin";
        { std::ofstream stream(original, std::ios::binary); stream << "existing-target"; }
        { std::ofstream stream(borrowed, std::ios::binary); stream << "native-provider-header"; }
        constexpr std::uint64_t fileSize = 70ull * 1024ull * 1024ull;
        fs::resize_file(borrowed, fileSize);
        { std::fstream stream(borrowed, std::ios::binary | std::ios::in | std::ios::out);
          stream.seekp(static_cast<std::streamoff>(fileSize - 22)); stream << "native-provider-footer"; }
        auto& app = profile.Model(false, directory / L"layout");
        if (mapped || keyed) Access::SeedExternalStorage(app,
            mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection,
            mapped ? DesktopWidgetType::FolderMapping : DesktopWidgetType::Collection, false, original);
        else Access::PopulateModel(app, {original}, 0);
        Access::PrepareReload(app); Access::BindAsync(app);
        Require(Access::Save(app), "save large native Shell file ingress baseline");
        const auto staged = Staged();
        auto source = Microsoft::WRL::Make<Source>();
        source->asynchronous = asynchronous;
        source->Virtual(1, false);
        auto& bytes = source->formats[Source::Named(CFSTR_FILEDESCRIPTORW)];
        auto* descriptor = reinterpret_cast<FILEGROUPDESCRIPTORW*>(bytes.data());
        descriptor->fgd[0].nFileSizeLow = static_cast<DWORD>(fileSize);
        wcscpy_s(descriptor->fgd[0].cFileName, borrowed.filename().c_str());
        source->ShellNamespaceAlternative(borrowed);
        auto adapter = Access::ExternalAdapter(app);
        const POINT point = mapped || keyed ? POINT{350, 250} : POINT{150, 150};
        DWORD effect = DROPEFFECT_COPY; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragEnter(source.Get(), MK_LBUTTON | MK_CONTROL,
            Access::ExternalPoint(point), &effect)), "large Shell file enters actual OLE host routing");
        effect = DROPEFFECT_COPY; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->DragOver(MK_LBUTTON | MK_CONTROL, Access::ExternalPoint(point), &effect)),
            "large Shell file hover preserves its offered Copy action");
        Require(source->contentReads == 0 && (!asynchronous || source->reads == 0) &&
            source->starts == 0 && Staged() == staged,
            "large Shell file hover must not extract content or lease the source");
        effect = DROPEFFECT_COPY; Access::FreshExternalDwell(app);
        Require(SUCCEEDED(adapter->Drop(source.Get(), MK_CONTROL, Access::ExternalPoint(point), &effect)) &&
            effect == DROPEFFECT_COPY, "a real Shell file larger than 64 MiB must copy through host Drop");
        const auto destination = mapped ? original.parent_path() : desktop;
        const auto outputs = Outputs(destination, {original});
        Require(outputs.size() == 1 && fs::file_size(outputs.front()) == fileSize &&
            Read(outputs.front()) == Read(borrowed), "native transfer retains the entire large source bytes");
        Require(source->contentReads == 0 && source->starts == source->ends && !source->wrongApartment &&
            Staged() == staged && Read(original) == "existing-target",
            "native Shell files bypass the private stream decoder and retain source lifetime and unrelated data");
        native_drag_async_reload_cases::Refresh(app, true);
        const auto check = [&] {
            if (mapped || keyed)
            {
                const auto& widget = native_ole_widget_cases::Widget(app, L"ingress-root");
                const bool found = mapped ? std::any_of(widget.folderEntries.begin(), widget.folderEntries.end(),
                    [&](const auto& entry) { return fs::equivalent(entry.fullPath, outputs.front()); }) :
                    std::count(widget.itemKeys.begin(), widget.itemKeys.end(), ToUpperInvariant(outputs.front().wstring())) == 1;
                Require(found, "actual native Shell output retains its captured destination membership");
            }
            else Require(Access::HasMatchingRuntimeSlot(app, Find(app, ToUpperInvariant(outputs.front().wstring()))),
                "actual native Shell output lands in the desktop model");
        };
        check(); Require(Access::Save(app), "save native Shell output ownership");
        Access::Load(app); native_drag_async_reload_cases::Refresh(app, true); check();
        Access::StopModel(app); source.Reset();
        if (!mapped)
        {
            for (const auto& output : outputs) RemoveOwnedOutput(desktop, output);
            native_dock_return_cases::RemoveOwnedEntry(desktop, original);
        }
    }
}
}
