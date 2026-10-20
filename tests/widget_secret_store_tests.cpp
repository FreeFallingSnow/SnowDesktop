#include "widget/storage/widget_secret_store.h"
#include "widget/secret_store_legacy_fixture.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

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

void WriteBytes(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    Expect(static_cast<bool>(file), "synthetic store/cache fixture writes successfully");
}

void CheckPublishedRollback(const std::filesystem::path& root)
{
    using Current = snowdesktop::widget_runtime::WidgetSecretStore;
    using Published = snowdesktop::published_widget_runtime::WidgetSecretStore;
    const auto path = root / L"published-rollback.bin";
    std::string error, first, second, third, resolved;
    Published published(path);
    Expect(published.Load(error) &&
        published.Set("rollback", "owner", "first", "synthetic-first", first, error) &&
        published.Set("rollback", "owner", "second", "synthetic-second", second, error),
        "published production writer seeds two original secret records");
    Current current(path);
    Expect(current.Load(error) && current.ReadForEditing("rollback", "owner", "first", resolved, error) &&
        resolved == "synthetic-first", "current editor reads the published secret");
    if (!resolved.empty()) SecureZeroMemory(resolved.data(), resolved.size());
    resolved.clear();
    Expect(current.Set("rollback", "owner", "first", "candidate-first-updated", first, error),
        "current writer updates a secret before rollback");
    Published rollback(path);
    Expect(rollback.Load(error), "actual published production reader reopens the candidate store");
    Expect(rollback.Set("rollback", "owner", "third", "synthetic-third", third, error),
        "published production writer adds a secret after rollback");
    Current returned(path);
    Expect(returned.Load(error) && returned.Reference("rollback", "owner", "first") == first &&
        returned.Reference("rollback", "owner", "second") == second &&
        returned.Reference("rollback", "owner", "third") == third,
        "all three original identities survive a published rollback write");
    for (const auto& item : {std::pair{first, std::string("candidate-first-updated")},
            std::pair{second, std::string("synthetic-second")}, std::pair{third, std::string("synthetic-third")}})
    {
        Expect(returned.Resolve("rollback", "owner", item.first, resolved, error) && resolved == item.second,
            "actual persisted ciphertext survives the complete published/current round trip");
        if (!resolved.empty()) SecureZeroMemory(resolved.data(), resolved.size());
        resolved.clear();
    }
    // The old writer keeps references but has no knowledge of the mask cache.
    Expect(!returned.Length("rollback", "owner", "first"),
        "a published write invalidates lengths bound to the preceding file");
    Expect(returned.ReadForEditing("rollback", "owner", "first", resolved, error) &&
        resolved == "candidate-first-updated" &&
        returned.Length("rollback", "owner", "first") == std::string_view("candidate-first-updated").size(),
        "explicit access refreshes lengths after a published write");
    if (!resolved.empty()) SecureZeroMemory(resolved.data(), resolved.size());
    resolved.clear();
    Published changed(path);
    Expect(changed.Load(error) && changed.Set("rollback", "owner", "first", "short", first, error),
        "the published writer can update an existing reference to a different length");
    Current changedAgain(path);
    Expect(changedAgain.Load(error) && !changedAgain.Length("rollback", "owner", "first") &&
        changedAgain.ReadForEditing("rollback", "owner", "first", resolved, error) && resolved == "short" &&
        changedAgain.Length("rollback", "owner", "first") == 5u,
        "retained references cannot reuse stale lengths after an old-host edit");
    if (!resolved.empty()) SecureZeroMemory(resolved.data(), resolved.size());
}

