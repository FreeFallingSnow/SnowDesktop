#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop
{

struct DesktopNamespaceRegistration
{
    std::wstring clsid;
    std::wstring displayName;
    std::wstring targetPath;
    Pidl absolutePidl;
    bool userScoped = false;
};

std::vector<DesktopNamespaceRegistration>
LoadDesktopNamespaceRegistrations();

std::wstring ResolveRegisteredDesktopNamespaceClsid(
    PCIDLIST_ABSOLUTE itemPidl,
    std::wstring_view itemPath,
    const std::vector<DesktopNamespaceRegistration>& registrations,
    bool* visibleByDefault = nullptr);

bool IsStandardDesktopIconClsid(std::wstring_view clsid);

} // namespace snowdesktop
