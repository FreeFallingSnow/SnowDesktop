#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace snowdesktop::widget_runtime
{
/**
 * Stores widget setting secrets as per-user DPAPI ciphertext.
 *
 * Lua only receives the opaque reference. Plaintext is decrypted transiently
 * for a host-owned operation and is never serialized into widget storage.
 */
class WidgetSecretStore
{
public:
    static constexpr std::size_t MaximumSecretBytes = 16 * 1024;
    static constexpr std::size_t MaximumRecords = 512;

    explicit WidgetSecretStore(std::filesystem::path path);

    bool Load(std::string& error);

    bool Set(std::string_view packageId, std::string_view instanceId,
        std::string_view settingKey, std::string_view plaintext,
        std::string& reference, std::string& error);
    bool Resolve(std::string_view packageId, std::string_view instanceId,
        std::string_view reference, std::string& plaintext,
        std::string& error) const;
    bool Has(std::string_view packageId, std::string_view instanceId,
        std::string_view settingKey, std::string_view reference) const;
    std::string Reference(std::string_view packageId,
        std::string_view instanceId, std::string_view settingKey) const;
    bool RemoveSetting(std::string_view packageId,
        std::string_view instanceId, std::string_view settingKey,
        bool& removed, std::string& error);
    bool RemoveInstance(std::string_view instanceId, bool& removed,
        std::string& error);

    static bool IsReference(std::string_view value) noexcept;

private:
    struct Record
    {
        std::string reference;
        std::string packageId;
        std::string instanceId;
        std::string settingKey;
        std::string ciphertext;
    };

    bool Save(const std::unordered_map<std::string, Record>& records,
        std::string& error) const;
    static std::string EntropyFor(const Record& record);
    static bool Protect(const Record& record, std::string_view plaintext,
        std::string& ciphertext, std::string& error);
    static bool Unprotect(const Record& record, std::string& plaintext,
        std::string& error);
    static bool GenerateReference(std::string& reference,
        std::string& error);

    std::filesystem::path path_;
    std::unordered_map<std::string, Record> records_;
};
}
