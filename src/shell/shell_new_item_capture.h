#pragma once

#include <shlobj.h>
#include <servprov.h>
#include <wrl.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop
{
// One invocation owns one capture. The Shell may retain its site after a
// modeless New command returns; callbacks never reference the DesktopApp.
class ShellNewItemCapture
{
public:
    ShellNewItemCapture(const std::wstring& directory, HWND notifyWindow, UINT notifyMessage)
        : directory_(NormalizePath(directory)),
          notifyWindow_(notifyWindow), notifyMessage_(notifyMessage) {}

    void Record(const std::wstring& path, bool edit)
    {
        const auto normalized = NormalizePath(path);
        if (_wcsicmp(normalized.parent_path().c_str(), directory_.c_str()) != 0) return;
        {
            std::lock_guard lock(mutex_);
            const auto output = std::find_if(outputs_.begin(), outputs_.end(), [&](const auto& entry) {
                return _wcsicmp(entry.path.c_str(), normalized.c_str()) == 0;
            });
            if (output == outputs_.end())
                outputs_.push_back({normalized.wstring(), GetTickCount64(), edit});
            else
            {
                // Shell can select the output first, then ask to edit the same path.
                if (!edit || output->edit) return;
                output->edit = true;
                output->reportedAt = GetTickCount64();
                if (output->delivery == Delivery::Complete) output->delivery = Delivery::Rename;
            }
        }
        Notify();
    }

    void Finish()
    {
        { std::lock_guard lock(mutex_); finished_ = true; }
        Notify();
    }

    bool HasPendingPaths()
    {
        std::lock_guard lock(mutex_);
        return std::any_of(outputs_.begin(), outputs_.end(), [](const auto& item) {
            return item.delivery != Delivery::Complete;
        });
    }

    template<class Consumer>
    void ForEachUnplacedPath(Consumer&& consumer)
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : outputs_)
            if (item.delivery == Delivery::Placement) consumer(item.path);
    }

    // Keep exact outputs across refreshes until enumeration catches up. A
    // deleted output expires; an unrelated file is never inferred from a diff.
    // The consumer runs on the host thread and must not invoke Shell callbacks.
    template<class Consumer>
    bool Consume(Consumer&& consumer, ULONGLONG now = GetTickCount64())
    {
        std::lock_guard lock(mutex_);
        bool pending = false;
        for (auto& item : outputs_)
        {
            if (item.delivery == Delivery::Complete) continue;
            const bool expired = now >= item.reportedAt && now - item.reportedAt > 30000;
            if (expired || consumer(item.path, item.delivery == Delivery::Placement, item.edit))
                item.delivery = Delivery::Complete;
            else
                pending = true;
        }
        return finished_ && !pending;
    }

private:
    static std::filesystem::path NormalizePath(const std::wstring& path)
    {
        const DWORD required = GetLongPathNameW(path.c_str(), nullptr, 0);
        std::wstring expanded(required, L'\0');
        const DWORD length = required ? GetLongPathNameW(path.c_str(), expanded.data(), required) : 0;
        if (length && length < required) expanded.resize(length);
        else expanded = path;
        auto normalized = std::filesystem::path(expanded).lexically_normal().make_preferred();
        if (!normalized.has_filename() && normalized != normalized.root_path())
            normalized = normalized.parent_path();
        return normalized;
    }

    void Notify() const
    {
        if (notifyWindow_ && notifyMessage_)
            PostMessageW(notifyWindow_, notifyMessage_, 0, 0);
    }
    enum class Delivery { Placement, Rename, Complete };
    struct Output
    {
        std::wstring path;
        ULONGLONG reportedAt;
        bool edit;
        Delivery delivery = Delivery::Placement;
    };
    std::filesystem::path directory_;
    HWND notifyWindow_;
    UINT notifyMessage_;
    std::mutex mutex_;
    std::vector<Output> outputs_;
    bool finished_ = false;
};

class ShellNewItemClient final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    INewMenuClient, IServiceProvider>
{
public:
    explicit ShellNewItemClient(std::shared_ptr<ShellNewItemCapture> capture)
        : capture_(std::move(capture)) {}
    ~ShellNewItemClient() { capture_->Finish(); }

    IFACEMETHODIMP QueryService(REFGUID service, REFIID iid, void** result) override
    {
        if (!result) return E_POINTER;
        *result = nullptr;
        return service == SID_SNewMenuClient ? QueryInterface(iid, result) : E_NOINTERFACE;
    }
    IFACEMETHODIMP IncludeItems(NMCII_FLAGS* flags) override
    {
        if (!flags) return E_POINTER;
        *flags = static_cast<NMCII_FLAGS>(NMCII_ITEMS | NMCII_FOLDERS);
        return S_OK;
    }
    IFACEMETHODIMP SelectAndEditItem(PCIDLIST_ABSOLUTE item, NMCSAEI_FLAGS flags) override
    {
        if (!item) return E_INVALIDARG;
        PWSTR path = nullptr;
        const HRESULT hr = SHGetNameFromIDList(item, SIGDN_FILESYSPATH, &path);
        if (FAILED(hr)) return hr;
        try { capture_->Record(path, (flags & NMCSAEI_EDIT) != 0); }
        catch (...) { CoTaskMemFree(path); return E_OUTOFMEMORY; }
        CoTaskMemFree(path);
        return S_OK;
    }

private:
    std::shared_ptr<ShellNewItemCapture> capture_;
};

inline HRESULT AttachShellNewItemCapture(IContextMenu* menu,
    const std::shared_ptr<ShellNewItemCapture>& capture)
{
    if (!menu || !capture) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IObjectWithSite> site;
    HRESULT hr = menu->QueryInterface(IID_PPV_ARGS(site.GetAddressOf()));
    if (FAILED(hr)) return hr;
    const auto client = Microsoft::WRL::Make<ShellNewItemClient>(capture);
    if (!client) return E_OUTOFMEMORY;
    return site->SetSite(static_cast<INewMenuClient*>(client.Get()));
}
}
