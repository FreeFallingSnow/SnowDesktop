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
// partial menu or retain that menu's tokens. Shortcut/folder association warmup
// and provider-specific metadata/Start queries keep their existing paths.
template <class QueryOnce>
Reply QueryWithCataloguePriming(const Request &request, bool &backgroundReady, bool &filesReady, QueryOnce &&queryOnce)
{
    auto reply = queryOnce(request);
    if (!reply.ok) return reply;
    const bool background = request.background && request.sourceClsid.empty();
    if (!background && !NeedsFileCataloguePriming(request)) return reply;
    bool &ready = background ? backgroundReady : filesReady;
    if (!ready)
    {
        reply = queryOnce(request);
        ready = reply.ok;
    }
    return reply;
}
} // namespace snowdesktop::shell_extensions
