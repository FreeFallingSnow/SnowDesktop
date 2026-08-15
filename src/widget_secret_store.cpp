#include "widget_secret_store.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::array<char, 8> FileMagic{
    'S', 'D', 'S', 'E', 'C', 'R', '0', '1' };
constexpr std::string_view ReferencePrefix = "secret:v1:";
constexpr std::size_t ReferenceRandomBytes = 16;
constexpr std::size_t MaximumOwnerBytes = 256;
constexpr std::size_t MaximumSettingKeyBytes = 128;
constexpr std::size_t MaximumCiphertextBytes = 128 * 1024;
constexpr std::uint64_t MaximumFileBytes = 64ull * 1024ull * 1024ull;

void Append16(std::string& output, std::uint16_t value)
{
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8u) & 0xffu));
}

void Append32(std::string& output, std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<char>((value >> shift) & 0xffu));
}

bool Read16(std::string_view input, std::size_t& offset,
    std::uint16_t& value)
{
    if (offset > input.size() || input.size() - offset < 2) return false;
    value = static_cast<std::uint16_t>(
        static_cast<unsigned char>(input[offset])) |
        static_cast<std::uint16_t>(
            static_cast<unsigned char>(input[offset + 1]) << 8u);
    offset += 2;
    return true;
}

bool Read32(std::string_view input, std::size_t& offset,
    std::uint32_t& value)
{
    if (offset > input.size() || input.size() - offset < 4) return false;
    value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8)
    {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(input[offset++])) << shift;
    }
    return true;
}

bool ReadField(std::string_view input, std::size_t& offset,
    std::size_t length, std::string& value)
{
    if (offset > input.size() || length > input.size() - offset)
        return false;
    value.assign(input.substr(offset, length));
    offset += length;
    return true;
}

bool ValidOwnerPart(std::string_view value, std::size_t maximum)
{
    return !value.empty() && value.size() <= maximum &&
        value.find('\0') == std::string_view::npos;
}
}

WidgetSecretStore::WidgetSecretStore(std::filesystem::path path)
    : path_(std::move(path))
{
}

bool WidgetSecretStore::IsReference(std::string_view value) noexcept
{
    if (value.size() != ReferencePrefix.size() +
            ReferenceRandomBytes * 2 ||
        !value.starts_with(ReferencePrefix))
        return false;
    return std::all_of(value.begin() +
            static_cast<std::ptrdiff_t>(ReferencePrefix.size()), value.end(),
        [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') ||
                (ch >= 'a' && ch <= 'f');
        });
}

std::string WidgetSecretStore::EntropyFor(const Record& record)
{
    return "SnowDesktop/widget-secret/v1\n" + record.packageId + "\n" +
        record.instanceId + "\n" + record.settingKey + "\n" +
        record.reference;
}

bool WidgetSecretStore::GenerateReference(std::string& reference,
    std::string& error)
{
    std::array<unsigned char, ReferenceRandomBytes> random{};
    const NTSTATUS status = BCryptGenRandom(nullptr, random.data(),
        static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
    {
        error = "cannot generate a secret reference";
        return false;
    }
    static constexpr char Hex[] = "0123456789abcdef";
    reference = std::string(ReferencePrefix);
    reference.reserve(ReferencePrefix.size() + random.size() * 2);
    for (const unsigned char value : random)
    {
        reference.push_back(Hex[(value >> 4u) & 0x0fu]);
        reference.push_back(Hex[value & 0x0fu]);
    }
    return true;
}

bool WidgetSecretStore::Protect(const Record& record,
    std::string_view plaintext, std::string& ciphertext,
    std::string& error)
{
    if (plaintext.empty() || plaintext.size() > MaximumSecretBytes ||
        plaintext.size() > (std::numeric_limits<DWORD>::max)())
    {
        error = "secret length is out of range";
        return false;
    }
    std::string entropy = EntropyFor(record);
    DATA_BLOB input{
        static_cast<DWORD>(plaintext.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data())) };
    DATA_BLOB optionalEntropy{
        static_cast<DWORD>(entropy.size()),
        reinterpret_cast<BYTE*>(entropy.data()) };
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"SnowDesktop widget secret",
            &optionalEntropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
            &output))
    {
        error = "cannot protect widget secret";
        SecureZeroMemory(entropy.data(), entropy.size());
        return false;
    }
    ciphertext.assign(reinterpret_cast<const char*>(output.pbData),
        output.cbData);
    LocalFree(output.pbData);
    SecureZeroMemory(entropy.data(), entropy.size());
    return !ciphertext.empty();
}

