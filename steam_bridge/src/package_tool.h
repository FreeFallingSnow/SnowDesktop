// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::steam_bridge
{
struct WidgetInspection
{
    bool valid = false;
    std::string packageId;
    std::string slug;
    std::string version;
    std::string name;
    std::string description;
    std::string author;
    std::string license;
    std::filesystem::path preview;
    std::vector<std::string> permissions;
    std::vector<std::string> networkDomains;
    std::string validationJson;
};

struct PackagedWidget
{
    std::filesystem::path temporaryDirectory;
    std::filesystem::path packagePath;
    std::string packageId;
    std::string version;
    std::string sha256;

    PackagedWidget() = default;
    ~PackagedWidget();
    PackagedWidget(PackagedWidget&& other) noexcept;
    PackagedWidget& operator=(PackagedWidget&& other) noexcept;
    PackagedWidget(const PackagedWidget&) = delete;
    PackagedWidget& operator=(const PackagedWidget&) = delete;
    void Cleanup();
};

class PackageTool
{
public:
    explicit PackageTool(std::filesystem::path executable = {});

    bool Inspect(const std::filesystem::path& source,
        WidgetInspection& inspection, std::string& error) const;
    bool Pack(const std::filesystem::path& source,
        const WidgetInspection& expected, PackagedWidget& package,
        std::string& error) const;
    const std::filesystem::path& Executable() const { return executable_; }

private:
    bool Run(const std::vector<std::wstring>& arguments,
        std::string& output, std::string& error, unsigned timeoutMs) const;
    std::filesystem::path executable_;
};

std::wstring QuoteWindowsArgument(std::wstring_view argument);
}
