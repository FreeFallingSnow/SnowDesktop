#pragma once

#include <functional>
#include <utility>
#include <vector>

namespace snowdesktop
{
// Owner-STA coordination for activation and navigation refreshes. A nested
// settings IPC wait can request another refresh before the first one finishes.
// Keep every acknowledgement and start only one background platform query.
class SettingsExternalStateRefresh final
{
public:
    bool Request(std::function<void()> complete)
    {
        completions_.push_back(std::move(complete));
        return completions_.size() == 1;
    }

    void Complete()
    {
        auto completions = std::exchange(completions_, {});
        for (auto& complete : completions)
        {
            try { if (complete) complete(); } catch (...) {}
        }
    }

private:
    std::vector<std::function<void()>> completions_;
};
}
