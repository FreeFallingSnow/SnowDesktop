#pragma once
#include "settings_ipc_codec.h"
#include "shell_extension_settings.h"
#include <functional>
#include <memory>
#include <optional>
#include <windows.h>

namespace snowdesktop::shell_extensions
{
struct Request
{
    std::vector<std::wstring> paths;
    bool background = false;
    bool catalogueOnly = false;
    bool extended = false;
    Context context = Context::Automatic;
    friend bool operator==(const Request &, const Request &) = default;
};
struct Entry
{
    std::string provider, key;
    std::string registration; // Proven registration identity; empty when ownership is unknown.
    std::wstring label;
    UINT token = 0;
    bool enabled = true, checked = false, separator = false, native = false;
    std::vector<Entry> children;
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    wchar_t accessKey = 0;
};
struct Reply
{
    bool ok = false;
    std::vector<Entry> entries;
    std::string error;
};

// One fresh COM/menu session in a supervised child. Successful, uninvoked
// sessions may return the child to the owning UI thread's bounded warm pool.
// Tokens still expire when this object closes; they are never cached.
class Session
{
  public:
    explicit Session(const Request &request, DWORD queryTimeoutMs = 8000);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    std::optional<Reply> Poll();
    DWORD ProcessId() const noexcept;
    static void ReleaseIdleWorker();
    // Transfers ownership to a bounded invocation monitor; modeless dialogs
    // remain alive after the custom popup closes.
    void Invoke(UINT token, POINT position);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Internal host/settings cache boundary, not a component API. Registry change
// notifications invalidate the warm worker and settings catalogue generation.
std::uint64_t MenuCacheGeneration();
void InvalidateMenuCache();
using QueryExecutor = std::function<Reply(const Request &)>;
using InvokeExecutor = std::function<void(UINT, POINT)>;
std::optional<int> TryRunHelper(QueryExecutor query = {}, InvokeExecutor invoke = {});
Context ResolveContext(const Request &);
std::vector<Entry> VisibleEntries(const Preferences &, const std::vector<Entry> &, const Request &);
} // namespace snowdesktop::shell_extensions

namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::Request>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.paths, v.background, v.catalogueOnly, v.extended, v.context);
    }
};
template <> struct Fields<shell_extensions::Entry>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.provider, v.key, v.label, v.token, v.enabled, v.checked, v.separator, v.native, v.children,
                        v.width, v.height, v.pixels, v.accessKey, v.registration);
    }
};
template <> struct Fields<shell_extensions::Reply>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.ok, v.entries, v.error);
    }
};
} // namespace snowdesktop::settings_ipc
