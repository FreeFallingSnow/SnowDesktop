/**
 * @file category_settings.cpp
 * @brief 桌面文件分类设置持久化与规则匹配
 */

#include "category_settings.h"
#include "utils.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace
{
    struct LegacyRuleDescriptor
    {
        const wchar_t* id;
        const wchar_t* label;
        const char* jsonField;
        const wchar_t* extensions;
    };

    const LegacyRuleDescriptor* LegacyRules(size_t& count)
    {
        static const LegacyRuleDescriptor rules[] = {
            { L"videos", L"视频", "videos", L".MP4 .MOV .AVI .MKV .WMV .WEBM .M4V" },
            { L"images", L"图片", "images", L".PNG .JPG .JPEG .GIF .BMP .WEBP .HEIC .SVG" },
            { L"documents", L"文档", "documents", L".TXT .MD .DOC .DOCX .PDF .XLS .XLSX .PPT .PPTX .CSV" },
            { L"archives", L"压缩包", "archives", L".ZIP .RAR .7Z .TAR .GZ .BZ2 .XZ" },
            { L"audio", L"音频", "audio", L".MP3 .WAV .FLAC .AAC .M4A .OGG" },
        };
        count = sizeof(rules) / sizeof(rules[0]);
        return rules;
    }

    std::wstring Utf8ToWideLocal(const std::string& u)
    {
        if (u.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), nullptr, 0);
        std::wstring r(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), static_cast<int>(u.size()), r.data(), n);
        return r;
    }

    std::wstring UpperWide(std::wstring value)
    {
        for (wchar_t& ch : value)
            ch = static_cast<wchar_t>(std::towupper(ch));
        return value;
    }

    bool ReadDoubleField(const std::string& text, const char* field, double& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        try { out = std::stod(text.substr(p)); return true; }
        catch (...) { return false; }
    }

    bool ReadStringField(const std::string& text, const char* field, std::string& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos || text[p] != '"') return false;
        ++p;

        std::string value;
        for (; p < text.size(); ++p)
        {
            char ch = text[p];
            if (ch == '"')
            {
                out = value;
                return true;
            }
            if (ch == '\\' && p + 1 < text.size())
            {
                char esc = text[++p];
                switch (esc)
                {
                case '"': value.push_back('"'); break;
                case '\\': value.push_back('\\'); break;
                case '/': value.push_back('/'); break;
                case 'b': value.push_back('\b'); break;
                case 'f': value.push_back('\f'); break;
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                default: value.push_back(esc); break;
                }
            }
            else
            {
                value.push_back(ch);
            }
        }
        return false;
    }

    bool IsExtensionDelimiter(wchar_t ch)
    {
        return std::iswspace(ch) || ch == L',' || ch == L';' ||
            ch == L'，' || ch == L'；' || ch == L'|' || ch == L'/';
    }

    bool FindJsonArray(const std::string& text, const char* field, size_t& begin, size_t& end)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find('[', p);
        if (p == std::string::npos) return false;

        bool inString = false;
        bool escape = false;
        int depth = 0;
        for (size_t i = p; i < text.size(); ++i)
        {
            char ch = text[i];
            if (escape)
            {
                escape = false;
                continue;
            }
            if (ch == '\\' && inString)
            {
                escape = true;
                continue;
            }
            if (ch == '"')
            {
                inString = !inString;
                continue;
            }
            if (inString) continue;
            if (ch == '[')
            {
                if (depth == 0) begin = i + 1;
                ++depth;
            }
            else if (ch == ']')
            {
                --depth;
                if (depth == 0)
                {
                    end = i;
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<std::string> JsonObjectsInArray(const std::string& text, size_t begin, size_t end)
    {
        std::vector<std::string> result;
        bool inString = false;
        bool escape = false;
        int depth = 0;
        size_t objectBegin = std::string::npos;

        for (size_t i = begin; i < end; ++i)
        {
            char ch = text[i];
            if (escape)
            {
                escape = false;
                continue;
            }
            if (ch == '\\' && inString)
            {
                escape = true;
                continue;
            }
            if (ch == '"')
            {
                inString = !inString;
                continue;
            }
            if (inString) continue;

            if (ch == '{')
            {
                if (depth == 0) objectBegin = i;
                ++depth;
            }
            else if (ch == '}')
            {
                --depth;
                if (depth == 0 && objectBegin != std::string::npos)
                {
                    result.push_back(text.substr(objectBegin, i - objectBegin + 1));
                    objectBegin = std::string::npos;
                }
            }
        }
        return result;
    }

    std::wstring MakeRuleId(size_t index)
    {
        return L"category-" + std::to_wstring(index + 1);
    }

    bool IsRetiredBuiltinRule(const CategoryRule& rule)
    {
        return rule.id == L"programs" &&
            rule.extensions == L".EXE .MSI .BAT .CMD .LNK";
    }

    void NormalizeRules(CategorySettings& settings)
    {
        std::vector<CategoryRule> normalized;
        std::unordered_set<std::wstring> seenIds;
        normalized.reserve(settings.rules.size());

        for (size_t i = 0; i < settings.rules.size(); ++i)
        {
            CategoryRule rule = settings.rules[i];
            if (rule.label.empty())
                rule.label = L"未命名";
            if (rule.id.empty() || rule.id == L"all" || rule.id == L"folders" || rule.id == L"others")
                rule.id = MakeRuleId(i);

            rule.extensions = NormalizeCategoryExtensionText(rule.extensions);
            if (IsRetiredBuiltinRule(rule))
                continue;

            std::wstring baseId = rule.id;
            int suffix = 2;
            while (!seenIds.insert(rule.id).second)
                rule.id = baseId + L"-" + std::to_wstring(suffix++);

            normalized.push_back(std::move(rule));
        }

        settings.rules = std::move(normalized);
        settings.tabFontSize = std::clamp(settings.tabFontSize, 10.0f, 22.0f);
    }

    bool LoadRulesArray(const std::string& text, std::vector<CategoryRule>& rules)
    {
        size_t begin = 0;
        size_t end = 0;
        if (!FindJsonArray(text, "rules", begin, end))
            return false;

        std::vector<std::string> objects = JsonObjectsInArray(text, begin, end);
        rules.clear();
        for (const std::string& objectText : objects)
        {
            std::string idUtf8;
            std::string labelUtf8;
            std::string extensionsUtf8;
            ReadStringField(objectText, "id", idUtf8);
            ReadStringField(objectText, "label", labelUtf8);
            ReadStringField(objectText, "extensions", extensionsUtf8);

            CategoryRule rule;
            rule.id = Utf8ToWideLocal(idUtf8);
            rule.label = Utf8ToWideLocal(labelUtf8);
            rule.extensions = NormalizeCategoryExtensionText(Utf8ToWideLocal(extensionsUtf8));
            if (!rule.label.empty())
                rules.push_back(std::move(rule));
        }
        return true;
    }
}

CategorySettings CategorySettings::Defaults()
{
    CategorySettings s;
    size_t count = 0;
    const LegacyRuleDescriptor* defaults = LegacyRules(count);
    s.rules.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        CategoryRule rule;
        rule.id = defaults[i].id;
        rule.label = defaults[i].label;
        rule.extensions = defaults[i].extensions;
        s.rules.push_back(std::move(rule));
    }
    return s;
}

std::wstring GetCategorySettingsPath()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.categories.json");
    return path;
}

