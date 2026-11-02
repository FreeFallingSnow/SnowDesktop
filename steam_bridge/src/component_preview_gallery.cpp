// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT
#include "component_preview_gallery.h"

#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cwctype>
#include <fstream>
#include <iterator>

namespace snowdesktop::steam_bridge
{
namespace
{
std::string ImageHash(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size >= 1024 * 1024) return {};
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
        file.peek() != std::char_traits<char>::eof()) return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    struct Cleanup
    {
        BCRYPT_ALG_HANDLE algorithm; BCRYPT_HASH_HANDLE& hash;
        ~Cleanup() { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); }
    } cleanup{algorithm, hash};
    DWORD storageSize = 0, copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&storageSize), sizeof(storageSize), &copied, 0) < 0) return {};
    std::vector<unsigned char> storage(storageSize);
    std::array<unsigned char, 32> result{};
    if (BCryptCreateHash(algorithm, &hash, storage.data(), storageSize, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : result) { text += digits[byte >> 4]; text += digits[byte & 15]; }
    return text;
}
}

PreparedComponentPreviews::~PreparedComponentPreviews()
{
    std::error_code ignored;
    for (const auto& path : ownedFiles_) std::filesystem::remove(path, ignored);
    if (!directory_.empty()) std::filesystem::remove(directory_, ignored);
}

bool PreparedComponentPreviews::Matches(const std::filesystem::path& primaryPath,
    const std::vector<std::filesystem::path>& galleryPaths) const
{
    if (primary && ImageHash(primaryPath) != primary->sha256) return false;
    if (galleryPaths.size() != (gallery ? gallery->size() : 0)) return false;
    for (std::size_t index = 0; index < galleryPaths.size(); ++index)
        if (ImageHash(galleryPaths[index]) != (*gallery)[index].sha256) return false;
    return true;
}

std::shared_ptr<PreparedComponentPreviews> FreezeComponentPreviews(
    const std::optional<std::filesystem::path>& primary,
    const std::optional<std::vector<std::filesystem::path>>& gallery,
    const std::filesystem::path& artifactDirectory, std::string_view packageId,
    std::string& error)
{
    auto frozen = std::shared_ptr<PreparedComponentPreviews>(new PreparedComponentPreviews);
    std::error_code ec;
    const auto directory = artifactDirectory / L"component-previews";
    const bool safeIdentity = packageId.size() == 36 && std::all_of(packageId.begin(), packageId.end(),
        [](char ch) { return ch == '-' || (ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'); });
    if (!safeIdentity || artifactDirectory.empty() || (gallery && gallery->size() > 7) ||
        !std::filesystem::create_directory(directory, ec) || ec)
    { error = "Cannot create the component preview preparation directory"; return {}; }
    frozen->directory_ = directory;
    frozen->ownedFiles_.reserve(8);
    const auto copy = [&](const std::filesystem::path& source, const std::string& stem,
                          ComponentPreviewImage& image) {
        auto extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
        if (extension != L".png" && extension != L".jpg" && extension != L".jpeg" && extension != L".gif") return false;
        const auto hash = ImageHash(source);
        if (hash.empty()) return false;
        const auto destination = directory / (std::filesystem::path(stem + hash).wstring() + extension);
        if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec) || ec) return false;
        frozen->ownedFiles_.push_back(destination);
        if (ImageHash(destination) != hash) return false;
        image = {destination, hash, source};
        return true;
    };
    if (primary)
    {
        frozen->primary.emplace();
        if (!copy(*primary, "cover-", *frozen->primary))
        { error = "Primary preview must be a readable PNG/JPG/GIF smaller than 1 MiB"; return {}; }
    }
    if (gallery)
    {
        frozen->gallery.emplace();
        for (std::size_t index = 0; index < gallery->size(); ++index)
        {
            ComponentPreviewImage image;
            if (!copy((*gallery)[index], ComponentGalleryPrefix(packageId) + std::to_string(index) + "-", image))
            { error = "Gallery image changed or is not a readable PNG/JPG/GIF smaller than 1 MiB"; return {}; }
            frozen->gallery->push_back(std::move(image));
        }
    }
    return frozen;
}
}
