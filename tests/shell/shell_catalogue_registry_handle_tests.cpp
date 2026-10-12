#include <windows.h>
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "shell/shell_extension_catalogue.h"

namespace
{
namespace ext = snowdesktop::shell_extensions;

void Check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

struct OwnedKey
{
    HKEY value = nullptr;
    OwnedKey(HKEY root, const std::wstring &path)
    {
        Check(RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
            nullptr, &value, nullptr) == ERROR_SUCCESS, "create owned association key");
    }
    OwnedKey(const OwnedKey &) = delete;
    ~OwnedKey() { Close(); }
    void Close() { if (value) { RegCloseKey(value); value = nullptr; } }
};

struct PrivateRegistry
{
    HKEY value = nullptr;
    std::wstring path;
    bool owned = false;
    PrivateRegistry()
    {
        GUID id{};
        Check(SUCCEEDED(CoCreateGuid(&id)), "association fixture GUID");
        wchar_t text[40]{};
        Check(StringFromGUID2(id, text, 40) != 0, "association fixture GUID text");
        path = L"Software\\SnowDesktopCatalogueHandleTests\\" + std::wstring(text);
        DWORD disposition = 0;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &value, &disposition) == ERROR_SUCCESS,
            "create isolated association root");
        owned = disposition == REG_CREATED_NEW_KEY;
        if (!owned)
        {
            RegCloseKey(value); value = nullptr;
            throw std::runtime_error("never adopt an existing registry fixture");
        }
    }
    void Cleanup()
    {
        if (value) { RegCloseKey(value); value = nullptr; }
        if (owned)
        {
            Check(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS,
                "remove only this fixture's GUID root");
            owned = false;
        }
    }
    ~PrivateRegistry()
    {
        if (value) RegCloseKey(value);
        if (owned && RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) != ERROR_SUCCESS)
            fputs("FIXTURE_CLEANUP_FAILURE\n", stderr);
    }
};

// RegOverridePredefKey affects this test process only. No existing association
// or user security descriptor is changed; both hives refer to our GUID tree.
struct RegistryOverrides
{
    bool classes = false, user = false;
    RegistryOverrides(HKEY classKey, HKEY userKey)
    {
        Check(RegOverridePredefKey(HKEY_CURRENT_USER, userKey) == ERROR_SUCCESS,
            "override user within the fixture process");
        user = true;
        if (RegOverridePredefKey(HKEY_CLASSES_ROOT, classKey) != ERROR_SUCCESS)
        {
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); user = false;
            throw std::runtime_error("override classes within the fixture process");
        }
        classes = true;
    }
    void Restore()
    {
        if (classes)
        {
            Check(RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr) == ERROR_SUCCESS,
                "restore predefined classes mapping");
            classes = false;
        }
        if (user)
        {
            Check(RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) == ERROR_SUCCESS,
                "restore predefined user mapping");
            user = false;
        }
    }
    ~RegistryOverrides()
    {
        if (classes) RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        if (user) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    }
};

std::wstring CurrentUserSid()
{
    HANDLE token = nullptr;
    Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
        "query fixture user token");
    struct CloseToken { HANDLE value; ~CloseToken() { CloseHandle(value); } } close{token};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    Check(bytes > 0, "query token size");
    std::vector<BYTE> data(bytes);
    Check(GetTokenInformation(token, TokenUser, data.data(), bytes, &bytes) != FALSE,
        "read fixture user token");
    PWSTR sid = nullptr;
    Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,
        &sid) != FALSE, "format fixture user SID");
    std::wstring result(sid);
    LocalFree(sid);
    return result;
}

struct RestrictedKey
{
    HKEY key;
    std::vector<BYTE> original;
    bool armed = false;
    RestrictedKey(HKEY target, const std::wstring &sid, const wchar_t *denied) : key(target)
    {
        DWORD bytes = 0;
        Check(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &bytes) ==
            ERROR_INSUFFICIENT_BUFFER, "query private DACL size");
        original.resize(bytes);
        Check(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data()), &bytes) ==
            ERROR_SUCCESS, "read private DACL");
        const auto sddl = L"D:P(D;;" + std::wstring(denied) + L";;;" + sid +
            L")(A;;KA;;;" + sid + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
            SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "construct private DACL");
        const auto status = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(descriptor);
        Check(status == ERROR_SUCCESS, "restrict only the owned fixture key");
        armed = true;
    }
    void Restore()
    {
        if (armed)
        {
            Check(RegSetKeySecurity(key, DACL_SECURITY_INFORMATION,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data())) == ERROR_SUCCESS,
                "restore owned DACL");
            armed = false;
        }
    }
    ~RestrictedKey()
    {
        if (armed && RegSetKeySecurity(key, DACL_SECURITY_INFORMATION,
            reinterpret_cast<PSECURITY_DESCRIPTOR>(original.data())) != ERROR_SUCCESS)
            fputs("DACL_RESTORE_FAILURE\n", stderr);
    }
};

void Put(HKEY root, const std::wstring &path, const wchar_t *name, const std::wstring &value)
{
    OwnedKey key(root, path);
    Check(RegSetValueExW(key.value, name, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
        "write private association");
}

std::vector<std::wstring> Types(const ext::Catalogue &catalogue, const std::string &id)
{
    const auto row = std::find_if(catalogue.rows.begin(), catalogue.rows.end(),
        [&](const auto &item) { return item.id == id; });
    Check(row != catalogue.rows.end(), "expected private command exists");
    return row->types;
}
}

