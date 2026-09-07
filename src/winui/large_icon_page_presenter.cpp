#include "pch.h"
#include "large_icon_page_presenter.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;

struct LargeIconPagePresenter::Impl : std::enable_shared_from_this<Impl>
{
    std::function<std::wstring(std::string_view)> localize;
    x::Style style{nullptr};
    LargeIconSettingsAction action;
    LargeIconSettingsSnapshot snapshot;
    LargeIconConfig draft;
    c::StackPanel root, editors;
    c::ContentControl editorHost;
    c::TextBlock status;
    c::Border preview;
    c::Image previewImage;
    x::DispatcherTimer timer;
    bool active = false, submitting = false;

    std::wstring L(std::string_view key) { return localize ? localize(key) : std::wstring{}; }
    static winrt::Windows::UI::Color Color(std::uint32_t rgb, double alpha = 1)
    { return {static_cast<uint8_t>(alpha * 255), static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)}; }

    bool Send(std::string operation, std::wstring path = {})
    {
        if (!action || submitting) return false;
        submitting = true;
        const bool polling = operation == "status";
        const auto oldRevision = snapshot.revision;
        const auto oldError = snapshot.error;
        LargeIconSettingsRequest request{snapshot.key, snapshot.session, snapshot.revision,
            std::move(operation), EncodeLargeIconConfig(draft), std::move(path)};
        try { snapshot = action(std::move(request)); }
        catch (...) { snapshot.succeeded = false; snapshot.error = "largeIcon.unavailable"; }
        submitting = false;
        if (polling && snapshot.succeeded && snapshot.revision != oldRevision)
        { ReloadDraft(); Build(); }
        else if (polling && snapshot.error.empty() && (oldError == "largeIcon.saveFailed" || oldError == "largeIcon.invalid" || oldError == "largeIcon.stale")) snapshot.error = oldError;
        status.Text(snapshot.error.empty() ? L("largeIcon.livePreview") : L(snapshot.error));
        editorHost.IsEnabled(snapshot.editable && snapshot.available);
        UpdatePreview();
        return snapshot.succeeded;
    }

    void UpdatePreview()
    {
        preview.Width(230);
        preview.Height(std::clamp(230. * draft.rows / std::max(1, draft.columns), 90., 260.));
        preview.CornerRadius(x::CornerRadius{draft.radius});
        preview.Background(m::SolidColorBrush(Color(draft.autoColor ? 0x505866 : draft.manualColor, draft.opacity)));
        preview.BorderBrush(m::SolidColorBrush(Color(draft.borderColor, draft.borderOpacity)));
        preview.BorderThickness(x::Thickness{draft.border ? draft.borderWidth : 0});
        if (!snapshot.imagePath.empty())
        {
            m::Imaging::BitmapImage source;
            source.UriSource(winrt::Windows::Foundation::Uri(snapshot.imagePath));
            previewImage.Source(source);
            previewImage.Stretch(draft.fit == 1 && draft.content != 0 ? m::Stretch::UniformToFill : m::Stretch::Uniform);
            previewImage.Margin(x::Thickness{draft.content == 0 ? preview.Height() * (1 - draft.contentScale) / 2 : 0});
        }
    }

    c::StackPanel Group(const char* key)
    {
        c::Border border; border.Style(style);
        c::StackPanel panel; panel.Spacing(12);
        c::TextBlock heading; heading.Text(L(key));
        heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        heading.TextWrapping(x::TextWrapping::Wrap);
        panel.Children().Append(heading); border.Child(panel);
        editors.Children().Append(border); return panel;
    }
    template<class T> void Number(c::StackPanel panel, const char* key, T LargeIconConfig::* member,
        double min, double max, double step = 1)
    {
        c::NumberBox input; input.Header(winrt::box_value(L(key)));
        input.Minimum(min); input.Maximum(std::max(min, max)); input.SmallChange(step);
        input.SpinButtonPlacementMode(c::NumberBoxSpinButtonPlacementMode::Inline);
        input.Value(static_cast<double>(draft.*member));
        x::Automation::AutomationProperties::SetName(input, L(key));
        std::weak_ptr<Impl> weak = shared_from_this();
        input.ValueChanged([weak, member](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && std::isfinite(args.NewValue()))
            { self->draft.*member = static_cast<T>(args.NewValue()); self->Send("commit"); }
        });
        panel.Children().Append(input);
    }
    void Slider(c::StackPanel panel, const char* key, double LargeIconConfig::* member, double min, double max, double step = .01)
    {
        c::Slider input; input.Header(winrt::box_value(L(key)));
        input.Minimum(min); input.Maximum(max); input.StepFrequency(step); input.Value(draft.*member);
        x::Automation::AutomationProperties::SetName(input, L(key));
        std::weak_ptr<Impl> weak = shared_from_this();
        input.ValueChanged([weak, member](auto const&, auto const& args) {
            if (auto self = weak.lock())
            {
                self->draft.*member = args.NewValue();
                if (member == &LargeIconConfig::opacity && self->draft.hoverOpacityLinked)
                    self->draft.hoverOpacity = std::min(1., args.NewValue() + .15);
                if (member == &LargeIconConfig::hoverOpacity) self->draft.hoverOpacityLinked = false;
                self->Send("preview");
            }
        });
        const auto commit = [weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Send("commit"); };
        input.PointerCaptureLost(commit); input.KeyUp(commit); input.LostFocus(commit);
        panel.Children().Append(input);
    }
    void Toggle(c::StackPanel panel, const char* key, bool LargeIconConfig::* member)
    {
        c::ToggleSwitch input; input.Header(winrt::box_value(L(key))); input.IsOn(draft.*member);
        std::weak_ptr<Impl> weak = shared_from_this();
        input.Toggled([weak, member](auto const& sender, auto const&) {
            if (auto self = weak.lock()) { self->draft.*member = sender.template as<c::ToggleSwitch>().IsOn(); self->Send("commit"); }
        });
        panel.Children().Append(input);
    }
    void Choice(c::StackPanel panel, const char* key, int LargeIconConfig::* member, std::initializer_list<const char*> choices)
    {
        c::ComboBox input; input.Header(winrt::box_value(L(key))); input.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        for (const char* choice : choices) input.Items().Append(winrt::box_value(L(choice)));
        input.SelectedIndex(draft.*member);
        std::weak_ptr<Impl> weak = shared_from_this();
        input.SelectionChanged([weak, member](auto const& sender, auto const&) {
            if (auto self = weak.lock())
            {
                const auto selected = sender.template as<c::ComboBox>().SelectedIndex();
                if (selected < 0) return;
                self->draft.*member = selected;
                if (member == &LargeIconConfig::titleMode) self->draft.hoverContent = selected == 1 ? 1 : 0;
                if (member == &LargeIconConfig::hoverContent) self->draft.titleMode = selected == 1 ? 1 : 0;
                if (member == &LargeIconConfig::content) self->draft.fit = selected == 0 ? 0 : 1;
                self->Send("commit");
            }
        });
        panel.Children().Append(input);
    }
    void ColorPicker(c::StackPanel panel, const char* key, std::uint32_t LargeIconConfig::* member)
    {
        c::Button button; button.Content(winrt::box_value(L(key)));
        c::Flyout flyout; c::ColorPicker picker;
        picker.IsAlphaEnabled(false); picker.Color(Color(draft.*member)); flyout.Content(picker); button.Flyout(flyout);
        std::weak_ptr<Impl> weak = shared_from_this();
        picker.ColorChanged([weak, member](auto const&, auto const& args) {
            if (auto self = weak.lock()) { const auto v = args.NewColor(); self->draft.*member = (v.R << 16) | (v.G << 8) | v.B; self->Send("preview"); }
        });
        flyout.Closed([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->Send("commit"); });
        panel.Children().Append(button);
    }
    void Button(c::StackPanel panel, const char* key, std::function<void(Impl&)> callback)
    {
        c::Button button; button.Content(winrt::box_value(L(key)));
        std::weak_ptr<Impl> weak = shared_from_this();
        button.Click([weak, callback](auto const&, auto const&) { if (auto self = weak.lock()) callback(*self); });
        panel.Children().Append(button);
    }
    void Build()
    {
        root.Children().Clear(); editors.Children().Clear();
        root.Spacing(16); editors.Spacing(16);
        preview.Child(previewImage); preview.HorizontalAlignment(x::HorizontalAlignment::Center);
        root.Children().Append(preview);
        c::TextBlock name; name.Text(snapshot.name); name.TextWrapping(x::TextWrapping::Wrap);
        root.Children().Append(name); status.TextWrapping(x::TextWrapping::Wrap);
        root.Children().Append(status); editorHost.Content(editors); root.Children().Append(editorHost);
        auto content = Group("largeIcon.contentSize");
        Number(content, "largeIcon.columns", &LargeIconConfig::columns, 1, snapshot.maxColumns);
        Number(content, "largeIcon.rows", &LargeIconConfig::rows, 1, snapshot.maxRows);
        Slider(content, "largeIcon.contentScale", &LargeIconConfig::contentScale, .1, 1);
        Choice(content, "largeIcon.content", &LargeIconConfig::content, {"largeIcon.original", "largeIcon.image", "largeIcon.steam"});
        Button(content, "largeIcon.import", [](Impl& self) { self.Send("import"); self.ReloadDraft(); self.Build(); });
        Button(content, "largeIcon.original", [](Impl& self) {
            self.draft.content = 0; self.draft.image.clear(); self.draft.cachedCover.clear(); self.draft.fit = 0; self.Send("commit"); self.Build();
        });
        Choice(content, "largeIcon.fit", &LargeIconConfig::fit, {"largeIcon.contain", "largeIcon.cover"});
        Slider(content, "largeIcon.focusX", &LargeIconConfig::focusX, 0, 1);
        Slider(content, "largeIcon.focusY", &LargeIconConfig::focusY, 0, 1);
        Choice(content, "largeIcon.orientation", &LargeIconConfig::steamOrientation, {"largeIcon.auto", "largeIcon.landscape", "largeIcon.portrait"});
        Toggle(content, "largeIcon.localOnly", &LargeIconConfig::localOnly);
        Button(content, "largeIcon.refresh", [](Impl& self) { self.Send("refresh"); });
        auto frame = Group("largeIcon.frame");
        Slider(frame, "largeIcon.radius", &LargeIconConfig::radius, 0, 100, 1);
        Toggle(frame, "largeIcon.autoColor", &LargeIconConfig::autoColor);
        ColorPicker(frame, "largeIcon.manualColor", &LargeIconConfig::manualColor);
        Slider(frame, "largeIcon.colorMix", &LargeIconConfig::colorMix, 0, 1);
        Slider(frame, "largeIcon.opacity", &LargeIconConfig::opacity, 0, 1);
        Slider(frame, "largeIcon.hoverOpacity", &LargeIconConfig::hoverOpacity, 0, 1);
        Toggle(frame, "largeIcon.border", &LargeIconConfig::border);
        ColorPicker(frame, "largeIcon.borderColor", &LargeIconConfig::borderColor);
        Slider(frame, "largeIcon.borderWidth", &LargeIconConfig::borderWidth, 0, 16, .5);
        Slider(frame, "largeIcon.borderOpacity", &LargeIconConfig::borderOpacity, 0, 1);
        Toggle(frame, "largeIcon.shadow", &LargeIconConfig::shadow);
        Slider(frame, "largeIcon.shadowStrength", &LargeIconConfig::shadowStrength, 0, 1);
        auto effects = Group("largeIcon.effects");
        Choice(effects, "largeIcon.title", &LargeIconConfig::titleMode, {"largeIcon.floating", "largeIcon.left"});
        Number(effects, "largeIcon.titleSize", &LargeIconConfig::titleSize, 8, 72);
        Toggle(effects, "largeIcon.autoTitleColor", &LargeIconConfig::autoTitleColor);
        ColorPicker(effects, "largeIcon.titleColor", &LargeIconConfig::titleColor);
        Choice(effects, "largeIcon.hoverContent", &LargeIconConfig::hoverContent, {"largeIcon.none", "largeIcon.left", "largeIcon.zoom", "largeIcon.lift"});
        Choice(effects, "largeIcon.hoverFrame", &LargeIconConfig::hoverFrame, {"largeIcon.none", "largeIcon.background", "largeIcon.border", "largeIcon.shadow"});
        Toggle(effects, "largeIcon.press", &LargeIconConfig::press);
        Choice(effects, "largeIcon.launch", &LargeIconConfig::launch, {"largeIcon.none", "largeIcon.jump", "largeIcon.pulse"});
        Choice(effects, "largeIcon.coverHover", &LargeIconConfig::coverHover, {"largeIcon.none", "largeIcon.zoom", "largeIcon.scrim"});
        Slider(effects, "largeIcon.amplitude", &LargeIconConfig::amplitude, 0, 2);
        Number(effects, "largeIcon.delayMs", &LargeIconConfig::delayMs, 0, 2000, 10);
        Number(effects, "largeIcon.enterMs", &LargeIconConfig::enterMs, 0, 2000, 10);
        Number(effects, "largeIcon.exitMs", &LargeIconConfig::exitMs, 0, 2000, 10);
        Button(effects, "largeIcon.retry", [](Impl& self) { self.Send("commit"); });
        UpdatePreview(); editorHost.IsEnabled(snapshot.editable);
    }
    void ReloadDraft()
    {
        JsonValue value;
        if (ParseJson(snapshot.config, value)) DecodeLargeIconConfig(value, draft);
    }
};

