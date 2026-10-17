#pragma once

#include "external_drop_content.h"
#include "ole_drag_rules.h"
#include <optional>
#include <utility>

namespace snowdesktop::delayed_desktop_file_drop
{
// Production dispatch boundary for Explorer's delayed CF_HDROP. An engaged
// NONE is a handled rejection; nullopt leaves non-file formats/targets to their
// existing routes. Probe must not render an asynchronous source on the UI STA.
template<class Submit>
std::optional<DWORD> Dispatch(IDataObject* source, bool bareDesktop,
    bool asynchronousSource, DWORD keys, DWORD allowed, Submit&& submit)
{
    if (!bareDesktop || !asynchronousSource ||
        !external_drop_content::ProbeFileSource(source).available)
        return std::nullopt;
    const DWORD selected = ole_drag_rules::ChooseDropEffect(keys, allowed, DROPEFFECT_COPY);
    if (selected == DROPEFFECT_NONE) return DROPEFFECT_NONE;
    return std::forward<Submit>(submit)(selected);
}
}
