#include "l10n.h"
#include "json_value.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace
{
    bool EqualsAsciiInsensitive(const std::string& left,
        const std::string& right);

    struct LanguageNameSet
    {
        std::string_view code;
        std::string_view nativeName;
        std::array<std::string_view, 10> localizedNames;
    };

    constexpr std::array<LanguageNameSet, 10> kLanguageNames{{
        { "en-US", "English", {
            "English", "英语", "英文", "英語", "영어", "Englisch", // l10n-allow: language-name table
            "anglais", "inglés", "inglés", "inglês" } },
        { "zh-CN", "中文（简体）", { // l10n-allow: language-name table
            "Simplified Chinese", "简体中文", "簡體中文", "簡体字中国語", // l10n-allow: language-name table
            "중국어(간체)", "Chinesisch (vereinfacht)", // l10n-allow: language-name table
            "chinois simplifié", "chino simplificado", "chino simplificado", // l10n-allow: language-name table
            "chinês simplificado" } }, // l10n-allow: language-name table
        { "zh-TW", "中文（繁體）", { // l10n-allow: language-name table
            "Traditional Chinese", "繁体中文", "繁體中文", "繁体字中国語", // l10n-allow: language-name table
            "중국어(번체)", "Chinesisch (traditionell)", // l10n-allow: language-name table
            "chinois traditionnel", "chino tradicional", "chino tradicional", // l10n-allow: language-name table
            "chinês tradicional" } }, // l10n-allow: language-name table
        { "ja-JP", "日本語", { // l10n-allow: language-name table
            "Japanese", "日语", "日文", "日本語", "일본어", "Japanisch", // l10n-allow: language-name table
            "japonais", "japonés", "japonés", "japonês" } },
        { "ko-KR", "한국어", { // l10n-allow: language-name table
            "Korean", "韩语", "韓文", "韓国語", "한국어", "Koreanisch", // l10n-allow: language-name table
            "coréen", "coreano", "coreano", "coreano" } },
        { "de-DE", "Deutsch", {
            "German", "德语", "德文", "ドイツ語", "독일어", "Deutsch", // l10n-allow: language-name table
            "allemand", "alemán", "alemán", "alemão" } },
        { "fr-FR", "français", {
            "French", "法语", "法文", "フランス語", "프랑스어", "Französisch", // l10n-allow: language-name table
            "français", "francés", "francés", "francês" } },
        { "es-ES", "español（España）", {
            "Spanish (Spain)", "西班牙语（西班牙）", "西班牙文（西班牙）", // l10n-allow: language-name table
            "スペイン語（スペイン）", "스페인어(스페인)", "Spanisch (Spanien)", // l10n-allow: language-name table
            "espagnol (Espagne)", "español (España)", "español (España)",
            "espanhol (Espanha)" } },
        { "es-419", "español（Latinoamérica）", {
            "Spanish (Latin America)", "西班牙语（拉丁美洲）", "西班牙文（拉丁美洲）", // l10n-allow: language-name table
            "スペイン語（ラテンアメリカ）", "스페인어(라틴 아메리카)", // l10n-allow: language-name table
            "Spanisch (Lateinamerika)", "espagnol (Amérique latine)",
            "español (Latinoamérica)", "español (Latinoamérica)",
            "espanhol (América Latina)" } },
        { "pt-BR", "português（Brasil）", {
            "Portuguese (Brazil)", "葡萄牙语（巴西）", "葡萄牙文（巴西）", // l10n-allow: language-name table
            "ポルトガル語（ブラジル）", "포르투갈어(브라질)", // l10n-allow: language-name table
            "Portugiesisch (Brasilien)", "portugais (Brésil)",
            "portugués (Brasil)", "portugués (Brasil)",
            "português (Brasil)" } },
    }};

    const LanguageNameSet* FindLanguageNameSet(
        const std::string& code)
    {
        for (const LanguageNameSet& names : kLanguageNames)
        {
            if (EqualsAsciiInsensitive(std::string(names.code), code))
                return &names;
        }
        return nullptr;
    }

    size_t LanguageNameIndex(const std::string& code)
    {
        for (size_t index = 0; index < kLanguageNames.size(); ++index)
        {
            if (EqualsAsciiInsensitive(
                    std::string(kLanguageNames[index].code), code))
                return index;
        }
        return kLanguageNames.size();
    }

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
        if (const LanguageNameSet* names = FindLanguageNameSet(code))
            return std::string(names->nativeName);

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

    size_t LanguageSortRank(const std::string& code)
    {
        const size_t index = LanguageNameIndex(code);
        if (index == 0) return 0;
        if (index == 1) return 1;
        return index < kLanguageNames.size() ? index + 1 : 1000;
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
            const size_t leftRank = LanguageSortRank(left.code);
            const size_t rightRank = LanguageSortRank(right.code);
            if (leftRank != rightRank)
                return leftRank < rightRank;
            return left.code < right.code;
        });
}

std::string Locale::GetLocalizedLanguageName(
    const std::string& language) const
{
    const LanguageNameSet* names = FindLanguageNameSet(language);
    if (!names)
        return language;

    const LanguageNameSet* current =
        FindLanguageNameSet(GetEffectiveLanguage());
    if (!current)
        return std::string(names->nativeName);

    const size_t currentIndex = LanguageNameIndex(std::string(current->code));
    if (currentIndex >= kLanguageNames.size())
        return std::string(names->nativeName);

    if (currentIndex >= names->localizedNames.size())
        return std::string(names->nativeName);
    return std::string(names->localizedNames[currentIndex]);
}

std::string Locale::GetLanguageSelectionLabel(
    const std::string& language) const
{
    const std::string localizedName = GetLocalizedLanguageName(language);
    const std::string nativeName = NativeLanguageName(language);
    if (nativeName.empty() || localizedName == nativeName)
        return localizedName;
    if (localizedName.empty())
        return nativeName;
    return localizedName + " (" + nativeName + ")";
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
    std::vector<std::string> available;
    available.reserve(availableLanguages_.size());
    for (const LanguageInfo& info : availableLanguages_)
        available.push_back(info.code);

    const std::string selected =
        snowdesktop::localization::ResolveBestLanguage(available, requested);
    if (!selected.empty()) return selected;

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
