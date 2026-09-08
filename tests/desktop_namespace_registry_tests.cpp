#include "desktop_namespace_registry.h"

#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void TestTargetPathAliasResolution()
{
    std::vector<snowdesktop::DesktopNamespaceRegistration>
        registrations;
    snowdesktop::DesktopNamespaceRegistration registration;
    registration.clsid =
        L"{EEEEFCF7-867B-4FA2-9ABD-884CF531B612}";
    registration.targetPath =
        L"C:\\Users\\Example\\WPSDrive\\WPS Cloud";
    registration.userScoped = true;
    registrations.push_back(std::move(registration));

    bool visibleByDefault = false;
    Check(
        snowdesktop::ResolveRegisteredDesktopNamespaceClsid(
            nullptr,
            L"c:\\users\\example\\wpsdrive\\wps cloud\\",
            registrations,
            &visibleByDefault) ==
            L"{EEEEFCF7-867B-4FA2-9ABD-884CF531B612}",
        "a registered namespace alias must retain its CLSID when its parsing name is only a target path");
    Check(
        visibleByDefault,
        "a user-scoped namespace alias must remain visible when it has no explicit Explorer override");
    Check(
        snowdesktop::ResolveRegisteredDesktopNamespaceClsid(
            nullptr,
            L"C:\\Users\\Example\\Desktop\\Ordinary Folder",
            registrations).empty(),
        "an ordinary folder must not inherit an unrelated namespace CLSID");
}

void TestPidlIdentityTakesPrecedence()
{
    PIDLIST_ABSOLUTE thisPc = nullptr;
    const HRESULT parsed = SHParseDisplayName(
        L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
        nullptr, &thisPc, 0, nullptr);
    Check(SUCCEEDED(parsed) && thisPc,
        "the standard This PC PIDL must be available for the identity test");
    if (!thisPc)
        return;

    std::vector<snowdesktop::DesktopNamespaceRegistration>
        registrations;
    snowdesktop::DesktopNamespaceRegistration registration;
    registration.clsid =
        L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}";
    registration.targetPath = L"C:\\Unrelated";
    registration.absolutePidl.reset(ILCloneFull(thisPc));
    registrations.push_back(std::move(registration));

    Check(
        snowdesktop::ResolveRegisteredDesktopNamespaceClsid(
            thisPc, L"C:\\Different", registrations) ==
            L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
        "an exact Shell PIDL match must identify a registered namespace independently of its filesystem alias");
    ILFree(thisPc);
}

void TestStandardIconClassification()
{
    Check(
        snowdesktop::IsStandardDesktopIconClsid(
            L"{645ff040-5081-101b-9f08-00aa002f954e}"),
        "standard desktop CLSID matching must be case-insensitive");
    Check(
        !snowdesktop::IsStandardDesktopIconClsid(
            L"{EEEEFCF7-867B-4FA2-9ABD-884CF531B612}"),
        "a third-party namespace must not be treated as a protected standard icon");
}

} // namespace

int main()
{
    const HRESULT comResult = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED);
    TestTargetPathAliasResolution();
    TestPidlIdentityTakesPrecedence();
    TestStandardIconClassification();
    if (SUCCEEDED(comResult))
        CoUninitialize();

    if (failures != 0)
    {
        std::cerr << failures
            << " desktop namespace registry test(s) failed\n";
        return 1;
    }
    std::cout
        << "All desktop namespace registry tests passed\n";
    return 0;
}
