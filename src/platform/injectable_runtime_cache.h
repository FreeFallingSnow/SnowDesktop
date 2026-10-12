#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace snowdesktop::deployment
{
// Windows registers hook DLL paths in a shared, bounded table. Reusing an
// immutable copy prevents each host restart from consuming another path while
// recovery callbacks keep the previous DLL loaded in other applications.
class InjectableRuntimeCache final
{
    struct Lease
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Lease() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
public:
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

    std::filesystem::path Deploy(const std::filesystem::path& source)
    {
        std::error_code error;
        if (root_.empty() || Reparse(root_.parent_path()) || Reparse(root_) ||
            !std::filesystem::is_regular_file(source, error) || error) return {};
        const auto digest = Digest(source);
        if (digest.empty()) return {};
        const auto directory = root_ / digest;
        std::filesystem::create_directories(directory, error);
        if (error || Reparse(root_) || Reparse(directory)) return {};
        auto lease = std::make_unique<Lease>();
        // All hosts may hold read leases on the same file. A DELETE probe
        // fails until the final host releases it; startup cleanup cannot remove
        // a copy between another host selecting and loading it.
        const auto ownerLock = directory / L".owner.lock";
        if (Reparse(ownerLock)) return {};
        lease->value = CreateFileW(ownerLock.c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (lease->value == INVALID_HANDLE_VALUE) return {};
        const auto target = directory / source.filename();
        if (Reparse(target)) return {};
        if (!std::filesystem::exists(target, error) && !error)
        {
            static std::atomic<unsigned> serial{0};
            const auto temporary = directory / (source.filename().wstring() + L"." +
                std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                std::to_wstring(++serial) + L".tmp");
            if (!CopyFileW(source.c_str(), temporary.c_str(), TRUE)) return {};
            const bool matching = Digest(temporary) == digest;
            // Never replace an existing DLL: another process may have mapped
            // it already. Concurrent publishers accept only the matching copy.
            const bool published = matching && MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
            if (!published) DeleteFileW(temporary.c_str());
        }
        if (error || Digest(target) != digest) return {};
        leases_.push_back(std::move(lease));
        return target;
    }

private:
    static bool Reparse(const std::filesystem::path& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
    std::filesystem::path root_;
    std::vector<std::unique_ptr<Lease>> leases_;
};
}