std::vector<std::wstring> ParseCategoryExtensionList(const std::wstring& text)
{
    std::vector<std::wstring> result;
    std::unordered_set<std::wstring> seen;
    std::wstring token;

    auto flushToken = [&]() {
        if (token.empty()) return;
        while (!token.empty() && token.front() == L'*')
            token.erase(token.begin());
        if (!token.empty() && token.front() != L'.')
            token.insert(token.begin(), L'.');
        token = UpperWide(token);
        if (token.size() > 1 && seen.insert(token).second)
            result.push_back(token);
        token.clear();
    };

    for (wchar_t ch : text)
    {
        if (IsExtensionDelimiter(ch))
            flushToken();
        else
            token.push_back(ch);
    }
    flushToken();
    return result;
}

std::wstring NormalizeCategoryExtensionText(const std::wstring& text)
{
    std::vector<std::wstring> extensions = ParseCategoryExtensionList(text);
    std::wstring result;
    for (size_t i = 0; i < extensions.size(); ++i)
    {
        if (i > 0) result.push_back(L' ');
        result += extensions[i];
    }
    return result;
}

bool LoadCategorySettings(const wchar_t* path, CategorySettings& settings)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    double number = 0;
    if (ReadDoubleField(text, "tabFontSize", number))
        settings.tabFontSize = std::clamp(static_cast<float>(number), 10.0f, 22.0f);

    std::vector<CategoryRule> loadedRules;
    if (LoadRulesArray(text, loadedRules))
    {
        settings.rules = std::move(loadedRules);
    }
    else
    {
        CategorySettings migrated = CategorySettings::Defaults();
        size_t count = 0;
        const LegacyRuleDescriptor* defaults = LegacyRules(count);
        for (size_t i = 0; i < count; ++i)
        {
            std::string value;
            if (!ReadStringField(text, defaults[i].jsonField, value))
                continue;
            auto it = std::find_if(migrated.rules.begin(), migrated.rules.end(),
                [&](const CategoryRule& rule) { return rule.id == defaults[i].id; });
            if (it != migrated.rules.end())
                it->extensions = NormalizeCategoryExtensionText(Utf8ToWideLocal(value));
        }
        migrated.tabFontSize = settings.tabFontSize;
        settings = std::move(migrated);
    }

    NormalizeRules(settings);
    return true;
}

