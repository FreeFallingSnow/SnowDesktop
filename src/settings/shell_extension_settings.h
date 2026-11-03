#pragma once

#include "shell_extension_settings_types.h"

#include <set>
#include <string>

struct JsonValue;

namespace snowdesktop::shell_extensions
{

Category CategoryOf(Context context);
void Normalize(Preferences &value);
Preferences ReadPreferences(const JsonValue *json);
std::string Quote(const std::string &text);
std::string WritePreferences(Preferences value);
bool IsHidden(const Preferences &prefs, const std::string &id, Context context);
bool CommonShown(const Preferences &prefs, const std::string &id, Category category);
Visibility OverrideOf(const Preferences &prefs, const std::string &id, Context context);
void SetCommon(Preferences &prefs, const std::string &id, Category category, bool shown);
void SetOverride(Preferences &prefs, const std::string &id, Context context, Visibility value);
std::set<std::string> EffectiveShownIds(const Preferences &prefs, Context context);
bool HasOptIns(const Preferences &prefs, Context context = Context::Automatic);
void SetHidden(Preferences &prefs, const std::string &id, Context context, bool hidden);

} // namespace snowdesktop::shell_extensions
