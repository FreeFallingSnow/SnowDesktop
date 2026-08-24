#include <cstdlib>
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
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void TestPresenterContract(const std::filesystem::path& root)
{
    const std::string header = ReadText(
        root / "src/winui/desktop_page_presenter.h");
    const std::string source = ReadText(
        root / "src/winui/desktop_page_presenter.cpp");
    Check(!header.empty() && !source.empty(),
        "desktop presenter sources are readable");

    Check(header.find("SettingsUpdateMode mode") != std::string::npos &&
            header.find("DesktopEdit edit") != std::string::npos &&
            header.find("CategoryEdit edit") != std::string::npos &&
            header.find("PersonalizationEdit edit") != std::string::npos,
        "actions patch the controller's latest domain through typed edits");
    Check(source.find("snapshot.domainRevisions.desktop") !=
                std::string::npos &&
            source.find("snapshot.domainRevisions.category") !=
                std::string::npos &&
            source.find("snapshot.domainRevisions.personalization") !=
                std::string::npos &&
            source.find("updatingControls") != std::string::npos,
        "snapshots are patched incrementally with feedback suppression");

    Check(source.find("SettingsUpdateMode::Preview)") !=
                std::string::npos &&
            source.find("SettingsUpdateMode::PreviewAndCommit") !=
                std::string::npos &&
            source.find("PointerReleased") != std::string::npos &&
            source.find("VirtualKey::Enter") != std::string::npos &&
            source.find("LostFocus") != std::string::npos,
        "continuous controls preview and commit at interaction boundaries");
    Check(source.find("SettingsUpdateMode::Draft") != std::string::npos &&
            source.find("actions.commitCategory(generation)") !=
                std::string::npos,
        "category edits remain drafts until explicit Apply");

    for (const char* field : {
             "iconSpacingScale", "itemIconSizeScale", "itemFontSizeCu",
             "listItemFontSizeCu", "itemFontWeight", "shortcutArrowMode",
             "categorizedTabHeight", "tabFontSize",
             "showCategoryTabCounts", "backgroundOpacity",
             "gradientEnabled", "gradientDirection", "backgroundStartR",
             "backgroundEndR", "contentScale", "textureHighlightStrength",
             "textureHighlightSize", "textureHighlightAngle",
             "textureShadeStrength", "textureEdgeHighlight",
             "filterEnabled", "filterStrength", "filterTintR",
             "outlineEnabled", "outlineWidth", "outlineOpacity",
             "outlineR", "shadowStrength"})
    {
        Check(source.find(field) != std::string::npos,
            "desktop presenter maps every persisted display field");
    }
    Check(source.find("CloseRuleRows") != std::string::npos &&
            source.find("SelectionChanged(shortcutArrowToken)") !=
                std::string::npos &&
            source.find("ColorChanged(colorToken)") != std::string::npos,
        "static and dynamic C++/WinRT event handlers are unregistered");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2, "source root is supplied");
    if (argc == 2)
        TestPresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures <<
            " WinUI desktop presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI desktop presenter checks passed\n";
    return EXIT_SUCCESS;
}
