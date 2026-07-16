#include "dock_settings.h"

#include "data_paths.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace
{
bool ReadBoolField(const std::string& text, const char* field, bool& out)
{
    const std::string marker = "\"" + std::string(field) + "\"";
    size_t position = text.find(marker);
    if (position == std::string::npos) return false;
    position = text.find(':', position);
    if (position == std::string::npos) return false;
    position = text.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos) return false;
    if (text.compare(position, 4, "true") == 0) { out = true; return true; }
    if (text.compare(position, 5, "false") == 0) { out = false; return true; }
    return false;
}

bool ReadDoubleField(const std::string& text, const char* field, double& out)
{
    const std::string marker = "\"" + std::string(field) + "\"";
    size_t position = text.find(marker);
    if (position == std::string::npos) return false;
    position = text.find(':', position);
    if (position == std::string::npos) return false;
    position = text.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string::npos) return false;
    try
    {
        out = std::stod(text.substr(position));
        return true;
    }
    catch (...)
    {
        return false;
    }
}
}

std::wstring GetDockSettingsPath()
{
    return GetDataFilePath(L"SnowDesktop.dock.json");
}

bool LoadDockSettings(const wchar_t* path, DockSettings& settings)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream stream;
    stream << file.rdbuf();
    const std::string text = stream.str();
    if (text.empty()) return false;

    double value = 0.0;
    if (ReadDoubleField(text, "position", value))
        settings.position = static_cast<DockPosition>(std::clamp(static_cast<int>(value), 0, 3));
    ReadBoolField(text, "edgeAttached", settings.edgeAttached);
    ReadBoolField(text, "followPersonalization", settings.followPersonalization);
    ReadBoolField(text, "showFrequentItems", settings.showFrequentItems);
    if (ReadDoubleField(text, "frequentItemCount", value))
        settings.frequentItemCount = std::clamp(static_cast<int>(value), 1, 8);

    PersonalizationSettings& style = settings.appearance;
    if (ReadDoubleField(text, "backgroundR", value)) style.widgetBgR = static_cast<float>(value);
    if (ReadDoubleField(text, "backgroundG", value)) style.widgetBgG = static_cast<float>(value);
    if (ReadDoubleField(text, "backgroundB", value)) style.widgetBgB = static_cast<float>(value);
    if (ReadDoubleField(text, "borderR", value)) style.widgetBorderR = static_cast<float>(value);
    if (ReadDoubleField(text, "borderG", value)) style.widgetBorderG = static_cast<float>(value);
    if (ReadDoubleField(text, "borderB", value)) style.widgetBorderB = static_cast<float>(value);
    if (ReadDoubleField(text, "backgroundAlpha", value)) style.widgetAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "borderAlpha", value)) style.widgetBorderAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "cornerRadius", value)) style.cornerRadius = static_cast<float>(value);
    if (ReadDoubleField(text, "highlightAlpha", value)) style.highlightAlpha = static_cast<float>(value);
    if (ReadDoubleField(text, "noiseAlpha", value)) style.noiseAlpha = static_cast<float>(value);
    return true;
}

bool SaveDockSettings(const wchar_t* path, const DockSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    const PersonalizationSettings& style = settings.appearance;
    file << "{\n";
    file << "  \"position\": " << static_cast<int>(settings.position) << ",\n";
    file << "  \"edgeAttached\": "
         << (settings.edgeAttached ? "true" : "false") << ",\n";
    file << "  \"followPersonalization\": "
         << (settings.followPersonalization ? "true" : "false") << ",\n";
    file << "  \"showFrequentItems\": "
         << (settings.showFrequentItems ? "true" : "false") << ",\n";
    file << "  \"frequentItemCount\": " << settings.frequentItemCount << ",\n";
    file << "  \"backgroundR\": " << style.widgetBgR << ",\n";
    file << "  \"backgroundG\": " << style.widgetBgG << ",\n";
    file << "  \"backgroundB\": " << style.widgetBgB << ",\n";
    file << "  \"borderR\": " << style.widgetBorderR << ",\n";
    file << "  \"borderG\": " << style.widgetBorderG << ",\n";
    file << "  \"borderB\": " << style.widgetBorderB << ",\n";
    file << "  \"backgroundAlpha\": " << style.widgetAlpha << ",\n";
    file << "  \"borderAlpha\": " << style.widgetBorderAlpha << ",\n";
    file << "  \"cornerRadius\": " << style.cornerRadius << ",\n";
    file << "  \"highlightAlpha\": " << style.highlightAlpha << ",\n";
    file << "  \"noiseAlpha\": " << style.noiseAlpha << "\n";
    file << "}\n";
    return true;
}