bool SaveCategorySettings(const wchar_t* path, const CategorySettings& settings)
{
    CategorySettings normalized = settings;
    NormalizeRules(normalized);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    file << "{\n";
    file << "  \"tabFontSize\": " << normalized.tabFontSize << ",\n";
    file << "  \"rules\": [\n";
    for (size_t i = 0; i < normalized.rules.size(); ++i)
    {
        const CategoryRule& rule = normalized.rules[i];
        file << "    { \"id\": \"" << JsonEscapeUtf8(rule.id)
             << "\", \"label\": \"" << JsonEscapeUtf8(rule.label)
             << "\", \"extensions\": \"" << JsonEscapeUtf8(rule.extensions)
             << "\" }";
        if (i + 1 < normalized.rules.size())
            file << ",";
        file << "\n";
    }
    file << "  ]\n";
    file << "}\n";
    return true;
}

std::vector<std::wstring> GetCategoryOrder(const CategorySettings& settings)
{
    std::vector<std::wstring> order;
    order.reserve(settings.rules.size() + 3);
    order.push_back(L"all");
    order.push_back(L"folders");
    for (const CategoryRule& rule : settings.rules)
        if (!rule.id.empty())
            order.push_back(rule.id);
    order.push_back(L"others");
    return order;
}

std::wstring GetCategoryLabel(const CategorySettings& settings, const std::wstring& categoryId)
{
    if (categoryId == L"all") return L"全部";
    if (categoryId == L"folders") return L"文件夹";
    if (categoryId == L"others") return L"其他";
    auto found = std::find_if(settings.rules.begin(), settings.rules.end(),
        [&](const CategoryRule& rule) { return rule.id == categoryId; });
    return found != settings.rules.end() ? found->label : L"其他";
}

std::wstring CategoryIdForExtension(const CategorySettings& settings, const std::wstring& extensionUpper)
{
    const std::wstring ext = UpperWide(extensionUpper);
    if (ext.empty()) return L"";

    for (const CategoryRule& rule : settings.rules)
    {
        const std::vector<std::wstring> extensions = ParseCategoryExtensionList(rule.extensions);
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end())
            return rule.id;
    }
    return L"";
}
