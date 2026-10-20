#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::deployment
{
// Reuse DLL paths across rebuilds. A slot is immutable while leased or mapped;
// the pool expands only when every existing path is still unavailable.
class InjectableRuntimeCache final
{
    struct Lease
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~Lease() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
public:
    static constexpr unsigned kInitialSlotCount = 4;
    DWORD LastError() const noexcept { return lastError_; }
    std::size_t CandidateSlotCount() const noexcept { return slotCount_; }
    bool Expanded() const noexcept { return expanded_; }
    explicit InjectableRuntimeCache(std::filesystem::path root) : root_(std::move(root)) {}
    InjectableRuntimeCache(const InjectableRuntimeCache&) = delete;
    InjectableRuntimeCache& operator=(const InjectableRuntimeCache&) = delete;

    static std::filesystem::path ResidentModulePath(const std::filesystem::path& root,
        const wchar_t* filename, DWORD process, std::uintptr_t module)
    {
        if (root.empty() || !filename || !process || !module) return {};
        const auto directory = root / L"slots-v1" / filename;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, process);
        if (snapshot == INVALID_HANDLE_VALUE) return {};
        std::filesystem::path path;
        MODULEENTRY32W entry{sizeof(entry)};
        if (Module32FirstW(snapshot, &entry))
        {
            do
            {
                if (reinterpret_cast<std::uintptr_t>(entry.hModule) != module) continue;
                std::uint32_t slot = 0;
                const std::filesystem::path image(entry.szExePath);
                if (SlotNumber(image.parent_path().filename().wstring(), slot))
                {
                    const auto candidate = directory / (L"slot-" + std::to_wstring(slot)) / filename;
                    if (_wcsicmp(entry.szExePath, candidate.c_str()) == 0) path = entry.szExePath;
                }
                break;
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return path;
    }

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
        slotCount_ = 0;
        expanded_ = false;
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

        std::vector<std::uint32_t> slots;
        if (!ListSlots(directory, slots, error)) return Fail(static_cast<DWORD>(error.value()));
        slotCount_ = slots.size();

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
        for (const auto slot : slots)
        {
            const auto parent = directory / (L"slot-" + std::to_wstring(slot));
            const auto target = parent / source.filename();
            if (target == excluded || Reparse(parent) || Reparse(target) ||
                Reparse(parent / L".owner.lock")) continue;
            if (Digest(target) == digest) return retain(target);
        }
        DWORD publicationError = ERROR_SUCCESS;
        const auto publish = [&](std::uint32_t slot) -> std::filesystem::path
        {
            const auto parent = directory / (L"slot-" + std::to_wstring(slot));
            const auto target = parent / source.filename();
            if (target == excluded || Reparse(parent) || Reparse(target) ||
                Reparse(parent / L".owner.lock")) return {};
            error.clear();
            std::filesystem::create_directories(parent, error);
            if (error) { publicationError = static_cast<DWORD>(error.value()); return {}; }
            if (Reparse(parent)) return {};
            Lease writer;
            writer.value = CreateFileW((parent / L".owner.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (writer.value == INVALID_HANDLE_VALUE)
            {
                const DWORD code = GetLastError();
                if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION)
                    publicationError = code;
                return {};
            }
            // CopyFile refuses to overwrite a mapped image, even after its
            // originating host has released the lease. Never rename/delete a
            // mapped target to bypass that protection.
            if (!CopyFileW(source.c_str(), target.c_str(), FALSE))
            {
                const DWORD code = GetLastError();
                // Access denied on an existing image is also how Windows
                // refuses to overwrite a mapped DLL. Missing destinations,
                // disk failures and source errors must not grow empty slots.
                if (code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION &&
                    !(code == ERROR_ACCESS_DENIED && std::filesystem::is_regular_file(target, error)))
                    publicationError = code;
                return {};
            }
            if (Digest(target) != digest) { publicationError = ERROR_INVALID_DATA; return {}; }
            CloseHandle(writer.value);
            writer.value = INVALID_HANDLE_VALUE;
            const auto retained = retain(target);
            if (retained.empty()) publicationError = LastError();
            return retained;
        };
        for (const auto slot : slots)
        {
            const auto target = publish(slot);
            if (!target.empty()) return target;
            if (publicationError) return Fail(publicationError);
        }
        // Four is the initial pool size, not a failure threshold. Allocate the
        // first unused canonical number under the same publication lease. A
        // later host sees it and reuses its content or its released path.
        std::uint32_t next = 0;
        for (const auto slot : slots)
        {
            if (slot != next) break;
            if (next == (std::numeric_limits<std::uint32_t>::max)())
                return Fail(ERROR_ARITHMETIC_OVERFLOW);
            ++next;
        }
        ++slotCount_;
        const auto expanded = publish(next);
        if (!expanded.empty()) { expanded_ = true; return expanded; }
        return Fail(publicationError ? publicationError : ERROR_BUSY);
    }

private:
    static bool SlotNumber(const std::wstring& name, std::uint32_t& result) noexcept
    {
        constexpr std::wstring_view prefix = L"slot-";
        if (!name.starts_with(prefix) || name.size() == prefix.size() ||
            (name.size() > prefix.size() + 1 && name[prefix.size()] == L'0')) return false;
        std::uint32_t number = 0;
        for (std::size_t i = prefix.size(); i < name.size(); ++i)
        {
            const wchar_t digit = name[i];
            if (digit < L'0' || digit > L'9' || number >
                ((std::numeric_limits<std::uint32_t>::max)() - static_cast<unsigned>(digit - L'0')) / 10)
                return false;
            number = number * 10 + static_cast<unsigned>(digit - L'0');
        }
        result = number;
        return true;
    }
    static bool ListSlots(const std::filesystem::path& directory,
        std::vector<std::uint32_t>& slots, std::error_code& error)
    {
        for (unsigned i = 0; i < kInitialSlotCount; ++i) slots.push_back(i);
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) return false;
        const std::filesystem::directory_iterator end;
        for (; iterator != end; iterator.increment(error))
        {
            if (error) return false;
            std::uint32_t number = 0;
            if (SlotNumber(iterator->path().filename().wstring(), number)) slots.push_back(number);
        }
        if (error) return false;
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        return true;
    }
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
    std::size_t slotCount_ = 0;
    bool expanded_ = false;
    std::map<std::filesystem::path, std::unique_ptr<Lease>> leases_;
};
}
