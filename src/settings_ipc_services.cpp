#include "settings_ipc_services.h"
#include "settings_ipc_values.h"

namespace snowdesktop::settings_ipc
{
namespace
{
using namespace widget_runtime;

template<class T> bool ValidValue(const T&) { return true; }
bool ValidValue(const GeneralSettings& value)
{
    return std::find(std::begin(value.language), std::end(value.language), '\0') != std::end(value.language);
}

class ControllerProxy final : public ISettingsController
{
public:
    ControllerProxy(Channel& channel, Localize localize) : channel_(channel), localize_(std::move(localize))
    {
        channel_.Bind<void, SnapshotPtr>("controller.changed", [this](SnapshotPtr snapshot) {
            Accept(std::move(snapshot));
        });
        channel_.Bind<void>("controller.pending", [this] { if (pending_) pending_(); });
        Accept(channel_.Call<SnapshotPtr>("controller.subscribe"));
    }
    ~ControllerProxy() override
    {
        channel_.Unbind("controller.changed");
        channel_.Unbind("controller.pending");
    }
    SettingsActionResult Initialize() override { return Action("Initialize"); }
    SettingsActionResult Reload(SettingsReloadPolicy policy) override { return Action("Reload", policy); }
    SettingsActionResult Open(SettingsRoute route) override { return Action("Open", route); }
    SettingsActionResult CloseSession() override { return Action("CloseSession"); }
    SettingsActionResult FlushPending() override
    {
        return failed_ ? *std::exchange(failed_, {}) : Action("FlushPending");
    }
    SettingsActionResult FlushAll() override
    {
        return failed_ ? *std::exchange(failed_, {}) : Action("FlushAll");
    }
    bool RetryPending() override { return Action("FlushAll").Succeeded(); }
    SettingsActionResult InvokeHostAction(const SettingsHostActions::Request& request) override
    {
        return Action("InvokeHostAction", request);
    }
    void SetSnapshotChangedCallback(SnapshotChangedCallback callback) override { changed_ = std::move(callback); }
    void SetPendingWorkCallback(PendingWorkCallback callback) override { pending_ = std::move(callback); }
    SnapshotPtr Snapshot() const noexcept override { return snapshot_; }
    std::uint64_t Generation() const noexcept override { return snapshot_ ? snapshot_->generation : 0; }
    bool IsGenerationCurrent(std::uint64_t generation) const noexcept override
    {
        return snapshot_ && snapshot_->sessionActive && !snapshot_->externalReplacementPending &&
            snapshot_->generation == generation;
    }
#define SD_CONTROLLER_UPDATE(Name, Type, Member) \
    void Update##Name(Type settings, SettingsUpdateMode mode) override \
    { \
        const auto revision = snapshot_ ? snapshot_->domainRevisions.Member : 0; \
        const auto taskbarRevision = snapshot_ ? snapshot_->domainRevisions.systemTaskbar : 0; \
        auto result = Action("Update" #Name, Generation(), revision, taskbarRevision, settings, mode); \
        if (!result.Succeeded()) failed_ = std::move(result); \
        else failed_.reset(); \
        if (pending_) pending_(); \
    }
    SD_CONTROLLER_UPDATE(Personalization, PersonalizationSettings, personalization)
    SD_CONTROLLER_UPDATE(Dock, DockSettings, dock)
    SD_CONTROLLER_UPDATE(Navigation, NavigationSettings, navigation)
    SD_CONTROLLER_UPDATE(General, GeneralSettings, general)
    SD_CONTROLLER_UPDATE(Category, CategorySettings, category)
    SD_CONTROLLER_UPDATE(Desktop, DesktopDisplaySettings, desktop)
#undef SD_CONTROLLER_UPDATE
    void RequestCommit(SettingsDomain domains) override
    {
        auto result = Action("RequestCommit", domains);
        if (!result.Succeeded()) failed_ = std::move(result);
    }
    void PrepareForExternalDataReplacement() override
    {
        // Only the application backup backend can enter this terminal state.
        // It reaches the child through the controller snapshot notification.
    }
private:
    void Accept(SnapshotPtr snapshot)
    {
        if (!snapshot || (snapshot_ && snapshot->revision < snapshot_->revision)) return;
        snapshot_ = std::move(snapshot);
        if (snapshot_->externalReplacementPending) failed_.reset();
        if (changed_) changed_(snapshot_);
    }
    template<class... A> SettingsActionResult Action(const char* name, const A&... arguments)
    {
        try
        {
            auto result = channel_.Call<std::pair<SettingsActionResult, SnapshotPtr>>(
                std::string("controller.") + name, arguments...);
            Accept(std::move(result.second));
            return result.first;
        }
        catch (...)
        {
            // The child disconnect handler closes the UI when its sole
            // authoritative host is gone. Never turn transport failure into
            // a successful save acknowledgement.
            return SettingsActionResult::Failure(localize_ ? localize_("settings.process.connectionLost") : L"Settings process connection lost.");
        }
    }
    Channel& channel_;
    Localize localize_;
    SnapshotPtr snapshot_;
    SnapshotChangedCallback changed_;
    PendingWorkCallback pending_;
    std::optional<SettingsActionResult> failed_;
};
} // namespace

void BindController(Channel& channel, ISettingsController& controller, Localize localize)
{
    // Registering a concrete controller callback immediately publishes its
    // current snapshot. Wait until the child has installed its handlers.
    controller.SetSnapshotChangedCallback({});
    controller.SetPendingWorkCallback({});
    using SnapshotPtr = ISettingsController::SnapshotPtr;
    using Reply = std::pair<SettingsActionResult, SnapshotPtr>;
    channel.Bind<SnapshotPtr>("controller.Snapshot", [&] { return controller.Snapshot(); });
#define SD_CONTROLLER_ACTION(Name) \
    channel.Bind<Reply>("controller." #Name, [&] { \
        auto result = controller.Name(); return Reply{std::move(result), controller.Snapshot()}; });
    SD_CONTROLLER_ACTION(Initialize)
    SD_CONTROLLER_ACTION(CloseSession)
    SD_CONTROLLER_ACTION(FlushPending)
    SD_CONTROLLER_ACTION(FlushAll)
#undef SD_CONTROLLER_ACTION
#define SD_CONTROLLER_ARGUMENT(Name, Type) \
    channel.Bind<Reply, Type>("controller." #Name, [&](Type value) { \
        auto result = controller.Name(std::move(value)); \
        return Reply{std::move(result), controller.Snapshot()}; });
    SD_CONTROLLER_ARGUMENT(Reload, SettingsReloadPolicy)
    SD_CONTROLLER_ARGUMENT(Open, SettingsRoute)
    SD_CONTROLLER_ARGUMENT(InvokeHostAction, SettingsHostActions::Request)
#undef SD_CONTROLLER_ARGUMENT
    channel.Bind<Reply, SettingsDomain>("controller.RequestCommit", [&](SettingsDomain domains) {
        controller.RequestCommit(domains);
        return Reply{SettingsActionResult::Success(), controller.Snapshot()};
    });
#define SD_CONTROLLER_UPDATE(Name, Type, Member) \
    channel.Bind<Reply, std::uint64_t, std::uint64_t, std::uint64_t, Type, SettingsUpdateMode>( \
        "controller.Update" #Name, [&, localize](std::uint64_t generation, std::uint64_t revision, std::uint64_t taskbarRevision, \
            Type value, SettingsUpdateMode mode) { \
        const auto current = controller.Snapshot(); \
        if (!current || !controller.IsGenerationCurrent(generation) || \
            current->domainRevisions.Member != revision || \
            (std::is_same_v<Type, DockSettings> && current->domainRevisions.systemTaskbar != taskbarRevision) || \
            !ValidValue(value) || \
            static_cast<unsigned>(mode) > static_cast<unsigned>(SettingsUpdateMode::PreviewAndCommit)) \
            return Reply{SettingsActionResult::Busy(localize ? localize("settings.process.stale") : L"Settings changed; please retry the edit."), current}; \
        controller.Update##Name(std::move(value), mode); \
        return Reply{SettingsActionResult::Success(), controller.Snapshot()}; });
    SD_CONTROLLER_UPDATE(Personalization, PersonalizationSettings, personalization)
    SD_CONTROLLER_UPDATE(Dock, DockSettings, dock)
    SD_CONTROLLER_UPDATE(Navigation, NavigationSettings, navigation)
    SD_CONTROLLER_UPDATE(General, GeneralSettings, general)
    SD_CONTROLLER_UPDATE(Category, CategorySettings, category)
    SD_CONTROLLER_UPDATE(Desktop, DesktopDisplaySettings, desktop)
#undef SD_CONTROLLER_UPDATE
    channel.Bind<SnapshotPtr>("controller.subscribe", [&] {
        controller.SetSnapshotChangedCallback([&channel](SnapshotPtr snapshot) {
            try { if (channel.Connected()) channel.Notify("controller.changed", snapshot); } catch (...) {}
        });
        controller.SetPendingWorkCallback([&channel] {
            try { if (channel.Connected()) channel.Notify("controller.pending"); } catch (...) {}
        });
        return controller.Snapshot();
    });
}

std::unique_ptr<ISettingsController> CreateControllerProxy(Channel& channel, Localize localize)
{
    return std::make_unique<ControllerProxy>(channel, std::move(localize));
}
} // namespace snowdesktop::settings_ipc
