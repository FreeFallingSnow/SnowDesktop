#include "settings_ipc_backends.h"
#include "settings_ipc_values.h"
#include "../l10n.h"

#include <shellapi.h>
#include <unordered_map>

namespace snowdesktop::settings_ipc
{
using namespace winui;
namespace
{
using Path = std::optional<std::filesystem::path>;
using Token = std::uint64_t;

template<class T> class Completions
{
public:
    Token Add(std::function<void(T)> completion)
    {
        if (callbacks_.size() >= 32) throw ProtocolError("too many pending settings dialogs");
        callbacks_.emplace(++next_, std::move(completion));
        return next_;
    }
    void Complete(Token token, T result)
    {
        auto found = callbacks_.find(token);
        if (found == callbacks_.end()) return;
        auto callback = std::move(found->second);
        callbacks_.erase(found);
        if (callback) callback(std::move(result));
    }
    void Cancel()
    {
        auto callbacks = std::move(callbacks_);
        callbacks_.clear();
        for (auto& [token, callback] : callbacks)
        {
            (void)token;
            try { if (callback) callback(T{}); } catch (...) {}
        }
    }
private:
    Token next_ = 0;
    std::unordered_map<Token, std::function<void(T)>> callbacks_;
};

struct WidgetProxyState
{
    WidgetsPageBackendOptions options;
    std::shared_ptr<const WidgetsPageSnapshot> snapshot;
    bool active = false;
    bool closed = false;
    Token generation = 0;
};

class WidgetsProxy final : public IWidgetsPageBackend
{
public:
    WidgetsProxy(Channel& channel, WidgetsPageBackendOptions options)
        : channel_(channel), state_(std::make_shared<WidgetProxyState>())
    {
        state_->options = std::move(options);
        std::weak_ptr<WidgetProxyState> weak = state_;
        channel_.Bind<void, std::shared_ptr<const WidgetsPageSnapshot>>("widgets.changed",
            [weak](auto snapshot) {
                auto state = weak.lock();
                if (!state || state->closed || !snapshot) return;
                if (state->snapshot && state->snapshot->generation == snapshot->generation &&
                    state->snapshot->revision > snapshot->revision) return;
                state->snapshot = snapshot;
                if (state->options.snapshotChanged) state->options.snapshotChanged(std::move(snapshot));
            });
        channel_.Bind<void, Token, Token>("widgets.pick", [weak, &channel](Token id, Token generation) {
            const auto state = weak.lock();
            if (!state || state->closed || !state->options.pickPackage)
            { channel.Notify("widgets.picked", id, Path{}); return; }
            state->options.pickPackage(generation, [weak, &channel, id](Path selected) {
                if (auto live = weak.lock(); live && !live->closed && channel.Connected())
                    channel.Notify("widgets.picked", id, selected);
            });
        });
        channel_.Bind<void, Token, Token, WidgetInstallConfirmationRequest>("widgets.confirm",
            [weak, &channel](Token id, Token generation, WidgetInstallConfirmationRequest request) {
                const auto state = weak.lock();
                if (!state || state->closed || !state->options.confirmInstall)
                { channel.Notify("widgets.confirmed", id, false); return; }
                state->options.confirmInstall(generation, std::move(request), [weak, &channel, id](bool answer) {
                    if (auto live = weak.lock(); live && !live->closed && channel.Connected())
                        channel.Notify("widgets.confirmed", id, answer);
                });
            });
        channel_.Call<void>("widgets.create");
    }
    ~WidgetsProxy() override { Close(); }
    void SetSnapshotChangedCallback(WidgetsPageBackendOptions::SnapshotChangedCallback callback) override
    { state_->options.snapshotChanged = std::move(callback); }
    bool Activate(Token generation, bool discoverSources) override
    {
        state_->generation = generation;
        state_->active = true;
        try { state_->active = channel_.Call<bool>("widgets.Activate", generation, discoverSources); }
        catch (...) { state_->active = false; }
        return state_->active;
    }
    void Deactivate() noexcept override
    {
        state_->active = false;
        try { channel_.Call<void>("widgets.Deactivate"); } catch (...) {}
    }
    bool Refresh() override
    {
        try { return channel_.Call<bool>("widgets.Refresh"); } catch (...) { return false; }
    }
    bool Invoke(Token generation, WidgetsPageRequest request) override
    {
        try { return channel_.Call<bool>("widgets.Invoke", generation, request); } catch (...) { return false; }
    }
    std::shared_ptr<const WidgetsPageSnapshot> Snapshot() const noexcept override { return state_->snapshot; }
    Token Generation() const noexcept override { return state_->generation; }
    bool IsGenerationCurrent(Token generation) const noexcept override
    { return !state_->closed && state_->active && generation == state_->generation; }
    void Close() noexcept override
    {
        if (state_->closed) return;
        state_->closed = true;
        try { channel_.Call<void>("widgets.Close"); } catch (...) {}
        state_->snapshot.reset();
        state_->options = {};
    }
private:
    Channel& channel_;
    std::shared_ptr<WidgetProxyState> state_;
};

struct BackupProxyState
{
    BackupDataPageBackendOptions options;
    IBackupDataPageBackend::SnapshotChangedCallback changed;
    BackupDataPageSnapshot snapshot;
    Completions<bool> confirmations;
    Completions<Path> pickers;
    bool closed = false;
};

class BackupProxy final : public IBackupDataPageBackend
{
public:
    BackupProxy(Channel& channel, BackupDataPageBackendOptions options)
        : channel_(channel), state_(std::make_shared<BackupProxyState>())
    {
        state_->options = std::move(options);
        std::weak_ptr<BackupProxyState> weak = state_;
        channel_.Bind<void, BackupDataPageSnapshot>("backup.changed", [weak](BackupDataPageSnapshot snapshot) {
            const auto state = weak.lock();
            if (!state || state->closed) return;
            if (snapshot.generation == state->snapshot.generation &&
                snapshot.revision < state->snapshot.revision) return;
            state->snapshot = std::move(snapshot);
            if (state->changed) state->changed(state->snapshot);
        });
        channel_.Bind<void, Token, bool>("backup.confirmed", [weak](Token id, bool answer) {
            if (auto state = weak.lock(); state && !state->closed) state->confirmations.Complete(id, answer);
        });
        channel_.Bind<void, Token, Path>("backup.picked", [weak](Token id, Path selected) {
            if (auto state = weak.lock(); state && !state->closed) state->pickers.Complete(id, std::move(selected));
        });
        channel_.Bind<void, Token, BackupDataConfirmationRequest>("backup.dialog",
            [weak, &channel](Token id, BackupDataConfirmationRequest request) {
                const auto state = weak.lock();
                if (!state || state->closed || !state->options.confirm)
                { channel.Notify("backup.dialog.result", id, false); return; }
                state->options.confirm(state->options.ownerWindow(), std::move(request),
                    [weak, &channel, id](bool answer) {
                        if (auto live = weak.lock(); live && !live->closed && channel.Connected())
                            channel.Notify("backup.dialog.result", id, answer);
                    });
            });
        channel_.Bind<void, Token, BackupDataPickerRequest>("backup.picker",
            [weak, &channel](Token id, BackupDataPickerRequest request) {
                const auto state = weak.lock();
                if (!state || state->closed || !state->options.pickPath)
                { channel.Notify("backup.picker.result", id, Path{}); return; }
                state->options.pickPath(state->options.ownerWindow(), std::move(request),
                    [weak, &channel, id](Path selected) {
                        if (auto live = weak.lock(); live && !live->closed && channel.Connected())
                            channel.Notify("backup.picker.result", id, selected);
                    });
            });
        channel_.Call<void>("backup.create");
    }
    ~BackupProxy() override { Close(); }
    BackupDataPageActions Actions() override
    {
        BackupDataPageActions actions;
        std::weak_ptr<BackupProxyState> weak = state_;
        auto* channel = &channel_;
        actions.invoke = [weak, channel](Token generation, Token revision, BackupDataActionRequest request) {
            if (auto state = weak.lock(); state && !state->closed)
                channel->Notify("backup.invoke", generation, revision, request);
        };
        actions.cancel = [weak, channel](Token generation, Token revision, Token id) {
            if (auto state = weak.lock(); state && !state->closed)
                channel->Notify("backup.cancel", generation, revision, id);
        };
        actions.confirm = [weak, channel](Token generation, Token revision,
            BackupDataConfirmationRequest request, auto done) {
            if (auto state = weak.lock(); state && !state->closed)
            {
                const auto id = state->confirmations.Add(std::move(done));
                channel->Notify("backup.confirm", id, generation, revision, request);
            }
            else if (done) done(false);
        };
        actions.pickPath = [weak, channel](Token generation, Token revision,
            BackupDataPickerRequest request, auto done) {
            if (auto state = weak.lock(); state && !state->closed)
            {
                const auto id = state->pickers.Add(std::move(done));
                channel->Notify("backup.pick", id, generation, revision, request);
            }
            else if (done) done(std::nullopt);
        };
        return actions;
    }
    void SetSnapshotChangedCallback(SnapshotChangedCallback callback) override
    {
        state_->changed = std::move(callback);
        if (state_->changed) state_->changed(state_->snapshot);
    }
    BackupDataPageSnapshot CurrentSnapshot() const override { return state_->snapshot; }
    void Activate(Token generation) override
    { try { channel_.Call<void>("backup.Activate", generation); } catch (...) {} }
    void Deactivate() noexcept override
    { try { channel_.Call<void>("backup.Deactivate"); } catch (...) {} }
    void Refresh() override
    { try { channel_.Call<void>("backup.Refresh"); } catch (...) {} }
    void Close() noexcept override
    {
        if (state_->closed) return;
        state_->closed = true;
        // This acknowledgement must precede controller.CloseSession: a backup
        // may already have committed external replacement while we were open.
        try { channel_.Call<void>("backup.Close"); } catch (...) {}
        state_->confirmations.Cancel();
        state_->pickers.Cancel();
        state_->snapshot = {};
        state_->options = {};
        state_->changed = {};
    }
private:
    Channel& channel_;
    std::shared_ptr<BackupProxyState> state_;
};
}

struct BackendServer::Impl
{
    Channel& channel;
    ISettingsController& controller;
    WidgetEngine* engine;
    SettingsWindowHostOptions options;
    std::unique_ptr<WidgetsPageBackend> widgets;
    std::unique_ptr<BackupDataPageBackend> backup;
    Completions<bool> confirmations;
    Completions<Path> pickers;

