#include "widget_package.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
using detail::Lower;
using detail::PathUtf8;
using detail::ReadPackageFile;
using detail::Utf8ToWide;

std::uint32_t Crc32(const unsigned char* data, std::size_t size)
{
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void Write16(std::ostream& output, std::uint16_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    output.write(bytes, 2);
}

void Write32(std::ostream& output, std::uint32_t value)
{
    const char bytes[] = {
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    output.write(bytes, 4);
}

std::uint16_t Read16(const unsigned char* data)
{
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t Read32(const unsigned char* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

struct ZipItem
{
    std::string path;
    std::string data;
    std::uint32_t crc = 0;
    std::uint32_t offset = 0;
};

bool WriteStoreZip(const std::filesystem::path& root,
    const std::filesystem::path& output, std::string& error,
    std::string& sha256)
{
    std::vector<ZipItem> items;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        ZipItem item;
        item.path = PathUtf8(std::filesystem::relative(it->path(), root, ec));
        if (ec || item.path.empty())
        {
            error = "cannot create archive-relative path";
            return false;
        }
        std::replace(item.path.begin(), item.path.end(), '\\', '/');
        if (!ReadPackageFile(it->path(), item.data, kMaxExtractedBytes))
        {
            error = "cannot read package file: " + item.path;
            return false;
        }
        item.crc = Crc32(reinterpret_cast<const unsigned char*>(item.data.data()),
            item.data.size());
        items.push_back(std::move(item));
    }
    if (ec)
    {
        error = "cannot enumerate package: " + ec.message();
        return false;
    }
    std::sort(items.begin(), items.end(),
        [](const ZipItem& a, const ZipItem& b) { return a.path < b.path; });

    const auto absoluteOutput = std::filesystem::absolute(output, ec).lexically_normal();
    if (ec)
    {
        error = "cannot resolve archive output: " + ec.message();
        return false;
    }
    std::filesystem::create_directories(absoluteOutput.parent_path(), ec);
    if (ec)
    {
        error = "cannot create archive directory: " + ec.message();
        return false;
    }
    const auto temporary = absoluteOutput.wstring() + L".tmp-" +
        Utf8ToWide(WidgetPackageManager::GenerateUuid());
    struct TemporaryArchive
    {
        std::filesystem::path path;
        ~TemporaryArchive() noexcept
        {
            try { if (!path.empty()) { std::error_code ignored; std::filesystem::remove(path, ignored); } }
            catch (...) {}
        }
    } cleanup{temporary};
    std::ofstream zip(temporary, std::ios::binary | std::ios::trunc);
    if (!zip)
    {
        error = "cannot create archive";
        return false;
    }
    for (auto& item : items)
    {
        const auto position = zip.tellp();
        if (position < 0 || position > std::numeric_limits<std::uint32_t>::max() ||
            item.data.size() > std::numeric_limits<std::uint32_t>::max() ||
            item.path.size() > std::numeric_limits<std::uint16_t>::max())
        {
            error = "package is too large for ZIP32";
            return false;
        }
        item.offset = static_cast<std::uint32_t>(position);
        Write32(zip, 0x04034b50);
        Write16(zip, 20);
        Write16(zip, 0x0800);
        Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0);
        Write32(zip, item.crc);
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write16(zip, static_cast<std::uint16_t>(item.path.size()));
        Write16(zip, 0);
        zip.write(item.path.data(), static_cast<std::streamsize>(item.path.size()));
        zip.write(item.data.data(), static_cast<std::streamsize>(item.data.size()));
    }
    const auto centralOffsetValue = zip.tellp();
    if (centralOffsetValue < 0 ||
        centralOffsetValue > std::numeric_limits<std::uint32_t>::max())
    {
        error = "package is too large for ZIP32";
        return false;
    }
    const auto centralOffset = static_cast<std::uint32_t>(centralOffsetValue);
    for (const auto& item : items)
    {
        Write32(zip, 0x02014b50);
        Write16(zip, 20); Write16(zip, 20);
        Write16(zip, 0x0800); Write16(zip, 0);
        Write16(zip, 0); Write16(zip, 0);
        Write32(zip, item.crc);
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write32(zip, static_cast<std::uint32_t>(item.data.size()));
        Write16(zip, static_cast<std::uint16_t>(item.path.size()));
        Write16(zip, 0); Write16(zip, 0); Write16(zip, 0); Write16(zip, 0);
        Write32(zip, 0);
        Write32(zip, item.offset);
        zip.write(item.path.data(), static_cast<std::streamsize>(item.path.size()));
    }
    const auto endValue = zip.tellp();
    if (endValue < 0 || endValue > std::numeric_limits<std::uint32_t>::max() ||
        items.size() > std::numeric_limits<std::uint16_t>::max())
    {
        error = "package is too large for ZIP32";
        return false;
    }
    const auto centralSize = static_cast<std::uint32_t>(endValue) - centralOffset;
    Write32(zip, 0x06054b50);
    Write16(zip, 0); Write16(zip, 0);
    Write16(zip, static_cast<std::uint16_t>(items.size()));
    Write16(zip, static_cast<std::uint16_t>(items.size()));
    Write32(zip, centralSize); Write32(zip, centralOffset); Write16(zip, 0);
    zip.flush();
    if (!zip)
    {
        error = "cannot finish archive";
        zip.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    zip.close();
    const auto bytes = std::filesystem::file_size(temporary, ec);
    if (!zip || ec || bytes > kMaxArchiveBytes)
    {
        error = !zip ? "cannot close archive" : ec ? "cannot measure archive" :
            "exported package exceeds 20 MiB";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    const auto digest = WidgetPackageManager::Sha256File(temporary);
    if (digest.empty())
    {
        error = "cannot hash archive";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), absoluteOutput.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = "cannot atomically publish archive";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    sha256 = digest;
    cleanup.path.clear();
    return true;
}
}

bool WidgetPackageManager::ExtractArchive(
    const std::filesystem::path& archive,
    const std::filesystem::path& destination, ValidationReport& report,
    std::string& error, bool* malformedArchive) const
{
    const auto malformed = [&](std::string message) {
        if (malformedArchive) *malformedArchive = true;
        error = std::move(message);
        return false;
    };
    std::string data;
    if (!ReadPackageFile(archive, data, kMaxArchiveBytes))
    {
        error = "cannot read package archive";
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t position = 0;
    std::set<std::string> names;
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    while (position + 4 <= data.size() && Read32(bytes + position) == 0x04034b50)
    {
        if (position + 30 > data.size())
            return malformed("truncated ZIP local header");
        const std::uint16_t flags = Read16(bytes + position + 6);
        const std::uint16_t method = Read16(bytes + position + 8);
        const std::uint32_t expectedCrc = Read32(bytes + position + 14);
        const std::uint32_t compressedSize = Read32(bytes + position + 18);
        const std::uint32_t uncompressedSize = Read32(bytes + position + 22);
        const std::uint16_t nameSize = Read16(bytes + position + 26);
        const std::uint16_t extraSize = Read16(bytes + position + 28);
        if ((flags & 0x0001) || (flags & 0x0008) || method != 0)
            return malformed("v1 accepts unencrypted ZIP entries using the store method");
        const std::size_t payload = position + 30 + nameSize + extraSize;
        if (payload > data.size() ||
            compressedSize > data.size() - payload ||
            compressedSize != uncompressedSize)
            return malformed("invalid ZIP entry bounds");
        const std::string name(data.data() + position + 30, nameSize);
        if (name.find('\0') != std::string::npos || name.ends_with('/'))
        {
            position = payload + compressedSize;
            continue;
        }
        const std::filesystem::path relative(Utf8ToWide(name));
        const std::string canonicalName =
            Lower(PathUtf8(relative.lexically_normal()));
        if (!WidgetPackageValidator::IsSafeRelativePath(relative) ||
            canonicalName.empty() || !names.insert(canonicalName).second)
            return malformed("archive contains an unsafe or duplicate path: " + name);
        ++report.fileCount;
        report.totalBytes += uncompressedSize;
        if (report.fileCount > kMaxPackageFiles ||
            report.totalBytes > kMaxExtractedBytes)
            return malformed("archive exceeds extraction quotas");
        const auto target = destination / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec)
        {
            error = "cannot create extracted directory: " + ec.message();
            return false;
        }
        const auto* payloadBytes = bytes + payload;
        if (Crc32(payloadBytes, uncompressedSize) != expectedCrc)
            return malformed("archive entry CRC mismatch: " + name);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(payloadBytes),
            static_cast<std::streamsize>(uncompressedSize));
        if (!file)
        {
            error = "cannot extract archive entry: " + name;
            return false;
        }
        position = payload + compressedSize;
    }
    if (report.fileCount == 0)
        return malformed("archive contains no files");
    return true;
}

bool WidgetPackageManager::ExportArchive(const std::string& packageId,
    const std::filesystem::path& output, PackageArtifact& artifact,
    ValidationReport& report, std::string& error) const
{
    const auto package = Resolve(packageId);
    if (!package)
    {
        error = "package is not installed or is disabled";
        return false;
    }
    report = validator_.ValidateDirectory(package->root);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (Lower(output.extension().string()) != ".snowwidget")
    {
        error = "output must use the .snowwidget extension";
        return false;
    }
    std::string digest;
    if (!WriteStoreZip(package->root, output, error, digest)) return false;
    artifact.localPath = output;
    artifact.packageId = package->manifest.id;
    artifact.version = package->manifest.version;
    artifact.sha256 = std::move(digest);
    return true;
}

bool WidgetPackageManager::ExportDirectory(
    const std::filesystem::path& source,
    const std::filesystem::path& output, PackageArtifact& artifact,
    ValidationReport& report, std::string& error) const
{
    PackageManifest manifest;
    report = validator_.ValidateDirectory(source, &manifest);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (Lower(output.extension().string()) != ".snowwidget")
    {
        error = "output must use the .snowwidget extension";
        return false;
    }
    std::string digest;
    if (!WriteStoreZip(source, output, error, digest)) return false;
    artifact.localPath = output;
    artifact.packageId = manifest.id;
    artifact.version = manifest.version;
    artifact.sha256 = std::move(digest);
    return true;
}
}
