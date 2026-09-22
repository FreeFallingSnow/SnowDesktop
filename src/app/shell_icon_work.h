#pragma once

#include "../background_work.h"

namespace snowdesktop::shell_icon_request
{
// Thumbnail/metadata providers may never return. Reserve independent workers
// for the first visible icons instead of allowing refinement to occupy them.
class Work final
{
public:
    explicit Work(unsigned firstWorkers = 4, unsigned detailWorkers = 2)
        : first_(firstWorkers), detail_(detailWorkers) {}

    template<class Provider, class Apply>
    bool Submit(bool detail, std::wstring key, Provider provider, Apply apply,
        HWND window, UINT message)
    {
        return (detail ? detail_ : first_).Submit(std::move(key),
            std::move(provider), std::move(apply), window, message);
    }

    void Drain() { first_.Drain(); detail_.Drain(); }
    void Cancel(const std::wstring& prefix = {})
    { first_.Cancel(prefix); detail_.Cancel(prefix); }
    void Stop() { first_.Stop(); detail_.Stop(); }

private:
    BackgroundWork first_, detail_;
};
}
