#include "behavior_fixture.h"
#include "common/json_value.h"
#include "widget/packages/widget_package.h"
#include <algorithm>
#include <iostream>
#include <set>

namespace behavior
{
std::string ManifestString(const JsonValue& object, std::string_view key)
{
    const auto* value = object.Find(key);
    Require(value && value->IsString() && !value->string.empty(), "manifest has a nonempty " + std::string(key));
    return value->string;
}
void BuildManifest(const fs::path& output)
{
    JsonValue manifest; std::string error;
    const bool parsed = ParseJson(Read(output/L"SnowDesktop.deployment.json"),manifest,&error);
    Require(parsed,"parse actual deployment inventory: "+error);
    const auto* schema = manifest.Find("schemaVersion");
    Require(schema && schema->IsNumber() && schema->number == 1,"supported deployment inventory schema");
    std::set<std::string> paths;
    for (const auto* collection : {"files","appxFragments","notices"})
    {
        const auto* rows = manifest.Find(collection);
        Require(rows && rows->IsArray() && !rows->array.empty(),"actual deployment collection: "+std::string(collection));
        std::set<std::string> unique;
        for (const auto& row : rows->array)
        {
            const auto path = ManifestString(row,"path");
            Require(unique.insert(path).second,"duplicate inventory path: "+path);
            const auto* alternate = row.Find("buildPath");
            const auto physical = alternate ? ManifestString(row,"buildPath") : path;
            const fs::path relative = fs::u8path(physical);
            Require(!relative.is_absolute() && !relative.has_root_name() &&
                std::none_of(relative.begin(),relative.end(),[](const auto& part) { return part == L".."; }),
                "inventory path must stay inside its build output");
            const auto file = output / relative;
            Require(fs::is_regular_file(file),"missing inventory file: "+physical);
            auto actual = snowdesktop::widget::WidgetPackageManager::Sha256File(file);
            auto expected = ManifestString(row,"sha256");
            const auto upper=[](unsigned char c) { return static_cast<char>(c >= 'a' && c <= 'f' ? c-'a'+'A' : c); };
            std::transform(actual.begin(),actual.end(),actual.begin(),upper);
            std::transform(expected.begin(),expected.end(),expected.begin(),upper);
            Require(actual.size()==64 && actual==expected,"actual inventory hash mismatch: "+physical);
            if (std::string_view(collection)=="files") paths.insert(path);
        }
    }
    for (const auto* required : {"App.xbf","SettingsShell.xbf","SnowDesktop.pri","SnowDesktop.winmd",
        "Microsoft.ui.xaml.dll","Microsoft.WindowsAppRuntime.dll","Microsoft.UI.Xaml.winmd",
        "Assets/Fonts/MiSans/MiSans-Regular.otf","Assets/Fonts/HarmonyOS-Sans/HarmonyOS_Sans_SC_Regular.ttf"})
        Require(paths.contains(required),"mandatory deployed UI/runtime asset missing from inventory: "+std::string(required));
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        behavior::scene = "manifest";
        behavior::Require(argc == 2, "actual product output directory is required");
        behavior::BuildManifest(argv[1]);
        std::cout << "PASS manifest\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
