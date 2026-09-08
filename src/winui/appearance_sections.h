#pragma once
#include "settings_presenter_controls.h"
#include <winrt/Windows.UI.Text.h>

namespace snowdesktop::winui
{
// A common disclosure structure; each presenter supplies only supported rows.
struct AppearanceSections
{
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using Expander = winrt::Microsoft::UI::Xaml::Controls::Expander;
    Panel colors{nullptr}, material{nullptr}, border{nullptr}, bottomBar{nullptr};
    Expander materialGroup{nullptr}, borderGroup{nullptr}, bottomBarGroup{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock colorsTitle{nullptr};
    bool highlights = true;

    void PlaceOpacity(const winrt::Microsoft::UI::Xaml::UIElement& row, bool gradient) const
    {
        const Panel target = gradient ? bottomBar : colors;
        if (!target) return;
        uint32_t index = 0;
        if (target.Children().IndexOf(row, index)) return;
        const Panel previous = gradient ? colors : bottomBar;
        if (previous && previous.Children().IndexOf(row, index)) previous.Children().RemoveAt(index);
        if (gradient) target.Children().InsertAt(0, row);
        else target.Children().Append(row);
    }

    static Panel Fold(const Panel& parent, Expander& group)
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        group = Expander{};
        group.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        group.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
        group.IsExpanded(false);
        Panel body; body.Spacing(4);
        group.Content(body);
        const auto weakBody = winrt::make_weak(body);
        group.SizeChanged([weakBody](auto const&, x::SizeChangedEventArgs const& args) {
            if (auto current = weakBody.get())
                current.Width(std::max(0.0, static_cast<double>(args.NewSize().Width) - 32.0));
        });
        parent.Children().Append(group);
        return body;
    }

    void Initialize(const Panel& parent, bool withHighlights = true, bool withBottomBar = false)
    {
        highlights = withHighlights;
        colorsTitle = winrt::Microsoft::UI::Xaml::Controls::TextBlock{};
        colorsTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        parent.Children().Append(colorsTitle);
        colors = Panel{}; colors.Spacing(4);
        parent.Children().Append(colors);
        material = Fold(parent, materialGroup);
        border = Fold(parent, borderGroup);
        if (withBottomBar) bottomBar = Fold(parent, bottomBarGroup);
    }

    template<class Localize> void RefreshLocalizedText(Localize localize)
    {
        colorsTitle.Text(localize("largeIcon.colorAndFill"));
        materialGroup.Header(winrt::box_value(localize("largeIcon.material")));
        borderGroup.Header(winrt::box_value(localize(highlights ? "largeIcon.borderAndHighlight" : "appearance.border")));
        if (bottomBarGroup) bottomBarGroup.Header(winrt::box_value(localize("appearance.bottomBar")));
    }
};
}