void TestCatalogueRegistryHandles()
{
    PrivateRegistry registry;
    {
        OwnedKey classes(registry.value, L"Classes"), user(registry.value, L"User");
        const auto sid = CurrentUserSid();
        const std::wstring choiceParent =
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts";
        // Eighty extensions exercise the actual parallel association reader and
        // metadata workers, including the predefined-root ownership boundary.
        for (int i = 0; i < 80; ++i)
            Put(classes.value, L".snapshot" + std::to_wstring(i), nullptr, L"Choice.A");
        for (const auto *name : {L"Choice.A", L"Choice.B", L"Choice.C"})
            Put(classes.value, std::wstring(name) + L"\\shell\\inspect\\command",
                nullptr, L"unused.exe %1");
        Put(classes.value, L".snapshot0\\OpenWithProgids", L"Choice.C", L"");
        Put(user.value, choiceParent + L"\\.snapshot0\\UserChoice", L"ProgId", L"Choice.B");
        OwnedKey extension(classes.value, L".snapshot0"), choiceRoot(user.value, choiceParent),
            choice(user.value, choiceParent + L"\\.snapshot0\\UserChoice");
        RegistryOverrides overrides(classes.value, user.value);
        wchar_t raw[64]{};
        DWORD bytes = sizeof(raw);
        Check(RegGetValueW(HKEY_CLASSES_ROOT, L".snapshot0", nullptr, RRF_RT_REG_SZ,
            nullptr, raw, &bytes) == ERROR_SUCCESS && std::wstring(raw) == L"Choice.A",
            "isolated classes mapping is active before production catalogue reads");

        const auto first = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        Check(first.rows.size() == 3 && Types(first, "reg:choice.a\\shell\\inspect").size() == 80 &&
            Types(first, "reg:choice.b\\shell\\inspect") == std::vector<std::wstring>{L".snapshot0"} &&
            Types(first, "reg:choice.c\\shell\\inspect") == std::vector<std::wstring>{L".snapshot0"},
            "default, user choice and alternate retain all independent associations");
        bytes = sizeof(raw);
        Check(RegGetValueW(HKEY_CLASSES_ROOT, L".snapshot0", nullptr, RRF_RT_REG_SZ,
            nullptr, raw, &bytes) == ERROR_SUCCESS && std::wstring(raw) == L"Choice.A",
            "catalogue does not close the borrowed predefined classes key");

        RestrictedKey choiceRestriction(choiceRoot.value, sid, L"0x1"),
            extensionRestriction(extension.value, sid, L"0x8");
        HKEY denied = nullptr;
        const auto deniedStatus = RegOpenKeyExW(HKEY_CURRENT_USER, choiceParent.c_str(),
            0, KEY_QUERY_VALUE, &denied);
        if (denied) RegCloseKey(denied);
        Check(deniedStatus == ERROR_ACCESS_DENIED, "owned parent rejects query access");
        bytes = sizeof(raw);
        Check(RegGetValueW(HKEY_CURRENT_USER, (choiceParent + L"\\.snapshot0\\UserChoice").c_str(),
            L"ProgId", RRF_RT_REG_SZ, nullptr, raw, &bytes) == ERROR_SUCCESS &&
            std::wstring(raw) == L"Choice.B", "child remains readable through its full path");
        Check(snowdesktop::settings_ipc::Pack(ext::ReadCatalogue(HKEY_CLASSES_ROOT, false)) ==
            snowdesktop::settings_ipc::Pack(first),
            "restricted parent access preserves readable children and association fingerprints");

        Put(choice.value, L"", L"ProgId", L"CHOICE.B");
        const auto caseChanged = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        Check(Types(caseChanged, "reg:choice.b\\shell\\inspect") ==
            std::vector<std::wstring>{L".snapshot0"} && caseChanged.revision != first.revision &&
            caseChanged.folderRevision == first.folderRevision,
            "next scan preserves the raw user choice case in row fingerprints");
        Put(choice.value, L"", L"ProgId", L"Choice.C");
        const auto changed = ext::ReadCatalogue(HKEY_CLASSES_ROOT, false);
        Check(Types(changed, "reg:choice.b\\shell\\inspect") ==
            std::vector<std::wstring>{L"progid:choice.b"} && changed.revision != first.revision &&
            changed.folderRevision == first.folderRevision,
            "next scan follows live user choice under parent restrictions");

        choiceRestriction.Restore();
        choice.Close(); choiceRoot.Close();
        Check(RegDeleteTreeW(user.value, choiceParent.c_str()) == ERROR_SUCCESS,
            "replace only the owned choice parent");
        Put(user.value, choiceParent + L"\\.snapshot0\\UserChoice", L"ProgId", L"Choice.B");
        Check(snowdesktop::settings_ipc::Pack(ext::ReadCatalogue(HKEY_CLASSES_ROOT, false)) ==
            snowdesktop::settings_ipc::Pack(first),
            "new scan reopens a replaced parent instead of retaining deleted handles");
        extensionRestriction.Restore();
        overrides.Restore();
    }
    registry.Cleanup();
}
