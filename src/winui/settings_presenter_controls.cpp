#include "pch.h"
#include "settings_presenter_controls.h"

#include "number_box_update_rules.h"

namespace snowdesktop::winui::presenter_controls
{
namespace
{
bool IsNumberBoxTextEditing(const muxc::NumberBox& number)
{
    const auto root = number.XamlRoot();
    if (!root) return false;
    auto focused = muxi::FocusManager::GetFocusedElement(root)
        .try_as<mux::DependencyObject>();
    if (!focused || !focused.try_as<muxc::TextBox>()) return false;
    while (focused)
    {
        if (focused == number) return true;
        focused = muxm::VisualTreeHelper::GetParent(focused);
    }
    return false;
}
} // namespace

void SetNumberBoxValue(const muxc::NumberBox& number, double value)
{
    if (number_box_update_rules::ShouldWriteValue(number.Value(), value))
        number.Value(value);
}

void SyncNumberBoxValue(const muxc::NumberBox& number, double value)
{
    const double current = number.Value();
    if (!number_box_update_rules::ShouldWriteValue(current, value)) return;
    if (number_box_update_rules::ShouldWriteValue(
        current, value, IsNumberBoxTextEditing(number)))
    {
        number.Value(value);
    }
}

double QuantizeNumericValue(
    double value,
    double minimum,
    double maximum,
    double step) noexcept
{
    value = std::clamp(value, minimum, maximum);
    if (!std::isfinite(step) || step <= 0.0)
        return value;
    const double ticks = std::round((value - minimum) / step);
    return std::clamp(minimum + ticks * step, minimum, maximum);
}

ContinuousNumberBinding::~ContinuousNumberBinding()
{
    Close();
}

void ContinuousNumberBinding::Bind(const muxc::Slider& slider,
    const muxc::NumberBox& number, const muxc::Button& reset, double defaultValue,
    bool& synchronizingPair, Callbacks callbacks)
{
    slider_ = slider;
    number_ = number;
    reset_ = reset;
    synchronizingPair_ = &synchronizingPair;
    callbacks_ = std::move(callbacks);

    idleCommit_ = mux::DispatcherTimer{};
    idleCommit_.Interval(std::chrono::milliseconds(650));
    idleCommitTick_ = idleCommit_.Tick(
        [this](const auto&, const auto&) { Commit(); });
    preview_.Initialize([this](const double& value) {
        if (dirty_ && callbacks_.canEmit())
            callbacks_.update(SettingsUpdateMode::Preview, value);
    });

    sliderChanged_ = slider_.ValueChanged([this](const auto&, const auto&) {
        if (!CanSynchronize()) return;
        const double value = slider_.Value();
        *synchronizingPair_ = true;
        SetNumberBoxValue(number_, value);
        *synchronizingPair_ = false;
        Preview(value);
    });
    numberChanged_ = number_.ValueChanged([this](const auto&, const auto&) {
        if (!CanSynchronize()) return;
        double value = number_.Value();
        if (std::isnan(value)) value = slider_.Value();
        value = std::clamp(value, slider_.Minimum(), slider_.Maximum());
        *synchronizingPair_ = true;
        slider_.Value(value);
        *synchronizingPair_ = false;
        Preview(value);
    });

    const auto commit = [this](const auto&, const auto&) { Commit(); };
    const auto commitOnEnter = [this](const auto&, const muxi::KeyRoutedEventArgs& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter)
            Commit();
    };
    sliderReleased_ = slider_.PointerReleased(commit);
    numberReleased_ = number_.PointerReleased(commit);
    sliderLostFocus_ = slider_.LostFocus(commit);
    numberLostFocus_ = number_.LostFocus(commit);
    sliderKeyDown_ = slider_.KeyDown(commitOnEnter);
    numberKeyDown_ = number_.KeyDown(commitOnEnter);
    if (reset_)
    {
        resetClicked_ = reset_.Click([this, defaultValue](const auto&, const auto&) {
            if (!synchronizingPair_ || !std::isfinite(defaultValue)) return;
            Cancel();
            callbacks_.update(SettingsUpdateMode::PreviewAndCommit, defaultValue);
        });
    }
}

bool ContinuousNumberBinding::CanSynchronize() const
{
    return synchronizingPair_ && !*synchronizingPair_ && callbacks_.canSynchronize();
}

void ContinuousNumberBinding::PatchValue(double value)
{
    value = QuantizeNumericValue(value, slider_.Minimum(), slider_.Maximum(),
        slider_.StepFrequency());
    slider_.Value(value);
    SyncNumberBoxValue(number_, value);
}

void ContinuousNumberBinding::Preview(double value)
{
    if (!synchronizingPair_ || !callbacks_.canEmit()) return;
    dirty_ = true;
    idleCommit_.Stop();
    idleCommit_.Start();
    preview_.Queue(value);
}

void ContinuousNumberBinding::Commit()
{
    if (!synchronizingPair_) return;
    if (idleCommit_) idleCommit_.Stop();
    preview_.Cancel();
    if (!dirty_ || !callbacks_.canEmit()) return;
    dirty_ = false;
    callbacks_.update(SettingsUpdateMode::PreviewAndCommit, slider_.Value());
}

void ContinuousNumberBinding::Cancel() noexcept
{
    dirty_ = false;
    preview_.Cancel();
    try
    {
        if (idleCommit_) idleCommit_.Stop();
    }
    catch (...) {}
}

void ContinuousNumberBinding::Close() noexcept
{
    if (!synchronizingPair_) return;
    synchronizingPair_ = nullptr;
    Cancel();
    preview_.Close();
    try
    {
        if (idleCommit_) idleCommit_.Tick(idleCommitTick_);
        slider_.ValueChanged(sliderChanged_);
        number_.ValueChanged(numberChanged_);
        slider_.PointerReleased(sliderReleased_);
        number_.PointerReleased(numberReleased_);
        slider_.LostFocus(sliderLostFocus_);
        number_.LostFocus(numberLostFocus_);
        slider_.KeyDown(sliderKeyDown_);
        number_.KeyDown(numberKeyDown_);
        if (reset_) reset_.Click(resetClicked_);
    }
    catch (...) {}
    callbacks_ = {};
}
} // namespace snowdesktop::winui::presenter_controls
