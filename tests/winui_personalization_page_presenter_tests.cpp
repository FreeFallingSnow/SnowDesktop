#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void TestPresenterContract(const std::filesystem::path& repository)
{
    const std::string header = ReadText(repository /
        "src/winui/personalization_page_presenter.h");
    const std::string source = ReadText(repository /
        "src/winui/personalization_page_presenter.cpp");
    const std::string controls = ReadText(repository /
        "src/winui/settings_presenter_controls.h");

    Check(!header.empty() && !source.empty() && !controls.empty(),
        "personalization presenter and shared control sources are readable");
    Check(header.find("SettingsUpdateMode mode") != std::string::npos &&
            header.find("std::uint64_t generation") != std::string::npos &&
            header.find("Edit edit") != std::string::npos,
        "actions carry generation, update mode, and a latest-state edit");
    Check(header.find("ThemeContent() const noexcept") !=
                std::string::npos &&
            header.find("WidgetLayoutContent() const noexcept") !=
                std::string::npos &&
            header.find("\n        Content() const noexcept") ==
                std::string::npos &&
            source.find("impl_->themeRoot") != std::string::npos &&
            source.find("impl_->widgetLayoutRoot") != std::string::npos,
        "theme and widget layout expose independent cached page roots");
    Check(source.find(
              "InitializeCard(themeTargetsCard, cardStyle, themeRoot)") !=
                std::string::npos &&
            source.find(
              "InitializeCard(widgetAppearanceCard, cardStyle, themeRoot)") !=
                std::string::npos &&
            source.find(
              "InitializeCard(contextMenuCard, cardStyle, themeRoot)") !=
                std::string::npos &&
            source.find(
              "InitializeCard(layoutCard, cardStyle, widgetLayoutRoot)") !=
                std::string::npos &&
            source.find("customCard") == std::string::npos,
        "custom theme controls stay on the theme page while layout owns the widget page");
    Check(source.find("snapshot.domainRevisions.personalization") !=
                std::string::npos &&
            source.find("SettingsSnapshot snapshot_") == std::string::npos &&
            source.find("PersonalizationSettings settings_") ==
                std::string::npos &&
            source.find("updatingControls") != std::string::npos,
        "snapshot patching is domain-revision based and suppresses feedback");
    Check(source.find("SettingsUpdateMode::Preview,") !=
                std::string::npos &&
            source.find("SettingsUpdateMode::PreviewAndCommit,") !=
                std::string::npos &&
            source.find("PointerReleased") != std::string::npos &&
            source.find("LostFocus") != std::string::npos &&
            source.find("IsEnter(args)") != std::string::npos,
        "continuous controls preview and commit on interaction boundaries");
    Check(source.find("QuantizeNumericValue(value,") !=
                std::string::npos &&
            controls.find("inline double QuantizeNumericValue(") !=
                std::string::npos,
        "personalization numeric snapshots are quantized to their declared slider step");

    for (const char* member : {
             "widgetAlpha", "widgetBorderAlpha", "widgetBorderStyle",
             "widgetBorderWidth", "widgetBorderEffectStrength",
             "gradientEndA",
             "glassBlurRadius", "cornerRadius", "barHeight",
             "categorizedTabHeight",
             "glassEnabled", "acrylicEnabled",
             "contentTheme", "contextMenuStyle",
             "widgetBgR", "widgetBorderR"})
    {
        Check(source.find(member) != std::string::npos,
            "the declared personalization field has a real WinUI binding");
    }
    Check(source.find("ContinuousControl categorizedTabHeight") !=
                std::string::npos &&
            source.find(
              "&PersonalizationSettings::categorizedTabHeight") !=
                std::string::npos &&
            source.find("SetContinuousText(categorizedTabHeight") !=
                std::string::npos &&
            source.find("\"desktop.categoryLayout\"") !=
                std::string::npos &&
            source.find("\"personalization.tabHeight\"") !=
                std::string::npos &&
            source.find("showCategoryTabCountsToggle") ==
                std::string::npos &&
            source.find("showCategoryCountsRow") == std::string::npos &&
            source.find("app.settings.category_show_count") ==
                std::string::npos,
        "widget layout owns category tab height while category count behavior stays elsewhere");
    for (const char* control : {
             "muxc::Slider", "muxc::NumberBox",
             "muxc::ToggleSwitch", "muxc::ComboBox"})
    {
        Check(source.find(control) != std::string::npos,
            "personalization uses the required native WinUI control type");
    }
    Check(source.find("ColorFlyoutEditor editor") != std::string::npos &&
            controls.find("muxc::ColorPicker picker") != std::string::npos,
        "personalization colors use the shared native WinUI ColorPicker flyout");
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
            source.find("control.preview.Queue(uiValue)") !=
                std::string::npos &&
            controls.find("kContinuousPreviewInterval{33}") !=
                std::string::npos &&
            controls.find("applyToken = apply.Click") ==
                std::string::npos,
        "personalization previews are frame-coalesced and colors commit on light-dismiss or page teardown while explicit Cancel rolls back");
    Check(source.find("kAppearancePresetDark") != std::string::npos &&
            source.find("kAppearancePresetLight") != std::string::npos &&
            source.find("kAppearancePresetGlassDark") != std::string::npos &&
            source.find("kAppearancePresetGlassLight") != std::string::npos &&
            source.find("kAppearancePresetAcrylicDark") !=
                std::string::npos &&
            source.find("kAppearancePresetAcrylicLight") !=
                std::string::npos &&
            source.find("kAppearancePresetCustom") != std::string::npos,
        "all seven global appearance presets remain selectable");
    Check(source.find("personalization.borderWidth") != std::string::npos &&
            source.find("personalization.borderStyle") != std::string::npos &&
            source.find("personalization.borderEffectStrength") !=
                std::string::npos &&
            source.find("PanelBorderStyle::Dimensional") !=
                std::string::npos &&
            source.find("borderEffectStrength.row.SetEnabled") !=
                std::string::npos,
        "custom themes expose searchable border width, style, and conditional dimensional strength controls");
    Check(source.find(
              "gradientToggle.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find(
              "glassToggle.HorizontalAlignment(mux::HorizontalAlignment::Right)") !=
                std::string::npos &&
            source.find("glassRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find("acrylicRow.SetControlAlignment(") !=
                std::string::npos &&
            source.find(
              "control.reset.VerticalAlignment(mux::VerticalAlignment::Center)") !=
                std::string::npos,
        "compact personalization toggles align right and every inline default reset is vertically centered");
    Check(source.find("control.editor.Close()") != std::string::npos &&
            controls.find("picker.ColorChanged(colorToken)") !=
                std::string::npos &&
            source.find("ValueChanged(control.sliderChanged)") !=
                std::string::npos &&
            source.find("SelectionChanged(presetToken)") !=
                std::string::npos,
        "Close revokes ColorPicker, continuous, and discrete event handlers");
}
}

int main(int argc, char** argv)
{
    Check(argc == 2,
        "source root is supplied for the personalization presenter contract");
    if (argc == 2)
        TestPresenterContract(std::filesystem::path(argv[1]));

    if (failures != 0)
    {
        std::cerr << failures
                  << " WinUI personalization presenter check(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "WinUI personalization presenter checks passed\n";
    return EXIT_SUCCESS;
}
