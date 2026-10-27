#pragma once

// Windows propkey.h exports KIND_UNKNOWN as a string macro, which collides
// with an upstream protobuf enum. Keep that caller macro outside this SDK
// include boundary, and restore it for Shell consumers afterwards.
#pragma push_macro("KIND_UNKNOWN")
#undef KIND_UNKNOWN
#include <perfetto.h>
#pragma pop_macro("KIND_UNKNOWN")

// Finite categories are the collection boundary. Module/instance identities
// belong in event arguments, never in dynamically registered categories.
PERFETTO_DEFINE_CATEGORIES_IN_NAMESPACE(snowdesktop::performance::native_trace,
    perfetto::Category("snow.resource"),
    perfetto::Category("snow.lua.callback"),
    perfetto::Category("snow.lua.detail"),
    perfetto::Category("snow.task"),
    perfetto::Category("snow.task.detail"),
    perfetto::Category("snow.frame"),
    perfetto::Category("snow.input"),
    perfetto::Category("snow.dock"),
    perfetto::Category("snow.popup"),
    perfetto::Category("snow.layout"),
    perfetto::Category("snow.render"),
    perfetto::Category("snow.icon"),
    perfetto::Category("snow.cache.detail"),
    perfetto::Category("snow.profiler"));

PERFETTO_USE_CATEGORIES_FROM_NAMESPACE(snowdesktop::performance::native_trace);
