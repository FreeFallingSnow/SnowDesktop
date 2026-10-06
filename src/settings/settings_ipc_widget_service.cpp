#include "settings_ipc_services.h"
#include "settings_ipc_values.h"

namespace snowdesktop::settings_ipc
{
using namespace widget_runtime;
namespace
{
class WidgetServiceProxy final : public IWidgetSettingsService
{
public:
    explicit WidgetServiceProxy(Channel& channel) : channel_(channel)
    {
        channel_.Bind<void, WidgetSettingsSnapshotChanged>("widget.changed", [this](auto value) {
            if (changed_) changed_(std::move(value));
        });
        channel_.Bind<void, WidgetSettingSearchCompleted>("widget.search", [this](auto value) {
            if (searched_) searched_(std::move(value));
        });
        channel_.Call<void>("widget.subscribe");
    }
    ~WidgetServiceProxy() override
    {
        channel_.Unbind("widget.changed");
        channel_.Unbind("widget.search");
    }
    void SetEventCallbacks(SnapshotChangedCallback changed, SearchCompletedCallback searched) override
    { changed_ = std::move(changed); searched_ = std::move(searched); }
    WidgetSettingsLoadResult Load(std::wstring widgetId) override
    {
        try { return channel_.Call<WidgetSettingsLoadResult>("widget.Load", widgetId); }
        catch (...) { return {}; }
    }
    WidgetSettingsLoadResult Reload(std::wstring_view widgetId) override
    {
        try { return channel_.Call<WidgetSettingsLoadResult>("widget.Reload", std::wstring(widgetId)); }
        catch (...) { return {}; }
    }
    std::optional<WidgetSettingsSnapshot> Snapshot(std::wstring_view widgetId) const override
    {
        try { return channel_.Call<std::optional<WidgetSettingsSnapshot>>("widget.Snapshot", std::wstring(widgetId)); }
        catch (...) { return {}; }
    }
    void Close(std::wstring_view widgetId) override
    {
        try { channel_.Call<void>("widget.Close", std::wstring(widgetId)); }
        catch (...) {  }
    }
    void CloseAll() override
    {
        try { channel_.Call<void>("widget.CloseAll"); }
        catch (...) {  }
    }
    WidgetSettingMutationResult SetOrdinary(const WidgetSettingMutationGuard& guard, std::string_view key, const InteractionValue& value) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.SetOrdinary", guard, std::string(key), value); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult PreviewOrdinary(const WidgetSettingMutationGuard& guard, std::string_view key, const InteractionValue& value) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.PreviewOrdinary", guard, std::string(key), value); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult SetSearchQuery(const WidgetSettingMutationGuard& guard, std::string_view key, std::string query) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.SetSearchQuery", guard, std::string(key), query); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult SetSecret(const WidgetSettingMutationGuard& guard, std::string_view key, std::string_view plaintext) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.SetSecret", guard, std::string(key), std::string(plaintext)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult ChooseFilesystemHandle(const WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        try { return channel_.CallWithTimeout<WidgetSettingMutationResult>(INFINITE, "widget.ChooseFilesystemHandle", guard, std::string(key)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult OpenEntityReferencePicker(const WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        try { return channel_.CallWithTimeout<WidgetSettingMutationResult>(INFINITE, "widget.OpenEntityReferencePicker", guard, std::string(key)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult ClearOpaque(const WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.ClearOpaque", guard, std::string(key)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult ApplyPreset(const WidgetSettingMutationGuard& guard, std::string_view presetId) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.ApplyPreset", guard, std::string(presetId)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult UpdateHostAppearance(const WidgetSettingMutationGuard& guard, const WidgetHostAppearancePatch& patch) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.UpdateHostAppearance", guard, patch); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult PreviewHostAppearance(const WidgetSettingMutationGuard& guard, const WidgetHostAppearancePatch& patch) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.PreviewHostAppearance", guard, patch); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult CommitPreview(const WidgetSettingMutationGuard& guard) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.CommitPreview", guard); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult RevertPreview(const WidgetSettingMutationGuard& guard) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.RevertPreview", guard); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult ResetField(const WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.ResetField", guard, std::string(key)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult Reset(const WidgetSettingMutationGuard& guard) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.Reset", guard); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult StartSearch(const WidgetSettingMutationGuard& guard, std::string_view key, std::string query, std::size_t maximumResults) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.StartSearch", guard, std::string(key), query, maximumResults); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult CancelSearch(const WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.CancelSearch", guard, std::string(key)); }
        catch (...) { return {}; }
    }
    WidgetSettingMutationResult CommitSearchResult(const WidgetSettingMutationGuard& guard, std::string_view key, std::uint64_t requestId, std::string_view resultId) override
    {
        try { return channel_.Call<WidgetSettingMutationResult>("widget.CommitSearchResult", guard, std::string(key), requestId, std::string(resultId)); }
        catch (...) { return {}; }
    }
    std::optional<WidgetSettingSearchSnapshot> SearchSnapshot(std::wstring_view widgetId, std::string_view key) const override
    {
        try { return channel_.Call<std::optional<WidgetSettingSearchSnapshot>>("widget.SearchSnapshot", std::wstring(widgetId), std::string(key)); }
        catch (...) { return {}; }
    }
private:
    Channel& channel_;
    SnapshotChangedCallback changed_;
    SearchCompletedCallback searched_;
};
} // namespace

void BindWidgetService(Channel& channel, IWidgetSettingsService& service)
{
    service.SetEventCallbacks({}, {});
    channel.Bind<WidgetSettingsLoadResult, std::wstring>("widget.Load", [&](std::wstring widgetId) {
        return service.Load(widgetId);
    });
    channel.Bind<WidgetSettingsLoadResult, std::wstring>("widget.Reload", [&](std::wstring widgetId) {
        return service.Reload(widgetId);
    });
    channel.Bind<std::optional<WidgetSettingsSnapshot>, std::wstring>("widget.Snapshot", [&](std::wstring widgetId) {
        return service.Snapshot(widgetId);
    });
    channel.Bind<void, std::wstring>("widget.Close", [&](std::wstring widgetId) {
        service.Close(widgetId);
    });
    channel.Bind<void>("widget.CloseAll", [&]() {
        service.CloseAll();
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string, InteractionValue>("widget.SetOrdinary", [&](WidgetSettingMutationGuard guard, std::string key, InteractionValue value) {
        return service.SetOrdinary(guard, key, value);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string, InteractionValue>("widget.PreviewOrdinary", [&](WidgetSettingMutationGuard guard, std::string key, InteractionValue value) {
        return service.PreviewOrdinary(guard, key, value);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string, std::string>("widget.SetSearchQuery", [&](WidgetSettingMutationGuard guard, std::string key, std::string query) {
        return service.SetSearchQuery(guard, key, query);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string, std::string>("widget.SetSecret", [&](WidgetSettingMutationGuard guard, std::string key, std::string plaintext) {
        return service.SetSecret(guard, key, plaintext);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string>("widget.ChooseFilesystemHandle", [&](WidgetSettingMutationGuard guard, std::string key) {
        return service.ChooseFilesystemHandle(guard, key);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string>("widget.OpenEntityReferencePicker", [&](WidgetSettingMutationGuard guard, std::string key) {
        return service.OpenEntityReferencePicker(guard, key);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string>("widget.ClearOpaque", [&](WidgetSettingMutationGuard guard, std::string key) {
        return service.ClearOpaque(guard, key);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string>("widget.ApplyPreset", [&](WidgetSettingMutationGuard guard, std::string presetId) {
        return service.ApplyPreset(guard, presetId);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, WidgetHostAppearancePatch>("widget.UpdateHostAppearance", [&](WidgetSettingMutationGuard guard, WidgetHostAppearancePatch patch) {
        return service.UpdateHostAppearance(guard, patch);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, WidgetHostAppearancePatch>("widget.PreviewHostAppearance", [&](WidgetSettingMutationGuard guard, WidgetHostAppearancePatch patch) {
        return service.PreviewHostAppearance(guard, patch);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard>("widget.CommitPreview", [&](WidgetSettingMutationGuard guard) {
        return service.CommitPreview(guard);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard>("widget.RevertPreview", [&](WidgetSettingMutationGuard guard) {
        return service.RevertPreview(guard);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string>("widget.ResetField", [&](WidgetSettingMutationGuard guard, std::string key) {
        return service.ResetField(guard, key);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard>("widget.Reset", [&](WidgetSettingMutationGuard guard) {
        return service.Reset(guard);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string, std::string, std::size_t>("widget.StartSearch", [&](WidgetSettingMutationGuard guard, std::string key, std::string query, std::size_t maximumResults) {
        return service.StartSearch(guard, key, query, maximumResults);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string>("widget.CancelSearch", [&](WidgetSettingMutationGuard guard, std::string key) {
        return service.CancelSearch(guard, key);
    });
    channel.Bind<WidgetSettingMutationResult, WidgetSettingMutationGuard, std::string, std::uint64_t, std::string>("widget.CommitSearchResult", [&](WidgetSettingMutationGuard guard, std::string key, std::uint64_t requestId, std::string resultId) {
        return service.CommitSearchResult(guard, key, requestId, resultId);
    });
    channel.Bind<std::optional<WidgetSettingSearchSnapshot>, std::wstring, std::string>("widget.SearchSnapshot", [&](std::wstring widgetId, std::string key) {
        return service.SearchSnapshot(widgetId, key);
    });
    const auto post = channel.Poster();
    // Rebinding expires the previous token, including notifications already
    // queued by a search worker during the previous UI process lifetime.
    const auto token = std::make_shared<int>(0);
    channel.Bind<void>("widget.subscribe", [post, &channel, &service, token] {
        const std::weak_ptr<int> weak = token;
        service.SetEventCallbacks([post, &channel, weak](WidgetSettingsSnapshotChanged value) {
            (void)post([&channel, weak, value = std::move(value)] {
                try { if (!weak.expired() && channel.Connected()) channel.Notify("widget.changed", value); } catch (...) {}
            });
        }, [post, &channel, weak](WidgetSettingSearchCompleted value) {
            (void)post([&channel, weak, value = std::move(value)] {
                try { if (!weak.expired() && channel.Connected()) channel.Notify("widget.search", value); } catch (...) {}
            });
        });
    });
}

std::unique_ptr<IWidgetSettingsService> CreateWidgetServiceProxy(Channel& channel)
{
    return std::make_unique<WidgetServiceProxy>(channel);
}
} // namespace snowdesktop::settings_ipc
