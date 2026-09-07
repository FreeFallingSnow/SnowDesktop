#include "pch.h"
#include "large_icon_page_presenter.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Windows.System.h>
#include "../large_icon_render_rules.h"

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
    c::Canvas previewStage;
    c::Canvas previewCanvas;
    c::Image previewImage;
    c::TextBlock previewTitle, previewSource;
    c::Border floatingTitleBackdrop, innerTitleBackdrop;
    c::TextBlock floatingTitle;
    std::array<c::Image, 2> coverImages;
    std::array<c::TextBlock, 2> coverSources;
    std::array<std::wstring, 2> coverPaths;
    std::wstring previewPath;
    m::Imaging::BitmapImage previewBitmap{nullptr};
    std::vector<std::function<void()>> syncControls;
    x::DispatcherTimer timer;
    bool active = false, submitting = false, syncing = false, hovered = false, pressed = false, previewDirty = false;

    std::wstring L(std::string_view key) { return localize ? localize(key) : std::wstring{}; }
    std::wstring SourceLabel(std::string_view source)
    {
        if (source == "original") return L("largeIcon.source.original");
        if (source == "local") return L("largeIcon.source.local");
        if (source == "steam-local") return L("largeIcon.source.steam-local");
        if (source == "steam-online") return L("largeIcon.source.steam-online");
        if (source == "cache") return L("largeIcon.source.cache");
        return L("largeIcon.unavailable");
    }
    static winrt::Windows::UI::Color Color(std::uint32_t rgb, double alpha = 1)
    { return {static_cast<uint8_t>(alpha * 255), static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)}; }

    bool Send(std::string operation, std::wstring path = {})
    {
        if (!action || submitting) return false;
        submitting = true;
        const bool polling = operation == "status";
        const bool previewing = operation == "preview";
        const bool finishesPreview = operation == "commit" || operation == "cancel" || operation == "read";
        const auto oldRevision = snapshot.revision;
        const auto oldError = snapshot.error;
        LargeIconSettingsRequest request{snapshot.key, snapshot.session, snapshot.revision,
            std::move(operation), EncodeLargeIconConfig(draft), std::move(path)};
        try { snapshot = action(std::move(request)); }
        catch (...) { snapshot.succeeded = false; snapshot.error = "largeIcon.unavailable"; }
        submitting = false;
        if (snapshot.succeeded && previewing) previewDirty = true;
        if ((snapshot.succeeded && finishesPreview) || !snapshot.editable || !snapshot.available) previewDirty = false;
        if (polling && snapshot.succeeded && snapshot.revision != oldRevision)
        { ReloadDraft(); Build(); }
        else if (polling && snapshot.error.empty() && (oldError == "largeIcon.saveFailed" || oldError == "largeIcon.invalid" || oldError == "largeIcon.stale")) snapshot.error = oldError;
        status.Text(snapshot.error.empty() ? L("largeIcon.livePreview") : L(snapshot.error));
        editorHost.IsEnabled(snapshot.editable && snapshot.available);
        if (!snapshot.editable || !snapshot.available) ReloadDraft();
        syncing = true;
        for (auto& sync : syncControls) sync();
        syncing = false;
        UpdatePreview();
        return snapshot.succeeded;
    }

    void UpdatePreview()
    {
        const double width = std::max(30., std::min(300., 260. * draft.columns / std::max(1, draft.rows)));
        const double height = std::max(30., std::min(260., 300. * draft.rows / std::max(1, draft.columns)));
        preview.Width(width); preview.Height(height);
        previewStage.Width(330); previewStage.Height(height + std::max(60., draft.titleSize * 2.8 + 20));
        c::Canvas::SetLeft(preview, (330 - width) / 2);
        previewCanvas.Width(preview.Width()); previewCanvas.Height(preview.Height());
        preview.CornerRadius(x::CornerRadius{draft.radius});
        const unsigned neutral = root.ActualTheme() == x::ElementTheme::Light ? 0xc9ced6u : 0x414751u;
        const auto mix = [&](int shift) { return static_cast<unsigned>(((neutral >> shift) & 255) * (1 - draft.colorMix) + ((snapshot.accent >> shift) & 255) * draft.colorMix); };
        const auto background = draft.autoColor ? (mix(16) << 16) | (mix(8) << 8) | mix(0) : draft.manualColor;
        preview.Background(m::SolidColorBrush(Color(background, hovered && draft.hoverFrame == 1 ? draft.hoverOpacity : draft.opacity)));
        preview.BorderBrush(m::SolidColorBrush(Color(draft.borderColor, std::min(1., (draft.border ? draft.borderOpacity : 0) + (hovered && draft.hoverFrame == 2 ? .4 : 0)))));
        preview.BorderThickness(x::Thickness{draft.border || (hovered && draft.hoverFrame == 2) ? draft.borderWidth : 0});
        auto visual = x::Hosting::ElementCompositionPreview::GetElementVisual(previewCanvas);
        auto geometry = visual.Compositor().CreateRoundedRectangleGeometry();
        geometry.Size({static_cast<float>(preview.Width()), static_cast<float>(preview.Height())});
        const float radius = static_cast<float>(std::min({draft.radius, preview.Width() / 2, preview.Height() / 2}));
        geometry.CornerRadius({radius, radius});
        visual.Clip(visual.Compositor().CreateGeometricClip(geometry));
        if (previewPath != snapshot.imagePath)
        {
            previewPath = snapshot.imagePath;
            if (previewPath.empty()) { previewBitmap = nullptr; previewImage.Source(nullptr); }
            else
            {
                previewBitmap = m::Imaging::BitmapImage{};
                previewBitmap.UriSource(winrt::Windows::Foundation::Uri(previewPath));
                previewImage.Source(previewBitmap);
            }
        }
        const double sw = previewBitmap ? std::max(1, previewBitmap.PixelWidth()) : 1;
        const double sh = previewBitmap ? std::max(1, previewBitmap.PixelHeight()) : 1;
        const double edge = std::min(width, height) * draft.contentScale;
        const bool raw = draft.content == 0 || snapshot.source == "original";
        const double factor = raw ? std::min({1., edge / sw, edge / sh}) :
            draft.fit == 0 ? std::min(width / sw, height / sh) : std::max(width / sw, height / sh);
        const bool left = raw && (draft.titleMode == 1 || draft.hoverContent == 1) &&
            large_icon_render_rules::CanRevealTitle(static_cast<float>(width), static_cast<float>(height), static_cast<float>(edge), static_cast<float>(draft.titleSize), 1);
        double zoom = hovered && ((raw && draft.hoverContent == 2) || (!raw && draft.coverHover == 1)) ? 1 + .06 * draft.amplitude : 1;
        if (pressed && draft.press) zoom *= .96;
        const double iw = sw * factor * zoom, ih = sh * factor * zoom;
        previewImage.Width(iw); previewImage.Height(ih); previewImage.Stretch(m::Stretch::Fill);
        c::Canvas::SetLeft(previewImage, hovered && left ? 12 : (width - iw) * (raw || draft.fit == 0 ? .5 : draft.focusX));
        c::Canvas::SetTop(previewImage, (height - ih) * (raw || draft.fit == 0 ? .5 : draft.focusY) - (hovered && raw && draft.hoverContent == 3 ? 6 * draft.amplitude : 0));
        previewTitle.Text(snapshot.name); previewTitle.FontSize(draft.titleSize);
        previewTitle.TextWrapping(x::TextWrapping::Wrap); previewTitle.MaxLines(2);
        previewTitle.TextTrimming(x::TextTrimming::CharacterEllipsis);
        const auto textColor = draft.autoTitleColor ? 0xffffffu : draft.titleColor;
        previewTitle.Foreground(m::SolidColorBrush(Color(textColor)));
        innerTitleBackdrop.Visibility(hovered && (left || (!raw && draft.coverHover == 2)) ? x::Visibility::Visible : x::Visibility::Collapsed);
        previewTitle.Width(std::max(1., width - (left ? edge + 36 : 12)));
        innerTitleBackdrop.Background(m::SolidColorBrush(Color(large_icon_render_rules::TitleBackdrop(textColor), .88)));
        c::Canvas::SetLeft(innerTitleBackdrop, left ? edge + 24 : 6);
        c::Canvas::SetTop(innerTitleBackdrop, left ? std::max(0., (height - draft.titleSize * 2.7) / 2) : std::max(0., height - draft.titleSize * 2.7));
        floatingTitle.Text(snapshot.name); floatingTitle.FontSize(draft.titleSize);
        floatingTitle.TextWrapping(x::TextWrapping::Wrap); floatingTitle.MaxLines(2); floatingTitle.TextTrimming(x::TextTrimming::CharacterEllipsis);
        floatingTitle.Foreground(m::SolidColorBrush(Color(textColor)));
        floatingTitleBackdrop.Visibility(hovered && !left ? x::Visibility::Visible : x::Visibility::Collapsed);
        floatingTitleBackdrop.Background(m::SolidColorBrush(Color(large_icon_render_rules::TitleBackdrop(textColor), .96)));
        floatingTitleBackdrop.Width(300); floatingTitleBackdrop.Padding(x::Thickness{6}); floatingTitleBackdrop.CornerRadius(x::CornerRadius{6});
        floatingTitleBackdrop.IsHitTestVisible(false);
        c::Canvas::SetLeft(floatingTitleBackdrop, 15); c::Canvas::SetTop(floatingTitleBackdrop, height + 6);
        previewSource.Text(snapshot.source.empty() ? L("largeIcon.loading") : SourceLabel(snapshot.source));
        for (int i = 0; i < 2; ++i)
        {
            const auto& path = i == 0 ? snapshot.landscapePath : snapshot.portraitPath;
            if (coverPaths[i] != path)
            {
                coverPaths[i] = path;
                if (path.empty()) coverImages[i].Source(nullptr);
                else { m::Imaging::BitmapImage image; image.UriSource(winrt::Windows::Foundation::Uri(path)); coverImages[i].Source(image); }
            }
            const auto& source = i == 0 ? snapshot.landscapeSource : snapshot.portraitSource;
            coverSources[i].Text(SourceLabel(source));
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
            if (auto self = weak.lock(); self && !self->syncing && std::isfinite(args.NewValue()))
            { self->draft.*member = static_cast<T>(args.NewValue()); self->Send("commit"); }
        });
        syncControls.push_back([this, input, member] { input.Value(static_cast<double>(draft.*member)); });
        panel.Children().Append(input);
    }
    void Slider(c::StackPanel panel, const char* key, double LargeIconConfig::* member, double min, double max, double step = .01)
    {
        c::Slider input; input.Header(winrt::box_value(L(key)));
        input.Minimum(min); input.Maximum(max); input.StepFrequency(step); input.Value(draft.*member);
        x::Automation::AutomationProperties::SetName(input, L(key));
        std::weak_ptr<Impl> weak = shared_from_this();
        input.ValueChanged([weak, member](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && !self->syncing)
            {
                self->draft.*member = args.NewValue();
                if (member == &LargeIconConfig::opacity && self->draft.hoverOpacityLinked)
                    self->draft.hoverOpacity = std::min(1., args.NewValue() + .15);
                if (member == &LargeIconConfig::hoverOpacity) self->draft.hoverOpacityLinked = false;
                self->Send("preview");
            }
        });
        const auto commit = [weak](auto const&, auto const&) {
            if (auto self = weak.lock(); self && self->active && !self->syncing && self->previewDirty) self->Send("commit");
        };
        input.PointerCaptureLost(commit); input.KeyUp(commit); input.LostFocus(commit);
        syncControls.push_back([this, input, member] { input.Value(draft.*member); });
        panel.Children().Append(input);
    }
    void Toggle(c::StackPanel panel, const char* key, bool LargeIconConfig::* member)
    {
        c::ToggleSwitch input; input.Header(winrt::box_value(L(key))); input.IsOn(draft.*member);
        std::weak_ptr<Impl> weak = shared_from_this();
        input.Toggled([weak, member](auto const& sender, auto const&) {
            if (auto self = weak.lock(); self && !self->syncing) {
                self->draft.*member = sender.template as<c::ToggleSwitch>().IsOn();
                if (member == &LargeIconConfig::hoverOpacityLinked && self->draft.hoverOpacityLinked)
                    self->draft.hoverOpacity = std::min(1., self->draft.opacity + .15);
                self->Send("commit"); }
        });
        syncControls.push_back([this, input, member] { input.IsOn(draft.*member); });
        panel.Children().Append(input);
    }
    void Choice(c::StackPanel panel, const char* key, int LargeIconConfig::* member, std::initializer_list<const char*> choices)
    {
        c::ComboBox input; input.Header(winrt::box_value(L(key))); input.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        for (const char* choice : choices) input.Items().Append(winrt::box_value(L(choice)));
        input.SelectedIndex(draft.*member);
        std::weak_ptr<Impl> weak = shared_from_this();
        input.SelectionChanged([weak, member](auto const& sender, auto const&) {
            if (auto self = weak.lock(); self && !self->syncing)
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
        syncControls.push_back([this, input, member] { input.SelectedIndex(draft.*member); });
        panel.Children().Append(input);
    }
    void ColorPicker(c::StackPanel panel, const char* key, std::uint32_t LargeIconConfig::* member)
    {
        c::Button button; button.Content(winrt::box_value(L(key)));
        c::Flyout flyout; c::ColorPicker picker;
        picker.IsAlphaEnabled(false); picker.Color(Color(draft.*member)); flyout.Content(picker); button.Flyout(flyout);
        std::weak_ptr<Impl> weak = shared_from_this();
        picker.ColorChanged([weak, member](auto const&, auto const& args) {
            if (auto self = weak.lock(); self && self->active && !self->syncing) { const auto v = args.NewColor(); self->draft.*member = (v.R << 16) | (v.G << 8) | v.B; self->Send("preview"); }
        });
        picker.KeyDown([weak, flyout](auto const&, auto const& args) {
            if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
                if (auto self = weak.lock()) { self->Send("cancel"); self->ReloadDraft(); self->UpdatePreview(); args.Handled(true); flyout.Hide(); }
        });
        flyout.Closed([weak](auto const&, auto const&) {
            if (auto self = weak.lock(); self && self->active && !self->syncing && self->previewDirty) self->Send("commit");
        });
        syncControls.push_back([this, picker, member] { picker.Color(Color(draft.*member)); });
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
        syncing = true;
        syncControls.clear(); root.Children().Clear(); editors.Children().Clear();
        root.Spacing(16); editors.Spacing(16);
        previewStage.Children().Clear(); previewCanvas.Children().Clear();
        innerTitleBackdrop.Child(previewTitle); floatingTitleBackdrop.Child(floatingTitle);
        previewCanvas.Children().Append(previewImage); previewCanvas.Children().Append(innerTitleBackdrop);
        preview.Child(previewCanvas); preview.HorizontalAlignment(x::HorizontalAlignment::Center);
        previewStage.HorizontalAlignment(x::HorizontalAlignment::Center);
        previewStage.Children().Append(preview); previewStage.Children().Append(floatingTitleBackdrop);
        root.Children().Append(previewStage);
        root.Children().Append(previewSource);
        c::TextBlock name; name.Text(snapshot.name); name.TextWrapping(x::TextWrapping::Wrap);
        root.Children().Append(name); status.TextWrapping(x::TextWrapping::Wrap);
        root.Children().Append(status); editorHost.Content(editors); root.Children().Append(editorHost);
        auto content = Group("largeIcon.contentSize");
        Number(content, "largeIcon.columns", &LargeIconConfig::columns, 1, snapshot.maxColumns);
        Number(content, "largeIcon.rows", &LargeIconConfig::rows, 1, snapshot.maxRows);
        Slider(content, "largeIcon.contentScale", &LargeIconConfig::contentScale, .1, 1);
        if (snapshot.steam) Choice(content, "largeIcon.content", &LargeIconConfig::content, {"largeIcon.original", "largeIcon.image", "largeIcon.steam"});
        else Choice(content, "largeIcon.content", &LargeIconConfig::content, {"largeIcon.original", "largeIcon.image"});
        Button(content, "largeIcon.import", [](Impl& self) { self.Send("import"); self.ReloadDraft(); self.Build(); });
        Button(content, "largeIcon.restoreContent", [](Impl& self) {
            self.draft.content = 0; self.draft.image.clear(); self.draft.cachedCover.clear(); self.draft.fit = 0; self.Send("commit"); self.Build();
        });
        Choice(content, "largeIcon.fit", &LargeIconConfig::fit, {"largeIcon.contain", "largeIcon.cover"});
        Slider(content, "largeIcon.focusX", &LargeIconConfig::focusX, 0, 1);
        Slider(content, "largeIcon.focusY", &LargeIconConfig::focusY, 0, 1);
        if (snapshot.steam)
        {
            Choice(content, "largeIcon.orientation", &LargeIconConfig::steamOrientation, {"largeIcon.auto", "largeIcon.landscape", "largeIcon.portrait"});
            c::StackPanel covers; covers.Orientation(c::Orientation::Horizontal); covers.Spacing(12);
            for (int i = 0; i < 2; ++i)
            {
                c::StackPanel panel; panel.Spacing(6);
                coverImages[i].Width(128); coverImages[i].Height(96); coverImages[i].Stretch(m::Stretch::Uniform);
                panel.Children().Append(coverImages[i]); panel.Children().Append(coverSources[i]);
                Button(panel, i == 0 ? "largeIcon.landscape" : "largeIcon.portrait", [i](Impl& self) {
                    self.draft.steamOrientation = i + 1; self.draft.content = 2; self.draft.fit = 1; self.Send("commit");
                });
                covers.Children().Append(panel);
            }
            content.Children().Append(covers);
            Toggle(content, "largeIcon.localOnly", &LargeIconConfig::localOnly);
            Button(content, "largeIcon.refresh", [](Impl& self) { self.Send("refresh"); });
        }
        auto frame = Group("largeIcon.frame");
        Slider(frame, "largeIcon.radius", &LargeIconConfig::radius, 0, 100, 1);
        Toggle(frame, "largeIcon.autoColor", &LargeIconConfig::autoColor);
        ColorPicker(frame, "largeIcon.manualColor", &LargeIconConfig::manualColor);
        Slider(frame, "largeIcon.colorMix", &LargeIconConfig::colorMix, 0, 1);
        Slider(frame, "largeIcon.opacity", &LargeIconConfig::opacity, 0, 1);
        c::Expander advanced; advanced.Header(winrt::box_value(L("largeIcon.advancedBackground")));
        advanced.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        c::StackPanel advancedBackground; advancedBackground.Spacing(12);
        Toggle(advancedBackground, "largeIcon.hoverOpacityLinked", &LargeIconConfig::hoverOpacityLinked);
        Slider(advancedBackground, "largeIcon.hoverOpacity", &LargeIconConfig::hoverOpacity, 0, 1);
        advanced.Content(advancedBackground); frame.Children().Append(advanced);
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
        syncing = false;
        UpdatePreview(); editorHost.IsEnabled(snapshot.editable && snapshot.available);
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
    impl_->preview.PointerEntered([weak](auto const&, auto const&) { if (auto self = weak.lock()) { self->hovered = true; self->UpdatePreview(); } });
    impl_->preview.PointerExited([weak](auto const&, auto const&) { if (auto self = weak.lock()) { self->hovered = false; self->pressed = false; self->UpdatePreview(); } });
    impl_->preview.PointerPressed([weak](auto const&, auto const&) { if (auto self = weak.lock()) { self->pressed = true; self->UpdatePreview(); } });
    impl_->preview.PointerReleased([weak](auto const&, auto const&) { if (auto self = weak.lock()) { self->pressed = false; self->UpdatePreview(); } });
    impl_->previewImage.ImageOpened([weak](auto const&, auto const&) { if (auto self = weak.lock()) self->UpdatePreview(); });
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
    const bool wasActive = impl_->active;
    impl_->active = false;
    if (wasActive) impl_->Send("cancel");
}
void LargeIconPagePresenter::RefreshLocalizedText() { if (impl_->active) impl_->Build(); }
}