bool WidgetSecretStore::Unprotect(const Record& record,
    std::string& plaintext, std::string& error)
{
    if (record.ciphertext.empty() ||
        record.ciphertext.size() > (std::numeric_limits<DWORD>::max)())
    {
        error = "secret ciphertext is invalid";
        return false;
    }
    std::string entropy = EntropyFor(record);
    DATA_BLOB input{
        static_cast<DWORD>(record.ciphertext.size()),
        reinterpret_cast<BYTE*>(
            const_cast<char*>(record.ciphertext.data())) };
    DATA_BLOB optionalEntropy{
        static_cast<DWORD>(entropy.size()),
        reinterpret_cast<BYTE*>(entropy.data()) };
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &optionalEntropy, nullptr,
            nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
    {
        error = "secret is unavailable for this Windows user";
        SecureZeroMemory(entropy.data(), entropy.size());
        return false;
    }
    plaintext.assign(reinterpret_cast<const char*>(output.pbData),
        output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    SecureZeroMemory(entropy.data(), entropy.size());
    if (plaintext.empty() || plaintext.size() > MaximumSecretBytes)
    {
        SecureZeroMemory(plaintext.data(), plaintext.size());
        plaintext.clear();
        error = "unprotected secret length is out of range";
        return false;
    }
    return true;
}

bool WidgetSecretStore::Load(std::string& error)
{
    records_.clear();
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path_, filesystemError))
        return !filesystemError;
    const auto size = std::filesystem::file_size(path_, filesystemError);
    if (filesystemError || size > MaximumFileBytes)
    {
        error = "widget secret store is too large or unreadable";
        return false;
    }
    std::ifstream file(path_, std::ios::binary);
    if (!file)
    {
        error = "cannot open widget secret store";
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string bytes = stream.str();
    if (!file.good() && !file.eof())
    {
        error = "cannot read widget secret store";
        return false;
    }
    if (bytes.size() < FileMagic.size() + 4 ||
        !std::equal(FileMagic.begin(), FileMagic.end(), bytes.begin()))
    {
        error = "widget secret store has an invalid header";
        return false;
    }
    std::size_t offset = FileMagic.size();
    std::uint32_t count = 0;
    if (!Read32(bytes, offset, count) || count > MaximumRecords)
    {
        error = "widget secret store record count is invalid";
        return false;
    }
    std::unordered_map<std::string, Record> loaded;
    std::unordered_set<std::string> ownerKeys;
    for (std::uint32_t index = 0; index < count; ++index)
    {
        std::uint16_t referenceLength = 0;
        std::uint16_t packageLength = 0;
        std::uint16_t instanceLength = 0;
        std::uint16_t keyLength = 0;
        std::uint32_t ciphertextLength = 0;
        if (!Read16(bytes, offset, referenceLength) ||
            !Read16(bytes, offset, packageLength) ||
            !Read16(bytes, offset, instanceLength) ||
            !Read16(bytes, offset, keyLength) ||
            !Read32(bytes, offset, ciphertextLength))
        {
            error = "widget secret store is truncated";
            return false;
        }
        Record record;
        if (!ReadField(bytes, offset, referenceLength, record.reference) ||
            !ReadField(bytes, offset, packageLength, record.packageId) ||
            !ReadField(bytes, offset, instanceLength, record.instanceId) ||
            !ReadField(bytes, offset, keyLength, record.settingKey) ||
            !ReadField(bytes, offset, ciphertextLength,
                record.ciphertext) ||
            !IsReference(record.reference) ||
            !ValidOwnerPart(record.packageId, MaximumOwnerBytes) ||
            !ValidOwnerPart(record.instanceId, MaximumOwnerBytes) ||
            !ValidOwnerPart(record.settingKey, MaximumSettingKeyBytes) ||
            record.ciphertext.empty() ||
            record.ciphertext.size() > MaximumCiphertextBytes)
        {
            error = "widget secret store contains an invalid record";
            return false;
        }
        const std::string ownerKey = record.packageId + "\n" +
            record.instanceId + "\n" + record.settingKey;
        if (!ownerKeys.insert(ownerKey).second ||
            !loaded.emplace(record.reference, std::move(record)).second)
        {
            error = "widget secret store contains duplicate records";
            return false;
        }
    }
    if (offset != bytes.size())
    {
        error = "widget secret store contains trailing data";
        return false;
    }
    records_.swap(loaded);
    return true;
}

