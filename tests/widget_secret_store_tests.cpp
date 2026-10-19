#include "widget/storage/widget_secret_store.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream output;
    output << file.rdbuf();
    return output.str();
}
}

int main()
{
    using snowdesktop::widget_runtime::WidgetSecretStore;

    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktop-widget-secret-tests-" +
            std::to_wstring(GetCurrentProcessId()));
    const auto path = root / L"secrets.bin";
    std::error_code filesystemError;
    std::filesystem::remove_all(root, filesystemError);

    Expect(!WidgetSecretStore::IsReference("secret:v1:short") &&
        !WidgetSecretStore::IsReference(
            "secret:v1:0000000000000000000000000000000G") &&
        WidgetSecretStore::IsReference(
            "secret:v1:00000000000000000000000000000000"),
        "opaque secret references use the exact versioned lowercase format");

    WidgetSecretStore store(path);
    std::string error;
    Expect(store.Load(error) && error.empty(),
        "a missing secret store loads as empty");

    const std::string firstSecret = "first-api-token-do-not-serialize";
    std::string firstReference;
    Expect(store.Set("package-a", "instance-a", "apiToken",
            firstSecret, firstReference, error) &&
        WidgetSecretStore::IsReference(firstReference) &&
        firstReference.find(firstSecret) == std::string::npos,
        "setting a secret returns only an opaque reference");
    const std::string persisted = ReadBytes(path);
    Expect(store.Length("package-a", "instance-a", "apiToken") == firstSecret.size() &&
            !store.Length("package-a", "instance-b", "apiToken"),
        "mask length is available without resolving plaintext and remains owner scoped");
    // An actual v1 record has no length slot. Keep the DPAPI ciphertext and
    // owner identity intact while exercising lazy metadata migration.
    auto legacyBytes = persisted;
    legacyBytes[7] = '1';
    legacyBytes.erase(24, 4);
    const auto legacyPath = root / L"legacy.bin";
    { std::ofstream file(legacyPath, std::ios::binary); file.write(legacyBytes.data(), static_cast<std::streamsize>(legacyBytes.size())); }
    WidgetSecretStore legacy(legacyPath);
    std::string legacyPlaintext;
    Expect(legacy.Load(error) && !legacy.Length("package-a", "instance-a", "apiToken"),
        "legacy records load without decrypting to discover length");
    Expect(legacy.ReadForEditing("package-a", "instance-a", "apiToken", legacyPlaintext, error) &&
            legacyPlaintext == firstSecret,
        "the first explicit read can open an existing encrypted record");
    SecureZeroMemory(legacyPlaintext.data(), legacyPlaintext.size());
    legacyPlaintext.clear();
    WidgetSecretStore migrated(legacyPath);
    Expect(migrated.Load(error) && migrated.Length("package-a", "instance-a", "apiToken") == firstSecret.size(),
        "explicit legacy reads persist length for future masked opens");
    Expect(!persisted.empty() &&
        persisted.find(firstSecret) == std::string::npos,
        "the persistent secret store never contains plaintext");
    Expect(store.Has("package-a", "instance-a", "apiToken",
            firstReference) &&
        !store.Has("package-a", "instance-b", "apiToken",
            firstReference),
        "secret references are bound to package, instance, and setting");

    std::string resolved;
    Expect(store.Resolve("package-a", "instance-a", firstReference,
            resolved, error) && resolved == firstSecret,
        "the owning widget instance can resolve a secret transiently");
    SecureZeroMemory(resolved.data(), resolved.size());
    resolved.clear();
    Expect(!store.Resolve("package-a", "instance-b", firstReference,
            resolved, error) && resolved.empty(),
        "another instance cannot resolve a secret reference");
    Expect(!store.Resolve("package-b", "instance-a", firstReference,
            resolved, error) && resolved.empty(),
        "another package cannot resolve a secret reference");

    const std::string replacement = "replacement-api-token";
    std::string replacementReference;
    Expect(store.Set("package-a", "instance-a", "apiToken",
            replacement, replacementReference, error) &&
        replacementReference == firstReference,
        "updating one setting preserves its opaque reference");
    Expect(store.Resolve("package-a", "instance-a", firstReference,
            resolved, error) && resolved == replacement,
        "an updated reference resolves to the latest secret");
    SecureZeroMemory(resolved.data(), resolved.size());
    resolved.clear();

    std::string secondReference;
    Expect(store.Set("package-a", "instance-a", "webhookSecret",
            "second-value", secondReference, error) &&
        secondReference != firstReference,
        "different secret settings receive different references");

    WidgetSecretStore reloaded(path);
    Expect(reloaded.Load(error) &&
        reloaded.Resolve("package-a", "instance-a", firstReference,
            resolved, error) && resolved == replacement,
        "DPAPI secrets survive a store reload for the same Windows user");
    SecureZeroMemory(resolved.data(), resolved.size());
    resolved.clear();

    bool removed = false;
    Expect(reloaded.RemoveSetting("package-a", "instance-a",
            "apiToken", removed, error) && removed &&
        !reloaded.Resolve("package-a", "instance-a", firstReference,
            resolved, error),
        "removing a setting revokes its reference");
    Expect(reloaded.RemoveInstance("instance-a", removed, error) &&
        removed && !reloaded.Resolve("package-a", "instance-a",
            secondReference, resolved, error),
        "removing an instance revokes all of its secret references");

    WidgetSecretStore emptyReload(path);
    Expect(emptyReload.Load(error),
        "an empty saved store reloads successfully");
    std::string unicodeReference;
    Expect(emptyReload.Set("package-a", "instance-a", "unicode", "a\xe4\xb8\xad\xf0\x9f\x94\x91",
            unicodeReference, error) && emptyReload.Length("package-a", "instance-a", "unicode") == 4u,
        "mask length matches UTF-16 editor characters rather than UTF-8 bytes");

    std::filesystem::remove_all(root, filesystemError);
    if (failures == 0)
        std::cout << "widget secret store tests passed\n";
    return failures == 0 ? 0 : 1;
}
