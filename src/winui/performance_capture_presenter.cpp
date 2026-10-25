#include "pch.h"
#include "performance_capture_presenter.h"
#include "settings_presenter_controls.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace automation = winrt::Microsoft::UI::Xaml::Automation;
struct PerformanceCapturePresenter::Impl
{
    Localize L;
    std::function<void(const PerformanceCaptureRequest&)> action;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
    PerformanceCaptureStatus status;
    muxc::Border root;
    muxc::StackPanel content;
    muxc::TextBlock title, hint, path;
    muxc::ComboBox preset;
    muxc::NumberBox seconds, buffer;
    muxc::CheckBox cpu;
    muxc::Expander instances;
    muxc::ListView instanceList;
    muxc::InfoBar info;
    muxc::Button start, stop, analyze, folder, report;
    presenter_controls::SettingRow presetRow, secondsRow, bufferRow, cpuRow;
    bool applyingStatus = false;

    Impl(Localize localize, const mux::Style& style) : L(std::move(localize))
    {
        content.Spacing(8);
        title.TextWrapping(mux::TextWrapping::Wrap);
        (void)presenter_controls::InitializeSettingsGroup(root, title, content, style, true);
        preset.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        for (int i = 0; i < 3; ++i) preset.Items().Append(muxc::ComboBoxItem{});
        preset.SelectedIndex(0);
        presetRow.Initialize(preset); content.Children().Append(presetRow.root);
        seconds.Minimum(1); seconds.Maximum(600); seconds.Value(120);
        buffer.Minimum(1); buffer.Maximum(64); buffer.Value(32);
        for (auto number : {seconds, buffer})
        {
            number.Width(presenter_controls::kSettingNumberWidth);
            number.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
        }
        secondsRow.Initialize(seconds); content.Children().Append(secondsRow.root);
        bufferRow.Initialize(buffer); content.Children().Append(bufferRow.root);
        cpuRow.Initialize(cpu); content.Children().Append(cpuRow.root);
        cpu.IsChecked(false);
        automation::AutomationProperties::SetAutomationId(instanceList, L"PerformanceCaptureInstances");
        instanceList.SelectionMode(muxc::ListViewSelectionMode::Multiple);
        instanceList.MaxHeight(240);
        instances.Content(instanceList);
        content.Children().Append(instances);
        info.IsClosable(false); info.IsOpen(true);
        content.Children().Append(info);
        path.TextWrapping(mux::TextWrapping::Wrap); path.IsTextSelectionEnabled(true);
        content.Children().Append(path);
        const auto buttons = [&](std::initializer_list<muxc::Button> values) {
            muxc::Grid grid;
            int column = 0;
            for (const auto& button : values)
            {
                muxc::ColumnDefinition definition;
                definition.Width({1, mux::GridUnitType::Star});
                grid.ColumnDefinitions().Append(definition);
                button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                button.Margin({column ? 4.0 : 0.0, 0, 0, 0});
                muxc::Grid::SetColumn(button, column++);
                grid.Children().Append(button);
            }
            content.Children().Append(grid);
        };
        buttons({start, stop}); buttons({analyze, folder, report});
        hint.TextWrapping(mux::TextWrapping::Wrap); hint.Opacity(0.72);
        content.Children().Append(hint);
        const auto weak = std::weak_ptr<int>(alive);
        preset.SelectionChanged([this, weak](const auto&, const auto&) {
            if (weak.expired() || applyingStatus) return;
            const auto choice = preset.SelectedIndex();
            if (choice >= 0) { seconds.Value(choice == 0 ? 120 : choice == 2 ? 30 : 60); buffer.Value(choice == 0 ? 32 : 64); }
        });
        const auto bind = [&](const muxc::Button& button, PerformanceCaptureRequest::Action command) {
            button.Click([this, weak, command](const auto&, const auto&) {
                if (weak.expired() || !action) return;
                PerformanceCaptureRequest request;
                request.action = command; request.session = status.session; request.generation = status.generation;
                if (command == PerformanceCaptureRequest::Action::Start)
                {
                    const double duration = seconds.Value(), capacity = buffer.Value();
                    if (!std::isfinite(duration) || !std::isfinite(capacity) ||
                        duration < 1 || duration > 600 || capacity < 1 || capacity > 64 ||
                        std::floor(duration) != duration || std::floor(capacity) != capacity) return;
                    request.preset = static_cast<unsigned>(preset.SelectedIndex());
                    request.seconds = static_cast<unsigned>(duration); request.bufferMiB = static_cast<unsigned>(capacity);
                    request.threadCpu = cpu.IsChecked().Value();
                    for (const auto& item : instanceList.SelectedItems())
                    {
                        const auto row = item.as<muxc::ListViewItem>();
                        request.instances.push_back(winrt::unbox_value<winrt::hstring>(row.Tag()).c_str());
                    }
                    if (request.instances.size() > 8 || (request.preset == 2 && request.instances.empty()))
                    {
                        info.Message(L("performance.selectHint")); info.Severity(muxc::InfoBarSeverity::Warning); return;
                    }
                }
                action(request);
            });
        };
        bind(start, PerformanceCaptureRequest::Action::Start); bind(stop, PerformanceCaptureRequest::Action::Stop);
        bind(analyze, PerformanceCaptureRequest::Action::Analyze); bind(folder, PerformanceCaptureRequest::Action::OpenFolder);
        bind(report, PerformanceCaptureRequest::Action::OpenReport);
        RefreshText(); Render();
    }
    ~Impl() { alive.reset(); }
    void ButtonText(const muxc::Button& button, std::string_view key)
    {
        muxc::TextBlock text; text.Text(L(key)); text.TextWrapping(mux::TextWrapping::Wrap);
        button.Content(text); automation::AutomationProperties::SetName(button, L(key));
    }
    void RefreshText()
    {
        title.Text(L("performance.title")); hint.Text(L("performance.hint"));
        presetRow.SetText(L("performance.preset"), {}); secondsRow.SetText(L("performance.seconds"), {});
        bufferRow.SetText(L("performance.buffer"), {}); cpuRow.SetText(L("performance.cpu"), L("performance.cpuHint"));
        automation::AutomationProperties::SetName(preset, L("performance.preset"));
        automation::AutomationProperties::SetName(seconds, L("performance.seconds"));
        automation::AutomationProperties::SetName(buffer, L("performance.buffer"));
        automation::AutomationProperties::SetName(cpu, L("performance.cpu"));
        for (int i = 0; i < 3; ++i)
        {
            constexpr const char* keys[]{"performance.idle", "performance.interaction", "performance.widget"};
            preset.Items().GetAt(i).as<muxc::ComboBoxItem>().Content(winrt::box_value(L(keys[i])));
        }
        instances.Header(winrt::box_value(L("performance.instances")));
        ButtonText(start, "performance.start"); ButtonText(stop, "performance.stop"); ButtonText(analyze, "performance.analyze");
        ButtonText(folder, "performance.folder"); ButtonText(report, "performance.report"); Render();
    }
    void Render()
    {
        const bool busy = status.phase == "starting" || status.phase == "recording" || status.phase == "stopping" || status.phase == "finalizing";
        const bool analyzing = status.analysis == "queued" || status.analysis == "running";
        const bool complete = status.phase == "completed";
        if (busy)
        {
            applyingStatus = true;
            preset.SelectedIndex(static_cast<int>(status.preset)); seconds.Value(status.requestedSeconds);
            buffer.Value(status.bufferKiB / 1024.0); cpu.IsChecked(status.threadCpu);
            instanceList.SelectedItems().Clear();
            for (const auto& item : instanceList.Items())
            {
                const auto row = item.as<muxc::ListViewItem>();
                const std::wstring id = winrt::unbox_value<winrt::hstring>(row.Tag()).c_str();
                if (std::find(status.selectedInstances.begin(), status.selectedInstances.end(), id) != status.selectedInstances.end())
                    instanceList.SelectedItems().Append(row);
            }
            applyingStatus = false;
        }
        start.IsEnabled(!busy && !analyzing); stop.IsEnabled(status.phase == "starting" || status.phase == "recording");
        analyze.IsEnabled(complete && !analyzing); folder.IsEnabled(!status.trace.empty()); report.IsEnabled(!status.report.empty());
        for (auto row : {presetRow, secondsRow, bufferRow, cpuRow}) row.SetEnabled(!busy);
        instances.IsEnabled(!busy);
        std::wstring stateText = L("performance.state.idle");
        if (analyzing) stateText = L("performance.state.analyzing");
        else if (status.phase == "starting") stateText = L("performance.state.starting");
        else if (status.phase == "recording") stateText = L("performance.state.recording");
        else if (status.phase == "stopping") stateText = L("performance.state.stopping");
        else if (status.phase == "finalizing") stateText = L("performance.state.finalizing");
        else if (status.phase == "completed") stateText = L("performance.state.completed");
        else if (status.phase == "failed") stateText = L("performance.state.failed");
        info.Title(stateText);
        info.Severity(status.error.empty() ? muxc::InfoBarSeverity::Informational : muxc::InfoBarSeverity::Error);
        std::wstring detail;
        if (busy) detail = L("performance.remaining") + L" " + std::to_wstring(static_cast<unsigned>(
            std::ceil(std::max(0.0, status.requestedSeconds - status.elapsedMs / 1000.0))));
        if (!status.error.empty()) detail += (detail.empty() ? L"" : L"\n") + std::wstring(status.error.begin(), status.error.end());
        if (status.analysis == "failed") detail += L"\n" + L("performance.analysisFailed");
        if (status.analysis == "completed")
        {
            std::wstring qualityText = L("performance.quality.unknown");
            if (status.quality == "valid") qualityText = L("performance.quality.valid");
            else if (status.quality == "partial") qualityText = L("performance.quality.partial");
            else if (status.quality == "invalid") qualityText = L("performance.quality.invalid");
            detail += (detail.empty() ? L"" : L"\n") + qualityText;
            if (status.quality == "invalid" || status.quality == "partial") info.Severity(muxc::InfoBarSeverity::Warning);
        }
        info.Message(detail); path.Text(status.trace);
    }
    void Apply(const PerformanceCaptureStatus& value)
    {
        // Rebuild labels only if inventory changed; preserve local selection.
        const auto same = [&] {
            if (value.instances.size() != status.instances.size()) return false;
            for (std::size_t i = 0; i < value.instances.size(); ++i)
                if (value.instances[i].id != status.instances[i].id || value.instances[i].name != status.instances[i].name) return false;
            return true;
        }();
        if (!same)
        {
            std::vector<std::wstring> selected;
            for (const auto& item : instanceList.SelectedItems()) selected.emplace_back(winrt::unbox_value<winrt::hstring>(item.as<muxc::ListViewItem>().Tag()).c_str());
            instanceList.Items().Clear();
            for (const auto& instance : value.instances)
            {
                muxc::ListViewItem row; row.Content(winrt::box_value(instance.name)); row.Tag(winrt::box_value(instance.id));
                instanceList.Items().Append(row);
                if (std::find(selected.begin(), selected.end(), instance.id) != selected.end())
                    instanceList.SelectedItems().Append(row);
            }
        }
        status = value; Render();
    }
};
PerformanceCapturePresenter::PerformanceCapturePresenter(Localize L, const mux::Style& style) : impl_(std::make_unique<Impl>(std::move(L), style)) {}
PerformanceCapturePresenter::~PerformanceCapturePresenter() = default;
void PerformanceCapturePresenter::SetAction(std::function<void(const PerformanceCaptureRequest&)> action) { impl_->action = std::move(action); }
void PerformanceCapturePresenter::ApplyStatus(const PerformanceCaptureStatus& status) { impl_->Apply(status); }
void PerformanceCapturePresenter::RefreshText() { impl_->RefreshText(); }
mux::FrameworkElement PerformanceCapturePresenter::Root() const { return impl_->root; }
mux::FrameworkElement PerformanceCapturePresenter::FocusTarget() const { return impl_->preset; }
}
