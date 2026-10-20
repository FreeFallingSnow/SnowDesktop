#include "pch.h"
#include "dock_tray_settings_editor.h"
#include "settings_presenter_controls.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <shobjidl.h>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace rules = snowdesktop::dock_tray;

struct DockTraySettingsEditor::Impl
{
    Localize localize;
    Edit edit;
    Query query;
    muxc::Expander expander{nullptr};
    muxc::TextBlock title;
    muxc::StackPanel list;
    muxc::ComboBox match, running;
    muxc::TextBox target;
    muxc::Button add, browse;
    muxc::InfoBar error;
    presenter_controls::SettingRow matchRow, targetRow, runningRow;
    winrt::event_token matchToken{}, addToken{}, browseToken{}, runningOpenToken{}, runningSelectToken{};
    struct Row
    {
        presenter_controls::SettingRow row;
        muxc::ToggleSwitch enabled;
        muxc::ComboBox action;
        muxc::Button remove;
        winrt::event_token enabledToken{}, actionToken{}, removeToken{};
    };
    std::vector<Row> rows;
    std::vector<DockTrayRule> values;
    std::vector<DockFullscreenApplication> applications;
    struct Lifetime { bool alive = true; std::uint64_t revision = 0; };
    std::shared_ptr<Lifetime> lifetime = std::make_shared<Lifetime>();
    bool updating = false;