void CheckCacheAndFailedLoad(const std::filesystem::path& root)
{
    using Store = snowdesktop::widget_runtime::WidgetSecretStore;
    const auto path = root / L"optional-cache.bin";
    const std::filesystem::path cache = path.wstring() + L".lengths";
    std::string error, reference, resolved;
    Store initial(path);
    Expect(initial.Load(error) && initial.Set("cache", "owner", "key", "synthetic-cache", reference, error),
        "cache fixture persists an encrypted record");
    const auto primary = ReadBytes(path);
    const auto metadata = ReadBytes(cache);
    Expect(primary.starts_with("SDSECR01") && metadata.starts_with("SDSECL01") &&
        metadata.find("synthetic-cache") == std::string::npos,
        "length metadata is separate from the published primary layout and contains no plaintext");
    for (const auto& damaged : {std::string{}, metadata.substr(0, metadata.size() / 2), std::string("invalid")})
    {
        WriteBytes(cache, damaged);
        Store reopen(path);
        Expect(reopen.Load(error) && !reopen.Length("cache", "owner", "key") &&
            reopen.Resolve("cache", "owner", reference, resolved, error) && resolved == "synthetic-cache",
            "missing/truncated/invalid optional metadata cannot hide or corrupt a secret");
        if (!resolved.empty()) SecureZeroMemory(resolved.data(), resolved.size());
        resolved.clear();
    }
    std::error_code fs;
    std::filesystem::remove(cache, fs);
    Store absentCache(path);
    Expect(absentCache.Load(error) && !absentCache.Length("cache", "owner", "key"),
        "a missing optional cache permits loading the published primary file");
    auto mismatched = metadata;
    if (mismatched.size() > 8) mismatched[8] ^= 1;
    WriteBytes(cache, mismatched);
    Store mismatch(path);
    Expect(mismatch.Load(error) && !mismatch.Length("cache", "owner", "key"),
        "cache digest mismatch is ignored before publishing length metadata");
    std::filesystem::remove(cache, fs);
    std::filesystem::create_directory(cache, fs);
    Store cacheBlocked(path);
    Expect(cacheBlocked.Load(error) && cacheBlocked.Set("cache", "owner", "key", "changed-cache", reference, error),
        "an unwritable optional cache does not fail the committed primary secret write");
    Store blockedReload(path);
    Expect(blockedReload.Load(error) && blockedReload.Resolve("cache", "owner", reference, resolved, error) &&
        resolved == "changed-cache", "secrets reload after an optional cache publication failure");
    if (!resolved.empty()) SecureZeroMemory(resolved.data(), resolved.size());
    const auto invalid = root / L"invalid.bin";
    WriteBytes(invalid, "SDSECR-invalid-original-data");
    Store failed(invalid);
    Expect(!failed.Load(error) && !failed.Set("cache", "owner", "key", "new-value", reference, error) &&
        ReadBytes(invalid) == "SDSECR-invalid-original-data",
        "a failed primary load cannot be followed by a destructive empty-store save");
    WriteBytes(invalid, primary);
    Expect(failed.Load(error) && failed.Set("cache", "owner", "another", "new-value", reference, error),
        "a successful explicit reload clears the failed-load write guard");
}
}

int main(int argc, char** argv)
{
    using snowdesktop::widget_runtime::WidgetSecretStore;

    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktop-widget-secret-tests-" +
            std::to_wstring(GetCurrentProcessId()));
    const auto path = root / L"secrets.bin";
    std::error_code filesystemError;
    std::filesystem::remove_all(root, filesystemError);
    CheckPublishedRollback(root);
    if (argc == 2 && std::string_view(argv[1]) == "--rollback-only")
    {
        std::filesystem::remove_all(root, filesystemError);
        return failures == 0 ? 0 : 1;
    }
    CheckCacheAndFailedLoad(root);

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
    // Copy only the backwards-compatible primary, without its optional cache.
    auto legacyBytes = persisted;
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
    Expect(ReadBytes(legacyPath) == legacyBytes,
        "reading an existing secret for editing leaves published ciphertext bytes unchanged");
    // Recover files written by the earlier 1.0.11.0 inline-length candidate.
    auto previousBytes = persisted;
    previousBytes[7] = '2';
    const auto length = static_cast<std::uint32_t>(firstSecret.size());
    std::string inlineLength;
    for (unsigned shift = 0; shift < 32; shift += 8)
        inlineLength.push_back(static_cast<char>((length >> shift) & 0xffu));
    previousBytes.insert(24, inlineLength);
    const auto previousPath = root / L"previous-v2.bin";
    WriteBytes(previousPath, previousBytes);
    WidgetSecretStore previous(previousPath);
    Expect(previous.Load(error) && previous.Length("package-a", "instance-a", "apiToken") == firstSecret.size() &&
        ReadBytes(previousPath) == persisted,
        "opening an earlier inline-length store restores the published layout without changing ciphertext");
    snowdesktop::published_widget_runtime::WidgetSecretStore previousPublished(previousPath);
    Expect(previousPublished.Load(error) && previousPublished.Resolve("package-a", "instance-a", firstReference,
        legacyPlaintext, error) && legacyPlaintext == firstSecret,
        "the actual published reader decrypts the recovered inline-length store");
    if (!legacyPlaintext.empty()) SecureZeroMemory(legacyPlaintext.data(), legacyPlaintext.size());
    legacyPlaintext.clear();
    const auto blockedPrevious = root / L"blocked-v2.bin";
    WriteBytes(blockedPrevious, previousBytes);
    std::filesystem::create_directory(blockedPrevious.wstring() + L".tmp", filesystemError);
    WidgetSecretStore cannotConvert(blockedPrevious);
    std::string failedReference;
    Expect(!cannotConvert.Load(error) &&
        !cannotConvert.Set("package-a", "instance-a", "extra", "new-value", failedReference, error) &&
        ReadBytes(blockedPrevious) == previousBytes,
        "failed inline-format conversion preserves the entire primary file and prohibits later overwrite");
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
