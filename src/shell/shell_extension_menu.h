#pragma once
#include "settings/settings_ipc_codec.h"
#include "settings/shell_extension_settings.h"
#include "shell_start_pin_protocol.h"
#include <functional>
#include <memory>
#include <optional>
#include <windows.h>
#include <shlobj.h>

namespace snowdesktop::shell_extensions
{
struct Request
{
    std::vector<std::wstring> paths;
    bool background = false;
    bool catalogueOnly = false;
    bool extended = false;
    Context context = Context::Automatic;
    // Private source query: supplies display metadata, never invocation tokens.
    // A click is resolved again against the actual selection's full Shell menu.
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
    // Private popup read projection. Absent keeps complete inspection; an empty
    // set reads no extension details. Part of the snapshot/query identity.
    std::optional<std::vector<std::string>> visibleMenuProviders;
    // Settings command inspection retains roots, never a complete popup/click
    // snapshot. Installed-extension management uses catalogueOnly on Inspect.
    bool rootMetadataOnly = false;
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

inline bool HasVisibilityRule(const Preferences& prefs, const std::string& id, Context context)
{
    const auto key = StateVisibilityId(id);
    return std::any_of(prefs.rules.begin(), prefs.rules.end(), [&](const auto& rule) {
        return StateVisibilityId(rule.id) == key && rule.category == CategoryOf(context);
    }) || std::any_of(prefs.overrides.begin(), prefs.overrides.end(), [&](const auto& rule) {
        return StateVisibilityId(rule.id) == key && rule.context == context && rule.visibility != Visibility::Inherit;
    }) || std::any_of(prefs.shown.begin(), prefs.shown.end(), [&](const auto& rule) {
        return StateVisibilityId(rule.id) == key && rule.context == context;
    });
}
inline std::string EntryVisibilityId(const Preferences& prefs, const Entry& entry, Context context)
{
    if (const auto* pair = StatePairForVerb(entry.key)) return std::string(pair->id);
    if (HasVisibilityRule(prefs, entry.provider, context)) return entry.provider;
    return !entry.registration.empty() && HasVisibilityRule(prefs, entry.registration, context)
        ? entry.registration : entry.provider;
}

struct NativeMenuResult
{
    bool ok = false, invoked = false;
    std::string verb, error;
};
struct NativeMenuSelection
{
    std::vector<unsigned char> parent;
    std::vector<std::vector<unsigned char>> children;
    bool lightTheme = true;
    // Retain the caller's actual Shell namespace, including merged Desktop
    // selections whose physical files belong to different directories.
    static NativeMenuSelection Capture(IShellFolder*, UINT, PCUITEMID_CHILD_ARRAY);
};
// Query, native menu loop and arbitrary verbs all stay in the supervised STA.
// Poll never waits for an extension or a popup to finish.
class NativeMenuSession
{
public:
    NativeMenuSession(const Request&, POINT, HWND owner, NativeMenuSelection selection = {}, DWORD timeoutMs = 8000);
    ~NativeMenuSession();
    NativeMenuSession(const NativeMenuSession&) = delete;
    NativeMenuSession& operator=(const NativeMenuSession&) = delete;
    std::optional<NativeMenuResult> Poll();
    bool Preparing() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One fresh COM/menu selection in a supervised child. Explicitly opted-in,
// healthy ordinary workers have a bounded pool; Start uses a separate pool.
// Tokens expire when this object closes; they are never cached.
class Session
{
  public:
    explicit Session(const Request &request, DWORD queryTimeoutMs = 8000, bool usePreparedWorker = false);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    std::optional<Reply> Poll();
    // Transient root headers from the same query; the helper immediately
    // continues reading every cascade. Never executable or cacheable.
    std::optional<Reply> PollRoots();
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
using NativePopupExecutor = std::function<NativeMenuResult(const Request&, POINT)>;
std::optional<int> TryRunHelper(QueryExecutor query = {}, InvokeExecutor invoke = {}, StartPinExecutor startPin = {},
    NativePopupExecutor nativePopup = {});
bool CanQueryOriginalShortcutObjects(const Request &);
Context ResolveContext(const Request &);
std::vector<Entry> VisibleEntries(const Preferences &, const std::vector<Entry> &, const Request &);
} // namespace snowdesktop::shell_extensions

namespace snowdesktop::settings_ipc
{
template <> struct Fields<shell_extensions::NativeMenuResult>
{
    template <class T> static auto Tie(T &v) { return std::tie(v.ok, v.invoked, v.verb, v.error); }
};
template <> struct Fields<shell_extensions::NativeMenuSelection>
{
    template <class T> static auto Tie(T &v) { return std::tie(v.parent, v.children, v.lightTheme); }
};
template <> struct Fields<shell_extensions::Request>
{
    template <class T> static auto Tie(T &v)
    {
        return std::tie(v.paths, v.background, v.catalogueOnly, v.extended, v.context, v.sourceClsid, v.sourceKey,
                        v.startPinOnly, v.originalShortcutOnly, v.omitNvidiaCompatibility, v.visibleMenuProviders, v.rootMetadataOnly);
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
