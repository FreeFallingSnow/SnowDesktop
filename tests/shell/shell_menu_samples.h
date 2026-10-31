#pragma once
#include "shell/shell_extension_catalogue.h"
#include "shell_menu_sample_data.h"
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
}
