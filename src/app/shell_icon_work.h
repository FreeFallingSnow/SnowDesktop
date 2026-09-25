#pragma once

#include "../background_work.h"

namespace snowdesktop::shell_icon_request
{
// Shell providers may never return. Bitmap extraction, refinement and shortcut
// classification must not consume one another's reserved workers.
class Work final
{
public:
    explicit Work(unsigned firstWorkers = 4, unsigned detailWorkers = 2,
        unsigned shortcutWorkers = 2, unsigned fallbackWorkers = 2,
        std::size_t capacity = 2048)
        : first_(firstWorkers, capacity), detail_(detailWorkers, capacity),
          shortcut_(shortcutWorkers, capacity), fallback_(fallbackWorkers, capacity),
          capacity_(capacity) {}

    // Local misses leave the first-image lane before any Shell provider runs.
    // Each successful bitmap is delivered separately; stalled fallbacks retain
    // only their own bounded workers and never delay unrelated local images.
    template<class Local, class Shell, class Ready, class Classify,
        class ApplyImage, class ApplyShortcut>
    bool SubmitFirstWithFallback(std::wstring key, Local local, Shell shell,
        Ready ready, Classify classify, ApplyImage applyImage,
        ApplyShortcut applyShortcut, HWND window, UINT message)
    {
        // Bound deferred classifiers too. Rejected first requests remain
        // retryable in the host; already delivered pixels are never discarded.
        if (pendingShortcuts_.size() >= capacity_) return false;
        auto deliver = [this, key, classify = std::move(classify),
            applyImage = std::move(applyImage), applyShortcut = std::move(applyShortcut),
            window, message](auto result) mutable {
            if (applyImage(std::move(result)))
            {
                auto submit = [this, key, classify = std::move(classify),
                    applyShortcut = std::move(applyShortcut), window, message] {
                    return shortcut_.Submit(key, classify, applyShortcut, window, message);
                };
                if (!submit()) pendingShortcuts_.push_back({key, std::move(submit)});
            }
        };
        return first_.Submit(key, std::move(local),
            [this, key, shell = std::move(shell), ready = std::move(ready),
                deliver = std::move(deliver), window, message](auto result) mutable {
                if (ready(result))
                    deliver(std::move(result));
                else if (!fallback_.Submit(key, std::move(shell), deliver, window, message))
                    deliver(std::move(result)); // Preserve the host's failure/cleanup path.
            }, window, message);
    }

    template<class Provider, class Apply>
    bool Submit(bool detail, std::wstring key, Provider provider, Apply apply,
        HWND window, UINT message)
    {
        return (detail ? detail_ : first_).Submit(std::move(key),
            std::move(provider), std::move(apply), window, message);
    }

    void Drain()
    {
        first_.Drain(); fallback_.Drain(); detail_.Drain(); shortcut_.Drain();
        for (unsigned count = 0; count < 32 && !pendingShortcuts_.empty(); ++count)
        {
            if (!pendingShortcuts_.front().submit()) break;
            pendingShortcuts_.pop_front();
        }
    }
    void Cancel(const std::wstring& prefix = {})
    {
        std::erase_if(pendingShortcuts_, [&](const auto& entry) { return entry.key.starts_with(prefix); });
        first_.Cancel(prefix); fallback_.Cancel(prefix); detail_.Cancel(prefix); shortcut_.Cancel(prefix);
    }
    void Stop()
    {
        pendingShortcuts_.clear();
        first_.Stop(); fallback_.Stop(); detail_.Stop(); shortcut_.Stop();
    }

private:
    BackgroundWork first_, detail_, shortcut_, fallback_;
    struct PendingShortcut { std::wstring key; std::function<bool()> submit; };
    // UI-owned; at most capacity_ deferred entries plus the bounded first and
    // fallback lanes already accepted when backpressure becomes active.
    std::deque<PendingShortcut> pendingShortcuts_;
    std::size_t capacity_;
};
}
