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
        unsigned shortcutWorkers = 2)
        : first_(firstWorkers), detail_(detailWorkers), shortcut_(shortcutWorkers) {}

    // Only schedule classification after the UI has accepted the first bitmap.
    // Providers own copied input; no worker calls the host or retains this Work.
    template<class Image, class Classify, class ApplyImage, class ApplyShortcut>
    bool SubmitFirst(std::wstring key, Image image, Classify classify,
        ApplyImage applyImage, ApplyShortcut applyShortcut, HWND window, UINT message)
    {
        return first_.Submit(key, std::move(image),
            [this, key, classify = std::move(classify),
                applyImage = std::move(applyImage),
                applyShortcut = std::move(applyShortcut), window, message]
            (auto result) mutable {
                if (applyImage(std::move(result)))
                    shortcut_.Submit(key, std::move(classify),
                        std::move(applyShortcut), window, message);
            }, window, message);
    }

    template<class Provider, class Apply>
    bool Submit(bool detail, std::wstring key, Provider provider, Apply apply,
        HWND window, UINT message)
    {
        return (detail ? detail_ : first_).Submit(std::move(key),
            std::move(provider), std::move(apply), window, message);
    }

    void Drain() { first_.Drain(); detail_.Drain(); shortcut_.Drain(); }
    void Cancel(const std::wstring& prefix = {})
    { first_.Cancel(prefix); detail_.Cancel(prefix); shortcut_.Cancel(prefix); }
    void Stop() { first_.Stop(); detail_.Stop(); shortcut_.Stop(); }

private:
    BackgroundWork first_, detail_, shortcut_;
};
}