bool WidgetSecretStore::Save(
    const std::unordered_map<std::string, Record>& records,
    std::string& error) const
{
    if (records.size() > MaximumRecords)
    {
        error = "widget secret record quota exceeded";
        return false;
    }
    std::vector<const Record*> ordered;
    ordered.reserve(records.size());
    for (const auto& [reference, record] : records)
    {
        (void)reference;
        ordered.push_back(&record);
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const Record* left, const Record* right) {
            return left->reference < right->reference;
        });

    std::string output(FileMagic.begin(), FileMagic.end());
    Append32(output, static_cast<std::uint32_t>(ordered.size()));
    for (const Record* record : ordered)
    {
        if (!IsReference(record->reference) ||
            !ValidOwnerPart(record->packageId, MaximumOwnerBytes) ||
            !ValidOwnerPart(record->instanceId, MaximumOwnerBytes) ||
            !ValidOwnerPart(record->settingKey,
                MaximumSettingKeyBytes) ||
            record->ciphertext.empty() ||
            record->ciphertext.size() > MaximumCiphertextBytes)
        {
            error = "cannot serialize an invalid widget secret record";
            return false;
        }
        Append16(output,
            static_cast<std::uint16_t>(record->reference.size()));
        Append16(output,
            static_cast<std::uint16_t>(record->packageId.size()));
        Append16(output,
            static_cast<std::uint16_t>(record->instanceId.size()));
        Append16(output,
            static_cast<std::uint16_t>(record->settingKey.size()));
        Append32(output,
            static_cast<std::uint32_t>(record->ciphertext.size()));
        output += record->reference;
        output += record->packageId;
        output += record->instanceId;
        output += record->settingKey;
        output += record->ciphertext;
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(path_.parent_path(),
        filesystemError);
    if (filesystemError)
    {
        error = "cannot create private widget state directory";
        return false;
    }
    const std::filesystem::path temporary = path_.wstring() + L".tmp";
    std::ofstream file(temporary,
        std::ios::binary | std::ios::trunc);
    if (!file)
    {
        error = "cannot create widget secret store";
        return false;
    }
    file.write(output.data(), static_cast<std::streamsize>(output.size()));
    file.flush();
    if (!file)
    {
        error = "cannot write widget secret store";
        return false;
    }
    file.close();
    if (!MoveFileExW(temporary.c_str(), path_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot publish widget secret store";
        return false;
    }
    return true;
}

bool WidgetSecretStore::Set(std::string_view packageId,
    std::string_view instanceId, std::string_view settingKey,
    std::string_view plaintext, std::string& reference,
    std::string& error)
{
    reference.clear();
    error.clear();
    if (!ValidOwnerPart(packageId, MaximumOwnerBytes) ||
        !ValidOwnerPart(instanceId, MaximumOwnerBytes) ||
        !ValidOwnerPart(settingKey, MaximumSettingKeyBytes))
    {
        error = "widget secret owner is invalid";
        return false;
    }
    if (plaintext.empty() || plaintext.size() > MaximumSecretBytes)
    {
        error = "secret length is out of range";
        return false;
    }

    auto candidate = records_;
    auto existing = std::find_if(candidate.begin(), candidate.end(),
        [&](const auto& item) {
            const Record& record = item.second;
            return record.packageId == packageId &&
                record.instanceId == instanceId &&
                record.settingKey == settingKey;
        });
    Record record;
    if (existing != candidate.end())
        record = existing->second;
    else
    {
        if (candidate.size() >= MaximumRecords)
        {
            error = "widget secret record quota exceeded";
            return false;
        }
        do
        {
            if (!GenerateReference(record.reference, error)) return false;
        } while (candidate.contains(record.reference));
        record.packageId = std::string(packageId);
        record.instanceId = std::string(instanceId);
        record.settingKey = std::string(settingKey);
    }
    if (!Protect(record, plaintext, record.ciphertext, error))
        return false;
    if (existing != candidate.end())
        existing->second = record;
    else
        candidate.emplace(record.reference, record);
    if (!Save(candidate, error)) return false;
    records_.swap(candidate);
    reference = record.reference;
    return true;
}

bool WidgetSecretStore::Resolve(std::string_view packageId,
    std::string_view instanceId, std::string_view reference,
    std::string& plaintext, std::string& error) const
{
    plaintext.clear();
    error.clear();
    const auto found = records_.find(std::string(reference));
    if (found == records_.end() || found->second.packageId != packageId ||
        found->second.instanceId != instanceId)
    {
        error = "secret reference is unavailable for this widget instance";
        return false;
    }
    return Unprotect(found->second, plaintext, error);
}

bool WidgetSecretStore::Has(std::string_view packageId,
    std::string_view instanceId, std::string_view settingKey,
    std::string_view reference) const
{
    const auto found = records_.find(std::string(reference));
    return found != records_.end() &&
        found->second.packageId == packageId &&
        found->second.instanceId == instanceId &&
        found->second.settingKey == settingKey;
}

std::string WidgetSecretStore::Reference(std::string_view packageId,
    std::string_view instanceId, std::string_view settingKey) const
{
    const auto found = std::find_if(records_.begin(), records_.end(),
        [&](const auto& item) {
            const Record& record = item.second;
            return record.packageId == packageId &&
                record.instanceId == instanceId &&
                record.settingKey == settingKey;
        });
    return found == records_.end() ? std::string{} :
        found->second.reference;
}

bool WidgetSecretStore::RemoveSetting(std::string_view packageId,
    std::string_view instanceId, std::string_view settingKey,
    bool& removed, std::string& error)
{
    removed = false;
    error.clear();
    auto candidate = records_;
    const auto erased = std::erase_if(candidate, [&](const auto& item) {
        const Record& record = item.second;
        return record.packageId == packageId &&
            record.instanceId == instanceId &&
            record.settingKey == settingKey;
    });
    if (erased == 0) return true;
    if (!Save(candidate, error)) return false;
    records_.swap(candidate);
    removed = true;
    return true;
}

bool WidgetSecretStore::RemoveInstance(std::string_view instanceId,
    bool& removed, std::string& error)
{
    removed = false;
    error.clear();
    auto candidate = records_;
    const auto erased = std::erase_if(candidate, [&](const auto& item) {
        return item.second.instanceId == instanceId;
    });
    if (erased == 0) return true;
    if (!Save(candidate, error)) return false;
    records_.swap(candidate);
    removed = true;
    return true;
}
}
