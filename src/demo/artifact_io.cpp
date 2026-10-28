#include "artifact_io.h"
#include "data/atomic_file.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
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
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || static_cast<std::uint64_t>(input.tellg()) > maximum)
        throw std::runtime_error("demo_json_unavailable_or_oversized");
    std::string text(static_cast<std::size_t>(input.tellg()), '\0'); input.seekg(0);
    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) throw std::runtime_error("demo_json_read_failed");
    control::Value result;
    if (!ParseJson(text, result)) throw std::runtime_error("demo_invalid_json"); return result;
}
void WriteJson(const std::filesystem::path& file, const control::Value& value)
{
    if (!atomic_file::WriteAll(file, control::Serialize(value))) throw std::runtime_error("demo_json_write_failed");
}
}
