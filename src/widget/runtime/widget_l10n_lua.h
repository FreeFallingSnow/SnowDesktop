#pragma once

#include "widget/packages/widget_manifest_locale.h"

struct lua_State;

namespace snowdesktop::widget_l10n
{
void PushWidgetL10nAPI(lua_State* state, const LocaleCatalogs& catalogs);
}
