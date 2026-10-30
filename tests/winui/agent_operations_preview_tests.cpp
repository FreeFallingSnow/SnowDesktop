#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../test_temporary_directory.h"
#include <fstream>
#include <iostream>
#include <iterator>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Preview(const std::filesystem::path& host, const wchar_t* locale,
    const wchar_t* theme, int width)
{
    snowdesktop::test::TemporaryDirectory temporary;
    const auto output = temporary.path / L"preview";
    auto command = L"\"" + host.wstring() + L"\" --settings-visual-preview \"" + output.wstring() +
        L"\" agent-operations " + theme + L" " + locale + L" " + std::to_wstring(width) + L" 1050 expanded";
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process) != FALSE, "isolated settings renderer starts");
    const auto wait = WaitForSingleObject(process.hProcess, 25000);
    if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 99); WaitForSingleObject(process.hProcess, 5000); }
    DWORD code = 999; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    const auto result = Read(output / L"result.txt");
    if (code || result.find("passed") != 0) std::cerr << result << '\n';
    Check(wait == WAIT_OBJECT_0 && code == 0 && result.find("passed") == 0,
        "production XAML page activates and accepts its six-assistant snapshot");
    Check(std::filesystem::is_regular_file(output / L"agent-operations-0.png") &&
        std::filesystem::file_size(output / L"agent-operations-0.png") > 4096,
        "the new route renders a real page image rather than an empty fixture");
    Check(Read(output / L"render.tsv").find("agent-operations") != std::string::npos,
        "the rendered surface belongs to the requested operation page");
}
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        Check(argc == 2, "host executable argument required");
        Preview(argv[1], L"zh-CN", L"light", 1000);
        Preview(argv[1], L"en-US", L"dark", 1000);
        Preview(argv[1], L"de-DE", L"light", 840);
        std::cout << "Agent operations production XAML preview checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
