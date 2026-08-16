#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

std::wstring Quote(std::wstring_view value)
{
    return L"\"" + std::wstring(value) + L"\"";
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        wchar_t root[MAX_PATH]{};
        Check(GetTempPathW(MAX_PATH, root) != 0,
            "temporary root is available");
        path = std::filesystem::path(root) /
            (L"SnowDesktopPreviewTest-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()));
        std::error_code error;
        Check(std::filesystem::create_directory(path, error),
            "temporary preview directory is created");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

std::pair<int, std::string> Run(std::wstring command)
{
    FILE* pipe = _wpopen(command.c_str(), L"rt");
    Check(pipe != nullptr, "snowwidget preview process starts");
    std::string output;
    std::array<char, 1024> buffer{};
    while (std::fgets(buffer.data(),
            static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    return { _pclose(pipe), std::move(output) };
}

void CheckPng(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(static_cast<bool>(input), "preview PNG exists");
    std::array<unsigned char, 8> signature{};
    input.read(reinterpret_cast<char*>(signature.data()), signature.size());
    constexpr std::array<unsigned char, 8> expected{
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    Check(signature == expected, "preview output has a PNG signature");
    std::error_code error;
    Check(std::filesystem::file_size(path, error) > 512 && !error,
        "real renderer writes nontrivial preview pixels");
}
}

int wmain(int argc, wchar_t** argv)
{
    Check(argc == 4,
        "test receives snowwidget, SnowDesktop, and repository root");
    const std::filesystem::path snowwidget = argv[1];
    const std::filesystem::path host = argv[2];
    const std::filesystem::path repository = argv[3];
    Check(std::filesystem::is_regular_file(snowwidget),
        "snowwidget executable exists");
    Check(std::filesystem::is_regular_file(host),
        "SnowDesktop preview host exists");

    TemporaryDirectory temporary;
    const auto output = temporary.path / L"analog-clock.png";
    const auto source = repository / L"widgets" / L"analog-clock";
    const std::wstring command = Quote(snowwidget.wstring()) +
        L" preview " + Quote(source.wstring()) + L" " +
        Quote(output.wstring()) + L" --dpi 144 --storage showNumbers=1"
        L" --host " + Quote(host.wstring());
    const auto [exitCode, json] = Run(command);
    Check(exitCode == 0 &&
            json.find("\"ok\":true") != std::string::npos &&
            json.find("\"stage\":\"complete\"") != std::string::npos &&
            json.find("\"columns\":2") != std::string::npos &&
            json.find("\"rows\":2") != std::string::npos &&
            json.find("\"dpi\":144") != std::string::npos,
        "snowwidget preview reports a completed real API v2 render");
    CheckPng(output);

    const auto invalidOutput = temporary.path / L"invalid.png";
    const std::wstring invalid = Quote(snowwidget.wstring()) +
        L" preview " + Quote(source.wstring()) + L" " +
        Quote(invalidOutput.wstring()) + L" --columns 8 --host " +
        Quote(host.wstring());
    const auto [invalidExit, invalidJson] = Run(invalid);
    Check(invalidExit != 0 &&
            invalidJson.find("\"stage\":\"request.size\"") !=
                std::string::npos &&
            !std::filesystem::exists(invalidOutput),
        "preview rejects a size outside the manifest before rendering");

    std::cout << "widget author preview CLI tests passed\n";
    return 0;
}
