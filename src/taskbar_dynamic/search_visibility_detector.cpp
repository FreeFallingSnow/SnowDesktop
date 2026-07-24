#include "search_visibility_detector.h"

#include <array>
#include <atomic>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/WindowsUdk.UI.Shell.h>

namespace wush = winrt::WindowsUdk::UI::Shell;

struct SearchVisibilityDetector::Impl
{
    explicit Impl(std::function<void()> callback)
        : stateChanged(std::move(callback))
    {
        constexpr std::array views = {
            wush::ShellView::Search,
            wush::ShellView::FindInStart,
            wush::ShellView::ActionCenter,
            wush::ShellView::ControlCenter,
            wush::ShellView::Dashboard
        };
        for (std::size_t index = 0; index < views.size(); ++index)
        {
            try
            {
                coordinators[index] =
                    wush::ShellViewCoordinator(views[index]);
                tokens[index] = coordinators[index].VisibilityChanged(
                    [this](const wush::ShellViewCoordinator&,
                        const winrt::Windows::Foundation::IInspectable&) {
                        Update();
                    });
                available = true;
            }
            catch (...) {}
        }
        Update();
    }

    ~Impl()
    {
        Reset();
    }

    void Update() noexcept
    {
        if (!available) return;
        bool current = false;
        bool anyAvailable = false;
        for (auto& coordinator : coordinators)
        {
            if (!coordinator) continue;
            try
            {
                current = current ||
                    coordinator.Visibility() ==
                        wush::ViewVisibility::Visible;
                anyAvailable = true;
            }
            catch (...)
            {
                coordinator = nullptr;
            }
        }
        available = anyAvailable;
        const bool previous = visible.exchange(current);
        if (previous != current && stateChanged)
            stateChanged();
    }

    void Reset() noexcept
    {
        for (std::size_t index = 0; index < coordinators.size(); ++index)
        {
            auto& coordinator = coordinators[index];
            if (!coordinator) continue;
            try
            {
                if (tokens[index].value != 0)
                    coordinator.VisibilityChanged(tokens[index]);
            }
            catch (...) {}
            coordinator = nullptr;
            tokens[index] = {};
        }
        available = false;
        visible = false;
    }

    std::function<void()> stateChanged;
    std::array<wush::ShellViewCoordinator, 5> coordinators{
        nullptr, nullptr, nullptr, nullptr, nullptr
    };
    std::array<winrt::event_token, 5> tokens{};
    std::atomic_bool available{false};
    std::atomic_bool visible{false};
};

SearchVisibilityDetector::SearchVisibilityDetector(
    std::function<void()> stateChanged)
    : impl_(std::make_unique<Impl>(std::move(stateChanged)))
{
}

SearchVisibilityDetector::~SearchVisibilityDetector() = default;

bool SearchVisibilityDetector::IsAvailable() const noexcept
{
    return impl_ && impl_->available.load();
}

bool SearchVisibilityDetector::IsVisible() const noexcept
{
    return impl_ && impl_->available.load() && impl_->visible.load();
}
