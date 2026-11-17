#pragma once

#include "platform/runtime_module_status.h"

#include <functional>

namespace snowdesktop::winui
{

enum class RuntimeModulesQueryScope { Status, StatusAndResidency };
using RuntimeModulesQuery = std::function<void(
    RuntimeModulesQueryScope, std::function<void(runtime_modules::Snapshot)>)>;

} // namespace snowdesktop::winui
