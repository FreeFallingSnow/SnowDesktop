#pragma once
#include "settings_presenter_controls.h"
#include <winrt/Windows.UI.Text.h>

namespace snowdesktop::winui
{
// A common disclosure structure; each presenter supplies only supported rows.
struct AppearanceSections
{
    using Panel = winrt::Microsoft::UI::Xaml::Controls::StackPanel;
    using Heading = winrt::Microsoft::UI::Xaml::Controls::TextBlock;
    Panel colors{nullptr}, material{nullptr}, border{nullptr}, bottomBar{nullptr}, text{nullptr};
    Heading colorsTitle{nullptr}, materialTitle{nullptr}, borderTitle{nullptr}, bottomBarTitle{nullptr}, textTitle{nullptr};
    bool highlights = true;
    std::vector<winrt::Microsoft::UI::Xaml::Controls::Expander> disclosures;
    void ExpandAll() const { for (auto const& disclosure : disclosures) disclosure.IsExpanded(true); }

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

    static Panel Section(const Panel& parent, Heading& title,
        std::vector<winrt::Microsoft::UI::Xaml::Controls::Expander>* disclosures = nullptr)
    {
        namespace x = winrt::Microsoft::UI::Xaml;
        title = Heading{};
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(x::TextWrapping::Wrap);
        title.Margin({0, 0, 0, 0});
        Panel body; body.Spacing(8); body.Margin({0, 4, 0, 4});
        x::Controls::Expander disclosure;
        disclosure.Header(title); disclosure.Content(body);
        disclosure.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        disclosure.IsExpanded(false);
        parent.Children().Append(disclosure);
        if (disclosures) disclosures->push_back(disclosure);
        return body;
    }

    void Initialize(const Panel& parent, bool withHighlights = true, bool withBottomBar = false, bool withText = true)
    {
        highlights = withHighlights;
        disclosures.clear();
        colors = Section(parent, colorsTitle, &disclosures);
        material = Section(parent, materialTitle, &disclosures);
        border = Section(parent, borderTitle, &disclosures);
        if (withBottomBar) bottomBar = Section(parent, bottomBarTitle, &disclosures);
        if (withText) text = Section(parent, textTitle, &disclosures);
        disclosures.front().IsExpanded(true);
    }

    template<class Localize> void RefreshLocalizedText(Localize localize)
    {
        colorsTitle.Text(localize("largeIcon.colorAndFill"));
        materialTitle.Text(localize("largeIcon.material"));
        borderTitle.Text(localize(highlights ? "largeIcon.borderAndHighlight" : "appearance.border"));
        if (bottomBarTitle) bottomBarTitle.Text(localize("appearance.bottomBar"));
        if (textTitle) textTitle.Text(localize("appearance.text"));
    }
};
}
