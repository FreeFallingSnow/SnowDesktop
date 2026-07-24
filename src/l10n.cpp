#include "l10n.h"
#include "json_value.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
    std::string ReadFileUtf8(const std::wstring& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return {};
        std::ostringstream ss;
        ss << file.rdbuf();
        return ss.str();
    }

    std::wstring Utf8ToWideStatic(const std::string& s)
    {
        if (s.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0)
            return {};
        std::wstring r(n, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            s.c_str(), static_cast<int>(s.size()), r.data(), n);
        return r;
    }

    std::string WideToUtf8Static(const std::wstring& s)
    {
        if (s.empty())
            return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
            return {};
        std::string r(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
            static_cast<int>(s.size()), r.data(), n, nullptr, nullptr);
        return r;
    }

    bool ParseCatalog(const std::string& text,
        std::unordered_map<std::string, std::string>& output)
    {
        JsonValue root;
        if (!ParseJson(text, root) || !root.IsObject())
            return false;
        output.clear();
        for (const auto& [key, value] : root.object)
        {
            if (!value.IsString())
                return false;
            output.emplace(key, value.string);
        }
        return !output.empty();
    }

    std::string NativeLanguageName(const std::string& code)
    {
        std::wstring wideCode = Utf8ToWideStatic(code);
        wchar_t displayName[LOCALE_NAME_MAX_LENGTH]{};
        if (!wideCode.empty() &&
            GetLocaleInfoEx(wideCode.c_str(), LOCALE_SNATIVELANGUAGENAME,
                displayName, LOCALE_NAME_MAX_LENGTH) > 0)
        {
            return WideToUtf8Static(displayName);
        }
        return code;
    }

    bool EqualsAsciiInsensitive(const std::string& left, const std::string& right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t index = 0; index < left.size(); ++index)
        {
            unsigned char a = static_cast<unsigned char>(left[index]);
            unsigned char b = static_cast<unsigned char>(right[index]);
            if (std::tolower(a) != std::tolower(b))
                return false;
        }
        return true;
    }

    template<typename String>
    void ReplaceAll(String& text, const String& needle, const String& replacement)
    {
        size_t position = 0;
        while ((position = text.find(needle, position)) != String::npos)
        {
            text.replace(position, needle.size(), replacement);
            position += replacement.size();
        }
    }
}

Locale& Locale::Instance()
{
    static Locale instance;
    return instance;
}

void Locale::Init(const wchar_t* langDir)
{
    langDir_ = langDir ? langDir : L"";
    LoadAllLanguages();
    currentLanguage_ = "system";
    LoadLanguage(ResolveLanguage(currentLanguage_).c_str());
}

void Locale::SetLanguage(const char* language)
{
    if (!language || !*language)
        return;
    const std::string requested = language;
    if (requested != "system" && !HasLanguage(requested))
        return;
    if (currentLanguage_ == requested)
        return;
    currentLanguage_ = requested;
    LoadLanguage(ResolveLanguage(currentLanguage_).c_str());
}

std::string Locale::GetEffectiveLanguage() const
{
    return ResolveLanguage(currentLanguage_);
}

bool Locale::HasLanguage(const std::string& language) const
{
    return std::any_of(availableLanguages_.begin(), availableLanguages_.end(),
        [&](const LanguageInfo& info) {
            return EqualsAsciiInsensitive(info.code, language);
        });
}

void Locale::LoadAllLanguages()
{
    catalogs_.clear();
    availableLanguages_.clear();
    if (langDir_.empty())
        return;

    WIN32_FIND_DATAW data{};
    std::wstring search = langDir_ + L"\\*.json";
    HANDLE find = FindFirstFileW(search.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::wstring filename = data.cFileName;
        if (filename.size() <= 5)
            continue;
        std::wstring stem = filename.substr(0, filename.size() - 5);
        std::string code = WideToUtf8Static(stem);
        std::unordered_map<std::string, std::string> catalog;
        if (code.empty() ||
            !ParseCatalog(ReadFileUtf8(langDir_ + L"\\" + filename), catalog))
        {
            continue;
        }
        catalogs_.emplace(code, std::move(catalog));
        availableLanguages_.push_back({ code, NativeLanguageName(code) });
    } while (FindNextFileW(find, &data));
    FindClose(find);

    std::sort(availableLanguages_.begin(), availableLanguages_.end(),
        [](const LanguageInfo& left, const LanguageInfo& right) {
            return left.code < right.code;
        });
}

