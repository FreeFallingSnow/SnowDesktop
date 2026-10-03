#include "personalization.h"
#include "data_paths.h"

std::wstring GetPersonalizationPath()
{
    return GetDataFilePath(L"SnowDesktop.personalization.json");
}
