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
    std::vector<std::string> providers;
};
struct Entry
{
    std::string provider, key;
    std::wstring label;
    UINT token = 0;
    bool enabled = true, checked = false, separator = false, native = false;
    std::vector<Entry> children;
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
};
struct Reply
{
    bool ok = false;
    std::vector<Entry> entries;
};

// One COM/menu session in a supervised child. Query is asynchronous; all
// methods run on the owning UI thread. Tokens expire when this object closes.
class Session
{
  public:
    explicit Session(const Request &request, DWORD queryTimeoutMs = 8000);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    std::optional<Reply> Poll();
    // Transfers ownership to a bounded invocation monitor; modeless dialogs
    // remain alive after the custom popup closes.
    void Invoke(UINT token, POINT position);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using QueryExecutor = std::function<Reply(const Request &)>;
std::optional<int> TryRunHelper(QueryExecutor query = {});
Placement ResolvePlacement(const Preferences &, const Entry &);
std::vector<Entry> SelectEntries(const Preferences &, const std::vector<Entry> &, Placement);
} // namespace snowdesktop::shell_extensions

namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::Request>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.paths, v.background, v.catalogueOnly, v.extended, v.providers);
    }
};
template <> struct Fields<shell_extensions::Entry>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.provider, v.key, v.label, v.token, v.enabled, v.checked, v.separator, v.native,
                        v.children, v.width, v.height, v.pixels);
    }
};
template <> struct Fields<shell_extensions::Reply>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.ok, v.entries);
    }
};
} // namespace snowdesktop::settings_ipc
