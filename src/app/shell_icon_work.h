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
        unsigned shortcutWorkers = 2, unsigned fallbackWorkers = 2)
        : first_(firstWorkers), detail_(detailWorkers), shortcut_(shortcutWorkers),
          fallback_(fallbackWorkers) {}

    // Local misses leave the first-image lane before any Shell provider runs.
    // Each successful bitmap is delivered separately; stalled fallbacks retain
    // only their own bounded workers and never delay unrelated local images.
    template<class Local, class Shell, class Ready, class Classify,
        class ApplyImage, class ApplyShortcut>
    bool SubmitFirstWithFallback(std::wstring key, Local local, Shell shell,
        Ready ready, Classify classify, ApplyImage applyImage,
        ApplyShortcut applyShortcut, HWND window, UINT message)
    {
        auto deliver = [this, key, classify = std::move(classify),
            applyImage = std::move(applyImage), applyShortcut = std::move(applyShortcut),
            window, message](auto result) mutable {
            if (applyImage(std::move(result)))
                shortcut_.Submit(key, std::move(classify), std::move(applyShortcut), window, message);
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

    void Drain() { first_.Drain(); fallback_.Drain(); detail_.Drain(); shortcut_.Drain(); }
    void Cancel(const std::wstring& prefix = {})
    { first_.Cancel(prefix); fallback_.Cancel(prefix); detail_.Cancel(prefix); shortcut_.Cancel(prefix); }
    void Stop() { first_.Stop(); fallback_.Stop(); detail_.Stop(); shortcut_.Stop(); }

private:
    BackgroundWork first_, detail_, shortcut_, fallback_;
};
}
