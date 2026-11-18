#include "widget_package_read.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

namespace snowdesktop::widget::detail
{
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string PathUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.generic_wstring());
}

bool ReadPackageFile(const std::filesystem::path& path, std::string& output,
    std::uint64_t maxBytes)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maxBytes) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    output.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    return file.good() || file.eof();
}

bool StartsWithPath(const std::filesystem::path& child,
    const std::filesystem::path& root)
{
    auto childIt = child.begin();
    auto rootIt = root.begin();
    for (; rootIt != root.end(); ++rootIt, ++childIt)
    {
        if (childIt == child.end() ||
            _wcsicmp(childIt->c_str(), rootIt->c_str()) != 0)
            return false;
    }
    return true;
}

bool HasReparsePoint(const std::filesystem::path& path)
{
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE)
    {
        FILE_ATTRIBUTE_TAG_INFO information{};
        const bool isReparsePoint = GetFileInformationByHandleEx(handle,
            FileAttributeTagInfo, &information, sizeof(information)) &&
            (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        CloseHandle(handle);
        if (isReparsePoint) return true;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

std::string HashPackageFile(
    const std::filesystem::path& path, bool allowConcurrentAccess)
{
    const DWORD sharing = allowConcurrentAccess
        ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : FILE_SHARE_READ;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, sharing,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD hashSize = 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
        nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &resultSize, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize),
            &resultSize, 0) < 0)
    {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        return {};
    }
    std::vector<UCHAR> object(objectSize);
    std::vector<UCHAR> digest(hashSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize,
        nullptr, 0, 0) < 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        return {};
    }
    std::array<UCHAR, 64 * 1024> buffer{};
    bool readSucceeded = true;
    for (;;)
    {
        DWORD count = 0;
        if (!::ReadFile(file, buffer.data(),
                static_cast<DWORD>(buffer.size()), &count, nullptr))
        {
            // Read errors (for example a disconnected device/pipe) are not
            // EOF. Never return the digest of a successfully read prefix.
            readSucceeded = false;
            break;
        }
        if (count == 0) break;
        if (BCryptHashData(hash, buffer.data(), count, 0) < 0)
        {
            readSucceeded = false;
            break;
        }
    }
    const bool ok = readSucceeded &&
        BCryptFinishHash(hash, digest.data(), hashSize, 0) >= 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok) return {};
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (UCHAR byte : digest) out << std::setw(2) << static_cast<int>(byte);
    return out.str();
}
}
