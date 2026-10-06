#pragma once
#include "shell_extension_catalogue.h"
#include "shell_extension_discovery_samples.h"
#include <fstream>
#include <span>
#include <set>

namespace snowdesktop::shell_extensions
{
inline std::span<const unsigned char> DiscoverySample(std::wstring_view type)
{
    using namespace discovery_samples;
    if (type == L".png") return Png;
    if (type == L".jpg" || type == L".jpeg") return Jpeg;
    if (type == L".bmp") return Bmp;
    if (type == L".gif") return Gif;
    if (type == L".tif" || type == L".tiff") return Tiff;
    if (type == L".pdf") return Pdf;
    if (type == L".docx") return Docx;
    if (type == L".xlsx") return Xlsx;
    if (type == L".pptx") return Pptx;
    if (type == L".zip") return Zip;
    if (type == L".wav") return Wav;
    static constexpr unsigned char text[] = "SnowDesktop menu discovery\r\n";
    static constexpr unsigned char rtf[] = "{\\rtf1\\ansi SnowDesktop menu discovery}\r\n";
    static constexpr unsigned char csv[] = "name,value\r\nsample,1\r\n";
    static constexpr unsigned char html[] = "<!doctype html><html><head><title>Sample</title></head><body>Sample</body></html>";
    if (type == L".txt" || type == L".md") return {text, sizeof(text) - 1};
    if (type == L".rtf") return {rtf, sizeof(rtf) - 1};
    if (type == L".csv") return {csv, sizeof(csv) - 1};
    if (type == L".html" || type == L".htm") return {html, sizeof(html) - 1};
    return {};
}

// No arbitrary extension is assigned an empty/fake document. Only supported,
// registered formats get valid owned samples; other formats use real selection
// inspection. A pair discovers single-, multi-selection and Shift-only roots.
inline std::vector<Request> DiscoverFileTypes(const Catalogue &catalogue, const std::filesystem::path &directory)
{
    std::set<std::wstring> registered;
    for (const auto &row : catalogue.rows)
        if (row.systemEnabled && (row.contexts & ContextBit(Context::File)))
            for (const auto &type : row.types)
                if (!DiscoverySample(type).empty()) registered.insert(type);
    std::vector<Request> result;
    const auto root = directory / L"discovery-samples-v1";
    std::error_code error;
    if (registered.empty() || (!std::filesystem::create_directories(root, error) && error)) return result;
    for (const auto &type : registered)
    {
        const auto bytes = DiscoverySample(type);
        Request single;
        bool ready = true;
        for (int i = 1; i <= 2; ++i)
        {
            const auto file = root / (L"sample-" + std::to_wstring(i) + type);
            std::ifstream existing(file, std::ios::binary);
            std::vector<unsigned char> stored(bytes.size());
            if (!existing.read(reinterpret_cast<char *>(stored.data()), stored.size()) ||
                !std::equal(stored.begin(), stored.end(), bytes.begin()) || existing.peek() != EOF)
            {
                existing.close();
                std::ofstream output(file, std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()); output.close();
                if (!output) { ready = false; break; }
            }
            single.paths.push_back(file.wstring());
        }
        if (!ready) continue;
        const auto pair = single;
        single.paths.resize(1);
        result.push_back(single); result.push_back(pair);
        single.extended = true; result.push_back(single);
        auto extendedPair = pair; extendedPair.extended = true; result.push_back(std::move(extendedPair));
    }
    return result;
}
}