    template<class... A> void Notify(const char* name, const A&... args) noexcept
    { try { if (channel.Connected()) channel.Notify(name, args...); } catch (...) {} }

    Impl(Channel& channelValue, ISettingsController& controllerValue,
        WidgetEngine* engineValue, SettingsWindowHostOptions configured)
        : channel(channelValue), controller(controllerValue), engine(engineValue), options(std::move(configured))
    {
#define SD_OPTION(Name, Return) \
        channel.Bind<Return>("options." #Name, [this] { return options.Name ? options.Name() : Return{}; });
        SD_OPTION(searchInput, SettingsSearchIndexInput)
        SD_OPTION(startupConflict, GeneralStartupConflict)
        SD_OPTION(advancedFeatureStatus, GeneralAdvancedFeatureStatus)
        SD_OPTION(developerToolsVisible, bool)
        SD_OPTION(debugVisible, bool)
#undef SD_OPTION
        using Languages = std::vector<std::pair<std::string, std::wstring>>;
        channel.Bind<Languages>("options.languageCatalog", [this] {
            return options.languageCatalog ? options.languageCatalog() : Languages{};
        });
        channel.Bind<std::string>("options.locale", [this] {
            return options.widgetsPage.locale ? options.widgetsPage.locale() : std::string("en-US");
        });
        channel.Bind<std::wstring>("options.title", [this] { return options.windowTitle; });
        channel.Bind<HomeAboutStatusPatch, Token, Token>("options.homeAboutStatus", [this](Token generation, Token revision) {
            return options.homeAboutStatus ? options.homeAboutStatus(generation, revision) : HomeAboutStatusPatch{};
        });
        channel.Bind<bool, std::wstring>("options.ensureWidget", [this](std::wstring id) {
            return options.ensureWidgetSettingsInstance && options.ensureWidgetSettingsInstance(id);
        });
        channel.Bind<void>("options.refreshExternalState", [this] {
            if (options.refreshExternalState) options.refreshExternalState();
        });
        channel.Bind<void>("options.registerAdvancedFeatures", [this] {
            if (options.registerAdvancedFeatures) options.registerAdvancedFeatures();
        });
        channel.Bind<bool>("options.resetAdvancedFeatures", [this] {
            return options.resetAdvancedFeatures && options.resetAdvancedFeatures();
        });
        channel.Bind<PageLayoutSnapshot>("pages.capture", [this] {
            return options.pageLayoutPage.capture ? options.pageLayoutPage.capture() : PageLayoutSnapshot{};
        });
        channel.Bind<LargeIconSettingsSnapshot, LargeIconSettingsRequest>("largeIcon.edit", [this](auto request) {
            return options.largeIconSettings ? options.largeIconSettings(std::move(request)) : LargeIconSettingsSnapshot{};
        });
        channel.Bind<PageGridChangeImpact, std::wstring, int, int>("pages.analyze", [this](std::wstring id, int columns, int rows) {
            return options.pageLayoutPage.analyzeGrid ? options.pageLayoutPage.analyzeGrid(id, columns, rows) : PageGridChangeImpact{};
        });
        channel.Bind<PageLayoutOperationResult, Token, std::vector<std::wstring>>("pages.order", [this](Token revision, auto ids) {
            return options.pageLayoutPage.applyOrder ? options.pageLayoutPage.applyOrder(revision, std::move(ids)) : PageLayoutOperationResult{};
        });
        channel.Bind<PageLayoutOperationResult, Token, std::wstring, int, int>("pages.grid", [this](Token revision, auto id, int columns, int rows) {
            return options.pageLayoutPage.applyGrid ? options.pageLayoutPage.applyGrid(revision, id, columns, rows) : PageLayoutOperationResult{};
        });
        channel.Bind<PageLayoutOperationResult, Token>("pages.add", [this](Token revision) {
            return options.pageLayoutPage.addPage ? options.pageLayoutPage.addPage(revision) : PageLayoutOperationResult{};
        });

        channel.Bind<void>("widgets.create", [this] { CreateWidgets(); });
        channel.Bind<bool, Token, bool>("widgets.Activate", [this](Token generation, bool discover) {
            return widgets && widgets->Activate(generation, discover);
        });
        channel.Bind<void>("widgets.Deactivate", [this] { if (widgets) widgets->Deactivate(); });
        channel.Bind<bool>("widgets.Refresh", [this] { return widgets && widgets->Refresh(); });
        channel.Bind<bool, Token, WidgetsPageRequest>("widgets.Invoke", [this](Token generation, auto request) {
            return widgets && widgets->Invoke(generation, std::move(request));
        });
        channel.Bind<void>("widgets.Close", [this] { if (widgets) widgets->Close(); widgets.reset(); });
        channel.Bind<void, Token, Path>("widgets.picked", [this](Token id, Path path) { pickers.Complete(id, std::move(path)); });
        channel.Bind<void, Token, bool>("widgets.confirmed", [this](Token id, bool answer) { confirmations.Complete(id, answer); });

        channel.Bind<void>("backup.create", [this] { CreateBackup(); });
        channel.Bind<void, Token>("backup.Activate", [this](Token generation) { if (backup) backup->Activate(generation); });
        channel.Bind<void>("backup.Deactivate", [this] { if (backup) backup->Deactivate(); });
        channel.Bind<void>("backup.Refresh", [this] { if (backup) backup->Refresh(); });
        channel.Bind<void>("backup.Close", [this] { if (backup) backup->Close(); backup.reset(); });
        channel.Bind<void, Token, Token, BackupDataActionRequest>("backup.invoke", [this](Token generation, Token revision, auto request) {
            if (backup) backup->Actions().invoke(generation, revision, std::move(request));
        });
        channel.Bind<void, Token, Token, Token>("backup.cancel", [this](Token generation, Token revision, Token id) {
            if (backup) backup->Actions().cancel(generation, revision, id);
        });
        channel.Bind<void, Token, Token, Token, BackupDataConfirmationRequest>("backup.confirm", [this](Token id, Token generation, Token revision, auto request) {
            if (backup) backup->Actions().confirm(generation, revision, std::move(request),
                [this, id](bool answer) { Notify("backup.confirmed", id, answer); });
            else Notify("backup.confirmed", id, false);
        });
        channel.Bind<void, Token, Token, Token, BackupDataPickerRequest>("backup.pick", [this](Token id, Token generation, Token revision, auto request) {
            if (backup) backup->Actions().pickPath(generation, revision, std::move(request),
                [this, id](Path path) { Notify("backup.picked", id, path); });
            else Notify("backup.picked", id, Path{});
        });
        channel.Bind<void, Token, bool>("backup.dialog.result", [this](Token id, bool answer) { confirmations.Complete(id, answer); });
        channel.Bind<void, Token, Path>("backup.picker.result", [this](Token id, Path path) { pickers.Complete(id, std::move(path)); });
    }
    void CreateWidgets()
    {
        if (widgets) widgets->Close();
        if (!engine) throw ProtocolError("settings widget engine unavailable");
        auto configured = options.widgetsPage;
        configured.localize = options.localize;
        configured.dispatchToOwner = channel.Poster();
        configured.diagnosticsVisible = [this] {
            const auto current = controller.Snapshot();
            return current && current->sessionActive &&
                ((current->route.page == SettingsPage::DeveloperTools && options.developerToolsVisible && options.developerToolsVisible()) ||
                 (current->route.page == SettingsPage::Debug && options.debugVisible && options.debugVisible()));
        };
        configured.snapshotChanged = [this](auto snapshot) { Notify("widgets.changed", snapshot); };
        configured.pickPackage = [this](Token generation, auto done) {
            const auto id = pickers.Add(std::move(done)); Notify("widgets.pick", id, generation);
        };
        configured.confirmInstall = [this](Token generation, auto request, auto done) {
            const auto id = confirmations.Add(std::move(done)); Notify("widgets.confirm", id, generation, request);
        };
        widgets = std::make_unique<WidgetsPageBackend>(*engine, std::move(configured));
    }
    void CreateBackup()
    {
        if (backup) backup->Close();
        auto configured = options.backupDataPage;
        configured.ownerWindow = channel.WindowProvider();
        configured.postToUi = channel.Poster();
        configured.localize = options.localize;
        configured.confirm = [this](HWND, auto request, auto done) {
            const auto id = confirmations.Add(std::move(done)); Notify("backup.dialog", id, request);
        };
        configured.pickPath = [this](HWND, auto request, auto done) {
            const auto id = pickers.Add(std::move(done)); Notify("backup.picker", id, request);
        };
        backup = std::make_unique<BackupDataPageBackend>(controller, std::move(configured));
        backup->SetSnapshotChangedCallback([this](const auto& snapshot) { Notify("backup.changed", snapshot); });
    }
    void ClosePages() noexcept
    {
        if (widgets) widgets->Close();
        if (backup) backup->Close();
        widgets.reset();
        backup.reset();
        confirmations.Cancel();
        pickers.Cancel();
    }
};

BackendServer::BackendServer(Channel& channel, ISettingsController& controller,
    WidgetEngine* engine, SettingsWindowHostOptions options)
    : impl_(std::make_unique<Impl>(channel, controller, engine, std::move(options))) {}
BackendServer::~BackendServer() { impl_->ClosePages(); }
void BackendServer::ClosePages() noexcept { impl_->ClosePages(); }

SettingsWindowHostOptions CreateRemoteHostOptions(Channel& channel)
{
    SettingsWindowHostOptions options;
    options.windowTitle = channel.Call<std::wstring>("options.title");
    options.localize = [](std::string_view key) { return std::wstring(Locale::Instance().TrW(std::string(key).c_str())); };
    options.languageCatalog = [&channel] { return channel.Call<std::vector<std::pair<std::string, std::wstring>>>("options.languageCatalog"); };
#define SD_REMOTE_OPTION(Name, Return) \
    options.Name = [&channel] { return channel.Call<Return>("options." #Name); };
    SD_REMOTE_OPTION(searchInput, SettingsSearchIndexInput)
    SD_REMOTE_OPTION(startupConflict, GeneralStartupConflict)
    SD_REMOTE_OPTION(advancedFeatureStatus, GeneralAdvancedFeatureStatus)
    SD_REMOTE_OPTION(developerToolsVisible, bool)
    SD_REMOTE_OPTION(debugVisible, bool)
#undef SD_REMOTE_OPTION
    options.homeAboutStatus = [&channel](Token generation, Token revision) { return channel.Call<HomeAboutStatusPatch>("options.homeAboutStatus", generation, revision); };
    options.ensureWidgetSettingsInstance = [&channel](std::wstring_view id) { return channel.Call<bool>("options.ensureWidget", std::wstring(id)); };
    options.refreshExternalState = [&channel] { channel.Call<void>("options.refreshExternalState"); };
    options.registerAdvancedFeatures = [&channel] { channel.Call<void>("options.registerAdvancedFeatures"); };
    options.resetAdvancedFeatures = [&channel] { return channel.Call<bool>("options.resetAdvancedFeatures"); };
    options.pageLayoutPage.capture = [&channel] { return channel.Call<PageLayoutSnapshot>("pages.capture"); };
    options.largeIconSettings = [&channel](LargeIconSettingsRequest request) {
        return channel.Call<LargeIconSettingsSnapshot>("largeIcon.edit", request);
    };
    options.pageLayoutPage.analyzeGrid = [&channel](std::wstring_view id, int columns, int rows) { return channel.Call<PageGridChangeImpact>("pages.analyze", std::wstring(id), columns, rows); };
    options.pageLayoutPage.applyOrder = [&channel](Token revision, const std::vector<std::wstring>& ids) { return channel.Call<PageLayoutOperationResult>("pages.order", revision, ids); };
    options.pageLayoutPage.applyGrid = [&channel](Token revision, std::wstring_view id, int columns, int rows) { return channel.Call<PageLayoutOperationResult>("pages.grid", revision, std::wstring(id), columns, rows); };
    options.pageLayoutPage.addPage = [&channel](Token revision) { return channel.Call<PageLayoutOperationResult>("pages.add", revision); };
    options.createWidgetsBackend = [&channel](WidgetsPageBackendOptions configured) { return std::make_unique<WidgetsProxy>(channel, std::move(configured)); };
    options.createBackupBackend = [&channel](BackupDataPageBackendOptions configured) { return std::make_unique<BackupProxy>(channel, std::move(configured)); };
    return options;
}
} // namespace snowdesktop::settings_ipc
