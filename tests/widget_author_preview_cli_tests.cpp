#include <windows.h>

#include <array>
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

std::pair<int, std::string> Run(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments)
{
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    Check(CreatePipe(&readPipe, &writePipe, &security, 0) != FALSE,
        "preview output pipe is created");
    Check(SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0) != FALSE,
        "preview output read handle is private to the test");

    std::wstring command = Quote(executable.wstring());
    for (const auto& argument : arguments)
        command += L" " + Quote(argument);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL launched = CreateProcessW(executable.c_str(),
        mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    Check(launched != FALSE, "snowwidget preview process starts");
    CloseHandle(process.hThread);

    std::string output;
    std::array<char, 1024> buffer{};
    DWORD read = 0;
    while (ReadFile(readPipe, buffer.data(),
            static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0)
        output.append(buffer.data(), read);
    CloseHandle(readPipe);
    Check(WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0,
        "snowwidget preview process completes");
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return { static_cast<int>(exitCode), std::move(output) };
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
    const auto [exitCode, json] = Run(snowwidget, {
        L"preview", source.wstring(), output.wstring(),
        L"--dpi", L"144", L"--storage", L"showNumbers=1",
        L"--host", host.wstring() });
    Check(exitCode == 0 &&
            json.find("\"ok\":true") != std::string::npos &&
            json.find("\"stage\":\"complete\"") != std::string::npos &&
            json.find("\"columns\":2") != std::string::npos &&
            json.find("\"rows\":2") != std::string::npos &&
            json.find("\"dpi\":144") != std::string::npos,
        "snowwidget preview reports a completed real API v2 render");
    CheckPng(output);

    const auto invalidOutput = temporary.path / L"invalid.png";
    const auto [invalidExit, invalidJson] = Run(snowwidget, {
        L"preview", source.wstring(), invalidOutput.wstring(),
        L"--columns", L"8", L"--host", host.wstring() });
    Check(invalidExit != 0 &&
            invalidJson.find("\"stage\":\"request.size\"") !=
                std::string::npos &&
            !std::filesystem::exists(invalidOutput),
        "preview rejects a size outside the manifest before rendering");

    std::cout << "widget author preview CLI tests passed\n";
    return 0;
}
