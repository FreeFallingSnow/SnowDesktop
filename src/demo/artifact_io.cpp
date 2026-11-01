#include "artifact_io.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <fstream>

namespace snowdesktop::demo
{
std::string Utf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) throw std::runtime_error("demo_invalid_utf16");
    std::string result(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr))
        throw std::runtime_error("demo_invalid_utf16");
    return result;
}
std::wstring Wide(std::string_view value)
{
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) throw std::runtime_error("demo_invalid_utf8");
    std::wstring result(length, '\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length))
        throw std::runtime_error("demo_invalid_utf8");
    return result;
}
std::string Sha256(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("demo_hash_input_unavailable");
    struct Hash
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE value = nullptr;
        std::vector<UCHAR> object;
        ~Hash() { if (value) BCryptDestroyHash(value); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    } hash;
    DWORD size = 0, returned = 0;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(hash.algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &returned, 0) < 0)
        throw std::runtime_error("demo_hash_unavailable");
    hash.object.resize(size);
    if (BCryptCreateHash(hash.algorithm, &hash.value, hash.object.data(), size, nullptr, 0, 0) < 0) throw std::runtime_error("demo_hash_unavailable");
    std::array<char, 65536> bytes{};
    while (input)
    {
        input.read(bytes.data(), bytes.size());
        if (input.gcount() && BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(input.gcount()), 0) < 0)
            throw std::runtime_error("demo_hash_failed");
    }
    if (!input.eof()) throw std::runtime_error("demo_hash_read_failed");
    std::array<UCHAR, 32> result{};
    if (BCryptFinishHash(hash.value, result.data(), static_cast<ULONG>(result.size()), 0) < 0) throw std::runtime_error("demo_hash_failed");
    constexpr char hex[] = "0123456789abcdef"; std::string text;
    for (const auto byte : result) { text += hex[byte >> 4]; text += hex[byte & 15]; } return text;
}
control::Value ReadJson(const std::filesystem::path& file, std::size_t maximum)
{
    // Progress is atomically replaced by the worker while the host polls it.
    // Hold a stable file object while allowing that directory entry to change.
    struct Input { HANDLE value = INVALID_HANDLE_VALUE; ~Input() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } input;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        input.value = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (input.value != INVALID_HANDLE_VALUE) break;
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) break;
        Sleep(1); // Bounded accommodation for a legacy filesystem's replacement.
    }
    LARGE_INTEGER size{};
    if (input.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(input.value, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maximum)
        throw std::runtime_error("demo_json_unavailable_or_oversized");
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0'); std::size_t position = 0;
    while (position < text.size())
    {
        DWORD read = 0;
        if (!ReadFile(input.value, text.data() + position, static_cast<DWORD>(text.size() - position), &read, nullptr) || !read)
            throw std::runtime_error("demo_json_read_failed");
        position += read;
    }
    CloseHandle(input.value); input.value = INVALID_HANDLE_VALUE;
    control::Value result;
    if (!ParseJson(text, result)) throw std::runtime_error("demo_invalid_json"); return result;
}
void WriteJson(const std::filesystem::path& file, const control::Value& value)
{
    if (!file.is_absolute()) throw std::runtime_error("demo_snapshot_requires_absolute_path");
    std::filesystem::create_directories(file.parent_path());
    static std::atomic<std::uint64_t> sequence{0};
    const auto temporary = std::filesystem::path(file.wstring() + L".demo-tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence.fetch_add(1)));
    struct Output
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        std::filesystem::path temporary;
        bool owned = false, published = false;
        void Close() { if (value != INVALID_HANDLE_VALUE) { CloseHandle(value); value = INVALID_HANDLE_VALUE; } }
        ~Output() { Close(); if (owned && !published) DeleteFileW(temporary.c_str()); }
    } output;
    output.temporary = temporary;
    output.value = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    const auto fail = [&](const char* operation, DWORD error) {
        throw std::runtime_error("demo_json_write_failed: " + Utf8(file.filename().wstring()) + ": " + operation + ": " + std::to_string(error));
    };
    if (output.value == INVALID_HANDLE_VALUE) fail("create", GetLastError());
    output.owned = true;
    const auto text = control::Serialize(value); std::size_t position = 0;
    while (position < text.size())
    {
        DWORD written = 0;
        if (!WriteFile(output.value, text.data() + position, static_cast<DWORD>(text.size() - position), &written, nullptr) || !written)
            fail("write", GetLastError());
        position += written;
    }
    if (!FlushFileBuffers(output.value)) fail("flush", GetLastError());
    // POSIX replacement detaches an open old snapshot and binds new opens to
    // the new file. Legacy MoveFileEx cannot provide that guarantee, even when
    // readers permit delete-sharing. Only demo-owned outputs use this path.
    const auto name = file.native();
    const auto bytes = offsetof(FILE_RENAME_INFO, FileName) + name.size() * sizeof(wchar_t);
    std::vector<std::uint64_t> storage((bytes + sizeof(wchar_t) + 7) / 8);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->RootDirectory = nullptr; rename->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, name.data(), name.size() * sizeof(wchar_t));
    if (SetFileInformationByHandle(output.value, FileRenameInfoEx, rename, static_cast<DWORD>(bytes)))
    { output.published = true; return; }
    const auto error = GetLastError();
    if (error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_PARAMETER && error != ERROR_INVALID_FUNCTION)
        fail("rename-snapshot", error);
    output.Close();
    // Filesystems without the extended operation keep the replacement API and
    // bounded reader reopening above. An unsupported/failed write stays failed.
    if (ReplaceFileW(file.c_str(), temporary.c_str(), nullptr, 0, nullptr, nullptr)) { output.published = true; return; }
    const auto replaced = GetLastError();
    if (replaced == ERROR_FILE_NOT_FOUND && MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH))
    { output.published = true; return; }
    fail("replace-snapshot", replaced);
}
}
