#include "app_font.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <set>
#include <system_error>
#include <cwctype>
#include <map>

namespace snowdesktop::app_fonts
{
namespace
{
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
bool FontFile(const fs::path& path)
{
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
    return extension == L".ttf" || extension == L".otf" || extension == L".ttc";
}
bool SafePart(std::wstring_view part)
{
    return !part.empty() && part != L"." && part != L".." &&
        part.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") == std::wstring_view::npos;
}
struct Package
{
    ComPtr<IDWriteFactory6> factory;
    ComPtr<IDWriteFontCollection1> collection;
    std::vector<Choice> choices;
    std::vector<fs::path> files;
};
std::wstring LocalizedName(IDWriteLocalizedStrings* names, UINT32 index = 0)
{
    UINT32 length = 0;
    if (!names || FAILED(names->GetStringLength(index, &length))) return {};
    std::wstring name(length + 1, L'\0');
    if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
    name.resize(length);
    return name;
}
std::string Utf8(std::wstring_view value)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
Package Load(std::string_view id, const fs::path& folder, bool builtin)
{
    Package result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec) && FontFile(entry.path())) result.files.push_back(entry.path());
    if (ec || result.files.empty() || result.files.size() > 64) return {};
    std::sort(result.files.begin(), result.files.end());
    ComPtr<IDWriteFactory6> factory;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory6), reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) return {};
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory->CreateFontSetBuilder(&builder))) return {};
    std::map<std::string, int> preferredFace;
    for (const auto& file : result.files)
    {
        ComPtr<IDWriteFontFile> reference;
        if (FAILED(factory->CreateFontFileReference(file.c_str(), nullptr, &reference)) || FAILED(builder->AddFontFile(reference.Get()))) return {};
        ComPtr<IDWriteFontSet> single;
        ComPtr<IDWriteFontSetBuilder1> singleBuilder;
        if (FAILED(factory->CreateFontSetBuilder(&singleBuilder)) || FAILED(singleBuilder->AddFontFile(reference.Get())) || FAILED(singleBuilder->CreateFontSet(&single))) return {};
        ComPtr<IDWriteFontCollection2> collection;
        if (FAILED(factory->CreateFontCollectionFromFontSet(single.Get(), DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &collection))) return {};
        for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i)
        {
            ComPtr<IDWriteFontFamily2> family;
            ComPtr<IDWriteLocalizedStrings> names;
            collection->GetFontFamily(i, &family);
            if (!family || FAILED(family->GetFamilyNames(&names))) continue;
            const auto name = LocalizedName(names.Get());
            const auto key = Utf8(name);
            if (name.empty()) continue;
            ComPtr<IDWriteFont> face;
            family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &face);
            const int distance = face ? std::abs(static_cast<int>(face->GetWeight()) - 400) : 1000;
            const auto found = std::find_if(result.choices.begin(), result.choices.end(), [&](const Choice& c) { return c.selection.family == key; });
            if (found != result.choices.end() && preferredFace[key] <= distance) continue;
            const std::wstring prefix = builtin ? L"ms-appx:///Assets/Fonts/" : L"ms-appx:///SnowDesktopUserFonts/";
            const std::wstring wideId(id.begin(), id.end());
            // Use the original file URI so unpackaged and MSIX WinUI can both resolve it.
            const Choice choice{{std::string(id), key}, name,
                prefix + wideId + L"/" + file.filename().wstring() + L"#" + name};
            if (found == result.choices.end()) result.choices.push_back(choice);
            else *found = choice;
            preferredFace[key] = distance;
        }
    }
    ComPtr<IDWriteFontSet> fonts;
    ComPtr<IDWriteFontCollection2> collection;
    if (FAILED(builder->CreateFontSet(&fonts)) || FAILED(factory->CreateFontCollectionFromFontSet(fonts.Get(), DWRITE_FONT_FAMILY_MODEL_TYPOGRAPHIC, &collection))) return {};
    result.factory = factory;
    result.collection = collection;
    return result;
}

Package LoadInstalled()
{
    Package result;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory6),
            reinterpret_cast<IUnknown**>(result.factory.GetAddressOf())))) return {};
    // Exclude cloud fonts: choosing a local family must not trigger downloads.
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(result.factory->GetSystemFontCollection(FALSE, &collection, TRUE))) return {};
    result.collection = collection;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i)
    {
        ComPtr<IDWriteFontFamily> family;
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(collection->GetFontFamily(i, &family)) || !family ||
            family->GetFontCount() == 0 || FAILED(family->GetFamilyNames(&names))) continue;
        UINT32 canonicalIndex = 0, displayIndex = 0;
        BOOL exists = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &canonicalIndex, &exists)) || !exists) canonicalIndex = 0;
        exists = FALSE;
        if (!locale[0] || FAILED(names->FindLocaleName(locale, &displayIndex, &exists)) || !exists) displayIndex = canonicalIndex;
        const auto canonical = LocalizedName(names.Get(), canonicalIndex);
        const auto display = LocalizedName(names.Get(), displayIndex);
        if (!canonical.empty()) result.choices.push_back({{"installed", Utf8(canonical)},
            display.empty() ? canonical : display, canonical});
    }
    std::sort(result.choices.begin(), result.choices.end(), [](const Choice& a, const Choice& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return result;
}
}

