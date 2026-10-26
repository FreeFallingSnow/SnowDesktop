#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <sherrors.h>
#include <wrl/implements.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::file_history
{
// Session-local history of operations performed by this worker. Never inspect
// Explorer's global undo stack or search the Recycle Bin by display name.
struct Identity
{
    DWORD volume = 0;
    DWORD high = 0;
    DWORD low = 0;
    FILETIME created{};
    bool valid = false;
    bool operator==(const Identity& other) const noexcept
    {
        return valid && other.valid && volume == other.volume && high == other.high &&
            low == other.low && CompareFileTime(&created, &other.created) == 0;
    }
};

inline Identity Identify(const std::wstring& path)
{
    Identity result;
    HANDLE file = CreateFileW(path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return result;
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(file, &info))
        result = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
            info.ftCreationTime, info.nFileIndexHigh != 0 || info.nFileIndexLow != 0};
    CloseHandle(file);
    return result;
}

inline std::wstring Path(IShellItem* item)
{
    PWSTR text = nullptr;
    std::wstring result;
    if (item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &text)) && text)
        result = text;
    CoTaskMemFree(text);
    return result;
}

struct Entry
{
    enum class Kind { Rename, Delete } kind = Kind::Rename;
    std::wstring original;
    std::wstring current;
    Identity identity;
    std::vector<BYTE> recycledId;
};
using Group = std::vector<Entry>;
struct Result
{
    bool succeeded = false;
    std::vector<std::pair<std::wstring, std::wstring>> renames;
    std::vector<std::wstring> removed;
    std::wstring failedPath;
    HRESULT status = S_OK;
};

class Progress final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IFileOperationProgressSink>
{
public:
    Entry entry;
    Microsoft::WRL::ComPtr<IShellItem> requested;
    bool strictRecycle = false;
    bool completed = false;
    HRESULT status = E_ABORT;

    bool Matches(IShellItem* item) const
    {
        int order = 1;
        return item && requested && SUCCEEDED(requested->Compare(item, SICHINT_CANONICAL, &order)) && order == 0;
    }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD flags, IShellItem* item, IShellItem*, LPCWSTR) override
    {
        if (!Matches(item)) return S_OK;
        // No replacement/merge, even if the destination appeared after preflight.
        if ((flags & TSF_OVERWRITE_EXIST) ||
            GetFileAttributesW(entry.original.c_str()) != INVALID_FILE_ATTRIBUTES)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        return Identify(Path(item)) == entry.identity ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem* item, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override
    {
        if (!Matches(item) || completed) return S_OK;
        status = hr;
        if (SUCCEEDED(hr) && hr != COPYENGINE_S_USER_IGNORED && created)
        {
            entry.original = Path(created);
            completed = !entry.original.empty();
        }
        return S_OK;
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD flags, IShellItem* item) override
    {
        if (!Matches(item)) return S_OK;
        if (strictRecycle && !(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) return E_ABORT;
        return (!strictRecycle && !entry.identity.valid) || Identify(entry.original) == entry.identity
            ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem* item, HRESULT hr, IShellItem* recycled) override
    {
        if (!Matches(item) || completed) return S_OK;
        status = hr;
        completed = SUCCEEDED(hr) && hr != COPYENGINE_S_USER_IGNORED;
        if (completed && recycled)
        {
            entry.current = Path(recycled);
            PIDLIST_ABSOLUTE id = nullptr;
            if (Identify(entry.current) == entry.identity && SUCCEEDED(SHGetIDListFromObject(recycled, &id)) && id)
            {
                const auto* bytes = reinterpret_cast<const BYTE*>(id);
                entry.recycledId.assign(bytes, bytes + ILGetSize(id));
            }
            CoTaskMemFree(id);
        }
        return S_OK;
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};

inline HRESULT Recycle(const std::vector<std::wstring>& paths, DWORD flags, Group& recorded, bool strict = false)
{
    using namespace Microsoft::WRL;
    ComPtr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (FAILED(hr)) return hr;
    // Retain the warning for volumes/items that cannot be recycled. Redo also
    // rejects a permanent-delete callback rather than repeating it silently.
    hr = operation->SetOperationFlags((flags & ~static_cast<DWORD>(FOF_ALLOWUNDO)) | FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING |
        ((flags & FOF_NOERRORUI) ? FOFX_EARLYFAILURE : 0u));
    if (FAILED(hr)) return hr;
    std::vector<ComPtr<Progress>> sinks;
    for (const auto& path : paths)
    {
        auto sink = Make<Progress>();
        if (!sink) return E_OUTOFMEMORY;
        sink->entry = {Entry::Kind::Delete, path, {}, Identify(path), {}};
        if (strict && !sink->entry.identity.valid) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        sink->strictRecycle = strict;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&sink->requested));
        if (FAILED(hr)) return hr;
        hr = operation->DeleteItem(sink->requested.Get(), sink.Get());
        if (FAILED(hr)) return hr;
        sinks.push_back(std::move(sink));
    }
    if (sinks.empty()) return E_INVALIDARG;
    const HRESULT performed = operation->PerformOperations();
    BOOL aborted = TRUE;
    const HRESULT queried = operation->GetAnyOperationsAborted(&aborted);
    hr = FAILED(performed) ? performed : FAILED(queried) ? queried : aborted ? E_ABORT : S_OK;
    for (const auto& sink : sinks)
    {
        if (sink->completed && !sink->entry.recycledId.empty()) recorded.push_back(sink->entry);
        if (!sink->completed && SUCCEEDED(hr)) hr = FAILED(sink->status) ? sink->status : E_ABORT;
    }
    return hr;
}

