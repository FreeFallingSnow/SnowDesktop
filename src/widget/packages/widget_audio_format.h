#pragma once

#include <array>
#include <string_view>

namespace snowdesktop::widget
{
struct WidgetAudioFormat
{
    std::string_view extension;
    std::wstring_view contentType;
};
inline constexpr auto WidgetAudioFormats = std::to_array<WidgetAudioFormat>({
    {".mp3", L"audio/mpeg"}, {".wav", L"audio/wav"},
    {".flac", L"audio/flac"}, {".m4a", L"audio/mp4"}, {".m4b", L"audio/mp4"},
    {".aac", L"audio/aac"}, {".adts", L"audio/aac"}, {".wma", L"audio/x-ms-wma"},
    {".ogg", L"audio/ogg"}, {".oga", L"audio/ogg"}, {".opus", L"audio/ogg"},
});
template<class Character>
inline const WidgetAudioFormat* FindWidgetAudioFormat(std::basic_string_view<Character> extension)
{
    for (const auto& format : WidgetAudioFormats)
    {
        if (extension.size() != format.extension.size()) continue;
        bool same = true;
        for (std::size_t index = 0; same && index < extension.size(); ++index)
        {
            auto character = extension[index];
            if (character >= 'A' && character <= 'Z') character = static_cast<Character>(character + ('a' - 'A'));
            same = character == static_cast<Character>(format.extension[index]);
        }
        if (same) return &format;
    }
    return nullptr;
}
// Header validation is not a decoder probe: an installed native codec is still
// needed at playback time. No file paths or network references are accepted here.
inline bool WidgetAudioContentIsValid(std::string_view extension, std::string_view data)
{
    const auto* format = FindWidgetAudioFormat(extension);
    if (!format || data.size() < 4) return false;
    const auto type = format->extension;
    if (type == ".wav") return data.size() >= 44 && data.substr(0, 4) == "RIFF" && data.substr(8, 4) == "WAVE";
    if (type == ".mp3") return data.substr(0, 3) == "ID3" ||
        (static_cast<unsigned char>(data[0]) == 0xff && (static_cast<unsigned char>(data[1]) & 0xe0) == 0xe0);
    if (type == ".flac") return data.substr(0, 4) == "fLaC";
    if (type == ".m4a" || type == ".m4b") return data.size() >= 12 && data.substr(4, 4) == "ftyp";
    if (type == ".aac" || type == ".adts") return static_cast<unsigned char>(data[0]) == 0xff &&
        (static_cast<unsigned char>(data[1]) & 0xf6) == 0xf0;
    if (type == ".wma") return data.size() >= 16 && data.substr(0, 16) ==
        std::string_view("\x30\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c", 16);
    return data.size() >= 28 && data.substr(0, 4) == "OggS";
}
}
