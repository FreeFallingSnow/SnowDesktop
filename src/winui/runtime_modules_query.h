#pragma once

#include "platform/runtime_module_status.h"

#include <functional>

namespace snowdesktop::winui
{

using RuntimeModulesQuery = std::function<void(std::function<void(runtime_modules::Snapshot)>)>;

} // namespace snowdesktop::winui
