#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <memory>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::deployment
{
// Bound the set of DLL paths presented to Windows, including across rebuilds.
// A slot is immutable while leased or mapped; inactive slots can be reused.
class InjectableRuntimeCache final
{
    struct Lease
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Lease() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
public:
    static constexpr unsigned kSlotCount = 4;
    DWORD LastError() const noexcept { return lastError_; }
    explicit InjectableRuntimeCache(std::filesystem::path root) : root_(std::move(root)) {}
    InjectableRuntimeCache(const InjectableRuntimeCache&) = delete;
    InjectableRuntimeCache& operator=(const InjectableRuntimeCache&) = delete;

    static std::wstring Digest(const std::filesystem::path& file)
    {
        Lease input;
        input.value = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) return {};
        struct Hash
        {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE value = nullptr;
            std::vector<UCHAR> object;
            ~Hash() { if (value) BCryptDestroyHash(value); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
        } hash;
        DWORD objectSize = 0, returned = 0;
        if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(hash.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                sizeof(objectSize), &returned, 0) < 0) return {};
        hash.object.resize(objectSize);
        if (BCryptCreateHash(hash.algorithm, &hash.value, hash.object.data(), objectSize, nullptr, 0, 0) < 0) return {};
        std::array<UCHAR, 32768> bytes{};
        DWORD read = 0;
        for (;;)
        {
            if (!ReadFile(input.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return {};
            if (!read) break;
            if (BCryptHashData(hash.value, bytes.data(), read, 0) < 0) return {};
        }
        std::array<UCHAR, 32> digest{};
        if (BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
        std::wstring result;
        constexpr wchar_t hex[] = L"0123456789abcdef";
        for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }

    std::filesystem::path Deploy(const std::filesystem::path& source,
        const std::filesystem::path& excluded = {})
    {
        std::error_code error;
        if (root_.empty() || Reparse(root_.parent_path()) || Reparse(root_) ||
            !std::filesystem::is_regular_file(source, error) || error)
        { return Fail(ERROR_FILE_NOT_FOUND); }
        const auto digest = Digest(source);
        if (digest.empty()) return Fail(GetLastError() ? GetLastError() : ERROR_INVALID_DATA);
        const auto pool = root_ / L"slots-v1";
        const auto directory = pool / source.filename();
        if (Reparse(pool) || Reparse(directory)) return Fail(ERROR_ACCESS_DENIED);
        std::filesystem::create_directories(directory, error);
        if (error || Reparse(root_) || Reparse(pool) || Reparse(directory))
        { return Fail(ERROR_ACCESS_DENIED); }

        // Serialize publication across processes, including the transition
        // from an exclusive writer lease to a shared host lease.
        Lease publisher;
        const auto publishLock = directory / L".publish.lock";
        if (Reparse(publishLock)) return Fail(ERROR_ACCESS_DENIED);
        const auto deadline = GetTickCount64() + 2000;
        do
        {
            publisher.value = CreateFileW(publishLock.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (publisher.value != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_SHARING_VIOLATION) return Fail(GetLastError());
            Sleep(10);
        } while (GetTickCount64() < deadline);
        if (publisher.value == INVALID_HANDLE_VALUE) return Fail(ERROR_BUSY);

        const auto retain = [&](const std::filesystem::path& target) -> std::filesystem::path {
            if (!leases_.contains(target))
            {
                auto lease = std::make_unique<Lease>();
                lease->value = CreateFileW((target.parent_path() / L".owner.lock").c_str(),
                    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
                if (lease->value == INVALID_HANDLE_VALUE) return Fail(GetLastError());
                leases_.emplace(target, std::move(lease));
            }
            lastError_ = ERROR_SUCCESS;
            SetLastError(ERROR_SUCCESS);
            return target;
        };
        for (unsigned slot = 0; slot < kSlotCount; ++slot)
        {
            const auto parent = directory / (L"slot-" + std::to_wstring(slot));
            const auto target = parent / source.filename();
            if (target == excluded || Reparse(parent) || Reparse(target) ||
                Reparse(parent / L".owner.lock")) continue;
            if (Digest(target) == digest) return retain(target);
        }
        for (unsigned slot = 0; slot < kSlotCount; ++slot)
        {
            const auto parent = directory / (L"slot-" + std::to_wstring(slot));
            const auto target = parent / source.filename();
            if (target == excluded || Reparse(parent) || Reparse(target) ||
                Reparse(parent / L".owner.lock")) continue;
            error.clear();
            std::filesystem::create_directories(parent, error);
            if (error || Reparse(parent)) continue;
            Lease writer;
            writer.value = CreateFileW((parent / L".owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (writer.value == INVALID_HANDLE_VALUE) continue;
            // CopyFile refuses to overwrite a mapped image, even after its
            // originating host has released the lease. Never rename/delete a
            // mapped target to bypass that protection.
            if (!CopyFileW(source.c_str(), target.c_str(), FALSE) || Digest(target) != digest) continue;
            CloseHandle(writer.value);
            writer.value = INVALID_HANDLE_VALUE;
            return retain(target);
        }
        return Fail(ERROR_BUSY);
    }

private:
    std::filesystem::path Fail(DWORD error) noexcept
    {
        lastError_ = error;
        SetLastError(error);
        return {};
    }
    static bool Reparse(const std::filesystem::path& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
    std::filesystem::path root_;
    DWORD lastError_ = ERROR_SUCCESS;
    std::map<std::filesystem::path, std::unique_ptr<Lease>> leases_;
};
}
