#include "personalization.h"
#include "data/data_paths.h"

std::wstring GetPersonalizationPath()
{
    return GetDataFilePath(L"SnowDesktop.personalization.json");
}
