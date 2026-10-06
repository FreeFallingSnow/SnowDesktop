#pragma once

#include <string>

constexpr int kNameSearchNoMatchRank = 9;

int NameSearchMatchRank(const std::wstring& name, const std::wstring& query);
bool NameMatchesQuery(const std::wstring& name, const std::wstring& query);

