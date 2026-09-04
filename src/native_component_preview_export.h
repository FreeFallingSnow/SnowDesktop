#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::native_component_preview
{

struct Request
{
    std::string component = "collection";
    std::filesystem::path outputDirectory;
    unsigned dpi = 144;
    std::string locale = "en-US";
    std::string appearance = "dark";
    std::filesystem::path backgroundImage;
    int canvasWidth = 1280;
    int canvasHeight = 720;
    int padding = 72;
    bool transparent = false;
};

struct Output
{
    std::string component;
    std::string preset;
    std::filesystem::path path;
    int componentWidth = 0;
    int componentHeight = 0;
    int placementX = 0;
    int placementY = 0;
};

struct Result
{
    bool ok = false;
    std::string stage;
    std::string error;
    Request request;
    std::vector<Output> outputs;

    std::string ToJson() const;
};

/** Handle the private SnowDesktop render-host command when present. */
int TryRunHostCommand(HINSTANCE instance, bool& handled);

} // namespace snowdesktop::native_component_preview
