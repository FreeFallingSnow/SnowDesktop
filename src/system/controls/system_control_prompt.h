#pragma once
#include "system_controls.h"
#include "theme/personalization.h"
#include <windows.h>

namespace snowdesktop
{
// Host-owned, UI-thread-only modal state. No secret or confirmation token is
// returned to a component. The validity callback closes a revoked prompt.
struct SystemControlPromptState
{
    HWND window = nullptr;
    bool cancelled = false;
    std::function<bool()> valid;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    std::string error;
};
bool ConfirmSystemControl(HWND owner, system_control::Request& request,
    const std::shared_ptr<SystemControlPromptState>& state,
    const PersonalizationSettings& appearance,
    const std::wstring& actor = {}, const system_control::Snapshot* wifi = nullptr);

// Internal offline evidence: only invisible, self-owned native controls and
// synthetic requests are used. No device backend or user credential is read.
struct SystemControlPromptPreview
{
    std::string name;
    int width=0,height=0;
    std::vector<std::uint32_t> pixels;
};
std::vector<SystemControlPromptPreview> RenderSystemControlPromptPreviews(
    const PersonalizationSettings&, unsigned dpi=96);
}
