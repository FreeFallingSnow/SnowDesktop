#include "pch.h"
#include "large_icon_page_presenter.h"
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Windows.System.h>
#include "../large_icon_render_rules.h"
#include "../large_icon_motion.h"
#include <chrono>

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
    c::Border preview, frameBackground, frameStroke, launchStroke;
    std::array<c::Border, 5> shadows;
    LargeIconMotion motion;
    winrt::event_token rendering{};
    bool renderingActive = false;
    double lastFrame = 0;
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

    static double Now()
    { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    void StopRendering()
    {
        if (renderingActive) { m::CompositionTarget::Rendering(rendering); renderingActive = false; }
    }
    void ScheduleRendering(bool moving)
    {
        if (!moving || !active) { StopRendering(); return; }
        if (renderingActive) return;
        std::weak_ptr<Impl> weak = shared_from_this();
        renderingActive = true;
        rendering = m::CompositionTarget::Rendering([weak](auto const&, auto const&) {
            if (auto self = weak.lock())
            {
                const double now = Now();
                const double interval = 1000. / (self->snapshot.frameLimit > 0 ? self->snapshot.frameLimit : 120);
                if (now - self->lastFrame >= interval) { self->lastFrame = now; self->UpdatePreview(); }
            }
        });
    }
    void CancelPreview()
    {
        if (!previewDirty) return;
        Send("cancel"); ReloadDraft();
        syncing = true; for (auto& sync : syncControls) sync(); syncing = false;
        UpdatePreview();
    }
    void UpdatePreview()
    {
        const double now = Now();
        const bool moving = motion.Advance(now, hovered, active && snapshot.available, snapshot.animations, snapshot.durationScale, draft);
        ScheduleRendering(moving);
        const double hover = motion.hover, wave = motion.LaunchWave(now, snapshot.durationScale);
        const double fw = std::max(1, draft.columns <= static_cast<int>(snapshot.frameWidths.size()) ? snapshot.frameWidths[draft.columns - 1] : snapshot.frameWidth);
        const double fh = std::max(1, draft.rows <= static_cast<int>(snapshot.frameHeights.size()) ? snapshot.frameHeights[draft.rows - 1] : snapshot.frameHeight);
        const double fit = std::min({1., 300. / fw, 260. / fh});
        const double width = fw * fit, height = fh * fit, scale = snapshot.unitScale * fit;
        const double offset = (330 - width) / 2, top = 12;
        preview.Width(width); preview.Height(height);
        previewStage.Width(330);
        c::Canvas::SetLeft(preview, offset); c::Canvas::SetTop(preview, top);
        previewCanvas.Width(width); previewCanvas.Height(height);
        preview.Background(m::SolidColorBrush(Color(0, 0))); preview.BorderThickness(x::Thickness{0});
        const double radius = std::min({draft.radius * scale, width / 2, height / 2});
        const auto background = large_icon_render_rules::Background(draft, snapshot.neutral, snapshot.accent);
        frameBackground.Width(width); frameBackground.Height(height); frameBackground.CornerRadius(x::CornerRadius{radius});
        frameBackground.Background(m::SolidColorBrush(Color(background, draft.opacity + (draft.hoverFrame == 1 ? (draft.hoverOpacity - draft.opacity) * hover : 0))));
        frameStroke.Width(width); frameStroke.Height(height); frameStroke.CornerRadius(x::CornerRadius{radius});
        frameStroke.BorderBrush(m::SolidColorBrush(Color(draft.borderColor, std::min(1., (draft.border ? draft.borderOpacity : 0) + (draft.hoverFrame == 2 ? hover * .4 * draft.amplitude : 0)))));
        frameStroke.BorderThickness(x::Thickness{draft.border || (hover > 0 && draft.hoverFrame == 2) ? draft.borderWidth * scale : 0});
        launchStroke.Width(width); launchStroke.Height(height); launchStroke.CornerRadius(x::CornerRadius{radius});
        launchStroke.BorderBrush(m::SolidColorBrush(Color(0xffffff, draft.launch == 2 && snapshot.animations ? std::min(1., wave * .65 * draft.amplitude) : 0)));
        launchStroke.BorderThickness(x::Thickness{2 * scale});
        for (int i = 0; i < 5; ++i)
        {
            const double spread = (5 - i) * scale;
            auto shadow = shadows[i]; shadow.Width(width + 2 * spread); shadow.Height(height + 2 * spread);
            shadow.CornerRadius(x::CornerRadius{radius + spread});
            shadow.Background(m::SolidColorBrush(Color(0, draft.shadowStrength * (draft.shadow ? .06 : 0) + (draft.hoverFrame == 3 ? hover * .035 * draft.amplitude : 0))));
            c::Canvas::SetLeft(shadow, offset - spread); c::Canvas::SetTop(shadow, top - spread + 2 * scale);
        }
        auto visual = x::Hosting::ElementCompositionPreview::GetElementVisual(previewCanvas);
        auto clip = visual.Compositor().CreateRoundedRectangleGeometry();
        clip.Size({static_cast<float>(width), static_cast<float>(height)});
        clip.CornerRadius({static_cast<float>(radius), static_cast<float>(radius)});
        visual.Clip(visual.Compositor().CreateGeometricClip(clip));
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
        const double sw = previewBitmap ? previewBitmap.PixelWidth() : 0;
        const double sh = previewBitmap ? previewBitmap.PixelHeight() : 0;
        const bool raw = draft.content == 0 || snapshot.source == "original" || snapshot.imagePath.empty();
        const auto content = large_icon_render_rules::ResolveContent(draft, fw, fh, sw, sh, snapshot.unitScale,
            raw, snapshot.animations, hover, pressed, wave);
        previewImage.Width(content.width * fit); previewImage.Height(content.height * fit); previewImage.Stretch(m::Stretch::Fill);
        c::Canvas::SetLeft(previewImage, content.x * fit); c::Canvas::SetTop(previewImage, content.y * fit);
        const bool left = content.leftReveal;
        const double lineHeight = draft.titleSize * scale * 1.3;
        const double titleLeft = left ? content.titleLeft * fit : 6 * scale;
        const double titleWidth = std::max(1., width - titleLeft - 12 * scale);
        const double titlePadding = (6 + (1 - hover) * 6) * scale;
        previewTitle.Text(snapshot.name); previewTitle.FontSize(draft.titleSize * scale);
        previewTitle.TextWrapping(x::TextWrapping::Wrap); previewTitle.MaxLines(2);
        previewTitle.LineStackingStrategy(x::LineStackingStrategy::BlockLineHeight); previewTitle.LineHeight(lineHeight);
        previewTitle.TextTrimming(x::TextTrimming::CharacterEllipsis);
        previewTitle.VerticalAlignment(x::VerticalAlignment::Center);
        const auto textColor = draft.autoTitleColor ? 0xffffffu : draft.titleColor;
        previewTitle.Foreground(m::SolidColorBrush(Color(textColor)));
        innerTitleBackdrop.Visibility(left || (!raw && draft.coverHover == 2) ? x::Visibility::Visible : x::Visibility::Collapsed);
        innerTitleBackdrop.Opacity(hover); innerTitleBackdrop.Width(titleWidth); innerTitleBackdrop.Height(lineHeight * 2);
        innerTitleBackdrop.Padding(x::Thickness{titlePadding, 0, 6 * scale, 0}); innerTitleBackdrop.CornerRadius(x::CornerRadius{4 * scale});
        innerTitleBackdrop.Background(m::SolidColorBrush(Color(large_icon_render_rules::TitleBackdrop(textColor), .88)));
        c::Canvas::SetLeft(innerTitleBackdrop, titleLeft);
        c::Canvas::SetTop(innerTitleBackdrop, left ? height / 2 - lineHeight : height - lineHeight * 2 - 6 * scale);
        floatingTitle.Text(snapshot.name); floatingTitle.FontSize(draft.titleSize * scale);
        floatingTitle.TextWrapping(x::TextWrapping::Wrap); floatingTitle.MaxLines(0); floatingTitle.TextTrimming(x::TextTrimming::None);
        floatingTitle.LineStackingStrategy(x::LineStackingStrategy::BlockLineHeight); floatingTitle.LineHeight(lineHeight);
        floatingTitle.TextAlignment(x::TextAlignment::Center);
        floatingTitle.Foreground(m::SolidColorBrush(Color(textColor)));
        // Measure an untrimmed name to decide whether the two-line inner title
        // needs the same full-name floating hint as the desktop renderer.
        c::TextBlock measure; measure.Text(snapshot.name); measure.FontSize(draft.titleSize * scale);
        measure.TextWrapping(x::TextWrapping::Wrap); measure.LineHeight(lineHeight); measure.LineStackingStrategy(x::LineStackingStrategy::BlockLineHeight);
        measure.Measure({static_cast<float>(std::max(1., titleWidth - 12 * scale)), 100000.f});
        const bool fullHint = !left || measure.DesiredSize().Height > lineHeight * 2 + .1;
        floatingTitleBackdrop.Visibility(fullHint ? x::Visibility::Visible : x::Visibility::Collapsed);
        floatingTitleBackdrop.Opacity(hover);
        floatingTitleBackdrop.Background(m::SolidColorBrush(Color(large_icon_render_rules::TitleBackdrop(textColor), .96)));
        const double titleExtent = std::min(330., std::max(160., 260. * snapshot.unitScale) * fit);
        floatingTitleBackdrop.Width(titleExtent); floatingTitleBackdrop.Padding(x::Thickness{6 * scale}); floatingTitleBackdrop.CornerRadius(x::CornerRadius{6 * scale});
        floatingTitleBackdrop.Measure({static_cast<float>(titleExtent), 100000.f});
        const double titleHeight = std::max(36., double(floatingTitleBackdrop.DesiredSize().Height));
        previewStage.Height(top + height + 6 * scale + titleHeight + 12);
        c::Canvas::SetLeft(floatingTitleBackdrop, (330 - titleExtent) / 2); c::Canvas::SetTop(floatingTitleBackdrop, top + height + 5 * scale);
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
                if (auto self = weak.lock()) { self->CancelPreview(); args.Handled(true); flyout.Hide(); }
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
        frameBackground.IsHitTestVisible(false); frameStroke.IsHitTestVisible(false); launchStroke.IsHitTestVisible(false);
        innerTitleBackdrop.IsHitTestVisible(false); floatingTitleBackdrop.IsHitTestVisible(false);
        previewImage.IsHitTestVisible(false);
        previewCanvas.Children().Append(frameBackground); previewCanvas.Children().Append(previewImage);
        previewCanvas.Children().Append(innerTitleBackdrop); previewCanvas.Children().Append(frameStroke); previewCanvas.Children().Append(launchStroke);
        preview.Child(previewCanvas); preview.HorizontalAlignment(x::HorizontalAlignment::Center);
        previewStage.HorizontalAlignment(x::HorizontalAlignment::Center);
        for (auto shadow : shadows) { shadow.IsHitTestVisible(false); previewStage.Children().Append(shadow); }
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
    impl_->preview.PointerReleased([weak](auto const&, auto const&) { if (auto self = weak.lock()) { if (self->pressed && self->hovered) self->motion.Launch(Impl::Now(), self->snapshot.animations, self->draft); self->pressed = false; self->UpdatePreview(); } });
    impl_->root.PreviewKeyDown([weak](auto const&, auto const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Escape)
            if (auto self = weak.lock(); self && self->previewDirty) { self->CancelPreview(); args.Handled(true); }
    });
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
    impl_->timer.Stop(); impl_->StopRendering(); impl_->hovered = impl_->pressed = false; impl_->motion = {};
    const bool wasActive = impl_->active;
    impl_->active = false;
    if (wasActive) impl_->Send("cancel");
}
void LargeIconPagePresenter::RefreshLocalizedText() { if (impl_->active) impl_->Build(); }
}
