#include "settings_window_open_rules.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}
}

int main(int argc, char** argv)
{
    using snowdesktop::settings_window_open_rules::RequestState;

    RequestState state;
    Check(!state.Pending(), "new state has no pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "request becomes pending and resets retry count");
    Check(state.RecordFailure(3) && state.RetryCount() == 1,
        "first failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 2,
        "second failure schedules a retry");
    Check(state.RecordFailure(3) && state.RetryCount() == 3,
        "third failure schedules the final retry");
    Check(!state.RecordFailure(3) && state.Pending(),
        "retry exhaustion preserves the pending request");

    state.Request();
    Check(state.Pending() && state.RetryCount() == 0,
        "a new user request restores the retry budget");
    state.MarkShown();
    Check(!state.Pending() && state.RetryCount() == 0,
        "successful display clears pending state and retries");
    Check(!state.RecordFailure(3),
        "completed requests cannot schedule retries");

    Check(argc == 2, "source root argument is provided");
    if (argc == 2)
    {
        const std::string source = ReadFile(
            std::filesystem::path(argv[1]) / "src" / "settings_window.cpp");
        const std::size_t newFrame = source.find("ImGui::NewFrame();");
        const std::size_t physicalCursor = source.find("GetCursorPos(&mp);");
        const std::size_t correctedMousePos = source.find(
            "ImGui::GetIO().MousePos = ImVec2((float)mp.x, (float)mp.y);");
        Check(!source.empty(), "settings window source is readable");
        Check(newFrame != std::string::npos &&
                physicalCursor != std::string::npos &&
                correctedMousePos != std::string::npos &&
                newFrame < physicalCursor && physicalCursor < correctedMousePos,
            "physical cursor correction follows queued ImGui input processing");
    }

    if (failures == 0)
        std::cout << "All settings window open rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