inline HRESULT Restore(Entry& entry)
{
    using namespace Microsoft::WRL;
    if (entry.recycledId.empty() || !(Identify(entry.current) == entry.identity))
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (GetFileAttributesW(entry.original.c_str()) != INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    const std::filesystem::path original(entry.original);
    ComPtr<IShellItem> folder;
    HRESULT hr = SHCreateItemFromParsingName(original.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(hr)) return hr; // Never recreate a removed parent directory.
    auto sink = Make<Progress>();
    if (!sink) return E_OUTOFMEMORY;
    sink->entry = entry;
    hr = SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(entry.recycledId.data()),
        IID_PPV_ARGS(&sink->requested));
    if (FAILED(hr)) return hr;
    ComPtr<IFileOperation> operation;
    hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (FAILED(hr)) return hr;
    hr = operation->SetOperationFlags(FOF_SILENT | FOF_NOERRORUI | FOFX_EARLYFAILURE |
        FOF_RENAMEONCOLLISION | FOF_NO_CONNECTED_ELEMENTS);
    if (SUCCEEDED(hr)) hr = operation->MoveItem(sink->requested.Get(), folder.Get(), original.filename().c_str(), sink.Get());
    if (FAILED(hr)) return hr;
    hr = operation->PerformOperations();
    // A committed item remains committed even if a later Shell callback fails.
    if (sink->completed)
    {
        entry.original = sink->entry.original;
        entry.recycledId.clear();
        return S_OK;
    }
    return FAILED(hr) ? hr : FAILED(sink->status) ? sink->status : E_ABORT;
}

inline HRESULT Rename(const std::wstring& from, const std::wstring& to, const Identity& expected)
{
    // Rename the verified handle, so a path replacement between validation and
    // commit cannot select a different file. ReplaceIfExists stays FALSE.
    HANDLE file = CreateFileW(from.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    BY_HANDLE_FILE_INFORMATION info{};
    HRESULT hr = E_FAIL;
    if (GetFileInformationByHandle(file, &info))
    {
        const Identity actual{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow, info.ftCreationTime, true};
        if (actual == expected)
        {
            // The Win32 API consumes a NUL-terminated path even though the
            // counted name excludes that terminator. Reserve and zero it.
            const size_t bytes = offsetof(FILE_RENAME_INFO, FileName) + (to.size() + 1) * sizeof(wchar_t);
            if (bytes <= MAXDWORD)
            {
                std::vector<BYTE> storage(bytes);
                auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
                rename->ReplaceIfExists = FALSE;
                rename->FileNameLength = static_cast<DWORD>(to.size() * sizeof(wchar_t));
                std::copy(to.begin(), to.end(), rename->FileName);
                hr = SetFileInformationByHandle(file, FileRenameInfo, rename, static_cast<DWORD>(bytes))
                    ? S_OK : HRESULT_FROM_WIN32(GetLastError());
            }
        }
        else hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }
    CloseHandle(file);
    if (SUCCEEDED(hr)) SHChangeNotify((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ?
        SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM, SHCNF_PATHW, from.c_str(), to.c_str());
    return hr;
}

class History
{
public:
    bool CanUndo() const noexcept { return canUndo_.load(); }
    bool CanRedo() const noexcept { return canRedo_.load(); }
    void Clear() { undo_.clear(); redo_.clear(); Publish(); }
    void Record(Group group)
    {
        if (group.empty()) return;
        redo_.clear();
        if (undo_.size() == 32) undo_.erase(undo_.begin());
        undo_.push_back(std::move(group));
        Publish();
    }
    Result Apply(bool redo)
    {
        Result result;
        auto& source = redo ? redo_ : undo_;
        auto& destination = redo ? undo_ : redo_;
        if (source.empty()) { result.status = S_FALSE; return result; }
        Group pending = std::move(source.back());
        source.pop_back();
        Group failed, completed;
        // Undo a multi-file operation in reverse order; redo follows the
        // opposite order. Successful members are never retried after failure.
        for (auto it = pending.rbegin(); it != pending.rend(); ++it)
        {
            auto entry = *it;
            HRESULT hr;
            if (entry.kind == Entry::Kind::Rename)
            {
                const auto& from = redo ? entry.original : entry.current;
                const auto& to = redo ? entry.current : entry.original;
                hr = Rename(from, to, entry.identity);
                if (SUCCEEDED(hr)) result.renames.emplace_back(from, to);
            }
            else if (!redo) hr = Restore(entry);
            else
            {
                Group recycled;
                hr = Identify(entry.original) == entry.identity ? Recycle({entry.original},
                    FOF_SILENT | FOF_NOERRORUI | FOF_NOCONFIRMATION, recycled, true)
                    : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                if (!recycled.empty()) { entry = std::move(recycled.front()); hr = S_OK; result.removed.push_back(entry.original); }
                else if (SUCCEEDED(hr)) hr = E_ABORT;
            }
            if (SUCCEEDED(hr)) completed.push_back(std::move(entry));
            else
            {
                if (result.failedPath.empty()) { result.failedPath = entry.original; result.status = hr; }
                failed.push_back(std::move(entry));
            }
        }
        result.succeeded = failed.empty();
        if (!failed.empty()) { std::reverse(failed.begin(), failed.end()); source.push_back(std::move(failed)); }
        if (!completed.empty()) destination.push_back(std::move(completed));
        Publish();
        return result;
    }
private:
    void Publish() { canUndo_ = !undo_.empty(); canRedo_ = !redo_.empty(); }
    std::vector<Group> undo_, redo_; // Used only by the serial STA.
    std::atomic<bool> canUndo_{false}, canRedo_{false};
};
}
