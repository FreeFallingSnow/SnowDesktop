#pragma once

#include "desktop_backdrop_update_rules.h"
#include "icons/icon_beautify.h"
#include "ui/render/icon_backdrop_clip.h"

#include <memory>
#include <utility>
#include <vector>

namespace snowdesktop
{

// A retained child surface owns its native icon glass. Collect requests without
// touching live visuals; publish only after the corresponding content succeeds.
class WidgetIconBackdrops
{
public:
    struct Request
    {
        RECT frame{};
        IconBeautifyShape shape{};
        float blurRadius = 0, opacity = 1;
        bool inheritedGlass = false;
        std::optional<IconBackdropClip> clip;
    };

    void BeginDraw() { candidate_.clear(); }
    void Collect(Request request) { candidate_.push_back(std::move(request)); }
    void DiscardDraw() { candidate_.clear(); }

    template<class Compositor>
    void Publish(Compositor& compositor)
    {
        while (panels_.size() > candidate_.size())
        {
            Remove(compositor, *panels_.back());
            panels_.pop_back();
        }
        while (panels_.size() < candidate_.size())
            panels_.push_back(std::make_unique<Panel>());
        for (std::size_t i = 0; i < candidate_.size(); ++i)
            panels_[i]->request = std::move(candidate_[i]);
        candidate_.clear();
        Restore(compositor);
    }

    template<class Compositor>
    void Restore(Compositor& compositor) const
    {
        for (const auto& panel : panels_)
        {
            const auto& r = panel->request;
            desktop_backdrop_update_rules::ReconcileIconPanel(compositor,
                r.frame, r.shape, r.blurRadius, r.opacity, Key(*panel),
                r.inheritedGlass, r.clip ? &*r.clip : nullptr);
        }
    }

    template<class Compositor>
    void Hide(Compositor& compositor) const
    {
        for (const auto& panel : panels_) Remove(compositor, *panel);
    }

private:
    struct Panel { Request request; };
    static std::uintptr_t Key(const Panel& panel)
    {
        return reinterpret_cast<std::uintptr_t>(&panel) | 1u;
    }
    template<class Compositor>
    static void Remove(Compositor& compositor, const Panel& panel)
    {
        (void)compositor.RemoveIconPanel(panel.request.frame, Key(panel));
    }

    std::vector<std::unique_ptr<Panel>> panels_;
    std::vector<Request> candidate_;
};

} // namespace snowdesktop