LargeIconPagePresenter::LargeIconPagePresenter(std::function<std::wstring(std::string_view)> localize,
    x::Style style, LargeIconSettingsAction action) : impl_(std::make_shared<Impl>())
{
    impl_->localize = std::move(localize); impl_->style = std::move(style); impl_->action = std::move(action);
    std::weak_ptr<Impl> weak = impl_;
    impl_->timer.Interval(std::chrono::seconds(1));
    impl_->timer.Tick([weak](auto const&, auto const&) {
        if (auto self = weak.lock(); self && self->active) self->Send("status");
    });
}
LargeIconPagePresenter::~LargeIconPagePresenter() { Deactivate(); }
c::StackPanel LargeIconPagePresenter::Content() const { return impl_->root; }
void LargeIconPagePresenter::Activate(std::wstring key)
{
    if (impl_->active && impl_->snapshot.key == key) return;
    Deactivate(); impl_->snapshot.key = std::move(key); impl_->active = true;
    impl_->Send("read"); impl_->ReloadDraft(); impl_->Build(); impl_->timer.Start();
}
void LargeIconPagePresenter::Deactivate()
{
    impl_->timer.Stop();
    if (impl_->active) impl_->Send("cancel");
    impl_->active = false;
}
void LargeIconPagePresenter::RefreshLocalizedText() { if (impl_->active) impl_->Build(); }
}
