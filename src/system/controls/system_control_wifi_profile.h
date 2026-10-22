#pragma once
#include <windows.h>
#include <wlanapi.h>
#include <algorithm>
#include <string>
#include <string_view>

namespace snowdesktop::system_control::windows
{
inline std::wstring WifiSsidHex(const DOT11_SSID& ssid)
{
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring result;
    for (ULONG i = 0; i < (std::min)(ssid.uSSIDLength, 32ul); ++i)
    { result.push_back(digits[ssid.ucSSID[i] >> 4]); result.push_back(digits[ssid.ucSSID[i] & 15]); }
    return result;
}

inline std::wstring WifiProfileBaseName(const DOT11_SSID& ssid)
{
    const auto length = static_cast<int>((std::min)(ssid.uSSIDLength, 32ul));
    const auto* bytes = reinterpret_cast<const char*>(ssid.ucSSID);
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length, nullptr, 0);
    if (size > 0)
    {
        std::wstring name(static_cast<size_t>(size), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, length, name.data(), size) == size &&
            std::none_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32 || ch == 0xFFFE || ch == 0xFFFF; }))
            return name;
    }
    // Binary SSIDs still need an XML-safe, recognizable profile name. Their
    // original bytes remain authoritative in SSIDConfig/SSID/hex.
    return WifiSsidHex(ssid);
}

inline void AppendWifiProfileXml(std::wstring& output, std::wstring_view value)
{
    for (const wchar_t character : value)
    {
        switch (character)
        {
        case L'&': output += L"&amp;"; break;
        case L'<': output += L"&lt;"; break;
        case L'>': output += L"&gt;"; break;
        case L'\"': output += L"&quot;"; break;
        case L'\'': output += L"&apos;"; break;
        default: output += character; break;
        }
    }
}

struct WifiProfileXml
{
    std::wstring text;
    ~WifiProfileXml() { if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t)); }
};

// The writer must create without overwriting. Retry only a name collision;
// credentials, permissions and other failures must not be hidden by retries.
template<class Stop, class WriteProfile>
DWORD CreateWifiProfile(const DOT11_SSID& ssid, std::string_view security, std::wstring_view password,
    bool hidden, std::wstring& profile, Stop stop, WriteProfile writeProfile)
{
    if (!ssid.uSSIDLength || ssid.uSSIDLength > 32) return ERROR_INVALID_PARAMETER;
    const auto baseName = WifiProfileBaseName(ssid);
    for (unsigned ordinal = 1; ordinal <= 1024; ++ordinal)
    {
        if (stop()) return ERROR_CANCELLED;
        const auto name = ordinal == 1 ? baseName : baseName + L" (" + std::to_wstring(ordinal) + L")";
        WifiProfileXml xml;
        xml.text.reserve(4096);
        xml.text = L"<?xml version=\"1.0\"?><WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>";
        AppendWifiProfileXml(xml.text, name);
        xml.text += L"</name><SSIDConfig><SSID><hex>" + WifiSsidHex(ssid) + L"</hex></SSID><nonBroadcast>";
        xml.text += hidden ? L"true" : L"false";
        xml.text += L"</nonBroadcast></SSIDConfig><connectionType>ESS</connectionType><connectionMode>auto</connectionMode><MSM><security><authEncryption><authentication>";
        xml.text += security == "open" ? L"open" : security == "wpa3" ? L"WPA3SAE" : L"WPA2PSK";
        xml.text += L"</authentication><encryption>";
        xml.text += security == "open" ? L"none" : L"AES";
        xml.text += L"</encryption><useOneX>false</useOneX></authEncryption>";
        if (security != "open")
        {
            xml.text += L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>";
            AppendWifiProfileXml(xml.text, password);
            xml.text += L"</keyMaterial></sharedKey>";
        }
        xml.text += L"</security></MSM></WLANProfile>";
        if (stop()) return ERROR_CANCELLED;
        const DWORD status = writeProfile(xml.text.c_str());
        if (status == ERROR_ALREADY_EXISTS) continue;
        if (status == ERROR_SUCCESS) profile = name;
        return status;
    }
    return ERROR_ALREADY_EXISTS;
}
}
