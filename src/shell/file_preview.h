#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::file_preview
{
enum class Kind { QuickLook, Peek };

struct Shortcut
{
    UINT key = VK_SPACE;
    UINT modifiers = 0;
    bool operator==(const Shortcut&) const = default;
};

struct Provider
{
    Kind kind = Kind::QuickLook;
    Shortcut shortcut;
    // QuickLook's per-user pipe, or the absolute Peek executable path.
    std::wstring endpoint;
};

struct Availability
{
    std::vector<Provider> providers;
};

struct InputContext
{
    bool textInput = false;
    bool overlay = false;
    bool gesture = false;
};

struct ProcessIdentity
{
    DWORD pid = 0;
    std::uint64_t created = 0;
    bool operator==(const ProcessIdentity&) const = default;
};

struct PeekInstallation
{
    std::filesystem::path executable;
    unsigned major = 0;
    unsigned minor = 0;
};

// OS discovery is separated from reading the real provider files/pipe. The
// caller owns this snapshot; probing never changes another application's data.
struct Environment
{
    std::wstring quickLookPipe;
    bool powerToysRunning = false;
    std::filesystem::path powerToysSettings;
    std::vector<PeekInstallation> peekInstallations;
};

bool AllowsPreview(const InputContext& context);
bool IsFileSystemPath(std::wstring_view path);
std::optional<Shortcut> ParsePeekShortcut(
    std::string_view generalSettings, std::string_view peekSettings);
std::optional<Provider> MatchProvider(
    const Availability& availability, Shortcut pressed);
Environment ReadEnvironment();
Availability Probe(const Environment& environment);
ProcessIdentity IdentifyProcess(DWORD pid);

using PublishSession = std::function<void(ProcessIdentity)>;
bool Invoke(const Provider& provider, const std::wstring& path,
    const PublishSession& publishSession);

// Detection, file access and IPC run on one bounded queue, away from the host
// input thread. The worker owns shared state only, never DesktopApp callbacks.
class Service final
{
public:
    struct Backend
    {
        std::function<Availability()> detect;
        std::function<bool(const Provider&, const std::wstring&,
            const PublishSession&)> invoke;
    };

    Service();
    explicit Service(Backend backend);
    ~Service();
    Service(Service&& other) noexcept;
    Service& operator=(Service&& other) noexcept;
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    void Start();
    void Stop();
    void Refresh();
    Availability Current() const;
    bool Matches(Shortcut pressed) const;
    bool Request(Shortcut pressed, std::wstring path, bool repeated);
    // Called only after the host has excluded its own interaction surfaces.
    // An unrelated external focus transfer ends the preview's selection lease.
    bool RetainsSelection(HWND window);

private:
    struct State;
    std::shared_ptr<State> state_;
    static void Run(const std::shared_ptr<State>& state);
};
}
