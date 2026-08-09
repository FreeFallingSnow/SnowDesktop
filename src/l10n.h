#pragma once

#include "language_fallback.h"

#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

struct LanguageInfo
{
    std::string code;
    std::string displayName;
};

class Locale
{
public:
    static Locale& Instance();

    void Init(const wchar_t* langDir);
    void SetLanguage(const char* language);
    const char* GetLanguage() const { return currentLanguage_.c_str(); }
    std::string GetEffectiveLanguage() const;
    bool IsSystemDefault() const { return currentLanguage_ == "system"; }
    const std::vector<LanguageInfo>& GetAvailableLanguages() const
    { return availableLanguages_; }
    std::string GetLocalizedLanguageName(
        const std::string& language) const;
    bool HasLanguage(const std::string& language) const;

    const char* Tr(const char* key) const;
    const wchar_t* TrW(const char* key) const;
    bool IsTranslationValue(const char* key, const std::wstring& value) const;
    std::vector<std::wstring> TranslationValues(const char* key) const;
    std::string TrFormat(const char* key,
        std::initializer_list<std::string> arguments) const;
    std::wstring TrFormatW(const char* key,
        std::initializer_list<std::wstring> arguments) const;

private:
    Locale() = default;
    void LoadAllLanguages();
    void LoadLanguage(const char* language);
    std::string DetectSystemLanguage() const;
    std::string ResolveLanguage(const std::string& lang) const;

    std::wstring langDir_;
    std::string currentLanguage_;
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::string>> catalogs_;
    std::vector<LanguageInfo> availableLanguages_;
    std::unordered_map<std::string, std::string> strings_;
    mutable std::unordered_map<std::string, std::wstring> wideCache_;
};

#define _L(key) Locale::Instance().Tr(key)
#define _LW(key) Locale::Instance().TrW(key)
#define _LF(key, ...) Locale::Instance().TrFormat(key, {__VA_ARGS__})
#define _LFW(key, ...) Locale::Instance().TrFormatW(key, {__VA_ARGS__})
#define L10N_KEY(key) key
