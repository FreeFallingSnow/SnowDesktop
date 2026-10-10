#pragma once
#include "settings/settings_ipc_codec.h"
#include "settings/shell_extension_settings.h"
#include "shell_start_pin_protocol.h"
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
    // Private metadata query: never displayed or invoked as a menu session.
    std::wstring sourceClsid, sourceKey;
    // Private executable query of Microsoft's Start handler, isolated from
    // arbitrary extension discovery and its worker pool.
    bool startPinOnly = false;
    // Recovery query for the original shortcut files after target discovery fails.
    // It has its own service identity and never replaces a complete disk snapshot.
    bool originalShortcutOnly = false;
    // Private popup optimization: full management/default queries keep this false.
    // It forms part of the disk cache identity so an omitted compatibility
    // source cannot be reused after the source is enabled.
    bool omitNvidiaCompatibility = false;
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

// One fresh COM/menu session in a supervised child. Ordinary workers retire
// with their first real selection; only the system Start handler is pooled.
// Tokens expire when this object closes; they are never cached.
class Session
{
  public:
    explicit Session(const Request &request, DWORD queryTimeoutMs = 8000, bool usePreparedWorker = false);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    std::optional<Reply> Poll();
    DWORD ProcessId() const noexcept;
    // Metadata resolved inside the supervised helper, never on the scheduler.
    unsigned SelectionContexts() const noexcept;
    static void ReleaseIdleWorker();
    // Owning STA only. Preparation reserves one of the two ordinary slots and
    // never queries the eventual selection or supplies cached command tokens.
    enum class PreparationState { Absent, Pending, Ready };
    static bool PrepareFirstMenuWorker(DWORD timeoutMs = 8000);
    static PreparationState PollPreparedMenuWorker();
    static void ReleasePreparedMenuWorker();
    // Transfers ownership to a bounded invocation monitor; modeless dialogs
    // remain alive after the custom popup closes.
    void Invoke(UINT token, POINT position, HWND owner = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Internal host/settings cache boundary, not a component API. Registry change
// notifications invalidate the warm worker and settings catalogue generation.
std::uint64_t MenuCacheGeneration();
void InvalidateMenuCache();
bool TakeMenuRegistryChanges();
// Non-owning notification handles on the calling STA; never close them.
std::vector<HANDLE> MenuRegistryWaitHandles(bool &complete);
using QueryExecutor = std::function<Reply(const Request &)>;
using InvokeExecutor = std::function<void(UINT, POINT)>;
// Private composition boundary: the host supplies its deployment-staged
// Explorer transport; tests can substitute only that final execution boundary.
using StartPinExecutor = std::function<HRESULT(shell_start_pin::Action, const std::wstring &, HWND, POINT)>;
std::optional<int> TryRunHelper(QueryExecutor query = {}, InvokeExecutor invoke = {}, StartPinExecutor startPin = {});
bool CanQueryOriginalShortcutObjects(const Request &);
Context ResolveContext(const Request &);
std::vector<Entry> VisibleEntries(const Preferences &, const std::vector<Entry> &, const Request &);
} // namespace snowdesktop::shell_extensions

namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::Request>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.paths, v.background, v.catalogueOnly, v.extended, v.context, v.sourceClsid, v.sourceKey,
                        v.startPinOnly, v.originalShortcutOnly, v.omitNvidiaCompatibility);
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
