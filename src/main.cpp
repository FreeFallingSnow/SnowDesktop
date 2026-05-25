#include "app.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    if (commandLine != nullptr && wcsstr(commandLine, L"--restore-explorer-icons") != nullptr)
    {
        RestoreExplorerIconLayerNow();
        return 0;
    }

    UINT smokeTestMs = 0;
    if (commandLine != nullptr)
    {
        const wchar_t* smokeArg = wcsstr(commandLine, L"--smoke-test-ms=");
        if (smokeArg != nullptr)
        {
            smokeArg += wcslen(L"--smoke-test-ms=");
            smokeTestMs = static_cast<UINT>(std::max(0, _wtoi(smokeArg)));
        }
    }

    SnowDesktopApp app;
    return app.Run(instance, showCommand, smokeTestMs);
}
