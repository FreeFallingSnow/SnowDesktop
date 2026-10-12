#pragma once
#include "shell_extension_menu.h"
#include <algorithm>
#include <shlwapi.h>

namespace snowdesktop::shell_extensions
{
inline bool NeedsFileCataloguePriming(const Request &request)
{
    return !request.background && !request.startPinOnly && !request.originalShortcutOnly &&
        request.sourceClsid.empty() && ResolveContext(request) == Context::File &&
        std::none_of(request.paths.begin(), request.paths.end(), [](const auto &path) {
            return lstrcmpiW(PathFindExtensionW(path.c_str()), L".lnk") == 0;
        });
}

// A cold aggregate can report success before packaged command providers become
// visible. Rebind once in the same supervised STA; never publish its first
// partial menu or retain that menu's tokens. The priming pass binds and queries
// only; submenu materialization and icons belong to the final pass.
// Shortcut/folder association warmup
// and provider-specific metadata/Start queries keep their existing paths.
template <class QueryOnce>
Reply QueryWithCataloguePriming(const Request &request, bool &backgroundReady, bool &filesReady, QueryOnce &&queryOnce)
{
    const bool background = request.background && request.sourceClsid.empty();
    const bool file = NeedsFileCataloguePriming(request);
    bool &ready = background ? backgroundReady : filesReady;
    const bool priming = (background || file) && !ready;
    auto initial = request;
    // Directory background shares desktop providers without paying for the
    // virtual desktop aggregate twice. Only this unpublished pass changes
    // scope; the final bind retains desktop-only verbs and the exact selection.
    if (priming && background && initial.context == Context::Desktop)
        initial.context = Context::FolderBackground;
    auto reply = queryOnce(initial, priming);
    if (!reply.ok) return reply;
    if (priming)
    {
        reply = queryOnce(request, false);
        ready = reply.ok;
    }
    return reply;
}
} // namespace snowdesktop::shell_extensions
