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
    void CollapseAll() const { for (auto const& disclosure : disclosures) disclosure.IsExpanded(false); }
    static void CollapseWithin(const winrt::Microsoft::UI::Xaml::UIElement& element)
    {
        namespace c = winrt::Microsoft::UI::Xaml::Controls;
        if (!element) return;
        if (auto disclosure = element.try_as<c::Expander>())
        { disclosure.IsExpanded(false); CollapseWithin(disclosure.Content().try_as<winrt::Microsoft::UI::Xaml::UIElement>()); }
        else if (auto panel = element.try_as<c::Panel>())
        { for (auto const& child : panel.Children()) CollapseWithin(child); }
        else if (auto border = element.try_as<c::Border>()) CollapseWithin(border.Child());
        else if (auto host = element.try_as<c::ContentControl>())
            CollapseWithin(host.Content().try_as<winrt::Microsoft::UI::Xaml::UIElement>());
    }

    // Search navigation reveals only the path to its target. Inspect logical
    // content because collapsed Expanders have no realized visual descendants.
    static bool RevealWithin(const winrt::Microsoft::UI::Xaml::UIElement& element,
        const winrt::Microsoft::UI::Xaml::FrameworkElement& target)
    {
        namespace c = winrt::Microsoft::UI::Xaml::Controls;
        if (!element || !target) return false;
        if (element == target) return true;
        if (auto disclosure = element.try_as<c::Expander>())
        {
            if (RevealWithin(disclosure.Content().try_as<winrt::Microsoft::UI::Xaml::UIElement>(), target))
            { disclosure.IsExpanded(true); return true; }
        }
        else if (auto panel = element.try_as<c::Panel>())
        {
            for (auto const& child : panel.Children()) if (RevealWithin(child, target)) return true;
        }
        else if (auto border = element.try_as<c::Border>()) return RevealWithin(border.Child(), target);
        else if (auto host = element.try_as<c::ContentControl>())
            return RevealWithin(host.Content().try_as<winrt::Microsoft::UI::Xaml::UIElement>(), target);
        return false;
    }
    void Reveal(const winrt::Microsoft::UI::Xaml::FrameworkElement& target) const noexcept
    {
        try { for (auto const& disclosure : disclosures) if (RevealWithin(disclosure, target)) break; }
        catch (...) {}
    }

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
        body.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        x::Controls::Expander disclosure;
        disclosure.Header(title); disclosure.Content(body);
        disclosure.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        disclosure.HorizontalContentAlignment(x::HorizontalAlignment::Stretch);
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
