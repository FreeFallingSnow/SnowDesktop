#pragma once
#include "dock_app_identity_rules.h"
#include <windows.h>
#include <filesystem>
#include <vector>

namespace snowdesktop::dock_app_identity_rules
{
struct ExecutableProduct
{
    std::wstring company, product;
};
inline std::wstring ProductWithoutLauncher(std::wstring value)
{
    // A publisher's stable launcher and payload often name the same product
    // with a " Launcher" suffix. This is product metadata, never a brand table.
    constexpr std::wstring_view suffix = L" LAUNCHER";
    if (value.ends_with(suffix)) value.resize(value.size() - suffix.size());
    return value;
}
inline bool MatchesInstalledPayload(const std::wstring& launcher, const ExecutableProduct& source,
    const std::wstring& payload, const ExecutableProduct& target)
{
    const auto split = launcher.find_last_of(L'\\');
    if (split == std::wstring::npos || split <= 2 || source.company.empty() || source.product.empty() ||
        ProductWithoutLauncher(source.product) == source.product ||
        source.company != target.company || ProductWithoutLauncher(source.product) != ProductWithoutLauncher(target.product)) return false;
    const auto root = launcher.substr(0, split);
    if (!IsPathInsideDirectory(payload, root)) return false;
    const auto relative = payload.substr(root.size() + 1);
    const auto directoryEnd = relative.find(L'\\');
    return directoryEnd != std::wstring::npos && directoryEnd > 0 &&
        relative.substr(directoryEnd + 1) == launcher.substr(split + 1);
}
inline ExecutableProduct ReadExecutableProduct(const std::wstring& path)
{
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size || size > 1024 * 1024) return {};
    std::vector<BYTE> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
    struct Translation { WORD language, codepage; };
    Translation* translations = nullptr; UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length)) return {};
    for (UINT index = 0; index < length / sizeof(Translation); ++index)
    {
        const auto field = [&](const wchar_t* name) {
            wchar_t key[96]{};
            swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", translations[index].language, translations[index].codepage, name);
            wchar_t* text = nullptr; UINT count = 0;
            std::wstring value;
            if (VerQueryValueW(bytes.data(), key, reinterpret_cast<void**>(&text), &count) && text && count)
                value.assign(text, wcsnlen_s(text, count));
            if (!value.empty()) CharUpperBuffW(value.data(), static_cast<DWORD>(value.size()));
            return value;
        };
        ExecutableProduct product{field(L"CompanyName"), field(L"ProductName")};
        if (!product.company.empty() && !product.product.empty()) return product;
    }
    return {};
}
// Called only by the existing background Shell identity worker. Restrict the
// search to direct install children and reject junctions. Multiple installed
// versions are validated aliases, never a guess at the active launch target.
inline std::vector<std::wstring> ResolveInstalledExecutables(const std::wstring& launcher)
{
    const auto source = ReadExecutableProduct(launcher);
    if (source.company.empty() || source.product.empty() || ProductWithoutLauncher(source.product) == source.product) return {};
    const std::filesystem::path file(launcher), root = file.parent_path();
    if (root.empty() || root == root.root_path()) return {};
    std::error_code error;
    std::filesystem::directory_iterator directories(root, error);
    std::vector<std::wstring> matches;
    unsigned visited = 0;
    for (const auto end = std::filesystem::directory_iterator{}; !error && directories != end; directories.increment(error))
    {
        if (++visited > 128) return {};
        const auto child = directories->path();
        const DWORD attributes = GetFileAttributesW(child.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        auto payload = (child / file.filename()).wstring();
        const DWORD payloadAttributes = GetFileAttributesW(payload.c_str());
        if (payloadAttributes == INVALID_FILE_ATTRIBUTES ||
            (payloadAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        CharUpperBuffW(payload.data(), static_cast<DWORD>(payload.size()));
        if (!MatchesInstalledPayload(launcher, source, payload, ReadExecutableProduct(payload))) continue;
        matches.push_back(std::move(payload));
    }
    return !error ? matches : std::vector<std::wstring>{};
}
}