    std::wstring L(std::string_view key, std::wstring_view fallback) const
    {
        return localize ? localize(key, fallback) : std::wstring(fallback);
    }
    Impl(Localize text, Edit change, Query source) : localize(std::move(text)), edit(std::move(change)), query(std::move(source))
    {
        expander = mux::Markup::XamlReader::Load(
            LR"(<Expander xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" Background="{ThemeResource SubtleFillColorTransparentBrush}" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch" IsExpanded="False" Padding="0" />)")
            .as<muxc::Expander>();
        title.TextWrapping(mux::TextWrapping::Wrap);
        expander.Header(title);
        muxc::StackPanel content;
        content.Spacing(0);
        matchRow.Initialize(match);
        match.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presenter_controls::StyleSettingsSubrow(matchRow.root, true, 0);
        content.Children().Append(matchRow.root);
        muxc::Grid input;
        muxc::ColumnDefinition textColumn, addColumn, browseColumn;
        textColumn.Width({1, mux::GridUnitType::Star});
        addColumn.Width({1, mux::GridUnitType::Auto});
        browseColumn.Width({1, mux::GridUnitType::Auto});
        input.ColumnDefinitions().Append(textColumn);
        input.ColumnDefinitions().Append(addColumn);
        input.ColumnDefinitions().Append(browseColumn);
        input.ColumnSpacing(8);
        target.MinWidth(120);
        input.Children().Append(target);
        muxc::Grid::SetColumn(add, 1);
        input.Children().Append(add);
        muxc::Grid::SetColumn(browse, 2);
        input.Children().Append(browse);
        targetRow.Initialize(input, 440);
        presenter_controls::StyleSettingsSubrow(targetRow.root, false, 0);
        content.Children().Append(targetRow.root);
        runningRow.Initialize(running);
        running.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        presenter_controls::StyleSettingsSubrow(runningRow.root, false, 0);
        content.Children().Append(runningRow.root);
        error.IsClosable(false);
        error.IsOpen(false);
        error.Severity(muxc::InfoBarSeverity::Error);
        presenter_controls::BindInfoBarVisibility(error);
        content.Children().Append(error);
        list.Spacing(0);
        content.Children().Append(list);
        expander.Content(content);
        matchToken = match.SelectionChanged([this](const auto&, const auto&) {
            if (updating) return;
            target.PlaceholderText(match.SelectedIndex() == 1 ? L"D:\\Apps\\App.exe" : L"App.exe");
        });
        addToken = add.Click([this](const auto&, const auto&) { Add(std::wstring(target.Text())); });
        browseToken = browse.Click([this](const auto&, const auto&) { Browse(); });
        runningOpenToken = running.DropDownOpened([this](const auto&, const auto&) {
            if (!query) return;
            const auto gate = lifetime;
            const auto revision = gate->revision;
            const auto result = query();
            if (!gate->alive || gate->revision != revision) return;
            updating = true;
            applications = result;
            running.Items().Clear();
            running.Items().Append(winrt::box_value(L("settings.dock.fullscreenFromRunning", L"Choose from running apps")));
            for (const auto& application : applications)
                running.Items().Append(winrt::box_value(application.executable));
            running.SelectedIndex(0);
            updating = false;
        });
        runningSelectToken = running.SelectionChanged([this](const auto&, const auto&) {
            if (updating) return;
            const auto selected = running.SelectedIndex();
            if (selected <= 0 || static_cast<size_t>(selected) > applications.size()) return;
            const auto path = applications[static_cast<size_t>(selected - 1)].executable;
            running.SelectedIndex(0);
            Add(path);
        });
        RefreshText();
    }
    ~Impl()
    {
        lifetime->alive = false;
        match.SelectionChanged(matchToken);
        add.Click(addToken);
        browse.Click(browseToken);
        running.DropDownOpened(runningOpenToken);
        running.SelectionChanged(runningSelectToken);
        ClearRows();
    }
    void ClearRows()
    {
        for (auto& row : rows)
        {
            row.enabled.Toggled(row.enabledToken);
            row.action.SelectionChanged(row.actionToken);
            row.remove.Click(row.removeToken);
        }
        rows.clear();
        list.Children().Clear();
    }
    void Modify(const DockTrayRule& original, std::function<void(DockTrayRule&)> change)
    {
        if (!edit) return;
        edit([original, change = std::move(change)](DockSettings& settings) {
            for (auto& rule : settings.trayApplications)
                if (rule.matchPath == original.matchPath && rules::Equal(rule.executable, original.executable)) change(rule);
        });
    }
    void Rebuild()
    {
        ClearRows();
        for (const auto& value : values)
        {
            Row row;
            row.action.Items().Append(winrt::box_value(L("settings.dock.traySingleClick", L"Single-click tray icon")));
            row.action.Items().Append(winrt::box_value(L("settings.dock.trayDoubleClick", L"Double-click tray icon")));
            row.action.SelectedIndex(value.doubleClick ? 1 : 0);
            row.enabled.OnContent(winrt::box_value(L""));
            row.enabled.OffContent(winrt::box_value(L""));
            row.enabled.MinWidth(0);
            row.enabled.VerticalAlignment(mux::VerticalAlignment::Center);
            row.action.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            row.action.VerticalAlignment(mux::VerticalAlignment::Center);
            row.remove.VerticalAlignment(mux::VerticalAlignment::Center);
            row.enabled.IsOn(value.enabled);
            row.remove.Content(winrt::box_value(L("app.settings.delete", L"Delete")));
            muxc::Grid controls;
            muxc::ColumnDefinition actionColumn, enabledColumn, removeColumn;
            actionColumn.Width({1, mux::GridUnitType::Star});
            enabledColumn.Width({1, mux::GridUnitType::Auto});
            removeColumn.Width({1, mux::GridUnitType::Auto});
            controls.ColumnDefinitions().Append(actionColumn);
            controls.ColumnDefinitions().Append(enabledColumn);
            controls.ColumnDefinitions().Append(removeColumn);
            controls.ColumnSpacing(8);
            controls.Children().Append(row.action);
            muxc::Grid::SetColumn(row.enabled, 1);
            controls.Children().Append(row.enabled);
            muxc::Grid::SetColumn(row.remove, 2);
            controls.Children().Append(row.remove);
            row.row.Initialize(controls);
            presenter_controls::StyleSettingsSubrow(row.row.root, false, 0);
            row.row.SetText(value.executable, L(value.matchPath ? "settings.dock.trayMatchPath" : "settings.dock.trayMatchName",
                value.matchPath ? L"Full path" : L"Executable name"));
            row.row.label.TextWrapping(mux::TextWrapping::Wrap);
            muxa::AutomationProperties::SetName(row.action, value.executable + L" — " + L("settings.dock.trayAction", L"Tray action"));
            muxa::AutomationProperties::SetName(row.enabled, value.executable);
            muxa::AutomationProperties::SetName(row.remove, L("app.settings.delete", L"Delete") + L" " + value.executable);
            const auto action = row.action;
            row.actionToken = row.action.SelectionChanged([this, value, action](const auto&, const auto&) {
                if (action.SelectedIndex() < 0) return;
                const bool doubleClick = action.SelectedIndex() == 1;
                Modify(value, [doubleClick](auto& rule) { rule.doubleClick = doubleClick; });
            });
            const auto enabled = row.enabled;
            row.enabledToken = row.enabled.Toggled([this, value, enabled](const auto&, const auto&) {
                const bool on = enabled.IsOn();
                Modify(value, [on](auto& rule) { rule.enabled = on; });
            });
            row.removeToken = row.remove.Click([this, value](const auto&, const auto&) {
                if (edit) edit([value](DockSettings& settings) {
                    std::erase_if(settings.trayApplications, [&](const auto& rule) {
                        return rule.matchPath == value.matchPath && rules::Equal(rule.executable, value.executable);
                    });
                });
            });
            list.Children().Append(row.row.root);
            rows.push_back(std::move(row));
        }
        title.Text(L("settings.dock.trayApplications", L"Applications to open through the tray") + L" (" + std::to_wstring(values.size()) + L")");
        muxa::AutomationProperties::SetName(expander, title.Text());
    }
    void Add(std::wstring path)
    {
        const bool matchPath = match.SelectedIndex() == 1;
        if (!matchPath) path = rules::ExecutableName(path);
        path = rules::NormalizeTarget(std::move(path), matchPath);
        error.IsOpen(path.empty() || values.size() >= 128);
        if (path.empty() || values.size() >= 128 || !edit) return;
        target.Text(L"");
        edit([path, matchPath](DockSettings& settings) {
            for (auto& rule : settings.trayApplications)
                if (rule.matchPath == matchPath && rules::Equal(rule.executable, path)) { rule.enabled = true; return; }
            if (settings.trayApplications.size() < 128) settings.trayApplications.push_back({path, matchPath});
        });
    }
    void Browse()
    {
        const auto gate = lifetime;
        const auto revision = gate->revision;
        winrt::com_ptr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put())))) return;
        DWORD options = 0;
        if (FAILED(dialog->GetOptions(&options)) || FAILED(dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST))) return;
        dialog->SetTitle(L("settings.dock.fullscreenExceptionAdd", L"Add application").c_str());
        const COMDLG_FILTERSPEC filter{L"*.exe", L"*.exe"};
        if (FAILED(dialog->SetFileTypes(1, &filter)) || FAILED(dialog->Show(GetActiveWindow()))) return;
        if (!gate->alive || gate->revision != revision) return;
        winrt::com_ptr<IShellItem> selected;
        PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(selected.put())) || FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        const std::wstring value(path);
        CoTaskMemFree(path);
        Add(value);
    }
    void RefreshText()
    {
        updating = true;
        const int selected = std::max(0, match.SelectedIndex());
        match.Items().Clear();
        match.Items().Append(winrt::box_value(L("settings.dock.trayMatchName", L"Executable name")));
        match.Items().Append(winrt::box_value(L("settings.dock.trayMatchPath", L"Full path")));
        match.SelectedIndex(selected);
        matchRow.SetText(L("settings.dock.trayMatch", L"Match by"), L"");
        targetRow.SetText(L("settings.dock.trayProgram", L"Program"), L"");
        runningRow.SetText(L("settings.dock.fullscreenFromRunning", L"Choose from running apps"), L"");
        target.PlaceholderText(selected == 1 ? L"D:\\Apps\\App.exe" : L"App.exe");
        add.Content(winrt::box_value(L("settings.dock.trayAdd", L"Add")));
        browse.Content(winrt::box_value(L("settings.dock.fullscreenExceptionAdd", L"Add application")));
        error.Message(L("settings.dock.trayInvalidTarget", L"Enter an EXE name or an absolute EXE path. Up to 128 entries are supported."));
        running.Items().Clear();
        applications.clear();
        running.Items().Append(winrt::box_value(L("settings.dock.fullscreenFromRunning", L"Choose from running apps")));
        running.SelectedIndex(0);
        muxa::AutomationProperties::SetName(match, matchRow.label.Text());
        muxa::AutomationProperties::SetName(target, targetRow.label.Text());
        muxa::AutomationProperties::SetName(running, runningRow.label.Text());
        updating = false;
        Rebuild();
    }
};

DockTraySettingsEditor::DockTraySettingsEditor(Localize localize, Edit edit, Query query)
    : impl_(std::make_unique<Impl>(std::move(localize), std::move(edit), std::move(query))) {}
DockTraySettingsEditor::~DockTraySettingsEditor() = default;
mux::UIElement DockTraySettingsEditor::Content() const { return presenter_controls::SettingsDisclosureRow(impl_->expander); }
mux::FrameworkElement DockTraySettingsEditor::FocusTarget() { impl_->expander.IsExpanded(true); return impl_->target; }
void DockTraySettingsEditor::Update(const DockSettings& settings)
{
    if (impl_->values == settings.trayApplications) return;
    impl_->values = settings.trayApplications;
    ++impl_->lifetime->revision;
    impl_->Rebuild();
}
void DockTraySettingsEditor::RefreshText() { impl_->RefreshText(); }
}
