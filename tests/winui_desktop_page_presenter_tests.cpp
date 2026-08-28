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
    const std::string controls = ReadText(
        root / "src/winui/settings_presenter_controls.h");
    const std::string categoryHeader = ReadText(
        root / "src/category_settings.h");
    const std::string categorySource = ReadText(
        root / "src/category_settings.cpp");
    Check(!header.empty() && !source.empty() && !controls.empty() &&
            !categoryHeader.empty() && !categorySource.empty(),
        "desktop presenter, shared controls, and category persistence sources are readable");

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
            source.find("LostFocus") != std::string::npos &&
            source.find("mux::DispatcherTimer") != std::string::npos &&
            source.find("preview.Queue(value)") != std::string::npos &&
            controls.find("kContinuousPreviewInterval{33}") !=
                std::string::npos &&
            source.find("std::chrono::milliseconds(650)") !=
                std::string::npos,
        "continuous controls coalesce host previews while committing the final value on release, focus, Enter, or keyboard idle");
    Check(source.find("bool interactionActive = false") !=
                std::string::npos &&
            source.find("interactionActive = true") !=
                std::string::npos &&
            source.find("if (interactionActive) return;") !=
                std::string::npos &&
            source.find("HasActiveDesktopEdit()") !=
                std::string::npos &&
            source.find("if (newGeneration || !desktopEditActive)") !=
                std::string::npos &&
            controls.find("if (open) return;") != std::string::npos,
        "live desktop slider and color edits defer the complete desktop snapshot patch while interaction remains active");
    Check(source.find("QuantizeNumericValue(") != std::string::npos &&
            controls.find("inline double QuantizeNumericValue(") !=
                std::string::npos &&
            controls.find("std::round((value - minimum) / step)") !=
                std::string::npos,
        "desktop numeric snapshots and edits are quantized to the declared control step");
    Check(source.find("void CommitOpenColorEditors() noexcept") !=
                std::string::npos &&
            source.find("impl_->CommitOpenColorEditors();") !=
                std::string::npos &&
            controls.find("pointerReleasedToken = picker.PointerReleased") !=
                std::string::npos &&
            controls.find("lostFocusToken = picker.LostFocus") !=
                std::string::npos &&
            controls.find("keyDownToken = picker.KeyDown") !=
                std::string::npos &&
            controls.find("changed(original, SettingsUpdateMode::PreviewAndCommit)") !=
                std::string::npos &&
            controls.find("if (!canceled)") !=
                std::string::npos &&
            controls.find("applyToken = apply.Click") ==
                std::string::npos,
        "desktop colors coalesce live previews, commit on light-dismiss or page close, and roll back only through explicit Cancel");
    Check(source.find("iconSpacing->SetUnit(L\"%\")") !=
                std::string::npos &&
            source.find("itemFontSize->SetUnit(L\"cu\")") !=
                std::string::npos &&
            source.find("highlightAngle->SetUnit(L\"°\")") !=
                std::string::npos &&
            source.find("outlineWidth->SetUnit(L\"px\")") !=
                std::string::npos,
        "legacy percent, coordinate-unit, degree, and pixel suffixes remain visible");
    Check(source.find("SettingsUpdateMode::Draft") != std::string::npos &&
            source.find("actions.commitCategory(generation)") !=
                std::string::npos,
        "category edits remain drafts until explicit Apply");
    Check(source.find("UpdateCategory(SettingsUpdateMode::Commit") !=
                std::string::npos,
        "restoring category defaults saves immediately like the legacy page");
    Check(source.find(
              "SetCardText(displayCard, \"app.settings.desktop_icons\"") !=
                std::string::npos,
        "Desktop Icons remains the legacy group title above display settings");

    for (const char* field : {
             "iconSpacingScale", "itemIconSizeScale", "itemFontSizeCu",
             "listItemFontSizeCu", "itemFontWeight", "shortcutArrowMode",
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
    Check(source.find("tabFontSize") == std::string::npos &&
            source.find("app.settings.category_font_size") ==
                std::string::npos &&
            categoryHeader.find("float tabFontSize") !=
                std::string::npos &&
            categorySource.find(
              "ReadDoubleField(text, \"tabFontSize\"") !=
                std::string::npos &&
            categorySource.find("bool SaveCategorySettings(") !=
                std::string::npos &&
            categorySource.find("normalized.tabFontSize") !=
                std::string::npos,
        "the retired category-tab font-size has no presenter UI while legacy configuration still loads and saves the field");
    const auto displaySection = source.find(
        "InitializeCard(displayCard, cardStyle, desktopIconsRoot)");
    const auto beautifySection = source.find(
        "beautifyCard, cardStyle, iconBeautificationRoot)");
    const auto categoryLayoutSection = source.find(
        "InitializeCard(categoryLayoutCard, cardStyle, categoryRoot)");
    Check(displaySection != std::string::npos &&
            beautifySection != std::string::npos &&
            categoryLayoutSection != std::string::npos &&
            source.find("hiddenCompatibilityRoot") == std::string::npos &&
            header.find("DesktopIconsContent() const noexcept") !=
                std::string::npos &&
            header.find("IconBeautificationContent() const noexcept") !=
                std::string::npos &&
            header.find("AppearanceContent() const noexcept") ==
                std::string::npos &&
            source.find("DesktopPagePresenter::DesktopIconsContent()") !=
                std::string::npos &&
            source.find("return impl_ ? impl_->desktopIconsRoot : nullptr") !=
                std::string::npos &&
            source.find(
              "DesktopPagePresenter::IconBeautificationContent()") !=
                std::string::npos &&
            source.find(
              "return impl_ ? impl_->iconBeautificationRoot : nullptr") !=
                std::string::npos &&
            source.find("DesktopPagePresenter::CategoryContent()") !=
                std::string::npos &&
            source.find("return impl_ ? impl_->categoryRoot : nullptr") !=
                std::string::npos,
        "desktop icons and icon beautification have independent roots while Categories exposes its count and classification controls");
    Check(source.find("categorizedTabHeight") == std::string::npos &&
            source.find("app.settings.tab_height") == std::string::npos &&
            source.find("desktop.tabHeight") == std::string::npos,
        "category-tab height has moved out of the Desktop presenter without a hidden compatibility editor");
    Check(source.find("CloseRuleRows") != std::string::npos &&
            source.find("SelectionChanged(shortcutArrowToken)") !=
                std::string::npos &&
            source.find("editor.Close()") != std::string::npos &&
            controls.find("picker.ColorChanged(colorToken)") !=
                std::string::npos,
        "static and dynamic C++/WinRT event handlers are unregistered");
    Check(source.find("MakeDesktopNumber(50.0, 200.0, 1.0, 0") !=
                std::string::npos &&
            source.find("kMinimumItemIconSizeScale * 100.0") !=
                std::string::npos &&
            source.find("MakeBeautifyNumber(-45.0, 45.0, 1.0, 0") !=
                std::string::npos &&
            source.find("value.textureHighlightAngle * 45.0") !=
                std::string::npos,
        "legacy percentage and degree units are preserved in WinUI editors");
    Check(source.find("filterDetails.Visibility(filterEnabled.IsOn()") !=
                std::string::npos &&
            source.find("outlineDetails.Visibility(outlineEnabled.IsOn()") !=
                std::string::npos &&
            source.find("mux::Visibility::Collapsed") !=
                std::string::npos,
        "disabled filter and outline children collapse like the legacy page");
    Check(source.find("row->nameActions.Children().Append(row->remove)") !=
                std::string::npos &&
            source.find("newCategoryNameActions.Children().Append(addCategory)") !=
                std::string::npos &&
            source.find("categoryActionsRow.Initialize(categoryActions)") !=
                std::string::npos,
        "category name/delete, add, and save actions retain legacy row alignment");
    const auto beautifyPresetRow = source.find(
        "AppendCombo(beautifyCard, beautifyPresetRow, beautifyPreset)");
    const auto beautifyAdvanced = source.find(
        "beautifyCard.content.Children().Append(beautifyAdvanced)");
    Check(source.find(
              "showCategoryTabCountsRow.Initialize(showCategoryTabCounts)") !=
                std::string::npos &&
            source.find("showCategoryTabCountsRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("showCategoryTabCountsRow.root") !=
                std::string::npos &&
            source.find("showCategoryTabCounts.Header(") ==
                std::string::npos &&
            source.find("beautifyEnabled") == std::string::npos &&
            source.find("desktop.iconBeautify.enabled = true") !=
                std::string::npos &&
            beautifyPresetRow < beautifyAdvanced,
        "category counts use a right-aligned row while the legacy None, Default, and Custom preset remains the only beautification enablement UI");
    Check(source.find(
              "gradientEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "filterEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "outlineEnabled.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "reset.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos &&
            source.find(
              "reset.VerticalContentAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos &&
            source.find(
              "categoryActions.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "restoreCategory.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos &&
            source.find("categoryActionsRow.SetControlAlignment(") !=
                std::string::npos,
        "desktop toggles and action groups align right while inline restore buttons center both their frame and content vertically");
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