std::vector<Choice> List(const fs::path& assets, const fs::path& data)
{
    std::vector<Choice> choices{{{}, L"System", L"Segoe UI Variable, Segoe UI"}};
    for (const auto* id : {"MiSans", "HarmonyOS-Sans"})
    {
        auto package = Load(id, assets / L"Fonts" / id, true);
        choices.insert(choices.end(), package.choices.begin(), package.choices.end());
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(data / L"fonts", ec))
    {
        if (!entry.is_directory(ec) || !SafePart(entry.path().filename().wstring())) continue;
        auto package = Load(entry.path().filename().string(), entry.path(), false);
        choices.insert(choices.end(), package.choices.begin(), package.choices.end());
    }
    const auto installed = LoadInstalled();
    choices.insert(choices.end(), installed.choices.begin(), installed.choices.end());
    return choices;
}

bool Select(const Selection& selection, const fs::path& assets, const fs::path& data)
{
    const auto applied = current.load();
    if (applied && applied->selection == selection) return true;
    if (selection.package == "system") { current.store(nullptr); ++revision; return true; }
    const fs::path id(selection.package);
    if (!SafePart(id.wstring())) return false;
    const bool installed = selection.package == "installed";
    const bool builtin = selection.package == "MiSans" || selection.package == "HarmonyOS-Sans";
    auto package = installed ? LoadInstalled()
        : Load(selection.package, (builtin ? assets / L"Fonts" : data / L"fonts") / id, builtin);
    const auto choice = std::find_if(package.choices.begin(), package.choices.end(), [&](const Choice& c) { return c.selection == selection; });
    if (choice == package.choices.end()) return false;
    auto resource = std::make_shared<Resource>();
    resource->selection = selection;
    resource->family = installed ? choice->xamlSource : choice->name;
    resource->xamlSource = choice->xamlSource;
    resource->factory = package.factory;
    resource->collection = package.collection;
    for (const auto& file : package.files) AddFontResourceExW(file.c_str(), FR_PRIVATE, nullptr);
    current.store(std::move(resource));
    ++revision;
    return true;
}

bool Import(const fs::path& source, const fs::path& data, std::vector<Choice>& choices, std::string& error)
{
    error.clear(); choices.clear();
    try
    {
        std::vector<fs::path> files, notices;
        std::uintmax_t bytes = 0;
        const auto add = [&](const fs::path& file) {
            if (FontFile(file)) { files.push_back(file); bytes += fs::file_size(file); }
            else {
                auto name = file.filename().wstring();
                std::transform(name.begin(), name.end(), name.begin(), towlower);
                if (name.starts_with(L"license") || name.starts_with(L"copyright") || name.starts_with(L"readme")) notices.push_back(file);
            }
        };
        if (fs::is_directory(source))
        {
            for (auto iterator = fs::recursive_directory_iterator(source); iterator != fs::recursive_directory_iterator{}; ++iterator)
            {
                const auto& entry = *iterator;
                if ((GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0) { iterator.disable_recursion_pending(); continue; }
                if (entry.is_regular_file()) add(entry.path());
                if (files.size() > 64 || bytes > 128 * 1024 * 1024) { error = "font.packageTooLarge"; return false; }
            }
        }
        else if (fs::is_regular_file(source)) add(source);
        if (files.empty() || bytes > 128 * 1024 * 1024) { error = "font.invalidPackage"; return false; }
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) { error = "font.importFailed"; return false; }
        wchar_t buffer[40]{}; StringFromGUID2(guid, buffer, 40);
        const std::wstring id(buffer + 1, buffer + 37);
        const auto target = data / L"fonts" / id;
        fs::create_directories(target);
        for (std::size_t i = 0; i < files.size(); ++i)
            fs::copy_file(files[i], target / (L"font" + std::to_wstring(i) + files[i].extension().wstring()));
        for (std::size_t i = 0; i < notices.size(); ++i)
            if (fs::file_size(notices[i]) <= 8 * 1024 * 1024)
                fs::copy_file(notices[i], target / (L"notice" + std::to_wstring(i) + L"-" + notices[i].filename().wstring()));
        auto package = Load(Utf8(id), target, false);
        choices = std::move(package.choices);
        if (choices.empty()) { fs::remove_all(target); error = "font.invalidPackage"; return false; }
        return true;
    }
    catch (const fs::filesystem_error&) { error = "font.importFailed"; return false; }
}

fs::path ResolveXamlResource(std::wstring_view name, const fs::path& data)
{
    constexpr std::wstring_view prefix = L"Files/SnowDesktopUserFonts/";
    if (!name.starts_with(prefix)) return {};
    name.remove_prefix(prefix.size());
    const auto separator = name.find(L'/');
    if (separator == std::wstring_view::npos || !SafePart(name.substr(0, separator)) || !SafePart(name.substr(separator + 1))) return {};
    const auto path = data / L"fonts" / std::wstring(name.substr(0, separator)) / std::wstring(name.substr(separator + 1));
    std::error_code ec;
    return FontFile(path) && fs::is_regular_file(path, ec) ? path : fs::path{};
}
}
