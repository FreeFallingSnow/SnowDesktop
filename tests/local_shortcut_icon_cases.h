// The production reader consumes real files without COM initialization. These
// independent MS-SHLLINK fixtures protect icon/index selection and bounded reads;
// unsupported or malformed input must yield to the separately scheduled Shell lane.
using LinkFixture = std::vector<std::uint8_t>;

void LinkU32(LinkFixture& bytes, size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

LinkFixture LinkHeader(std::uint32_t flags)
{
    LinkFixture bytes(76);
    LinkU32(bytes, 0, 76);
    const std::uint8_t clsid[]{1, 0x14, 2, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    std::copy(std::begin(clsid), std::end(clsid), bytes.begin() + 4);
    LinkU32(bytes, 20, flags);
    return bytes;
}

void LinkString(LinkFixture& bytes, std::wstring_view value, bool unicode, bool counted = true)
{
    if (counted)
    {
        bytes.push_back(static_cast<std::uint8_t>(value.size()));
        bytes.push_back(static_cast<std::uint8_t>(value.size() >> 8));
    }
    for (const auto ch : value)
    {
        bytes.push_back(static_cast<std::uint8_t>(ch));
        if (unicode) bytes.push_back(static_cast<std::uint8_t>(ch >> 8));
    }
    if (!counted)
    {
        bytes.push_back(0);
        if (unicode) bytes.push_back(0);
    }
}

void WriteLinkFixture(const std::filesystem::path& path, const LinkFixture& bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(output.good(), "raw shortcut fixture writes successfully");
}

void CheckLocalShortcutIcons()
{
    namespace resources = snowdesktop::shortcut_icon_resource;
    TemporaryDirectory temporary;
    Check(!temporary.Path().empty(), "local icon reader has isolated files");
    if (temporary.Path().empty()) return;
    const auto link = temporary.Path() / L"raw.lnk";
    const auto icon = temporary.Path() / L"icon.ico";
    const auto target = temporary.Path() / L"target.exe";
    const auto wideTarget = temporary.Path() / L"应用.exe";
    for (const auto& path : {icon, target, wideTarget}) WriteLinkFixture(path, {0});
    const auto read = [&](LinkFixture bytes) {
        bytes.insert(bytes.end(), 4, 0); // TerminalBlock
        WriteLinkFixture(link, bytes);
        return resources::ReadLocalIconResources(link.wstring());
    };
    auto bytes = LinkHeader(0xe5); // IDList + name + arguments + icon + Unicode
    LinkU32(bytes, 56, 0xfffffff9); // signed resource ID -7
    bytes.insert(bytes.end(), {2, 0, 0, 0});
    LinkString(bytes, L"description", true);
    LinkString(bytes, L"--unused", true);
    LinkString(bytes, L"icon.ico", true);
    const auto explicitIcon = read(bytes);
    Check(explicitIcon.size() == 1 && explicitIcon[0].path == icon.wstring() && explicitIcon[0].index == -7,
        "raw Unicode icon location survives skipped PIDL/strings and retains negative resource index");
    bytes = LinkHeader(0x40);
    LinkString(bytes, L"icon.ico", false);
    const auto ansiIcon = read(bytes);
    Check(ansiIcon.size() == 1 && ansiIcon[0].path == icon.wstring(), "ANSI icon strings are decoded using the local code page");
    bytes = LinkHeader(0x88);
    LinkString(bytes, L"target.exe", true);
    const auto relative = read(bytes);
    Check(relative.size() == 1 && relative[0].path == target.wstring(), "a relative executable target resolves beside the link");

    const auto linkInfo = [&](bool unicode) {
        LinkFixture info(unicode ? 36 : 28);
        LinkU32(info, 4, static_cast<std::uint32_t>(info.size()));
        LinkU32(info, 8, 1);
        LinkU32(info, 12, static_cast<std::uint32_t>(info.size()));
        const auto volume = info.size();
        info.resize(volume + 17);
        LinkU32(info, volume, 17);
        LinkU32(info, volume + 4, DRIVE_FIXED);
        LinkU32(info, volume + 12, 16);
        LinkU32(info, 16, static_cast<std::uint32_t>(info.size()));
        LinkString(info, temporary.Path().wstring() + L"\\", false, false);
        LinkU32(info, 24, static_cast<std::uint32_t>(info.size()));
        LinkString(info, L"target.exe", false, false);
        if (unicode)
        {
            LinkU32(info, 28, static_cast<std::uint32_t>(info.size()));
            LinkString(info, wideTarget.wstring(), true, false);
            LinkU32(info, 32, static_cast<std::uint32_t>(info.size()));
            LinkString(info, L"", true, false);
        }
        LinkU32(info, 0, static_cast<std::uint32_t>(info.size()));
        return info;
    };
    for (bool unicode : {false, true})
    {
        bytes = LinkHeader(0x82);
        const auto info = linkInfo(unicode);
        bytes.insert(bytes.end(), info.begin(), info.end());
        const auto found = read(bytes);
        const auto expected = unicode ? wideTarget : target;
        Check(found.size() == 1 && found[0].path == expected.wstring(),
            "LinkInfo chooses Unicode when available and combines the local base/suffix otherwise");
        LinkU32(bytes, 20, 0x182);
        Check(read(bytes).empty(), "ForceNoLinkInfo cannot return a stale target resource");
    }
    bytes = LinkHeader(0x4000);
    LinkFixture environment(0x314);
    LinkU32(environment, 0, 0x314);
    LinkU32(environment, 4, 0xa0000007);
    LinkFixture value;
    const std::wstring variable = L"SNOWDESKTOP_ICON_TEST_" + std::to_wstring(GetCurrentProcessId());
    struct ClearVariable
    {
        const std::wstring& name;
        ~ClearVariable() { SetEnvironmentVariableW(name.c_str(), nullptr); }
    } clear{variable};
    Check(SetEnvironmentVariableW(variable.c_str(), temporary.Path().c_str()) != FALSE,
        "icon fixture environment variable is available");
    LinkString(value, L"%" + variable + L"%\\icon.ico", true, false);
    Check(value.size() <= 520, "environment icon fixture fits the format");
    std::copy(value.begin(), value.end(), environment.begin() + 268);
    bytes.insert(bytes.end(), environment.begin(), environment.end());
    const auto envIcon = read(bytes);
    Check(envIcon.size() == 1 && envIcon[0].path == icon.wstring(), "icon environment blocks expand their Unicode resource location");

    std::vector<LinkFixture> invalid{LinkFixture(20), LinkHeader(1), LinkHeader(2), LinkHeader(0x40)};
    auto oversizedPidl = LinkHeader(1);
    oversizedPidl.insert(oversizedPidl.end(), {0xff, 0xff});
    invalid.push_back(oversizedPidl);
    auto badString = LinkHeader(0xc0);
    badString.insert(badString.end(), {0xff, 0xff, 0});
    invalid.push_back(badString);
    auto badOffset = LinkHeader(2);
    auto info = linkInfo(false);
    LinkU32(info, 16, 0xfffffff0);
    badOffset.insert(badOffset.end(), info.begin(), info.end());
    invalid.push_back(badOffset);
    auto badClass = LinkHeader(0xc0);
    badClass[4] = 0;
    LinkString(badClass, L"icon.ico", true);
    invalid.push_back(badClass);
    for (const auto& malformed : invalid)
        Check(read(malformed).empty(), "truncated or malformed links yield to Shell without reading outside file bounds");
    bytes = LinkHeader(0xc0);
    LinkString(bytes, L"\\\\unavailable.invalid\\share\\icon.ico", true);
    Check(read(bytes).empty(), "remote icon resources stay out of the local first-image path");
    Check(resources::ReadLocalIconResources(L"\\\\unavailable.invalid\\share\\app.lnk").empty() &&
        resources::ReadLocalIconResources(link.wstring() + L":stream.lnk").empty(),
        "remote shortcuts and alternate streams are rejected before file reads");
    const auto url = temporary.Path() / L"local.url";
    WritePrivateProfileStringW(L"InternetShortcut", L"IconFile", L"icon.ico", url.c_str());
    Check(resources::ReadLocalIconResources(url.wstring()).size() == 1,
        "Internet shortcut explicit icons also use the local path");
}
