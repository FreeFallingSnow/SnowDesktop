#include "personalization.h"

#include <windows.h>
#include <shlwapi.h>
#include <fstream>
#include <sstream>

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}

static std::wstring Utf8ToWide(const std::string& u)
{
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), r.data(), n);
    return r;
}

static bool ReadDoubleField(const std::string& text, const char* field, double& out)
{
    std::string marker = "\"" + std::string(field) + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p);
    if (p == std::string::npos) return false;
    p = text.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return false;
    out = atof(text.c_str() + p);
    return true;
}

PersonalizationSettings PersonalizationSettings::DarkPreset()
{
    PersonalizationSettings s;
    s.widgetBgR = 0.08f; s.widgetBgG = 0.10f; s.widgetBgB = 0.13f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.36f; s.gradientEndA = 0.65f;
    return s;
}

PersonalizationSettings PersonalizationSettings::LightPreset()
{
    PersonalizationSettings s;
    s.widgetBgR = 0.95f; s.widgetBgG = 0.96f; s.widgetBgB = 0.97f;
    s.widgetBorderR = 0.5f; s.widgetBorderG = 0.5f; s.widgetBorderB = 0.55f;
    s.widgetAlpha = 0.15f; s.gradientEndA = 0.15f;
    return s;
}

std::wstring GetPersonalizationPath()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.personalization.json");
    return path;
}

bool LoadPersonalization(const wchar_t* path, PersonalizationSettings& s)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    double v = 0;
    if (ReadDoubleField(text, "widgetBgR", v)) s.widgetBgR = (float)v;
    if (ReadDoubleField(text, "widgetBgG", v)) s.widgetBgG = (float)v;
    if (ReadDoubleField(text, "widgetBgB", v)) s.widgetBgB = (float)v;
    if (ReadDoubleField(text, "widgetBorderR", v)) s.widgetBorderR = (float)v;
    if (ReadDoubleField(text, "widgetBorderG", v)) s.widgetBorderG = (float)v;
    if (ReadDoubleField(text, "widgetBorderB", v)) s.widgetBorderB = (float)v;
    if (ReadDoubleField(text, "widgetAlpha", v)) s.widgetAlpha = (float)v;
    if (ReadDoubleField(text, "gradientEndA", v)) s.gradientEndA = (float)v;
    return true;
}

bool SavePersonalization(const wchar_t* path, const PersonalizationSettings& s)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << "{\n";
    file << "  \"widgetBgR\": " << s.widgetBgR << ",\n";
    file << "  \"widgetBgG\": " << s.widgetBgG << ",\n";
    file << "  \"widgetBgB\": " << s.widgetBgB << ",\n";
    file << "  \"widgetBorderR\": " << s.widgetBorderR << ",\n";
    file << "  \"widgetBorderG\": " << s.widgetBorderG << ",\n";
    file << "  \"widgetBorderB\": " << s.widgetBorderB << ",\n";
    file << "  \"widgetAlpha\": " << s.widgetAlpha << ",\n";
    file << "  \"gradientEndA\": " << s.gradientEndA << "\n";
    file << "}\n";
    return true;
}