void Locale::LoadLanguage(const char* language)
{
    strings_.clear();
    wideCache_.clear();

    auto fallback = catalogs_.find("en-US");
    if (fallback != catalogs_.end())
        strings_ = fallback->second;

    if (language && *language)
    {
        auto selected = catalogs_.find(language);
        if (selected != catalogs_.end())
        {
            for (const auto& [key, value] : selected->second)
                strings_[key] = value;
        }
    }
    wideCache_.reserve(strings_.size() + 16);
    for (const auto& [key, value] : strings_)
        wideCache_[key] = Utf8ToWideStatic(value);
}

std::string Locale::DetectSystemLanguage() const
{
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0)
        return WideToUtf8Static(localeName);
    return {};
}

std::string Locale::ResolveLanguage(const std::string& lang) const
{
    const std::string requested = lang == "system" ? DetectSystemLanguage() : lang;
    for (const LanguageInfo& info : availableLanguages_)
        if (EqualsAsciiInsensitive(info.code, requested))
            return info.code;

    const size_t separator = requested.find('-');
    const std::string base = requested.substr(0, separator);
    for (const LanguageInfo& info : availableLanguages_)
    {
        const size_t candidateSeparator = info.code.find('-');
        if (EqualsAsciiInsensitive(info.code.substr(0, candidateSeparator), base))
            return info.code;
    }
    if (catalogs_.contains("en-US"))
        return "en-US";
    return availableLanguages_.empty() ? std::string{} : availableLanguages_.front().code;
}

const char* Locale::Tr(const char* key) const
{
    if (!key || !*key)
        return "";
    auto it = strings_.find(key);
    if (it != strings_.end())
        return it->second.c_str();
    return key;
}

bool Locale::IsTranslationValue(const char* key, const std::wstring& value) const
{
    if (!key || !*key || value.empty())
        return false;

    for (const auto& [language, catalog] : catalogs_)
    {
        (void)language;
        auto translation = catalog.find(key);
        if (translation != catalog.end() &&
            Utf8ToWideStatic(translation->second) == value)
            return true;
    }
    return false;
}

std::vector<std::wstring> Locale::TranslationValues(const char* key) const
{
    std::vector<std::wstring> result;
    if (!key || !*key)
        return result;
    std::unordered_set<std::wstring> seen;
    for (const auto& [language, catalog] : catalogs_)
    {
        (void)language;
        auto found = catalog.find(key);
        if (found == catalog.end())
            continue;
        std::wstring value = Utf8ToWideStatic(found->second);
        if (!value.empty() && seen.insert(value).second)
            result.push_back(std::move(value));
    }
    return result;
}

const wchar_t* Locale::TrW(const char* key) const
{
    if (!key || !*key)
        return L"";
    auto cacheIt = wideCache_.find(key);
    if (cacheIt != wideCache_.end())
        return cacheIt->second.c_str();
    auto [insertedIt, _] = wideCache_.emplace(key, Utf8ToWideStatic(key));
    return insertedIt->second.c_str();
}

std::string Locale::TrFormat(const char* key,
    std::initializer_list<std::string> arguments) const
{
    std::string result = Tr(key);
    size_t index = 0;
    for (const std::string& argument : arguments)
    {
        ReplaceAll(result, "{" + std::to_string(index) + "}", argument);
        ++index;
    }
    return result;
}

std::wstring Locale::TrFormatW(const char* key,
    std::initializer_list<std::wstring> arguments) const
{
    std::wstring result = TrW(key);
    size_t index = 0;
    for (const std::wstring& argument : arguments)
    {
        ReplaceAll(result, L"{" + std::to_wstring(index) + L"}", argument);
        ++index;
    }
    return result;
}
