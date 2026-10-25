#include "pch.h"
#include "settings_visual_preview.h"
#include "icon_library_page_presenter.h"
#include "settings/settings_search_catalog.h"
#include "SettingsShell.xaml.h"
#include "icons/icon_appearance_rules.h"
#include "icons/icon_control_source.h"
#include "icons/icon_image_codec.h"
#include "winui_runtime.h"
#include "common/l10n.h"
#include "data/data_paths.h"
#include "ui/preview/preview_png_writer.h"
#include <robuffer.h>
#include <shellapi.h>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Globalization.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace wr = widget_runtime;
namespace
{
template<class Ready> void PumpUntil(Ready ready)
{
    const auto deadline = GetTickCount64() + 15000;
    while (!ready())
    {
        if (GetTickCount64() >= deadline) throw std::runtime_error("settings render timed out");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) throw std::runtime_error("settings render closed");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!ready()) MsgWaitForMultipleObjectsEx(0, nullptr, 15, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
void Settle(const x::FrameworkElement& root)
{
    const auto until = GetTickCount64() + 180;
    PumpUntil([&] { root.UpdateLayout(); return GetTickCount64() >= until; });
}
template<class Operation> auto Await(const Operation& operation)
{
    try { PumpUntil([&] { return operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started; }); }
    catch (...) { operation.Cancel(); throw; }
    return operation.GetResults();
}
struct RenderHost
{
    WinUiRuntime runtime;
    HWND window = nullptr;
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* runtime = reinterpret_cast<WinUiRuntime*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            runtime = static_cast<WinUiRuntime*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(runtime));
        }
        if (runtime) runtime->HandleWindowMessage(message, wParam, lParam);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    ~RenderHost() { runtime.Detach(); if (window) DestroyWindow(window); runtime.Shutdown(); }
};
struct Apartment
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
};
template<class Visit> void Walk(const x::DependencyObject& node, Visit&& visit)
{
    visit(node);
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(node); ++i)
        Walk(x::Media::VisualTreeHelper::GetChild(node, i), visit);
}

bool IsPreviewElementVisible(const x::FrameworkElement& element)
{
    if (!element || element.ActualWidth() <= 0 || element.ActualHeight() <= 0) return false;
    auto node = element.as<x::DependencyObject>();
    while (node)
    {
        if (const auto current = node.try_as<x::UIElement>();
            current && current.Visibility() != x::Visibility::Visible) return false;
        node = x::Media::VisualTreeHelper::GetParent(node);
    }
    return true;
}

template<class Control> Control PreviewControl(const x::DependencyObject& root, std::wstring_view id)
{
    Control found{nullptr};
    Walk(root, [&](const x::DependencyObject& node) {
        if (const auto element = node.try_as<x::FrameworkElement>(); element &&
            x::Automation::AutomationProperties::GetAutomationId(element) == id)
            found = element.try_as<Control>();
    });
    if (!found) throw std::runtime_error("missing preview control: " + winrt::to_string(id));
    return found;
}

void InspectPackageInlineLayout(const x::FrameworkElement& root,
    const std::filesystem::path& output, bool oldLineBoxes)
{
    namespace d = x::Documents;
    Settle(root);
    std::vector<c::RichTextBlock> rows;
    Walk(root, [&](const x::DependencyObject& node) {
        const auto text = node.try_as<c::RichTextBlock>();
        if (!text || text.Blocks().Size() != 1) return;
        const auto paragraph = text.Blocks().GetAt(0).try_as<d::Paragraph>();
        if (paragraph && paragraph.Inlines().Size() > 0 &&
            paragraph.Inlines().GetAt(0).try_as<d::InlineUIContainer>())
            rows.push_back(text);
    });
    if (rows.size() != 3) throw std::runtime_error("expected three package inline preview rows");
    if (oldLineBoxes)
    {
        // Isolated negative control: restore the preceding production line-box
        // settings while retaining its real tag, font, text and wrapping path.
        for (const auto& row : rows)
        {
            row.LineStackingStrategy(x::LineStackingStrategy::MaxHeight);
            row.LineHeight(0.0);
            row.Padding({0, 0, 0, 0});
        }
        Settle(root);
    }
    std::ofstream metrics(output / L"package-inline-layout.tsv");
    metrics << "row\ttag_bottom\trow_height\tbaseline_error\tsecond_line_top\tsecond_line_x\tgap\n";
    bool foundWrappedDescription = false;
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        const auto& row = rows[index];
        const auto paragraph = row.Blocks().GetAt(0).as<d::Paragraph>();
        double tagBottom = 0.0, baselineError = 0.0;
        d::Run description{nullptr};
        for (const auto& inlineElement : paragraph.Inlines())
        {
            if (const auto container = inlineElement.try_as<d::InlineUIContainer>())
            {
                const auto tag = container.Child().as<c::Border>();
                const auto label = tag.Child().as<c::TextBlock>();
                const auto bounds = tag.TransformToVisual(row).TransformBounds(
                    {0, 0, static_cast<float>(tag.ActualWidth()), static_cast<float>(tag.ActualHeight())});
                tagBottom = std::max(tagBottom, static_cast<double>(bounds.Y + bounds.Height));
                const auto baseline = label.TransformToVisual(row).TransformPoint(
                    {0, static_cast<float>(label.BaselineOffset())});
                baselineError = std::max(baselineError,
                    std::abs(static_cast<double>(baseline.Y) - row.BaselineOffset()));
            }
            else if (const auto run = inlineElement.try_as<d::Run>())
                description = run;
        }
        double secondTop = -1.0, secondX = -1.0;
        if (description && !description.Text().empty())
        {
            const auto first = description.ContentStart().GetCharacterRect(d::LogicalDirection::Forward);
            for (std::uint32_t offset = 1; offset < description.Text().size(); ++offset)
            {
                const auto pointer = description.ContentStart().GetPositionAtOffset(
                    static_cast<int>(offset), d::LogicalDirection::Forward);
                if (!pointer) break;
                const auto bounds = pointer.GetCharacterRect(d::LogicalDirection::Forward);
                if (bounds.Y > first.Y + 1.0f)
                {
                    secondTop = bounds.Y;
                    secondX = bounds.X;
                    foundWrappedDescription = true;
                    break;
                }
            }
        }
        metrics << index << '\t' << tagBottom << '\t' << row.ActualHeight() << '\t'
            << baselineError << '\t' << secondTop << '\t' << secondX << '\t'
            << (secondTop < 0.0 ? -1.0 : secondTop - tagBottom) << '\n';
        metrics.flush();
        if (baselineError > 0.75) throw std::runtime_error("package tag and description baselines do not align");
        if (tagBottom > row.ActualHeight() + 0.75) throw std::runtime_error("package tag exceeds its line box");
        if (secondTop >= 0.0 && secondTop - tagBottom < 2.0)
            throw std::runtime_error("package tag overlaps the following description line");
        if (secondX > 0.75) throw std::runtime_error("wrapped package description does not start at the full left edge");
    }
    if (!foundWrappedDescription) throw std::runtime_error("package preview did not exercise a wrapped description");
}

void InvokePreview(const x::FrameworkElement& element)
{
    const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
    if (!peer) throw std::runtime_error("missing preview automation peer");
    peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke)
        .as<x::Automation::Provider::IInvokeProvider>().Invoke();
}

unsigned ExpandGradientDetails(const x::DependencyObject& cards, const x::FrameworkElement& shell)
{
    const winrt::hstring expandText{_LW("largeIcon.showDetails")};
    const winrt::hstring detailsName{_LW("panelGradient.more")};
    unsigned invoked = 0;
    for (;;)
    {
        c::Button target{nullptr};
        Walk(cards, [&](const x::DependencyObject& node) {
            const auto button = node.try_as<c::Button>();
            if (!target && button && button.IsEnabled() && IsPreviewElementVisible(button) &&
                winrt::unbox_value_or<winrt::hstring>(button.Content(), winrt::hstring{}) == expandText &&
                x::Automation::AutomationProperties::GetName(button) == detailsName) target = button;
        });
        if (!target) return invoked;
        if (invoked == 32) throw std::runtime_error("gradient details did not finish expanding");
        // Invoke after traversal: the production click handler rebuilds its rows.
        const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(target);
        if (!peer) throw std::runtime_error("gradient details automation peer is missing");
        peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke)
            .as<x::Automation::Provider::IInvokeProvider>().Invoke();
        ++invoked;
        Settle(shell);
    }
}

// Deliberately no backend: these are example author fields, never a user's
// component data. Only the explicit secret-input diagnostic accepts synthetic
// writes; it records lengths and status, never plaintext or real credentials.
class PreviewWidgetService final : public wr::IWidgetSettingsService
{
public:
    wr::WidgetSettingsSnapshot value;
    bool showSearch = false;
    bool allowSecretWrites = false;
    bool failSecretWrites = false;
    unsigned secretAttempts = 0;
    unsigned secretSuccesses = 0;
    std::size_t lastSecretBytes = 0;
    unsigned secretReads = 0;
    std::string syntheticSecret = "offline-existing";
    bool allControls = false;
    bool cancelNextPicker = false;
    std::map<std::string, wr::WidgetSettingSearchSnapshot, std::less<>> searches;
    SnapshotChangedCallback changed;
    SearchCompletedCallback searched;
    std::string searchResultTitle;
    explicit PreviewWidgetService(bool english, bool searching)
        : showSearch(searching), searchResultTitle(english ? "QQ Music" : "QQ音乐") // l10n-allow: isolated bilingual preview fixture, not production UI text.
    {
        value.widgetId = L"settings-visual-example"; value.packageId = "offline.example";
        value.widgetName = english ? "Example component" : "示例组件"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        value.generation = value.revision = 1;
        value.groups.push_back({"content", english ? "Content" : "内容", {}, false, true}); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        value.groups.push_back({"details", english ? "More options" : "更多选项", {}, true, false}); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        const auto add = [&](const char* key, const char* type, const char* zh, const char* en, wr::InteractionValue current) {
            wr::WidgetSettingFieldState field;
            field.schema.key = key; field.schema.rawType = type; field.schema.label = english ? en : zh;
            field.schema.group = "content"; field.currentValue = current; field.defaultValue = current;
            if (std::string_view(type) == "select") field.schema.options = {{"a", english ? "Automatic" : "自动"}, {"b", english ? "Custom" : "自定义"}}; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            value.fields.push_back(std::move(field));
        };
        wr::InteractionValue title; title.type = wr::InteractionValue::Type::String; title.string = "SnowDesktop";
        wr::InteractionValue enabled; enabled.type = wr::InteractionValue::Type::Boolean; enabled.boolean = true;
        wr::InteractionValue size; size.type = wr::InteractionValue::Type::Number; size.number = 50.;
        wr::InteractionValue mode; mode.type = wr::InteractionValue::Type::String; mode.string = "a";
        wr::InteractionValue count; count.type = wr::InteractionValue::Type::Integer; count.integer = 5;
        add("title", "text", "标题", "Title", title); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("enabled", "bool", "显示详细内容", "Show detailed content", enabled); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("size", "range", "内容大小", "Content size", size); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("mode", "select", "显示方式", "Display mode", mode); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("count", "int", "显示数量", "Item count", count); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        value.fields.back().schema.group = "details";
        add("launcher", "appReference", "搜索并选择启动项", "Search and select a launcher", {}); // l10n-allow: isolated bilingual preview fixture, not production UI text.
        auto& launcher = value.fields.back();
        launcher.schema.group.clear();
        launcher.opaque.configured = true;
        launcher.opaque.available = true;
        launcher.opaque.canChoose = launcher.opaque.canClear = true;
        launcher.opaque.displayLabel = english ? "NetEase Cloud Music" : "网易云音乐"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        add("api_key", "password", "API Key", "API Key", {}); // l10n-allow: isolated preview field label.
        auto& secret = value.fields.back();
        secret.schema.group.clear();
        secret.schema.description = english
            ? "Encrypted on this device; sent only to the selected provider."
            : "在本机加密保存，仅发送到所选厂商。"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        secret.opaque.available = secret.opaque.configured = secret.opaque.canClear = true;
        secret.opaque.secretLength = static_cast<std::uint32_t>(syntheticSecret.size());
        wr::WidgetSettingPresetSchema preset;
        preset.id = "standard";
        preset.label = english ? "Standard" : "标准"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        preset.isDefault = true;
        preset.values.emplace("title", title);
        value.presets.push_back(std::move(preset));
    }
    void PopulateAllControls(bool english)
    {
        allControls = allowSecretWrites = true;
        value.customStyle = true;
        value.widgetName = english ? "Lua setting controls" : "Lua 设置控件总览"; // l10n-allow: isolated bilingual preview fixture.
        value.fields.clear(); value.groups.clear(); value.presets.clear();
        value.groups = {{"inputs", english ? "Inputs" : "输入", {}, false, true}, // l10n-allow: isolated bilingual preview fixture.
            {"choices", english ? "Choices" : "选项", {}, false, true}, // l10n-allow: isolated bilingual preview fixture.
            {"objects", english ? "Objects and files" : "对象与文件", {}, false, true}, // l10n-allow: isolated bilingual preview fixture.
            {"states", english ? "State examples" : "状态示例", {}, false, true}}; // l10n-allow: isolated bilingual preview fixture.
        const auto add = [&](const char* key, const char* type, const char* zh, const char* en,
                             const char* group, wr::InteractionValue current) -> wr::WidgetSettingFieldState& {
            wr::WidgetSettingFieldState field;
            field.schema.key = key; field.schema.rawType = type; field.schema.label = english ? en : zh;
            field.schema.group = group; field.currentValue = field.defaultValue = std::move(current);
            value.fields.push_back(std::move(field)); return value.fields.back();
        };
        add("text", "text", "文本", "Text", "inputs", wr::MakeWidgetSettingString("SnowDesktop")); // l10n-allow: isolated bilingual preview fixture.
        auto& secret = add("api_key", "password", "API Key", "API Key", "inputs", {}); // l10n-allow: isolated preview label.
        secret.opaque = {true, true, true, true, {}, 16u};
        auto& url = add("url", "url", "网址", "URL", "inputs", wr::MakeWidgetSettingString("https://example.test/api")); // l10n-allow: isolated bilingual preview fixture.
        url.schema.validationMessage = english ? "Enter an HTTP or HTTPS URL." : "请输入 HTTP 或 HTTPS 网址。"; // l10n-allow: isolated bilingual preview fixture.
        add("int", "int", "整数", "Integer", "inputs", wr::MakeWidgetSettingInteger(12)); // l10n-allow: isolated bilingual preview fixture.
        auto& decimal = add("float", "float", "小数", "Decimal", "inputs", wr::MakeWidgetSettingNumber(1.25)); // l10n-allow: isolated bilingual preview fixture.
        decimal.schema.maximum = 10; decimal.schema.step = 0.05;
        add("range", "range", "范围", "Range", "inputs", wr::MakeWidgetSettingNumber(40)); // l10n-allow: isolated bilingual preview fixture.
        add("bool", "bool", "开关", "Toggle", "choices", wr::MakeWidgetSettingBoolean(true)); // l10n-allow: isolated bilingual preview fixture.
        add("color", "color", "颜色", "Color", "choices", wr::MakeWidgetSettingInteger(0x4C78D8)); // l10n-allow: isolated bilingual preview fixture.
        const std::string longOption = english
            ? "A long option label that remains readable when the settings window is narrow, with additional context that must not disappear."
            : "这是一条较长的选项名称，用于检查窄窗口下的换行、完整名称提示，以及多选列表能否利用整行空间显示内容。"; // l10n-allow: isolated bilingual preview fixture.
        auto& select = add("select", "select", "单选", "Single choice", "choices", wr::MakeWidgetSettingString("a")); // l10n-allow: isolated bilingual preview fixture.
        select.schema.options = {{"a", english ? "Automatic" : "自动"}, {"b", longOption}}; // l10n-allow: isolated bilingual preview fixture.
        auto& multi = add("multiSelect", "multiSelect", "多选", "Multiple choices", "choices", wr::MakeWidgetSettingStringArray({"a"})); // l10n-allow: isolated bilingual preview fixture.
        multi.schema.options = {{"a", english ? "First option" : "第一个选项"}, {"b", longOption}, {"c", english ? "Third option" : "第三个选项"}}; // l10n-allow: isolated bilingual preview fixture.
        add("date", "date", "日期", "Date", "choices", wr::MakeWidgetSettingString("1492-10-12")); // l10n-allow: isolated bilingual preview fixture.
        add("time", "time", "时间", "Time", "choices", wr::MakeWidgetSettingString("09:30")); // l10n-allow: isolated bilingual preview fixture.
        auto& duration = add("duration", "duration", "时长", "Duration", "choices", wr::MakeWidgetSettingInteger(90061)); // l10n-allow: isolated bilingual preview fixture.
        duration.schema.minimum = 0; duration.schema.maximum = 359999; duration.schema.step = 1;
        auto& focusDuration = add("focusDuration", "duration", "专注时长", "Focus duration", "choices", wr::MakeWidgetSettingInteger(25)); // l10n-allow: isolated bilingual preview fixture.
        focusDuration.schema.durationUnit = "minutes";
        focusDuration.schema.minimum = 1; focusDuration.schema.maximum = 120; focusDuration.schema.step = 1;
        add("appSearch", "appSearch", "应用名称搜索", "Application name search", "objects", wr::MakeWidgetSettingString("Example app")); // l10n-allow: isolated bilingual preview fixture.
        for (const auto type : {"appReference", "desktopItemReference", "fileReference", "folderReference", "fileHandle", "folderHandle"})
        {
            const std::map<std::string, std::pair<const char*, const char*>, std::less<>> labels{
                {"appReference", {"启动项", "Launcher"}}, {"desktopItemReference", {"桌面项目", "Desktop item"}}, // l10n-allow: isolated bilingual preview fixture.
                {"fileReference", {"文件引用", "File reference"}}, {"folderReference", {"文件夹引用", "Folder reference"}}, // l10n-allow: isolated bilingual preview fixture.
                {"fileHandle", {"授权文件", "Authorized file"}}, {"folderHandle", {"授权文件夹", "Authorized folder"}}}; // l10n-allow: isolated bilingual preview fixture.
            const auto& label = labels.at(type);
            auto& field = add(type, type, label.first, label.second, "objects", {});
            field.opaque = {true, true, true, true,
                std::string(type).find("folder") != std::string::npos ? "Documents" : "example-file-with-a-long-name.txt", {}};
        }
        auto& empty = add("empty", "fileReference", "未选择", "Empty selection", "states", {}); // l10n-allow: isolated bilingual preview fixture.
        empty.opaque = {false, false, true, false, {}, {}};
        auto& missing = add("unavailable", "folderHandle", "失效的对象", "Unavailable object", "states", {}); // l10n-allow: isolated bilingual preview fixture.
        missing.opaque = {true, false, true, true, "Unavailable-folder", {}};
        auto& disabled = add("disabled", "desktopItemReference", "禁用的选择", "Disabled selection", "states", {}); // l10n-allow: isolated bilingual preview fixture.
        disabled.enabled = false; disabled.opaque = {true, true, true, true, "Read-only example", {}};
        auto& required = add("requiredDate", "date", "必填日期", "Required date", "states", wr::MakeWidgetSettingString("2026-10-19")); // l10n-allow: isolated bilingual preview fixture.
        required.schema.required = true;
        required.schema.validationMessage = english ? "A date is required." : "日期不能为空。"; // l10n-allow: isolated bilingual preview fixture.
        auto& invalid = add("invalid", "text", "校验提示", "Validation example", "states", wr::MakeWidgetSettingString({})); // l10n-allow: isolated bilingual preview fixture.
        invalid.schema.required = true; invalid.valid = false;
        invalid.schema.validationMessage = invalid.validationError = english ? "Enter a value." : "请输入内容。"; // l10n-allow: isolated bilingual preview fixture.
    }
    wr::WidgetSettingFieldState* Field(std::string_view key)
    {
        for (auto& field : value.fields) if (field.schema.key == key) return &field;
        return nullptr;
    }
    wr::WidgetSettingMutationResult Result(bool applied = false)
    {
        return {applied ? wr::WidgetSettingMutationStatus::Applied : wr::WidgetSettingMutationStatus::Unchanged,
            value.generation, applied ? ++value.revision : value.revision, {}, {}};
    }
    bool CanWrite(const wr::WidgetSettingMutationGuard& guard, std::string_view key)
    {
        const auto field = Field(key);
        return allControls && guard.Matches(value) && field && field->enabled;
    }
    void SetEventCallbacks(SnapshotChangedCallback onChanged, SearchCompletedCallback onSearched) override
    {
        changed = std::move(onChanged); searched = std::move(onSearched);
        if (showSearch && searched) searched({value.widgetId, "launcher", value.generation, 1});
    }
    wr::WidgetSettingsLoadResult Load(std::wstring) override { return {wr::WidgetSettingMutationStatus::Unchanged, value, {}, {}}; }
    wr::WidgetSettingsLoadResult Reload(std::wstring_view) override { return Load({}); }
    std::optional<wr::WidgetSettingsSnapshot> Snapshot(std::wstring_view) const override { return value; }
    void Close(std::wstring_view) override {}
    void CloseAll() override {}
    wr::WidgetSettingMutationResult SetOrdinary(const wr::WidgetSettingMutationGuard& guard, std::string_view key, const wr::InteractionValue& input) override
    {
        if (!CanWrite(guard, key)) return {};
        auto& field = *Field(key);
        wr::InteractionValue normalized; std::string error;
        if (!wr::NormalizeWidgetSettingValue(field.schema, input, normalized, error))
            return {wr::WidgetSettingMutationStatus::InvalidValue, value.generation, value.revision, error, field.schema.validationMessage};
        field.currentValue = std::move(normalized); field.hasStoredValue = true; field.valid = true; field.validationError.clear();
        return Result(true);
    }
    wr::WidgetSettingMutationResult PreviewOrdinary(const wr::WidgetSettingMutationGuard& guard, std::string_view key, const wr::InteractionValue& input) override { return SetOrdinary(guard, key, input); }
    wr::WidgetSettingMutationResult SetSearchQuery(const wr::WidgetSettingMutationGuard& guard, std::string_view key, std::string query) override
    {
        if (!CanWrite(guard, key)) return {};
        Field(key)->searchQuery = std::move(query); return Result(true);
    }
    wr::WidgetSettingSecretReadResult ReadSecret(const wr::WidgetSettingMutationGuard& guard,
        std::string_view key) override
    {
        if (!allowSecretWrites || key != "api_key" || !guard.Matches(value)) return {};
        ++secretReads;
        return {{wr::WidgetSettingMutationStatus::Unchanged, value.generation, value.revision, {}, {}}, syntheticSecret};
    }
    wr::WidgetSettingMutationResult SetSecret(const wr::WidgetSettingMutationGuard& guard,
        std::string_view key, std::string_view plaintext) override
    {
        if (!allowSecretWrites || key != "api_key") return {};
        if (!guard.Matches(value))
            return {wr::WidgetSettingMutationStatus::StaleSnapshot, value.generation, value.revision, "staleSnapshot", {}};
        ++secretAttempts;
        if (failSecretWrites)
            return {wr::WidgetSettingMutationStatus::PersistenceFailed, value.generation, value.revision, "previewWriteFailed", {}};
        for (auto& field : value.fields)
            if (field.schema.key == key)
            {
                field.opaque.configured = field.opaque.canClear = !plaintext.empty();
                field.opaque.secretLength = static_cast<std::uint32_t>(plaintext.size());
            }
        syntheticSecret = plaintext;
        lastSecretBytes = plaintext.size();
        ++secretSuccesses;
        return {wr::WidgetSettingMutationStatus::Applied, value.generation, ++value.revision, {}, {}};
    }
    wr::WidgetSettingMutationResult ChooseFilesystemHandle(const wr::WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        if (!CanWrite(guard, key)) return {};
        if (cancelNextPicker) { cancelNextPicker = false; return Result(); }
        Field(key)->opaque = {true, true, true, true, "Chosen example", {}};
        return Result(true);
    }
    wr::WidgetSettingMutationResult OpenEntityReferencePicker(const wr::WidgetSettingMutationGuard& guard, std::string_view key) override { return ChooseFilesystemHandle(guard, key); }
    wr::WidgetSettingMutationResult ClearOpaque(const wr::WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        if (key == "api_key") return SetSecret(guard, key, {});
        if (!CanWrite(guard, key)) return {};
        Field(key)->opaque = {false, false, true, false, {}, {}}; return Result(true);
    }
    wr::WidgetSettingMutationResult ApplyPreset(const wr::WidgetSettingMutationGuard&, std::string_view) override { return {}; }
    wr::WidgetSettingMutationResult UpdateHostAppearance(const wr::WidgetSettingMutationGuard&, const wr::WidgetHostAppearancePatch&) override { return {}; }
    wr::WidgetSettingMutationResult PreviewHostAppearance(const wr::WidgetSettingMutationGuard&, const wr::WidgetHostAppearancePatch&) override { return {}; }
    wr::WidgetSettingMutationResult CommitPreview(const wr::WidgetSettingMutationGuard& guard) override { return allControls && guard.Matches(value) ? Result() : wr::WidgetSettingMutationResult{}; }
    wr::WidgetSettingMutationResult RevertPreview(const wr::WidgetSettingMutationGuard&) override { return {}; }
    wr::WidgetSettingMutationResult ResetField(const wr::WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        if (!CanWrite(guard, key)) return {};
        return SetOrdinary(guard, key, Field(key)->defaultValue);
    }
    wr::WidgetSettingMutationResult Reset(const wr::WidgetSettingMutationGuard&) override { return {}; }
    wr::WidgetSettingMutationResult StartSearch(const wr::WidgetSettingMutationGuard& guard, std::string_view key, std::string query, std::size_t) override
    {
        if (!CanWrite(guard, key)) return {};
        const std::string name(key);
        searches[name] = {value.widgetId, name, value.generation, value.revision, std::move(query), false, true, {},
            {{"first", "First example", "Offline", "application"}, {"second", "Second example", "Offline", "application"}}};
        if (searched) searched({value.widgetId, name, value.generation, searches[name].requestId});
        return {wr::WidgetSettingMutationStatus::Started, value.generation, value.revision, {}, {}};
    }
    wr::WidgetSettingMutationResult CancelSearch(const wr::WidgetSettingMutationGuard& guard, std::string_view key) override
    {
        if (!CanWrite(guard, key)) return {};
        searches.erase(std::string(key)); return Result();
    }
    wr::WidgetSettingMutationResult CommitSearchResult(const wr::WidgetSettingMutationGuard& guard, std::string_view key, std::uint64_t requestId, std::string_view id) override
    {
        if (!CanWrite(guard, key)) return {};
        const auto search = searches.find(std::string(key));
        if (search == searches.end() || search->second.requestId != requestId) return {};
        auto& field = *Field(key);
        if (field.schema.Kind() == wr::WidgetSettingKind::AppSearch)
            field.currentValue = wr::MakeWidgetSettingString(id == "second" ? "Second example" : "First example");
        else field.opaque = {true, true, true, true, id == "second" ? "Second example" : "First example", {}};
        searches.erase(search); return Result(true);
    }
    std::optional<wr::WidgetSettingSearchSnapshot> SearchSnapshot(std::wstring_view widgetId, std::string_view key) const override
    {
        if (allControls)
        {
            const auto found = searches.find(std::string(key));
            return widgetId == value.widgetId && found != searches.end()
                ? std::optional<wr::WidgetSettingSearchSnapshot>(found->second) : std::nullopt;
        }
        if (!showSearch || widgetId != value.widgetId || key != "launcher") return {};
        return wr::WidgetSettingSearchSnapshot{value.widgetId, "launcher", value.generation,
            1, "qq", false, true, {}, {{"qq", "QQ", "Applications", "application"},
                {"qqmusic", searchResultTitle, "Applications", "application"}}};
    }
};

void ExerciseSecretInput(const x::FrameworkElement& root,
    PreviewWidgetService& service, const std::filesystem::path& output)
{
    c::PasswordBox password{nullptr};
    Walk(root, [&](const x::DependencyObject& node) {
        if (const auto input = node.try_as<c::PasswordBox>(); input &&
            x::Automation::AutomationProperties::GetName(input) == L"API Key") // l10n-allow: isolated preview field identity.
            password = input;
    });
    if (!password) throw std::runtime_error("missing preview password editor");
    c::Button clear{nullptr};
    auto ancestor = password.as<x::DependencyObject>();
    const winrt::hstring clearName{_LW("app.settings.widget.clear")};
    while (ancestor && !clear)
    {
        Walk(ancestor, [&](const x::DependencyObject& node) {
            if (const auto button = node.try_as<c::Button>(); button &&
                x::Automation::AutomationProperties::GetName(button) == clearName)
                clear = button;
        });
        if (!clear) ancestor = x::Media::VisualTreeHelper::GetParent(ancestor);
    }
    if (!clear) throw std::runtime_error("missing inline password clear action");
    c::Primitives::ToggleButton reveal{nullptr};
    Walk(ancestor, [&](const x::DependencyObject& node) {
        if (const auto button = node.try_as<c::Primitives::ToggleButton>()) reveal = button;
    });
    if (!reveal) throw std::runtime_error("missing password reveal action");
    const auto toggleReveal = [&] {
        const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(reveal);
        peer.GetPattern(x::Automation::Peers::PatternInterface::Toggle)
            .as<x::Automation::Provider::IToggleProvider>().Toggle();
    };
    if (service.secretReads != 0 || !password.Password().empty() ||
        password.PlaceholderText() != std::wstring(16, L'\x25cf'))
        throw std::runtime_error("opening a password field decrypted plaintext or lost its mask length");
    toggleReveal();
    PumpUntil([&] { return service.secretReads != 0; });
    if (password.Password() != L"offline-existing" || password.PasswordRevealMode() != c::PasswordRevealMode::Visible ||
        service.secretReads != 1 || service.secretAttempts != 0)
        throw std::runtime_error("explicit password reveal did not read the saved value without writing it");
    toggleReveal();
    const auto inputBounds = password.TransformToVisual(root).TransformBounds(
        {0, 0, static_cast<float>(password.ActualWidth()), static_cast<float>(password.ActualHeight())});
    const auto clearBounds = clear.TransformToVisual(root).TransformBounds(
        {0, 0, static_cast<float>(clear.ActualWidth()), static_cast<float>(clear.ActualHeight())});
    if (std::abs(inputBounds.Y - clearBounds.Y) > 2.0 || clearBounds.X < inputBounds.X + inputBounds.Width)
        throw std::runtime_error("password clear action is not aligned beside the input");
    (void)password.Focus(x::FocusState::Programmatic);
    const bool focused = password.FocusState() != x::FocusState::Unfocused;
    password.Password(L"offline-a");
    password.Password(L"offline-ab");
    password.Password(L"offline-autosave");
    PumpUntil([&] { return service.secretAttempts != 0; });
    if (service.secretAttempts != 1 || service.secretSuccesses != 1 || service.lastSecretBytes != 16 ||
        (focused && password.Password() != L"offline-autosave"))
        throw std::runtime_error("password input did not save its latest draft without interrupting editing");
    (void)clear.Focus(x::FocusState::Programmatic);
    const auto afterBlur = GetTickCount64() + 850;
    PumpUntil([&] { return GetTickCount64() >= afterBlur; });
    if (service.lastSecretBytes != 16 || service.secretAttempts != 1)
        throw std::runtime_error("blurring a saved password changed its stored secret");
    const auto beforeEditRead = service.secretReads;
    (void)password.Focus(x::FocusState::Programmatic);
    const bool editFocused = password.FocusState() != x::FocusState::Unfocused;
    if (service.secretReads != beforeEditRead)
        throw std::runtime_error("automatic focus read plaintext without an input gesture");
    toggleReveal();
    if (password.Password() != L"offline-autosave" || service.secretReads != beforeEditRead + 1)
        throw std::runtime_error("reopening password editing did not reload the saved value");
    toggleReveal();
    service.failSecretWrites = true;
    password.Password(L"offline-retry");
    PumpUntil([&] { return service.secretAttempts == 2; });
    if (service.secretSuccesses != 1 || service.lastSecretBytes != 16 || password.Password() != L"offline-retry")
        throw std::runtime_error("failed password write lost its draft or replaced the stored value");
    service.failSecretWrites = false;
    unsigned clearClicks = 0;
    const auto clearObserved = clear.Click(winrt::auto_revoke,
        [&](const auto&, const auto&) { ++clearClicks; });
    const auto beforeClear = service.secretAttempts;
    InvokePreview(clear);
    const auto settleUntil = GetTickCount64() + 800;
    PumpUntil([&] { return GetTickCount64() >= settleUntil; });
    std::ofstream(output / L"secret-clear.tsv")
        << "attemptsBefore\tattemptsAfter\tsuccesses\tstoredBytes\tdraftCharacters\tclearClicks\tclearEnabled\n"
        << beforeClear << '\t' << service.secretAttempts << '\t' << service.secretSuccesses << '\t'
        << service.lastSecretBytes << '\t' << password.Password().size() << '\t' << clearClicks << '\t' << clear.IsEnabled() << '\n';
    if (service.lastSecretBytes != 0 || !password.Password().empty() || clearClicks != 1 || clear.IsEnabled())
        throw std::runtime_error("clearing a password saved or resurrected its pending draft");
    const auto afterClear = service.secretAttempts;
    password.Password(L"offline-final");
    PumpUntil([&] { return service.secretAttempts > afterClear; });
    if (service.lastSecretBytes != 13)
        throw std::runtime_error("password input did not recover after clearing");
    const auto afterFinal = service.secretAttempts;
    (void)clear.Focus(x::FocusState::Programmatic);
    const auto finalBlur = GetTickCount64() + 850;
    PumpUntil([&] { return GetTickCount64() >= finalBlur; });
    if (service.lastSecretBytes != 13 || service.secretAttempts != afterFinal)
        throw std::runtime_error("final password was cleared by a programmatic draft reset");
    std::ofstream(output / L"secret-input.tsv")
        << "masked-open\texplicit-reveal\tinput-autosave\tburst-coalesced\tfailure-retains-draft\tclear-discards-draft\tblur-keeps-secret\tedit-readback\n"
        << "passed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\t" << (editFocused ? "passed" : "not-observed") << '\n';
}

template<class Control> Control WidgetPreviewControl(const x::DependencyObject& field)
{
    Control found{nullptr};
    Walk(field, [&](const x::DependencyObject& node) { if (!found) found = node.try_as<Control>(); });
    if (!found) throw std::runtime_error("missing native widget control");
    return found;
}

x::FrameworkElement WidgetPreviewField(const x::FrameworkElement& root, std::string_view key)
{
    return PreviewControl<x::FrameworkElement>(root, winrt::to_hstring("WidgetSetting." + std::string(key)));
}

c::Button WidgetPreviewAction(const x::DependencyObject& field, const char* labelKey)
{
    c::Button found{nullptr};
    const winrt::hstring name{_LW(labelKey)};
    Walk(field, [&](const x::DependencyObject& node) {
        if (const auto button = node.try_as<c::Button>(); button &&
            x::Automation::AutomationProperties::GetName(button) == name) found = button;
    });
    return found;
}

c::Button WidgetPreviewReset(const x::DependencyObject& field)
{
    c::Button reset{nullptr};
    Walk(field, [&](const x::DependencyObject& node) {
        if (const auto button = node.try_as<c::Button>())
            if (const auto icon = button.Content().try_as<c::FontIcon>(); icon && icon.Glyph() == L"\xE777") reset = button;
    });
    if (!reset) throw std::runtime_error("widget reset action is missing");
    return reset;
}

void InspectWidgetControls(const x::FrameworkElement& root,
    const PreviewWidgetService& service, const std::filesystem::path& output)
{
    std::set<std::string> types;
    std::ofstream inventory(output / L"widget-controls.tsv");
    std::ofstream timeLayout(output / L"widget-time-layout.tsv");
    std::ofstream durationLayout(output / L"widget-duration-layout.tsv");
    timeLayout << "key\tpickerWidth\tbuttonWidth\tbuttonRight\n";
    durationLayout << "key\tpart\tnumberWidth\tnumberHeight\tunitLeft\tnumberRight\tcenterDifference\n";
    inventory << "key\ttype\tenabled\tconfigured\tavailable\twidth\theight\n";
    for (const auto& field : service.value.fields)
    {
        const auto element = WidgetPreviewField(root, field.schema.key);
        types.insert(field.schema.rawType);
        inventory << field.schema.key << '\t' << field.schema.rawType << '\t' << field.enabled << '\t'
            << field.opaque.configured << '\t' << field.opaque.available << '\t'
            << element.ActualWidth() << '\t' << element.ActualHeight() << '\n';
        if (element.ActualWidth() <= 0 || element.ActualHeight() <= 0)
            throw std::runtime_error("widget control has no layout: " + field.schema.key);
        if (field.schema.Kind() == wr::WidgetSettingKind::Time)
        {
            const auto picker = WidgetPreviewControl<c::TimePicker>(element);
            c::Button flyout{nullptr};
            Walk(picker, [&](const x::DependencyObject& node) {
                if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"FlyoutButton") flyout = button;
            });
            if (!flyout) throw std::runtime_error("time picker flyout button is missing");
            const auto bounds = flyout.TransformToVisual(picker).TransformBounds(
                {0, 0, static_cast<float>(flyout.ActualWidth()), static_cast<float>(flyout.ActualHeight())});
            timeLayout << field.schema.key << '\t' << picker.ActualWidth() << '\t' << flyout.ActualWidth()
                << '\t' << bounds.X + bounds.Width << '\n';
            timeLayout.flush();
            if (bounds.X < -0.5 || bounds.X + bounds.Width > picker.ActualWidth() + 0.5)
                throw std::runtime_error("time picker native border is clipped");
        }
        if (field.schema.Kind() == wr::WidgetSettingKind::Duration)
        {
            std::vector<c::NumberBox> numbers;
            Walk(element, [&](const x::DependencyObject& node) {
                if (const auto number = node.try_as<c::NumberBox>()) numbers.push_back(number);
            });
            for (std::size_t index = 0; index < numbers.size(); ++index)
            {
                const auto number = numbers[index];
                const auto unit = PreviewControl<c::TextBlock>(element, L"DurationUnit" + std::to_wstring(index));
                const auto numberBounds = number.TransformToVisual(element).TransformBounds(
                    {0, 0, static_cast<float>(number.ActualWidth()), static_cast<float>(number.ActualHeight())});
                const auto unitBounds = unit.TransformToVisual(element).TransformBounds(
                    {0, 0, static_cast<float>(unit.ActualWidth()), static_cast<float>(unit.ActualHeight())});
                const double centers = std::abs(numberBounds.Y + numberBounds.Height / 2 - unitBounds.Y - unitBounds.Height / 2);
                durationLayout << field.schema.key << '\t' << index << '\t' << number.ActualWidth() << '\t'
                    << number.ActualHeight() << '\t' << unitBounds.X << '\t' << numberBounds.X + numberBounds.Width
                    << '\t' << centers << '\n';
                durationLayout.flush();
                if (number.Header() || number.ActualHeight() > 40 || centers > 1 ||
                    unitBounds.X < numberBounds.X + numberBounds.Width ||
                    unitBounds.X + unitBounds.Width > element.ActualWidth() + 0.5)
                    throw std::runtime_error("duration unit is not fully visible beside its input: " + field.schema.key);
            }
        }
        if (field.schema.Channel() == wr::WidgetSettingValueChannel::EntityReference ||
            field.schema.Channel() == wr::WidgetSettingValueChannel::FilesystemHandle)
        {
            if (field.schema.Kind() == wr::WidgetSettingKind::AppReference) continue;
            const auto text = WidgetPreviewControl<c::TextBox>(element);
            const auto choose = WidgetPreviewAction(element, "app.settings.widget.choose");
            const auto clear = WidgetPreviewAction(element, "app.settings.widget.clear");
            if (!text.IsReadOnly() || !choose || !clear)
                throw std::runtime_error("reference control lost its readable name or actions");
            const auto bounds = text.TransformToVisual(element).TransformBounds(
                {0, 0, static_cast<float>(text.ActualWidth()), static_cast<float>(text.ActualHeight())});
            const auto button = choose.TransformToVisual(element).TransformBounds(
                {0, 0, static_cast<float>(choose.ActualWidth()), static_cast<float>(choose.ActualHeight())});
            if (button.X < bounds.X + bounds.Width - 1 || std::abs(button.Y - bounds.Y) > 2)
                throw std::runtime_error("reference actions are not beside the value: " + field.schema.key);
        }
    }
    if (types.size() != 20) throw std::runtime_error("widget preview does not cover all 20 setting types");
    const auto permissions = PreviewControl<x::FrameworkElement>(root, L"WidgetPermissionsRow");
    const auto reset = PreviewControl<x::FrameworkElement>(root, L"WidgetResetRow");
    std::ofstream(output / L"widget-action-card-layout.tsv") << "permissions\treset\n" << permissions.ActualHeight() << '\t' << reset.ActualHeight() << '\n';
    if (std::abs(permissions.ActualHeight() - reset.ActualHeight()) > 1)
        throw std::runtime_error("widget action cards do not share their standard row height");
    const auto follow = PreviewControl<x::FrameworkElement>(root, L"WidgetFollowGlobalRow");
    const auto theme = PreviewControl<x::FrameworkElement>(root, L"WidgetAppearanceThemeRow");
    const auto restore = PreviewControl<x::FrameworkElement>(root, L"WidgetRestoreBackgroundRow");
    std::ofstream(output / L"widget-appearance-card-layout.tsv") << "follow\ttheme\trestore\n" <<
        follow.ActualHeight() << '\t' << theme.ActualHeight() << '\t' << restore.ActualHeight() << '\n';
    if (std::abs(follow.ActualHeight() - theme.ActualHeight()) > 1 || std::abs(follow.ActualHeight() - restore.ActualHeight()) > 1)
        throw std::runtime_error("widget appearance cards do not share their single-line height");
}

void ExerciseWidgetControls(const x::FrameworkElement& root,
    PreviewWidgetService& service, const std::filesystem::path& output)
{
    std::ofstream checks(output / L"widget-control-checks.tsv");
    checks << "scenario\tresult\n";
    const auto check = [&](bool passed, const std::string& name) {
        checks << name << '\t' << (passed ? "passed" : "failed") << '\n'; checks.flush();
        if (!passed) throw std::runtime_error("widget control check failed: " + name);
    };
    const auto field = [&](const char* key) { return WidgetPreviewField(root, key); };
    const auto settle = [&] { Settle(root); };
    const auto savedString = [&](const char* key, std::string_view expected) {
        return service.Field(key)->currentValue.string == expected;
    };
    auto text = WidgetPreviewControl<c::TextBox>(field("text"));
    text.Text(L"Edited example");
    PumpUntil([&] { return savedString("text", "Edited example"); });
    check(true, "text-autosave");
    c::Button restoreText{nullptr};
    Walk(field("text"), [&](const x::DependencyObject& node) {
        if (const auto button = node.try_as<c::Button>(); button)
            if (const auto icon = button.Content().try_as<c::FontIcon>(); icon && icon.Glyph() == L"\xE777") restoreText = button;
    });
    check(static_cast<bool>(restoreText), "text-native-reset-action");
    InvokePreview(restoreText); settle();
    check(text.Text() == L"SnowDesktop" && savedString("text", "SnowDesktop"), "text-reset-refresh");
    auto url = WidgetPreviewControl<c::TextBox>(field("url"));
    url.Text(L"invalid-url");
    const auto invalidUntil = GetTickCount64() + 800;
    PumpUntil([&] { return GetTickCount64() >= invalidUntil; });
    check(url.Text() == L"invalid-url" && savedString("url", "https://example.test/api"), "url-invalid-draft-retained");
    check(WidgetPreviewReset(field("url")).IsEnabled(), "url-invalid-draft-reset-enabled");
    InvokePreview(WidgetPreviewReset(field("url"))); settle();
    check(url.Text() == L"https://example.test/api" && savedString("url", "https://example.test/api"), "url-reset-discards-invalid-same-value-draft");
    url.Text(L"https://example.test/edited");
    PumpUntil([&] { return savedString("url", "https://example.test/edited"); });
    check(true, "url-autosave");
    WidgetPreviewControl<c::ToggleSwitch>(field("bool")).IsOn(false);
    settle();
    check(!service.Field("bool")->currentValue.boolean, "bool-immediate");
    for (const auto key : {"int", "float", "range"})
    {
        const double desired = std::string_view(key) == "int" ? 17.0 : std::string_view(key) == "float" ? 2.5 : 65.0;
        WidgetPreviewControl<c::NumberBox>(field(key)).Value(desired);
        PumpUntil([&] {
            const auto& value = service.Field(key)->currentValue;
            return value.type == wr::InteractionValue::Type::Integer ? value.integer == static_cast<long long>(desired)
                : std::abs(value.number - desired) < 0.00001;
        });
        const auto idleUntil = GetTickCount64() + 750;
        PumpUntil([&] { return GetTickCount64() >= idleUntil; });
        check(true, std::string(key) + "-preview-and-commit");
    }
    auto single = WidgetPreviewControl<c::ComboBox>(field("select"));
    single.SelectedIndex(1);
    settle();
    check(savedString("select", "b"), "select-long-label");
    auto multi = WidgetPreviewControl<c::ListView>(field("multiSelect"));
    multi.StartBringIntoView(); settle();
    c::CheckBox second{nullptr};
    const auto secondLabel = winrt::to_hstring(service.Field("multiSelect")->schema.options[1].label);
    Walk(multi, [&](const x::DependencyObject& node) {
        if (const auto option = node.try_as<c::CheckBox>(); option &&
            x::Automation::AutomationProperties::GetName(option) == secondLabel) second = option;
    });
    check(static_cast<bool>(second), "multiSelect-options-realized");
    second.IsChecked(true);
    settle();
    check(service.Field("multiSelect")->currentValue.array.size() == 2, "multiSelect-immediate");
    const auto multiWidth = field("multiSelect").ActualWidth();
    check(multi.ActualWidth() > multiWidth * 0.8, "multiSelect-full-row");
    auto date = WidgetPreviewControl<c::CalendarDatePicker>(field("date"));
    winrt::Windows::Globalization::Calendar calendar;
    calendar.ChangeCalendarSystem(L"GregorianCalendar");
    calendar.SetDateTime(date.Date().Value());
    check(calendar.Year() == 1492 && calendar.Month() == 10 && calendar.Day() == 12, "date-historical-roundtrip");
    calendar.Day(1); calendar.Year(2028); calendar.Month(2); calendar.Day(29);
    date.Date(winrt::box_value(calendar.GetDateTime()).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
    settle();
    check(savedString("date", "2028-02-29"), "date-leap-day");
    InvokePreview(WidgetPreviewAction(field("date"), "app.settings.widget.clear")); settle();
    check(savedString("date", "") && !date.Date(), "date-clear");
    check(!WidgetPreviewAction(field("requiredDate"), "app.settings.widget.clear"), "required-date-no-clear");
    auto requiredDate = WidgetPreviewControl<c::CalendarDatePicker>(field("requiredDate"));
    requiredDate.Date(nullptr); settle();
    check(savedString("requiredDate", "2026-10-19") && static_cast<bool>(requiredDate.Date()), "required-date-rejected-clear-restores-display");
    auto time = WidgetPreviewControl<c::TimePicker>(field("time"));
    time.SelectedTime(winrt::box_value(std::chrono::duration_cast<winrt::Windows::Foundation::TimeSpan>(std::chrono::minutes(545)))
        .as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::TimeSpan>>());
    settle();
    check(savedString("time", "09:05"), "time-immediate");
    InvokePreview(WidgetPreviewAction(field("time"), "app.settings.widget.clear")); settle();
    check(savedString("time", "") && !time.SelectedTime(), "time-clear");
    std::vector<c::NumberBox> durationParts;
    Walk(field("duration"), [&](const x::DependencyObject& node) {
        if (const auto number = node.try_as<c::NumberBox>()) durationParts.push_back(number);
    });
    check(durationParts.size() == 3 && durationParts[0].Value() == 25 && durationParts[1].Value() == 1 &&
        durationParts[2].Value() == 1, "duration-over-24-hours-roundtrip");
    WidgetPreviewControl<c::TextBox>(durationParts[2]).Text(L"7");
    PumpUntil([&] { return service.Field("duration")->currentValue.integer == 90067; });
    check(true, "duration-text-autosave-without-blur");
    WidgetPreviewControl<c::TextBox>(durationParts[1]).Text(L"60");
    const auto durationInvalidUntil = GetTickCount64() + 800;
    PumpUntil([&] { return GetTickCount64() >= durationInvalidUntil; });
    check(WidgetPreviewControl<c::TextBox>(durationParts[1]).Text() == L"60" && service.Field("duration")->currentValue.integer == 90067,
        "duration-invalid-draft-keeps-saved-value");
    WidgetPreviewControl<c::TextBox>(durationParts[1]).Text(L"2");
    PumpUntil([&] { return service.Field("duration")->currentValue.integer == 90127; });
    check(true, "duration-corrected-draft-autosave");
    InvokePreview(WidgetPreviewReset(field("duration"))); settle();
    WidgetPreviewControl<c::TextBox>(durationParts[1]).Text(L"60");
    const auto resetDraftUntil = GetTickCount64() + 800;
    PumpUntil([&] { return GetTickCount64() >= resetDraftUntil; });
    check(WidgetPreviewReset(field("duration")).IsEnabled(), "duration-invalid-draft-reset-enabled");
    InvokePreview(WidgetPreviewReset(field("duration"))); settle();
    check(service.Field("duration")->currentValue.integer == 90061 &&
        WidgetPreviewControl<c::TextBox>(durationParts[1]).Text() == L"1", "duration-reset-discards-invalid-same-value-draft");
    std::vector<c::NumberBox> focusParts;
    Walk(field("focusDuration"), [&](const x::DependencyObject& node) {
        if (const auto number = node.try_as<c::NumberBox>()) focusParts.push_back(number);
    });
    check(focusParts.size() == 2 && focusParts[1].Value() == 25, "duration-minute-storage-roundtrip");
    WidgetPreviewControl<c::TextBox>(focusParts[1]).Text(L"30");
    PumpUntil([&] { return service.Field("focusDuration")->currentValue.integer == 30; });
    check(true, "duration-minute-unit-autosave");
    for (const auto key : {"desktopItemReference", "fileReference", "folderReference", "fileHandle", "folderHandle"})
    {
        auto item = field(key);
        const auto choose = WidgetPreviewAction(item, "app.settings.widget.choose");
        const auto clear = WidgetPreviewAction(item, "app.settings.widget.clear");
        const auto before = service.Field(key)->opaque;
        service.cancelNextPicker = true; InvokePreview(choose); settle();
        check(service.Field(key)->opaque == before, std::string(key) + "-cancel-keeps-selection");
        InvokePreview(clear); settle();
        check(!service.Field(key)->opaque.configured && !clear.IsEnabled(), std::string(key) + "-clear");
        InvokePreview(choose); settle();
        check(service.Field(key)->opaque.configured &&
            WidgetPreviewControl<c::TextBox>(item).Text() == L"Chosen example", std::string(key) + "-choose-refresh");
    }
    const auto missing = field("unavailable");
    check(WidgetPreviewAction(missing, "app.settings.widget.clear").IsEnabled() &&
        x::Automation::AutomationProperties::GetItemStatus(missing) == winrt::hstring{_LW("iconAppearance.unavailable")},
        "unavailable-announced-and-clearable");
    check(!WidgetPreviewAction(field("disabled"), "app.settings.widget.choose").IsEnabled() &&
        !WidgetPreviewAction(field("disabled"), "app.settings.widget.clear").IsEnabled(), "disabled-actions");
    check(WidgetPreviewControl<c::TextBox>(field("empty")).Text().empty() &&
        !WidgetPreviewAction(field("empty"), "app.settings.widget.clear").IsEnabled(), "empty-selection-no-fake-value");
    for (const auto key : {"appSearch", "appReference"})
    {
        auto item = field(key); item.StartBringIntoView(); settle();
        (void)service.StartSearch(wr::WidgetSettingMutationGuard::FromSnapshot(service.value), key, "example", 16);
        const auto results = WidgetPreviewControl<c::ListView>(item);
        PumpUntil([&] { return results.Items().Size() == 2; });
        results.SelectedIndex(1);
        const auto search = WidgetPreviewControl<c::AutoSuggestBox>(item);
        c::Button query{nullptr};
        Walk(search, [&](const x::DependencyObject& node) {
            if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"QueryButton") query = button;
        });
        check(static_cast<bool>(query), std::string(key) + "-native-query-action");
        InvokePreview(query); settle();
        check(std::string_view(key) == "appSearch" ? savedString(key, "Second example")
            : service.Field(key)->opaque.displayLabel == "Second example", std::string(key) + "-selected-result");
        check(search.Text() == L"Second example", std::string(key) + "-selected-name-visible");
    }
    const auto colorField = field("color"); colorField.StartBringIntoView(); settle();
    const auto colorButton = WidgetPreviewControl<c::Button>(colorField);
    colorButton.Flyout().ShowAt(colorButton); settle();
    const auto colorPanel = colorButton.Flyout().as<c::Flyout>().Content().as<x::DependencyObject>();
    const auto picker = WidgetPreviewControl<c::ColorPicker>(colorPanel);
    picker.Color({255, 24, 80, 160});
    settle();
    colorButton.Flyout().Hide(); settle();
    check(service.Field("color")->currentValue.integer == 0x1850A0, "color-preview-close-commit");
    check(service.secretReads == 0, "gallery-does-not-decrypt-secrets");
}

constexpr SettingsPage pages[] = {
    SettingsPage::General, SettingsPage::Desktop, SettingsPage::QuickNavigation,
    SettingsPage::DesktopPages, SettingsPage::WidgetBehavior, SettingsPage::DesktopCategories,
    SettingsPage::DesktopStyle, SettingsPage::Dock, SettingsPage::StatusBar, SettingsPage::Taskbar,
    SettingsPage::AppearanceTheme, SettingsPage::AppearanceDesktopIcons, SettingsPage::AppearanceIconBeautification,
    SettingsPage::IconLibrary, SettingsPage::RuntimeModules,
    SettingsPage::AnimationPerformance, SettingsPage::ContextMenu, SettingsPage::ThemeManager,
    SettingsPage::Widgets, SettingsPage::Calendar, SettingsPage::BackupAndData, SettingsPage::About,
    SettingsPage::DeveloperTools, SettingsPage::Debug, SettingsPage::WidgetSettings, SettingsPage::LargeIcon,
    // Revisit editors after the centralized theme page to verify reattachment.
    SettingsPage::Dock, SettingsPage::StatusBar, SettingsPage::Taskbar, SettingsPage::QuickNavigation};

void Export(const std::filesystem::path& output, std::string_view selected, bool dark,
    const std::string& locale, int width, int height, bool expanded, bool custom, bool disabled,
    bool scenarioRules, const std::string& focusId, std::string_view calendarScenario,
    bool searching, bool restore, bool secretInput, bool generalNotices)
{
    Locale::Instance().Init((std::filesystem::path(GetExecutableDirectoryPath()) / L"lang").c_str());
    if (!Locale::Instance().HasLanguage(locale)) throw std::runtime_error("preview locale is not installed");
    Locale::Instance().SetLanguage(locale.c_str());
    Apartment apartment; winrt::check_hresult(apartment.result);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RenderHost host;
    if (!host.runtime.Initialize()) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    WNDCLASSW previewClass{};
    previewClass.lpfnWndProc = RenderHost::Procedure;
    previewClass.hInstance = GetModuleHandleW(nullptr);
    previewClass.lpszClassName = L"SnowDesktop.SettingsRenderHost";
    if (!RegisterClassW(&previewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) winrt::throw_last_error();
    host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        previewClass.lpszClassName, L"SnowDesktop offline settings render", WS_POPUP,
        -32000, -32000, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), &host.runtime);
    if (!host.window) winrt::throw_last_error();
    ShowWindow(host.window, SW_SHOWNOACTIVATE);
    const double scale = GetDpiForWindow(host.window) / 96.;
    SetWindowPos(host.window, nullptr, 0, 0, static_cast<int>(std::ceil(width * scale)),
        static_cast<int>(std::ceil(height * scale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    const bool controlsGallery = scenarioRules && selected == "widget-settings";
    PreviewWidgetService widgets(locale == "en-US", searching);
    widgets.allowSecretWrites = secretInput;
    widgets.value.customStyle = custom;
    widgets.value.hostAppearance.followPersonalization = !custom;
    if (custom) widgets.value.hostAppearance.presetId = "__custom";
    if (controlsGallery) widgets.PopulateAllControls(locale == "en-US");
    auto shell = winrt::make_self<winrt::SnowDesktop::implementation::SettingsShell>();
    shell->RequestedTheme(dark ? x::ElementTheme::Dark : x::ElementTheme::Light);
    shell->SetLocalizer([](std::string_view key) { return std::wstring(_LW(std::string(key).c_str())); });
    shell->SetConditionalPagesVisible(true, true);
    shell->SetWidgetSettingsService(&widgets);
    // The default preview shows a satisfied advanced-feature card. The
    // general-notices render switches this fixture through every dormant
    // warning/error variant so production notice layout is captured in place.
    struct GeneralNoticeFixture
    {
        enum class Mode
        {
            Registered,
            SteamInitFailed,
            SteamOutdated,
            SteamNotOwned,
            SteamStorageFailed,
            SteamSessionOnly,
            VerificationFailed,
            SteamUnavailable,
            SteamOffline,
            AppMismatch,
            BridgeUnavailable,
            StartupInstalled,
            StartupOther,
        };
        Mode mode = Mode::Registered;
    };
    const auto generalNotice = std::make_shared<GeneralNoticeFixture>();
    GeneralPageActions general;
    general.onboarding.settingsIndex = [] {
        SettingsSearchIndexInput index; index.developerToolsVisible = index.debugVisible = true;
        PopulateSettingsSearchCatalog(index, [](std::string_view key) { return std::wstring(_LW(std::string(key).c_str())); }, true, true);
        return index.staticSettings;
    };
    general.probeHotkey = [](SettingsHostActions::HotkeyTarget, HotkeyChord, std::uint64_t,
        std::uint64_t, HotkeyRecorder::AvailabilityCompletion complete) { complete(true, {}); };
    general.languageCatalog = [] { return std::vector<SettingsLanguageOption>{{"zh-CN", L"简体中文"}, {"en-US", L"English"}}; }; // l10n-allow: isolated bilingual preview fixture, not production UI text.
    general.queryAdvancedFeatureStatus = [generalNotice] {
        GeneralAdvancedFeatureStatus status;
        status.bridgeAvailable = status.cardVisible = true;
        const auto mode = generalNotice->mode;
        if (mode == GeneralNoticeFixture::Mode::Registered)
        {
            status.state = GeneralAdvancedFeatureState::Registered;
            status.registered = true;
            return status;
        }
        if (mode == GeneralNoticeFixture::Mode::SteamSessionOnly)
        {
            status.state = GeneralAdvancedFeatureState::Registered;
            status.registered = true;
            status.failure = GeneralAdvancedFeatureFailure::StorageError;
            return status;
        }
        status.state = GeneralAdvancedFeatureState::RegistrationFailed;
        switch (mode)
        {
        case GeneralNoticeFixture::Mode::SteamInitFailed:
            status.connectionProblem = steam_bridge::SteamConnectionProblem::InitializationFailed;
            break;
        case GeneralNoticeFixture::Mode::SteamOutdated:
            status.connectionProblem = steam_bridge::SteamConnectionProblem::ClientOutdated;
            break;
        case GeneralNoticeFixture::Mode::SteamNotOwned:
            status.failure = GeneralAdvancedFeatureFailure::NotOwned;
            break;
        case GeneralNoticeFixture::Mode::SteamStorageFailed:
            status.failure = GeneralAdvancedFeatureFailure::StorageError;
            break;
        case GeneralNoticeFixture::Mode::SteamUnavailable:
            status.connectionProblem = steam_bridge::SteamConnectionProblem::ClientUnavailable;
            break;
        case GeneralNoticeFixture::Mode::SteamOffline:
            status.connectionProblem = steam_bridge::SteamConnectionProblem::Offline;
            break;
        case GeneralNoticeFixture::Mode::AppMismatch:
            status.connectionProblem = steam_bridge::SteamConnectionProblem::AppIdMismatch;
            break;
        case GeneralNoticeFixture::Mode::BridgeUnavailable:
            status.connectionProblem = steam_bridge::SteamConnectionProblem::SteamworksUnavailable;
            break;
        default:
            break;
        }
        status.errorDetail =
            L"steam_api64.dll: SteamAPI_InitEx failed\n"
            L"result=2 (k_ESteamAPIInitResult_NoSteamClient)\n"
            L"AppID=3801230 Profile=public-v1.0.12.0\n"
            L"client=2.10.91.91 (2026-09-18)";
        return status;
    };
    general.queryStartupConflict = [generalNotice] {
        GeneralStartupConflict conflict;
        if (generalNotice->mode == GeneralNoticeFixture::Mode::StartupInstalled)
        {
            conflict.kind = GeneralStartupConflictKind::InstalledVersionOwnsStartup;
            conflict.ownerCommand = L"SnowDesktop";
        }
        else if (generalNotice->mode == GeneralNoticeFixture::Mode::StartupOther)
        {
            conflict.kind = GeneralStartupConflictKind::NonPackagedVersionOwnsStartup;
            conflict.ownerCommand = L"C:\\Program Files\\SnowDesktop\\SnowDesktop.exe";
        }
        return conflict;
    };
    shell->SetGeneralPageActions(std::move(general));
    WidgetsPageActions widgetActions;
    widgetActions.invoke = [](std::uint64_t, WidgetsPageRequest) {};
    widgetActions.setDeveloperToolsEnabled = [](std::uint64_t, bool) { return true; };
    shell->SetWidgetsPageActions(std::move(widgetActions));
    PersonalizationPageActions appearance;
    const bool english = locale == "en-US";
    const auto themeLibrary = [english](std::uint64_t, const ThemeLibraryRequest& request) {
        ThemeLibraryResult result;
        if (request.command != ThemeLibraryCommand::Refresh) return result;
        result.succeeded = true;
        result.workshopAvailable = true;
        result.sharingAvailable = true;
        for (int index = 0; index < 4; ++index)
        {
            themes::Theme theme; theme.id = "preview-theme-" + std::to_string(index);
            theme.kind = index == 2 ? themes::Kind::Popup : index == 3 ? themes::Kind::QuickPanel : themes::Kind::Global;
            theme.name = english ? "Example theme for work and focus " : "工作与专注的示例主题 "; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            theme.name += std::to_string(index + 1); result.library.themes.emplace(theme.id, std::move(theme));
        }
        return result;
    };
    appearance.themeLibrary = themeLibrary;
    appearance.contextMenu = [english](const shell_extensions::Request&, bool) {
        shell_extensions::CatalogueView result; result.catalogue.revision = 1;
        for (int index = 0; index < 3; ++index)
        {
            shell_extensions::Registration item;
            item.id = "preview-menu-" + std::to_string(index);
            item.display.registration = item.id;
            item.display.label = english ? L"Example file action " : L"示例文件操作 "; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            item.display.label += std::to_wstring(index + 1);
            item.application = {"preview-tools", english ? L"Example tools" : L"示例工具"}; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            item.contexts = shell_extensions::ContextBit(shell_extensions::Context::File) |
                shell_extensions::ContextBit(shell_extensions::Context::Folder);
            item.linked = true; item.types = {L"*"};
            result.catalogue.rows.push_back(std::move(item));
        }
        return result;
    };
    appearance.listFonts = [] { return std::vector<app_fonts::Choice>{{{}, L"Segoe UI", L"Segoe UI"}}; };
    appearance.appliedFont = [] { return app_fonts::Selection{}; };
    shell->SetPersonalizationPageActions(std::move(appearance));
    DockPageActions dock; dock.themeLibrary = themeLibrary; shell->SetDockPageActions(std::move(dock));
    const auto modulePreviewPolicy = std::make_shared<runtime_modules::Policy>();
    shell->SetRuntimeModulesQuery([modulePreviewPolicy](auto completed) {
        runtime_modules::Snapshot snapshot;
        for (std::size_t i = 0; i < runtime_modules::kCount; ++i)
        {
            runtime_modules::Status status; status.module = static_cast<runtime_modules::Module>(i);
            status.allowed = modulePreviewPolicy->allowed[i]; status.active = status.allowed && i == 0 ? 1u : 0u;
            if (i < 2) status.targets.push_back({42, L"explorer.exe", true});
            snapshot.modules.push_back(std::move(status));
        }
        completed(std::move(snapshot));
    });
    CalendarPageActions calendarActions;
    calendarActions.events = [english](std::uint64_t) -> std::optional<std::vector<calendar::CalendarEvent>> {
        std::vector<calendar::CalendarEvent> result;
        for (int index = 0; index < 3; ++index)
        {
            calendar::CalendarEvent event; event.id = "preview-event-" + std::to_string(index); event.revision = 1;
            event.title = english ? "Project planning and weekly review" : "项目规划与每周回顾"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
            event.date = "2026-10-13"; event.startMinutes = 540 + index * 60; event.endMinutes = event.startMinutes + 30;
            result.push_back(std::move(event));
        }
        return result;
    };
    std::vector<calendar::CalendarSeries> previewSeries;
    for (const std::string kind : {"dates", "weekly", "monthly"})
    {
        calendar::CalendarSeries series;
        series.id = "preview-" + kind; series.revision = 1;
        series.event.title = kind + " / Preview";
        series.event.startMinutes = 600; series.event.endMinutes = 660; series.event.reminderMinutes = 15;
        series.rule.kind = kind; series.rule.startDate = "2026-10-06";
        if (kind == "dates")
        {
            series.rule.dates = {"2026-10-01", "2026-10-03", "2026-11-06"};
            series.rule.startDate = series.rule.dates.front(); series.rule.endDate = series.rule.dates.back();
        }
        else if (kind == "weekly")
        { series.rule.interval = 2; series.rule.weekdays = {2}; series.rule.endDate = "2026-11-16"; }
        else
        { series.rule.startDate = "2026-04-01"; series.rule.monthDay = 31; series.rule.endDate = "2027-04-30"; }
        series.event.date = series.rule.startDate;
        previewSeries.push_back(std::move(series));
    }
    calendarActions.series = [previewSeries](std::uint64_t) -> std::optional<std::vector<calendar::CalendarSeries>> {
        return previewSeries;
    };
    calendarActions.occurrence = [previewSeries](std::uint64_t, const std::string& id) -> std::optional<calendar::CalendarEvent> {
        const auto slash = id.find('/');
        if (slash == std::string::npos) return {};
        for (const auto& series : previewSeries)
        {
            const auto original = id.substr(slash + 1);
            if (series.id != id.substr(0, slash) || !calendar::CalendarService::MatchesRule(series.rule, original)) continue;
            auto event = series.event;
            event.id = id; event.revision = 1; event.seriesId = series.id; event.occurrenceDate = original;
            event.date = original == "2026-10-03" ? "2026-10-04" : original;
            event.occurrenceOverride = event.date != original;
            return event;
        }
        return {};
    };
    shell->SetCalendarPageActions(std::move(calendarActions));
    PageLayoutPageActions layout;
    layout.capture = [] { PageLayoutSnapshot snapshot; snapshot.revision = 1; snapshot.editable = true; snapshot.monitorCount = 2;
        snapshot.pages = {{L"example-1", L"Work / 工作", 12, 8, 10, 2}, {L"example-2", L"Projects / 项目", 10, 7, 5, 1}}; return snapshot; }; // l10n-allow: isolated bilingual preview fixture, not production UI text.
    shell->SetPageLayoutPageActions(std::move(layout));
    shell->SetLargeIconSettingsAction([expanded, disabled](LargeIconSettingsRequest request) {
        LargeIconSettingsSnapshot snapshot; snapshot.key = request.key; snapshot.name = L"Example / 示例"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
        snapshot.available = snapshot.editable = !disabled; snapshot.succeeded = true;
        snapshot.itemCount = expanded ? 3 : 1;
        if (disabled) snapshot.error = "largeIcon.locked";
        snapshot.session = snapshot.revision = 1;
        snapshot.maxColumns = snapshot.maxRows = 4; return snapshot;
    });
    auto libraryFixture = std::make_shared<IconLibrarySnapshot>();
    libraryFixture->succeeded = true; libraryFixture->revision = 1;
    const auto iconPreview = output / L"图标 # % preview.png"; // l10n-allow: isolated fixture checks Unicode and reserved characters in local image paths.
    std::vector<std::uint32_t> iconPixels(96 * 96);
    for (int y = 0; y < 96; ++y) for (int xx = 0; xx < 96; ++xx)
        if ((xx - 48) * (xx - 48) + (y - 48) * (y - 48) < 40 * 40) iconPixels[y * 96 + xx] = 0xffe88c22;
    std::string iconError;
    if (!preview_png::Save(iconPreview, 96, 96, iconPixels, iconError)) throw std::runtime_error(iconError);
    IconLibraryGroup sampleGroup; sampleGroup.id = "group-" + std::string(32, '1'); sampleGroup.name = L"Sample pack"; // l10n-allow: isolated preview fixture.
    libraryFixture->groups.push_back(sampleGroup);
    for (int index = 0; index < 7; ++index)
    {
        IconLibraryEntry entry; entry.id = std::string(63, 'a') + std::to_string(index) + ".png";
        entry.name = L"Preview icon " + std::to_wstring(index + 1); // l10n-allow: isolated preview fixture.
        entry.width = entry.height = 96; entry.format = "png"; entry.previewPath = iconPreview.wstring();
        entry.missing = index == 6; if (index % 2 == 0) entry.groupIds.push_back(sampleGroup.id);
        libraryFixture->entries.push_back(std::move(entry));
    }
    // Optional original input exercises the production library, including the
    // WebP decoder, without touching the desktop host or the user's library.
    wchar_t previewSource[32768]{};
    const auto previewSourceLength=GetEnvironmentVariableW(L"SNOWDESKTOP_ICON_LIBRARY_PREVIEW_SOURCE",previewSource,32768);
    if(previewSourceLength>0&&previewSourceLength<32768)
    {
        IconLibrary library(output/L"fixture-library"); IconLibraryRequest request;
        request.action="import"; request.paths={previewSource};
        const auto imported=library.Invoke(request);
        if(!imported.succeeded||imported.entries.size()!=1||imported.entries[0].missing)throw std::runtime_error("original preview import failed");
        libraryFixture->entries[1]=imported.entries[0];
    }
    libraryFixture->entries[2].groupNames.emplace(sampleGroup.id, L"Frost Browser");
    libraryFixture->entries[6].groupNames.emplace(sampleGroup.id, L"Frost Browser");
    libraryFixture->targetName = L"FrostBrowser.lnk";
    const auto pickerRequests=std::make_shared<std::vector<IconLibraryRequest>>();
    const auto libraryReads=std::make_shared<unsigned>(0);
    shell->SetIconLibraryAction([libraryFixture,pickerRequests,libraryReads](IconLibraryRequest request) {
        if (request.action != "read" && request.action != "refresh" && request.action!="apply" && request.action!="cancel") throw std::runtime_error("preview library mutations are disabled");
        if(request.action=="read"||request.action=="refresh")++*libraryReads;
        if(request.action=="apply"||request.action=="cancel")pickerRequests->push_back(request);
        auto result=*libraryFixture; result.target=request.target; result.session=1; return result;
    });
    struct AppearanceFixture
    {
        std::vector<IconAppearanceRecord> entries;
        IconBeautifySettings globalSettings;
        icon_appearance::EditPreview preview;
        unsigned liveUpdates = 0;
        std::uint64_t revision = 1, session = 0;
        std::wstring target;
        std::string png;
    };
    const auto appearanceFixture = std::make_shared<AppearanceFixture>();
    {
        auto style = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlassDark); style.mode = 1;
        appearanceFixture->entries.push_back({L"control:start", _LW("iconLibrary.startIcon"), style, {}});
        style.enabled = false;
        appearanceFixture->entries.push_back({L"recycle-empty:PREVIEW", _LW("iconLibrary.recycleEmpty"), style, {}});
        icon_image::Image source; std::string error;
        if (!icon_image::Decode(libraryFixture->entries[1].previewPath, 128, source, error) ||
            !icon_image::EncodePng(source, appearanceFixture->png, error)) throw std::runtime_error("appearance preview source failed");
    }
    shell->SetIconAppearanceAction([appearanceFixture, dark](IconAppearanceRequest request) {
        auto& state = *appearanceFixture; IconAppearanceSnapshot result;
        result.succeeded = true; result.available = true;
        if (request.action == "begin") { state.preview.Clear(); state.target = request.target; ++state.session; }
        if (request.action == "previewApply" || request.action == "previewInherit")
        {
            std::optional<IconAppearanceRecord> value;
            if (request.action == "previewApply") value = IconAppearanceRecord{request.target, {}, request.settings, request.themeId, true, request.followGlobalTheme};
            result.succeeded = state.preview.Update(request, state.target, state.session, state.revision, std::move(value));
            if (result.succeeded) ++state.liveUpdates;
            else result.error = "iconLibrary.stale";
            return result;
        }
        if (request.action == "close" || (request.action == "cancel" && request.target == state.target && request.session == state.session))
        { state.preview.Clear(); state.target.clear(); ++state.session; }
        if (request.action == "apply" || request.action == "inherit")
        {
            if ((request.action == "apply" || request.session) && !icon_appearance::CurrentEdit(request, state.target, state.session, state.revision))
            { result.succeeded = false; result.error = "iconLibrary.stale"; return result; }
            std::erase_if(state.entries, [&](const auto& record) { return record.target == request.target; });
            if (request.action == "apply") state.entries.push_back({request.target, _LW("iconLibrary.searchIcon"), request.settings, request.themeId, true, request.followGlobalTheme});
            ++state.revision; state.preview.Clear(); state.target.clear(); ++state.session;
        }
        result.current.target = request.target;
        result.current.name = request.target == L"control:search" ? _LW("iconLibrary.searchIcon") : _LW("iconLibrary.startIcon");
        result.globalSettings = state.globalSettings;
        result.current.settings = state.globalSettings;
        result.current.followGlobalTheme = true;
        for (const auto& entry : state.entries) if (entry.target == request.target) { result.current = entry; result.independent = true; }
        result.current.settings = icon_appearance::Resolve(state.globalSettings, result.independent ? &result.current : nullptr, false, false);
        result.entries = state.entries;
        for (auto& record : result.entries) record.settings = icon_appearance::Resolve(state.globalSettings, &record, false, false);
        result.session = state.session; result.revision = state.revision;
        result.previewPng = state.png;
        if (request.target == L"control:search")
        {
            std::string error;
            result.nativeControlSource = true;
            if (!icon_image::EncodePng(icon_control::MakeSource(request.target, !dark), result.previewPng, error))
                throw std::runtime_error("native control preview source failed");
        }
        return result;
    });
    bool loaded = false;
    const auto token = shell->Loaded([&](const auto&, const auto&) { loaded = true; });
    if (!host.runtime.Attach(host.window, shell.as<x::UIElement>())) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    host.runtime.ResizeToClient(); PumpUntil([&] { return loaded; }); shell->Loaded(token);
    std::ofstream manifest(output / L"render.tsv");
    manifest << "page\timage\twidthDip\theightDip\tscrollY\tscrollMax\n";
    std::ofstream rows(output / L"rows.tsv");
    std::ofstream choices(output / L"choices.tsv");
    choices << "page\tlabel\tcontrolRight\teditorRight\tcontrolWidth\n";
    rows << "page\tlabel\tx\ty\twidth\theight\tpaddingLeft\tpaddingTop\tpaddingRight\tpaddingBottom\teditorX\teditorY\teditorWidth\teditorHeight\n";
    std::ofstream actions(output / L"actions.tsv");
    actions << "page\tgradientDetailsInvoked\tvisibleRemoveStopButtons\n";
    // Diagnostic capture helpers for notices that are normally closed in the
    // default preview state. The General notice fixture drives every dormant
    // advanced-feature/startup warning, then this renders the production
    // InfoBar in place so the exported PNG matches the real card layout.
    const auto savePreviewElement = [&](const x::FrameworkElement& element, const std::wstring& name) {
        x::Media::Imaging::RenderTargetBitmap bitmap;
        Await(bitmap.RenderAsync(element));
        const auto pixels = Await(bitmap.GetPixelsAsync());
        BYTE* bytes = nullptr;
        winrt::check_hresult(pixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
        std::string error;
        if (!preview_png::Save(output / name, bitmap.PixelWidth(), bitmap.PixelHeight(),
            {reinterpret_cast<const std::uint32_t*>(bytes), pixels.Length() / 4}, error))
            throw std::runtime_error(error);
    };
    const auto scrollPreviewElementIntoView = [&](const x::FrameworkElement& element) {
        const auto scroll = shell->FindName(L"PageScrollViewer").as<c::ScrollViewer>();
        const auto cards = shell->FindName(L"PageCards").as<c::StackPanel>();
        const double offset = element.TransformToVisual(cards).TransformPoint({0, 0}).Y;
        (void)scroll.ChangeView(nullptr,
            winrt::box_value(std::max(0.0, offset - 64.0)).as<winrt::Windows::Foundation::IReference<double>>(),
            nullptr, true);
        Settle(shell.as<x::FrameworkElement>());
    };
    const auto captureGeneralNotices = [&] {
        const auto cards = shell->FindName(L"PageCards").as<c::StackPanel>();
        const auto findBars = [&] {
            std::vector<c::InfoBar> bars;
            Walk(cards, [&](const x::DependencyObject& node) {
                if (const auto bar = node.try_as<c::InfoBar>()) bars.push_back(bar);
            });
            if (bars.size() < 2) throw std::runtime_error("general page notices are not mounted");
            return bars;
        };
        std::ofstream report(output / L"general-notices.tsv");
        report << "variant\tseverity\twidth\theight\tvisibleActions\n";
        struct AdvancedVariant { const wchar_t* slug; GeneralNoticeFixture::Mode mode; };
        const AdvancedVariant advanced[] = {
            {L"notice-advanced-steam-init-failed", GeneralNoticeFixture::Mode::SteamInitFailed},
            {L"notice-advanced-steam-update-required", GeneralNoticeFixture::Mode::SteamOutdated},
            {L"notice-advanced-steam-not-owned", GeneralNoticeFixture::Mode::SteamNotOwned},
            {L"notice-advanced-storage-failed", GeneralNoticeFixture::Mode::SteamStorageFailed},
            {L"notice-advanced-session-only", GeneralNoticeFixture::Mode::SteamSessionOnly},
            {L"notice-advanced-verification-failed", GeneralNoticeFixture::Mode::VerificationFailed},
            {L"notice-advanced-steam-unavailable", GeneralNoticeFixture::Mode::SteamUnavailable},
            {L"notice-advanced-steam-offline", GeneralNoticeFixture::Mode::SteamOffline},
            {L"notice-advanced-app-mismatch", GeneralNoticeFixture::Mode::AppMismatch},
            {L"notice-advanced-bridge-unavailable", GeneralNoticeFixture::Mode::BridgeUnavailable}};
        for (const auto& variant : advanced)
        {
            generalNotice->mode = variant.mode;
            shell->RefreshRuntimeState();
            Settle(shell.as<x::FrameworkElement>());
            auto bars = findBars();
            auto bar = bars[1];
            if (bar.Visibility() != x::Visibility::Visible || !bar.IsOpen())
                throw std::runtime_error("advanced-feature notice did not open");
            scrollPreviewElementIntoView(bar);
            unsigned actions = 0;
            Walk(bar, [&](const x::DependencyObject& node) {
                if (const auto button = node.try_as<c::Button>();
                    button && button.Visibility() == x::Visibility::Visible) ++actions;
            });
            std::wstring fileName = L"general-";
            fileName += variant.slug;
            fileName += L".png";
            savePreviewElement(bar, fileName);
            report << winrt::to_string(winrt::hstring{variant.slug}) << '\t'
                   << static_cast<int>(bar.Severity()) << '\t' << bar.ActualWidth()
                   << '\t' << bar.ActualHeight() << '\t' << actions << '\n';
        }
        // The hint asks users to share the details below; capture that state
        // with the detail expander opened and the production text visible.
        {
            generalNotice->mode = GeneralNoticeFixture::Mode::SteamInitFailed;
            shell->RefreshRuntimeState();
            Settle(shell.as<x::FrameworkElement>());
            auto bars = findBars();
            auto bar = bars[1];
            const auto cardContent = bar.Parent().try_as<x::FrameworkElement>();
            if (cardContent)
            {
                Walk(cardContent, [&](const x::DependencyObject& node) {
                    const auto expander = node.try_as<c::Expander>();
                    if (!expander) return;
                    bool stringHeader = true;
                    try { (void)winrt::unbox_value<winrt::hstring>(expander.Header()); }
                    catch (...) { stringHeader = false; }
                    if (!stringHeader) return;
                    expander.IsExpanded(true);
                    expander.StartBringIntoView();
                });
                Settle(shell.as<x::FrameworkElement>());
                scrollPreviewElementIntoView(cardContent);
                savePreviewElement(cardContent, L"general-notice-advanced-steam-init-failed-details.png");
                report << "advanced-steam-init-failed-details\t-1\t" << cardContent.ActualWidth()
                       << '\t' << cardContent.ActualHeight() << '\t' << 0 << '\n';
            }
        }
        struct StartupVariant { const wchar_t* slug; GeneralNoticeFixture::Mode mode; };
        const StartupVariant startup[] = {
            {L"notice-startup-installed-version", GeneralNoticeFixture::Mode::StartupInstalled},
            {L"notice-startup-other-version", GeneralNoticeFixture::Mode::StartupOther}};
        for (const auto& variant : startup)
        {
            generalNotice->mode = variant.mode;
            shell->RefreshRuntimeState();
            Settle(shell.as<x::FrameworkElement>());
            auto bars = findBars();
            auto bar = bars[0];
            if (bar.Visibility() != x::Visibility::Visible || bar.Message().empty())
                throw std::runtime_error("startup-ownership notice did not open");
            scrollPreviewElementIntoView(bar);
            std::wstring fileName = L"general-";
            fileName += variant.slug;
            fileName += L".png";
            savePreviewElement(bar, fileName);
            report << winrt::to_string(winrt::hstring{variant.slug}) << '\t'
                   << static_cast<int>(bar.Severity()) << '\t' << bar.ActualWidth()
                   << '\t' << bar.ActualHeight() << '\t' << 0 << '\n';
        }
        generalNotice->mode = GeneralNoticeFixture::Mode::Registered;
        shell->RefreshRuntimeState();
        Settle(shell.as<x::FrameworkElement>());
    };
    std::uint64_t revision = 0;
    bool rendered = false;
    for (const auto page : pages)
    {
        const std::string key(SettingsPageKey(page));
        if (generalNotices && page != SettingsPage::General) continue;
        if (selected != "all" && selected != key) continue;
        rendered = true;
        SettingsSnapshot snapshot; snapshot.revision = ++revision; snapshot.generation = 1;
        snapshot.initialized = snapshot.sessionActive = true;
        snapshot.values.general.showAdvancedSettings = expanded || page == SettingsPage::RuntimeModules;
        snapshot.values.general.runtimeModules.allowed.fill(!disabled);
        *modulePreviewPolicy = snapshot.values.general.runtimeModules;
        snapshot.values.general.dockEnabled = !disabled;
        snapshot.values.general.statusBar.enabled = !disabled;
        snapshot.values.general.statusBar.batteryPercentage = expanded;
        snapshot.values.personalization.groupTabHoverSwitch = expanded;
        snapshot.values.personalization.categoryTabHoverSwitch = expanded;
        snapshot.values.personalization.popupHoverOpen = expanded;
        snapshot.values.dock.edgeRevealGesture = expanded ? 1 : 0;
        if (expanded && !custom)
        {
            snapshot.values.general.statusBar.position = snapshot.values.dock.position;
            snapshot.values.dock.edgeAttached = true;
        }
        snapshot.values.general.widgetDeveloperToolsEnabled = true;
        snapshot.values.dock.fullscreenExceptions = {
            {L"C:\\Preview\\Presentation.exe", DockFullscreenPolicy::Allow},
            {L"C:\\Preview\\Game.exe", DockFullscreenPolicy::FullProtection}};
        if (custom)
        {
            auto& icons = snapshot.values.desktop.iconBeautify;
            icons.enabled = true; icons.preset = IconBeautifyPreset::Custom;
            icons.glassEnabled = icons.edgeHighlightEnabled = icons.gradientEnabled = true;
            icons.filterEnabled = icons.outlineEnabled = true;
            snapshot.values.personalization.backgroundPreset = kAppearancePresetCustom;
            snapshot.values.personalization.panelGradient.enabled = true;
            snapshot.values.personalization.panelGradient.stops = {
                {0, 0xb6d6ef, .65}, {.5, 0xc8d0ef, .65}, {1, 0xd5c7ef, .65}};
            snapshot.values.dock.followComponentAppearance = false;
            snapshot.values.dock.appearancePreset = kAppearancePresetCustom;
            snapshot.values.dock.systemTaskbarBackdropEnabled = true;
            snapshot.values.dock.systemTaskbarFollowPersonalization = false;
            snapshot.values.dock.systemTaskbarAppearance.backgroundPreset = kAppearancePresetCustom;
            for (auto* theme : {&snapshot.values.general.quickNavigationAppearance,
                &snapshot.values.general.collectionPopupAppearance, &snapshot.values.general.statusBar.theme})
            { theme->mode = 4; theme->customized = true; theme->appearance.backgroundPreset = kAppearancePresetCustom; }
        }
        if (scenarioRules)
        {
            auto& taskbarRule = snapshot.values.dock.systemTaskbarShellUi;
            taskbarRule.enabled = true;
            taskbarRule.themeMode = SystemTaskbarThemeMode::Custom;
            taskbarRule.appearance.backgroundPreset = kAppearancePresetCustom;
            auto& statusRule = snapshot.values.general.statusBar.noWindow;
            statusRule.enabled = true;
            statusRule.theme.mode = 4;
            statusRule.theme.customized = true;
            statusRule.theme.appearance.backgroundPreset = kAppearancePresetCustom;
        }
        snapshot.domainRevisions = {revision, revision, revision, revision, revision, revision, revision};
        snapshot.route = SettingsRoute::ForPage(page);
        if (page == SettingsPage::WidgetSettings) snapshot.route = SettingsRoute::ForWidget(widgets.value.widgetId);
        if (page == SettingsPage::LargeIcon) snapshot.route.itemKey = L"settings-visual-example";
        snapshot.route.focusId = controlsGallery ? std::string{} : focusId;
        appearanceFixture->globalSettings = snapshot.values.desktop.iconBeautify;
        if (!shell->ApplySnapshot(snapshot)) throw std::runtime_error("preview route refused: " + key);
        HomeAboutStatusPatch status; status.generation = 1; status.revision = revision;
        status.usageGuideExpanded = expanded; status.installedWidgetCount = 3;
        status.backupState = SettingsBackupState::Ready; status.backupCount = 3;
        status.debugDataDirectory = L"C:\\Preview\\SnowDesktop";
        status.debugDesktopDirectory = L"C:\\Preview\\Desktop";
        if (page == SettingsPage::Debug)
        {
            status.performance.emplace(); auto& capture = *status.performance;
            capture.session = "preview-capture"; capture.generation = 1;
            capture.phase = expanded ? "recording" : "completed";
            capture.requestedSeconds = 120; capture.elapsedMs = 42000; capture.bufferKiB = 32768;
            capture.preset = 2; capture.threadCpu = true;
            capture.instances = {{L"preview-clock", L"Clock"}, {L"preview-calendar", L"Calendar"}};
            capture.selectedInstances = {L"preview-clock"};
            capture.trace = L"C:\\Preview\\Performance\\trace.pftrace";
            if (!expanded) { capture.analysis = "completed"; capture.quality = "valid"; capture.report = L"C:\\Preview\\Performance\\report.md"; }
        }
        (void)shell->ApplyHomeAboutStatusPatch(status);
        if (page == SettingsPage::Widgets || page == SettingsPage::DeveloperTools)
        {
            WidgetsPageSnapshot packages; packages.generation = 1; packages.revision = revision;
            if (searching) packages.task.kind = WidgetsPageTaskKind::Searching;
            packages.workshopAvailable = true;
            packages.developerPublisherAvailable = true;
            packages.developmentWorkspace = L"C:\\Preview\\Components";
            packages.componentCliPath = L"C:\\Preview\\SnowDesktop\\snowwidget.exe";
            WidgetAgentSkillTargetSnapshot skill; skill.id = "shared";
            skill.targetPath = L"C:\\Preview\\Skills";
            skill.state = WidgetAgentSkillInstallState::Current;
            skill.installed = skill.selected = true; skill.installedRevision = skill.bundledRevision = 1;
            packages.agentSkills.push_back(std::move(skill));
            for (int index = 0; index < 3; ++index)
            {
                InstalledWidgetPackageSnapshot package;
                package.packageId = L"preview-package-" + std::to_wstring(index);
                package.name = english ? L"Workspace overview and daily progress" : L"工作区概览与每日进度"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                package.name += L" " + std::to_wstring(index + 1);
                package.description = english ? L"An example component with a longer description for layout review." : L"用于检查长名称、状态与操作排列的示例组件。"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                if (index == 1)
                    package.description = english ? L"A desktop clock inspired by Xiaomi HyperOS, displaying 17:27 as Chinese numerals and full hours in a separate format, with configurable ink color, opacity, horizontal or vertical layout, and handwritten fonts." : L"一个仿小米澎湃系统的桌面时钟，用汉字数字写时间：17:27 显示「十七 二七」，整点显示「十七时」；可配置墨色、透明度、横竖排列与手迹字体。"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                if (index == 2 && focusId.starts_with("package-inline-layout"))
                    package.description.clear();
                package.version = L"1.2.0"; package.author = L"SnowDesktop"; package.builtIn = index == 0;
                package.enabled = index != 2; packages.installed.push_back(std::move(package));
                WidgetRuntimeDiagnosticSnapshot diagnostic;
                diagnostic.instanceId = L"widget-preview-" + std::to_wstring(index + 1);
                diagnostic.packageId = L"d81cee29-8023-47d8-bfaf-f39d8d7fd5ac";
                diagnostic.displayName = english ? L"Workspace overview" : L"工作区概览"; // l10n-allow: isolated bilingual preview fixture, not production UI text.
                diagnostic.displayName += L" " + std::to_wstring(index + 1);
                diagnostic.valid = diagnostic.hasManifest = true;
                diagnostic.scriptPath = L"C:\\Preview\\Components\\overview\\main.lua";
                packages.diagnostics.push_back(std::move(diagnostic));
            }
            if (!shell->ApplyWidgetsPageSnapshot(packages)) throw std::runtime_error("preview component snapshot refused");
        }
        if (page == SettingsPage::BackupAndData)
        {
            BackupDataPageSnapshot backups; backups.generation = 1; backups.revision = revision; backups.initialized = true;
            backups.dataDirectory = L"C:\\Preview\\SnowDesktop"; backups.fullBackupDirectory = L"D:\\Preview\\Backups";
            for (int index = 0; index < 3; ++index)
            {
                const auto id = L"preview-backup-" + std::to_wstring(index);
                const auto name = (english ? L"Before reorganizing the workspace " : L"重新整理工作区之前的备份 ") + std::to_wstring(index + 1); // l10n-allow: isolated bilingual preview fixture, not production UI text.
                backups.layoutBackups.push_back({id, name, L"2026-10-13 09:00", index == 0});
                backups.fullBackups.push_back({id, name, L"2026-10-13 09:00", L"Manual", 84, 12582912, false});
            }
            if (!shell->ApplyBackupDataPageSnapshot(backups)) throw std::runtime_error("preview backup snapshot refused");
        }
        Settle(shell.as<x::FrameworkElement>());
        const auto cards = shell->FindName(L"PageCards").as<c::StackPanel>();
        if (generalNotices)
        {
            captureGeneralNotices();
            return;
        }
        if (page == SettingsPage::Dock)
        {
            int appearanceActionCount = 0;
            Walk(cards, [&](const x::DependencyObject& node) {
                const auto button = node.try_as<c::Button>();
                if (!button || x::Automation::AutomationProperties::GetName(button) != _LW("iconAppearance.edit")) return;
                ++appearanceActionCount;
                const auto parent = x::Media::VisualTreeHelper::GetParent(button).as<c::Grid>();
                const auto bounds = button.TransformToVisual(parent).TransformBounds({0,0,static_cast<float>(button.ActualWidth()),static_cast<float>(button.ActualHeight())});
                if (bounds.X < 0 || bounds.X + bounds.Width > parent.ActualWidth() + 1)
                    throw std::runtime_error("Dock icon appearance action exceeds its native settings column");
                const auto icon = button.Content().try_as<c::IconElement>();
                if (!icon || icon.ActualWidth() <= 0 || icon.ActualHeight() <= 0)
                    throw std::runtime_error("Dock icon appearance action has no rendered icon");
                const auto iconBounds = icon.TransformToVisual(button).TransformBounds({0,0,static_cast<float>(icon.ActualWidth()),static_cast<float>(icon.ActualHeight())});
                if (iconBounds.X < 1 || iconBounds.Y < 1 ||
                    iconBounds.X + iconBounds.Width > button.ActualWidth() - 1 ||
                    iconBounds.Y + iconBounds.Height > button.ActualHeight() - 1)
                    throw std::runtime_error("Dock icon appearance glyph exceeds its button content area");
            });
            if (appearanceActionCount != 2)
                throw std::runtime_error("Dock preview must include both icon appearance actions");
        }
        if (page == SettingsPage::AppearanceIconBeautification)
        {
            auto list = PreviewControl<c::ListView>(cards, L"IconAppearanceList");
            if (list.Items().Size() != 2) throw std::runtime_error("following icons must not enter individual appearance list");
            auto targetRoute = SettingsRoute::ForPage(SettingsPage::AppearanceIconBeautification); targetRoute.itemKey = L"control:start";
            if (!shell->Navigate(targetRoute)) throw std::runtime_error("Start appearance route rejected");
            const auto open = [&] {
                shell->OpenRequestedIconAppearance(); c::ContentDialog opened{nullptr};
                PumpUntil([&] {
                    for (const auto& popup : x::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(shell->XamlRoot()))
                        if (popup.Child()) Walk(popup.Child(), [&](const x::DependencyObject& node) {
                            if (const auto dialog = node.try_as<c::ContentDialog>(); dialog && x::Automation::AutomationProperties::GetAutomationId(dialog) == L"IconAppearanceEditor") opened = dialog;
                        });
                    return opened && opened.ActualWidth() > 0;
                });
                Settle(opened);
                if (opened.ActualTheme() != shell->ActualTheme()) throw std::runtime_error("icon appearance dialog did not inherit its parent theme");
                return opened;
            };
            const auto close = [&](const c::ContentDialog& opened) {
                bool closed = false; const auto token = opened.Closed([&](const auto&, const auto&) { closed = true; });
                opened.Hide(); PumpUntil([&] { return closed; }); opened.Closed(token); Settle(shell.as<x::FrameworkElement>());
            };
            const auto captureAppearance = [&](const x::FrameworkElement& opened, const wchar_t* name) {
                Settle(opened);
                if (opened.ActualWidth() > shell->ActualWidth() || opened.ActualHeight() > shell->ActualHeight())
                    throw std::runtime_error("individual icon editor exceeds its settings window");
                x::Media::Imaging::RenderTargetBitmap bitmap; Await(bitmap.RenderAsync(opened));
                const auto pixels = Await(bitmap.GetPixelsAsync()); BYTE* bytes = nullptr;
                winrt::check_hresult(pixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
                std::string imageError;
                if (!preview_png::Save(output / name, bitmap.PixelWidth(), bitmap.PixelHeight(),
                    {reinterpret_cast<const std::uint32_t*>(bytes), pixels.Length()/4}, imageError)) throw std::runtime_error(imageError);
            };
            const auto applyAndClose = [&](const c::ContentDialog& opened, const c::Button& button, const char* stage) {
                std::ofstream(output / L"icon-appearance-stage.txt") << stage;
                const auto closed = std::make_shared<bool>(false);
                const auto token = opened.Closed([closed](const auto&, const auto&) { *closed = true; });
                try {
                    if (!opened.IsLoaded()) throw std::runtime_error("icon appearance dialog closed before its Apply action");
                    InvokePreview(button); PumpUntil([&] { return *closed; });
                }
                catch (...) {
                    opened.Closed(token);
                    try { captureAppearance(opened, L"icon-appearance-apply-failure.png"); } catch (...) {}
                    throw;
                }
                opened.Closed(token); Settle(shell.as<x::FrameworkElement>());
            };
            auto first = open();
            auto preview = PreviewControl<c::Image>(first, L"IconAppearancePreview");
            if (!preview.Source().try_as<x::Media::Imaging::WriteableBitmap>()) throw std::runtime_error("per-icon preview has no decoded pixels");
            c::ComboBox imageMode{nullptr};
            Walk(first, [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>(); combo &&
                    x::Automation::AutomationProperties::GetName(combo) == _LW("iconStyle.strategy")) imageMode = combo;
            });
            if (!imageMode) throw std::runtime_error("image no-enlargement selector is missing");
            imageMode.SelectedIndex(2);
            PumpUntil([&] { return appearanceFixture->liveUpdates > 0; });
            const auto liveStart = appearanceFixture->preview.Effective(L"control:start", nullptr);
            if (!liveStart || liveStart->settings.mode != 2 || appearanceFixture->entries.front().settings.mode != 1)
                throw std::runtime_error("live independent preview was not sent or changed the saved style before Apply");
            captureAppearance(first, L"icon-appearance-image-no-upscale.png");
            close(first);
            if (appearanceFixture->preview.Effective(L"control:start", nullptr))
                throw std::runtime_error("cancel left a live independent appearance active");
            auto second = open(); close(second);
            if (appearanceFixture->entries.size() != 2) throw std::runtime_error("cancel or reopening unexpectedly saved an icon override");
            targetRoute.itemKey = L"control:search";
            if (!shell->Navigate(targetRoute)) throw std::runtime_error("Search appearance route rejected");
            auto editor = open();
            const auto sourceMode = PreviewControl<c::ComboBox>(editor, L"IconAppearanceSource");
            if (sourceMode.SelectedIndex() != 0) throw std::runtime_error("unconfigured icons must default to full inheritance");
            sourceMode.SelectedIndex(1); Settle(editor);
            c::ComboBox mode{nullptr}, theme{nullptr};
            Walk(editor, [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>())
                {
                    const auto name = x::Automation::AutomationProperties::GetName(combo);
                    if (name == _LW("iconStyle.strategy")) mode = combo;
                    if (name == _LW("app.settings.icon_beautify")) theme = combo;
                }
            });
            if (!mode || !theme) throw std::runtime_error("shared processing mode/theme selectors are missing");
            if (mode.Items().Size() != 3 || theme.SelectedIndex() != 6)
                throw std::runtime_error("new per-icon editor must default to global theme with three independent strategies");
            mode.SelectedIndex(2);
            appearanceFixture->globalSettings = icon_beautify::MakePreset(IconBeautifyPreset::FrostedGlassDark);
            appearanceFixture->globalSettings.mode = 1;
            auto globalChange = snapshot; globalChange.route = targetRoute; globalChange.revision = ++revision; ++globalChange.domainRevisions.desktop;
            globalChange.values.desktop.iconBeautify = appearanceFixture->globalSettings;
            if (!shell->ApplySnapshot(globalChange)) throw std::runtime_error("live global theme update rejected");
            Settle(editor);
            const auto liveSearch = appearanceFixture->preview.Effective(L"control:search", nullptr);
            if (!liveSearch || !liveSearch->followGlobalTheme || liveSearch->settings.mode != 2 || appearanceFixture->entries.size() != 2)
                throw std::runtime_error("Start/Search live processing preview failed to retain global theme without saving");
            // Enabling the global theme first realizes its dependent strategy control.
            c::ComboBox globalMode{nullptr};
            Walk(shell->FindName(L"PageCards").as<x::DependencyObject>(), [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>(); combo &&
                    x::Automation::AutomationProperties::GetName(combo) == _LW("iconStyle.strategy")) globalMode = combo;
            });
            if (!globalMode || globalMode.Items().Size() != 2) throw std::runtime_error("global processing must expose exactly two strategies");
            if (theme.SelectedIndex() != 6 || mode.SelectedIndex() != 2)
                throw std::runtime_error("global theme update overwrote independent processing or theme inheritance");
            captureAppearance(editor, L"icon-appearance-follow-global.png");
            mode.SelectedIndex(0); theme.SelectedIndex(3);
            captureAppearance(editor, L"icon-appearance-native-smart.png");
            mode.SelectedIndex(2);
            captureAppearance(editor, L"icon-appearance-editor.png");
            c::Button apply{nullptr};
            Walk(editor, [&](const x::DependencyObject& node) { if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"PrimaryButton") apply = button; });
            if (!apply) throw std::runtime_error("individual appearance Apply button is missing");
            applyAndClose(editor, apply, "apply-search");
            if (appearanceFixture->entries.size() != 3) throw std::runtime_error("Search Apply did not save its independent record");
            if (appearanceFixture->entries.back().target != L"control:search" || appearanceFixture->entries.back().settings.mode != 2 ||
                appearanceFixture->entries.back().settings.preset != IconBeautifyPreset::FrostedGlassDark)
                throw std::runtime_error("Search mode/theme failed to reach the independent settings action");
            auto followEditor = open(); c::ComboBox followTheme{nullptr};
            Walk(followEditor, [&](const x::DependencyObject& node) {
                if (const auto combo = node.try_as<c::ComboBox>(); combo &&
                    x::Automation::AutomationProperties::GetName(combo) == _LW("app.settings.icon_beautify")) followTheme = combo;
            });
            if (!followTheme) throw std::runtime_error("per-icon theme selector is missing on reopen");
            followTheme.SelectedIndex(6); Settle(followEditor);
            c::Button followApply{nullptr};
            Walk(followEditor, [&](const x::DependencyObject& node) {
                if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"PrimaryButton") followApply = button;
            });
            if (!followApply) throw std::runtime_error("theme inheritance Apply is missing");
            applyAndClose(followEditor, followApply, "apply-followed-theme");
            if (!appearanceFixture->entries.back().followGlobalTheme || appearanceFixture->entries.back().settings.mode != 2)
                throw std::runtime_error("saving followed theme lost independent processing");
            list = PreviewControl<c::ListView>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceList");
            auto filter = PreviewControl<c::TextBox>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceSearch");
            filter.Text(_LW("iconLibrary.searchIcon")); Settle(shell.as<x::FrameworkElement>());
            if (list.Items().Size() != 1) throw std::runtime_error("individual icon list search failed");
            list.SelectedIndex(0);
            InvokePreview(PreviewControl<c::Button>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceInherit"));
            if (appearanceFixture->entries.size() != 2 || list.Items().Size() != 0) throw std::runtime_error("following global settings left an individual list record");
            filter.Text(L""); Settle(shell.as<x::FrameworkElement>());
            for (int remaining = 0; remaining < 2 && list.Items().Size(); ++remaining)
            {
                list.SelectedIndex(0);
                InvokePreview(PreviewControl<c::Button>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceInherit"));
                if (list.Items().Size() == 1)
                {
                    const auto single = PreviewControl<c::Grid>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceListContent");
                    captureAppearance(single, L"icon-appearance-one-item.png");
                }
            }
            Settle(shell.as<x::FrameworkElement>());
            const auto empty = PreviewControl<c::TextBlock>(shell->FindName(L"PageCards").as<x::DependencyObject>(), L"IconAppearanceEmpty");
            if (empty.Visibility() != x::Visibility::Visible || list.Visibility() != x::Visibility::Collapsed)
                throw std::runtime_error("individual appearance empty state failed to replace the list");
            x::Media::Imaging::RenderTargetBitmap emptyBitmap; Await(emptyBitmap.RenderAsync(shell.as<x::FrameworkElement>()));
            const auto emptyPixels = Await(emptyBitmap.GetPixelsAsync()); BYTE* emptyBytes = nullptr;
            winrt::check_hresult(emptyPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&emptyBytes));
            std::string emptyError;
            if (!preview_png::Save(output / L"icon-appearance-empty.png", emptyBitmap.PixelWidth(), emptyBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(emptyBytes), emptyPixels.Length()/4}, emptyError)) throw std::runtime_error(emptyError);
            std::ofstream(output / L"icon-appearance.tsv") << "following-hidden\tcancel\treopen\tpreview\tsearch-mode-theme\tremove-to-follow\npassed\tpassed\tpassed\tpassed\tpassed\tpassed\n";
        }
        if (page == SettingsPage::IconLibrary)
        {
            const auto grid = PreviewControl<c::GridView>(cards, L"IconLibraryGrid");
            const auto query = PreviewControl<c::TextBox>(cards, L"IconLibrarySearch");
            const auto groups = PreviewControl<c::ComboBox>(cards, L"IconLibraryGroups");
            const auto loadedImages = [&] {
                unsigned count = 0; bool complete = true;
                Walk(grid, [&](const x::DependencyObject& node) {
                    if (const auto image = node.try_as<c::Image>(); image &&
                        std::wstring_view(x::Automation::AutomationProperties::GetAutomationId(image)).starts_with(L"IconLibraryPreview-") &&
                        !winrt::unbox_value_or<bool>(image.Tag(), false))
                    {
                        ++count;
                        const auto source = image.Source().try_as<x::Media::Imaging::BitmapImage>();
                        complete = complete && source && source.PixelWidth() == 96 && source.PixelHeight() == 96;
                    }
                });
                return count == 6 && complete;
            };
            const auto waitForLibraryImages=[&](std::string_view stage) {
                try { PumpUntil(loadedImages); }
                catch (...) {
                    std::ofstream trace(output/L"icon-library-failure.tsv");
                    trace<<"stage\t"<<stage<<'\n';
                    Walk(grid,[&](const x::DependencyObject& node) {
                        trace<<winrt::to_string(winrt::get_class_name(node));
                        if(const auto element=node.try_as<x::FrameworkElement>())trace<<'\t'<<element.ActualWidth()<<'\t'<<element.ActualHeight()<<'\t'<<winrt::to_string(x::Automation::AutomationProperties::GetAutomationId(element));
                        if(const auto image=node.try_as<c::Image>())
                            if(const auto source=image.Source().try_as<x::Media::Imaging::BitmapImage>())trace<<"\timage="<<source.PixelWidth()<<'x'<<source.PixelHeight();
                        trace<<'\n';
                    });
                    try {
                        x::Media::Imaging::RenderTargetBitmap bitmap;Await(bitmap.RenderAsync(shell.as<x::UIElement>()));
                        const auto buffer=Await(bitmap.GetPixelsAsync());BYTE* bytes=nullptr;
                        winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
                        std::string error;preview_png::Save(output/L"icon-library-failure.png",bitmap.PixelWidth(),bitmap.PixelHeight(),
                            {reinterpret_cast<const std::uint32_t*>(bytes),buffer.Length()/4},error);
                    }catch(...){}
                    throw std::runtime_error("icon library images timed out: "+std::string(stage));
                }
            };
            // Inspect the production page's default before touching its filter.
            // Forcing All here would hide the first-group selection regression.
            Settle(shell.as<x::FrameworkElement>());
            if(groups.SelectedIndex()!=0||grid.Items().Size()!=7)
                throw std::runtime_error("library page did not default to all icons on entry");
            waitForLibraryImages("initial page");
            const auto libraryViewer = shell->FindName(L"PageScrollViewer").as<c::ScrollViewer>();
            const auto librarySurface = shell->FindName(L"PageSurface").as<c::Border>();
            const auto gridBottom = grid.TransformToVisual(libraryViewer).TransformPoint({0,static_cast<float>(grid.ActualHeight())}).Y;
            if (std::abs(gridBottom - (libraryViewer.ViewportHeight() - librarySurface.Margin().Bottom)) > 2.)
                throw std::runtime_error("library scroll list does not fill the remaining page viewport");
            if (query.Header() || grid.Items().Size() != 7 || groups.Items().Size() != 3)
                throw std::runtime_error("library tiles/groups/header mismatch: tiles=" + std::to_string(grid.Items().Size()) +
                    " groups=" + std::to_string(groups.Items().Size()) + " header=" + std::to_string(query.Header() ? 1 : 0));
            grid.SelectedIndex(0); groups.SelectedIndex(2); Settle(shell.as<x::FrameworkElement>());
            if (grid.Items().Size() != 4 || grid.SelectedItems().Size() != 1)
                throw std::runtime_error("group filter lost a visible selected resource identity");
            const auto readsBeforeRefresh=*libraryReads;
            PumpUntil([&]{return *libraryReads>readsBeforeRefresh;});
            if(groups.SelectedIndex()!=2||grid.Items().Size()!=4)
                throw std::runtime_error("library polling reset a manually selected group");
            const auto selectFiltered=PreviewControl<c::AppBarButton>(cards,L"IconLibrarySelectFiltered");
            InvokePreview(selectFiltered);
            if(grid.SelectedItems().Size()!=4)throw std::runtime_error("select filtered icons omitted resources in the current group");
            query.Text(L"no-matching-icon"); Settle(shell.as<x::FrameworkElement>());
            if (grid.Items().Size()) throw std::runtime_error("library search failed to filter resources");
            query.Text(L""); groups.SelectedIndex(0); Settle(shell.as<x::FrameworkElement>()); waitForLibraryImages("restore filter");
            const auto entries = libraryFixture->entries; libraryFixture->entries.clear(); ++libraryFixture->revision;
            PumpUntil([&] { return grid.Items().Size() == 0; });
            bool oldHint = false;
            Walk(cards, [&](const x::DependencyObject& node) {
                if (const auto text = node.try_as<c::TextBlock>(); text && text.Text() == _LW("iconLibrary.empty")) oldHint = true;
            });
            if (oldHint) throw std::runtime_error("removed empty-library hint is still mounted");
            libraryFixture->entries = entries; ++libraryFixture->revision;
            PumpUntil([&] { return grid.Items().Size() == 7 && loadedImages(); });
            // Supply only the host's completed-share snapshot. Keep the actual
            // presenter, timer, InfoBar close button and page navigation live;
            // this checks notice dismissal, not a real Steam upload.
            libraryFixture->completedAction="publishPrepared";
            libraryFixture->publishedUrl="https://steamcommunity.com/sharedfiles/filedetails/?id=1";
            libraryFixture->operationId=101;++libraryFixture->revision;
            c::InfoBar shareNotice{nullptr};
            PumpUntil([&] {
                Walk(cards,[&](const x::DependencyObject& node) {
                    if(const auto bar=node.try_as<c::InfoBar>();bar&&bar.IsOpen()&&
                        std::wstring_view(bar.Message()).find(_LW("iconWorkshop.openItem"))!=std::wstring_view::npos)shareNotice=bar;
                });return bool(shareNotice);
            });
            const auto closeShareNotice=[&] {
                Settle(shell.as<x::FrameworkElement>());
                c::Button close{nullptr};
                Walk(shareNotice,[&](const x::DependencyObject& node) {
                    if(const auto button=node.try_as<c::Button>();button&&IsPreviewElementVisible(button)) {
                        if(close)throw std::runtime_error("share notice has ambiguous close buttons");close=button;
                    }
                });
                if(!close)throw std::runtime_error("share notice close button missing");
                InvokePreview(close);PumpUntil([&]{return !shareNotice.IsOpen();});
            };
            closeShareNotice();
            const auto readsAfterClose=*libraryReads;
            PumpUntil([&]{return *libraryReads>=readsAfterClose+2;});
            if(shareNotice.IsOpen()||shareNotice.Visibility()!=x::Visibility::Collapsed)
                throw std::runtime_error("closed Workshop share notice reopened after polling");
            query.Text(L"Preview");Settle(shell.as<x::FrameworkElement>());
            if(shareNotice.IsOpen())throw std::runtime_error("gallery filtering reopened the dismissed share notice");
            query.Text(L"");Settle(shell.as<x::FrameworkElement>());
            if(!shell->Navigate(SettingsRoute::ForPage(SettingsPage::General))||
                !shell->Navigate(SettingsRoute::ForPage(SettingsPage::IconLibrary)))throw std::runtime_error("share notice return route failed");
            Settle(shell.as<x::FrameworkElement>());
            if(shareNotice.IsOpen())throw std::runtime_error("returning to the library reopened the dismissed share notice");
            libraryFixture->operationId=102;libraryFixture->busy=true;libraryFixture->progress=3;
            libraryFixture->completedAction.clear();++libraryFixture->revision;
            PumpUntil([&]{return shareNotice.IsOpen()&&std::wstring_view(shareNotice.Message()).find(_LW("iconLibrary.working"))!=std::wstring_view::npos;});
            libraryFixture->busy=false;libraryFixture->progress=0;libraryFixture->completedAction="publishPrepared";++libraryFixture->revision;
            // Updating the same Workshop URL is a new operation and must notify.
            PumpUntil([&]{return shareNotice.IsOpen()&&std::wstring_view(shareNotice.Message()).find(_LW("iconWorkshop.openItem"))!=std::wstring_view::npos;});
            closeShareNotice();const auto readsAfterSecondClose=*libraryReads;
            PumpUntil([&]{return *libraryReads>readsAfterSecondClose;});
            if(shareNotice.IsOpen())throw std::runtime_error("updated Workshop share notice reopened after dismissal");
            libraryFixture->completedAction.clear();libraryFixture->publishedUrl.clear();
            libraryFixture->error="iconLibrary.unavailable";libraryFixture->succeeded=false;++libraryFixture->revision;
            PumpUntil([&]{return shareNotice.IsOpen()&&shareNotice.Severity()==c::InfoBarSeverity::Error;});
            libraryFixture->error.clear();libraryFixture->succeeded=true;++libraryFixture->revision;
            PumpUntil([&]{return !shareNotice.IsOpen();});
            waitForLibraryImages("after share notice checks");
            std::ofstream(output/L"icon-library-share-notice.tsv")<<"close-and-poll\tfilter\tpage-return\tnew-progress\tnew-share-same-url\tnew-error\npassed\tpassed\tpassed\tpassed\tpassed\tpassed\n";
            struct PickerResult { bool completed = false; std::string id; };
            const auto pickerResult = std::make_shared<PickerResult>();
            const auto pickerReads = std::make_shared<unsigned>(0);
            struct PickerCleanup { std::function<void()> cancel; ~PickerCleanup() { if (cancel) cancel(); } } pickerCleanup;
            pickerCleanup.cancel = IconLibraryPagePresenter::Pick(shell->XamlRoot(),
                [](std::string_view name) { return std::wstring(_LW(std::string(name).c_str())); },
                [libraryFixture,pickerReads](IconLibraryRequest request) {
                    if (request.action != "read") throw std::runtime_error("picker preview mutations are disabled");
                    ++*pickerReads;
                    auto result = *libraryFixture; result.targetName.clear(); return result;
                }, entries.front().id, [pickerResult](std::string id) { pickerResult->id = std::move(id); pickerResult->completed = true; }, libraryFixture->targetName);
            c::ContentDialog pickerDialog{nullptr};
            PumpUntil([&] {
                for (const auto& popup : x::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(shell->XamlRoot()))
                    if (popup.Child()) Walk(popup.Child(), [&](const x::DependencyObject& node) {
                        if (const auto dialog = node.try_as<c::ContentDialog>()) pickerDialog = dialog;
                    });
                return pickerDialog && pickerDialog.ActualWidth() > 0;
            });
            const auto pickerGrid = PreviewControl<c::GridView>(pickerDialog, L"IconLibraryGrid");
            const auto pickerQuery = PreviewControl<c::TextBox>(pickerDialog, L"IconLibrarySearch");
            const auto pickerGroups = PreviewControl<c::ComboBox>(pickerDialog, L"IconLibraryGroups");
            Settle(pickerDialog);
            if (pickerGroups.SelectedIndex() != 0 || pickerGrid.Items().Size() != 7 ||
                winrt::unbox_value<winrt::hstring>(pickerGroups.SelectedItem().as<c::ComboBoxItem>().Tag()) != L"")
                throw std::runtime_error("fresh picker did not default to all icons");
            if (winrt::unbox_value<winrt::hstring>(pickerGrid.Items().GetAt(0)) != winrt::to_hstring(entries[2].id) ||
                winrt::unbox_value<winrt::hstring>(pickerGrid.SelectedItem()) != winrt::to_hstring(entries.front().id))
                throw std::runtime_error("All did not prioritize recommendations or changed the selected asset identity");
            if (pickerDialog.ActualTheme() != shell->ActualTheme())
                throw std::runtime_error("icon picker does not inherit its owner window theme");
            PumpUntil([&] { return pickerGrid.ContainerFromIndex(0) && pickerGrid.ContainerFromIndex(1) && pickerGrid.ContainerFromIndex(2); });
            const auto firstTile = pickerGrid.ContainerFromIndex(0).as<x::FrameworkElement>().TransformToVisual(pickerGrid).TransformPoint({0,0});
            const auto secondTile = pickerGrid.ContainerFromIndex(1).as<x::FrameworkElement>().TransformToVisual(pickerGrid).TransformPoint({0,0});
            const auto thirdTile = pickerGrid.ContainerFromIndex(2).as<x::FrameworkElement>().TransformToVisual(pickerGrid).TransformPoint({0,0});
            const int firstRowColumns = 1 + (std::abs(firstTile.Y - secondTile.Y) <= 1 ? 1 : 0) +
                (std::abs(firstTile.Y - thirdTile.Y) <= 1 ? 1 : 0);
            if (pickerDialog.Content().as<x::FrameworkElement>().ActualWidth() >= 500. &&
                (std::abs(firstTile.Y - secondTile.Y) > 1 || std::abs(firstTile.Y - thirdTile.Y) > 1 || firstTile.X >= secondTile.X || secondTile.X >= thirdTile.X))
                throw std::runtime_error("default icon picker does not show three columns");
            std::ofstream(output / L"icon-library-layout.tsv") << "gridHeight\tviewportHeight\tpickerWidth\tfirstRowColumns\n" <<
                grid.ActualHeight() << '\t' << libraryViewer.ViewportHeight() << '\t' << pickerDialog.Content().as<x::FrameworkElement>().ActualWidth() << '\t' << firstRowColumns << '\n';
            x::Media::Imaging::RenderTargetBitmap columnsBitmap; Await(columnsBitmap.RenderAsync(pickerDialog));
            const auto columnsPixels = Await(columnsBitmap.GetPixelsAsync()); BYTE* columnsBytes = nullptr;
            winrt::check_hresult(columnsPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&columnsBytes));
            if (!preview_png::Save(output / L"icon-picker-columns.png", columnsBitmap.PixelWidth(), columnsBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(columnsBytes), columnsPixels.Length() / 4}, iconError)) throw std::runtime_error(iconError);
            pickerGroups.SelectedIndex(1); Settle(pickerDialog);
            if (winrt::unbox_value<winrt::hstring>(pickerGroups.SelectedItem().as<c::ComboBoxItem>().Tag()) != L"recommended" ||
                pickerGrid.Items().Size() != 1 || winrt::unbox_value<winrt::hstring>(pickerGrid.Items().GetAt(0)) != winrt::to_hstring(entries[2].id) ||
                pickerDialog.IsPrimaryButtonEnabled())
                throw std::runtime_error("name recommendations did not use pack aliases, exclude missing assets or require explicit selection");
            pickerQuery.Text(L"Frost"); Settle(pickerDialog);
            if(pickerGrid.Items().Size()!=1)throw std::runtime_error("search ignored the alias used to recommend an asset");
            pickerQuery.Text(L""); Settle(pickerDialog);
            pickerGrid.SelectedIndex(0);
            // Polling must retain the user's filter and the captured target name,
            // even though ordinary library snapshots have no target metadata.
            const auto initialReads = *pickerReads;
            PumpUntil([&] {
                bool loaded=false; Walk(pickerGrid,[&](const x::DependencyObject& node) {
                    if(const auto image=node.try_as<c::Image>();image)
                        if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==96)loaded=true;
                });
                return *pickerReads > initialReads && loaded && pickerDialog.IsPrimaryButtonEnabled();
            });
            if(pickerGroups.SelectedIndex()!=1||pickerGrid.Items().Size()!=1)throw std::runtime_error("polling reset the recommendation filter");
            x::Media::Imaging::RenderTargetBitmap recommendedBitmap; Await(recommendedBitmap.RenderAsync(pickerDialog));
            const auto recommendedPixels = Await(recommendedBitmap.GetPixelsAsync()); BYTE* recommendedBytes = nullptr;
            winrt::check_hresult(recommendedPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&recommendedBytes));
            if (!preview_png::Save(output / L"icon-picker-recommended.png", recommendedBitmap.PixelWidth(), recommendedBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(recommendedBytes), recommendedPixels.Length() / 4}, iconError)) throw std::runtime_error(iconError);
            pickerGroups.SelectedIndex(0); Settle(pickerDialog);
            if(pickerGrid.Items().Size()!=7||winrt::unbox_value<winrt::hstring>(pickerGrid.Items().GetAt(0))!=winrt::to_hstring(entries[2].id))throw std::runtime_error("returning to All lost recommended-first ordering");
            pickerQuery.Text(L"Preview icon 3"); // l10n-allow: query matches the isolated preview fixture above.
            Settle(pickerDialog);
            if (pickerGrid.Items().Size() != 1) throw std::runtime_error("picker search lost its fixture resource");
            pickerGrid.SelectedIndex(0);
            PumpUntil([&] {
                bool pickerImageLoaded = false;
                Walk(pickerGrid, [&](const x::DependencyObject& node) {
                    if (const auto image = node.try_as<c::Image>(); image)
                        if (const auto bitmap = image.Source().try_as<x::Media::Imaging::BitmapImage>(); bitmap && bitmap.PixelWidth() == 96) pickerImageLoaded = true;
                });
                return pickerImageLoaded && pickerDialog.IsPrimaryButtonEnabled();
            });
            if (pickerDialog.ActualWidth() > shell->ActualWidth() || pickerDialog.ActualHeight() > shell->ActualHeight() ||
                pickerDialog.Content().as<x::FrameworkElement>().ActualWidth() > pickerDialog.ActualWidth())
                throw std::runtime_error("icon picker exceeds its dialog or window bounds");
            x::Media::Imaging::RenderTargetBitmap pickerBitmap;
            Await(pickerBitmap.RenderAsync(pickerDialog));
            const auto pickerPixels = Await(pickerBitmap.GetPixelsAsync()); BYTE* pickerBytes = nullptr;
            winrt::check_hresult(pickerPixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&pickerBytes));
            if (!preview_png::Save(output / L"icon-picker.png", pickerBitmap.PixelWidth(), pickerBitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(pickerBytes), pickerPixels.Length() / 4}, iconError)) throw std::runtime_error(iconError);
            c::Button useButton{nullptr};
            Walk(pickerDialog, [&](const x::DependencyObject& node) {
                if (const auto button = node.try_as<c::Button>(); button && button.Name() == L"PrimaryButton") useButton = button;
            });
            if (!useButton) throw std::runtime_error("icon picker use button is missing");
            InvokePreview(useButton);
            PumpUntil([&] { return pickerResult->completed; });
            if (pickerResult->id != entries[2].id) throw std::runtime_error("picker returned a display index instead of the selected resource identity");
            // The page remains active after dismissal. Repeat the same target
            // through the explicit host request used when route equality skips
            // page reconstruction, then select the actual imported WebP tile.
            auto targetRoute=SettingsRoute::ForPage(SettingsPage::IconLibrary);targetRoute.itemKey=L"item:preview";
            if(!shell->Navigate(targetRoute))throw std::runtime_error("icon target route rejected");
            const auto openDialog=[&]() {
                c::ContentDialog opened{nullptr};
                PumpUntil([&] {
                    for(const auto& popup:x::Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(shell->XamlRoot()))
                        if(popup.Child())Walk(popup.Child(),[&](const x::DependencyObject& node){if(const auto dialog=node.try_as<c::ContentDialog>())opened=dialog;});
                    return opened&&opened.ActualWidth()>0;
                });return opened;
            };
            const auto waitForAllPickerTiles=[&](const c::ContentDialog& opened) {
                const auto currentGrid=PreviewControl<c::GridView>(opened,L"IconLibraryGrid");
                const auto openedGroups=PreviewControl<c::ComboBox>(opened,L"IconLibraryGroups");
                if(openedGroups.SelectedIndex()!=0)throw std::runtime_error("reopened picker did not default to all icons");
                PumpUntil([&] {
                    unsigned loaded=0;
                    Walk(currentGrid,[&](const x::DependencyObject& node) {
                        if(const auto image=node.try_as<c::Image>();image&&!winrt::unbox_value_or<bool>(image.Tag(),false))
                            if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==96)++loaded;
                    });
                    return currentGrid.Items().Size()==7&&loaded==6;
                });
            };
            auto reopened=openDialog();
            waitForAllPickerTiles(reopened);
            PreviewControl<c::ComboBox>(reopened,L"IconLibraryGroups").SelectedIndex(1); Settle(reopened);
            if(PreviewControl<c::GridView>(reopened,L"IconLibraryGrid").Items().Size()!=1)throw std::runtime_error("host target name did not reach recommendations");
            reopened.Hide();PumpUntil([&]{return pickerRequests->size()==1;});
            libraryFixture->targetName=L"No matching application";
            shell->OpenRequestedIconPicker();reopened=openDialog();
            waitForAllPickerTiles(reopened);
            PreviewControl<c::ComboBox>(reopened,L"IconLibraryGroups").SelectedIndex(1); Settle(reopened);
            if(PreviewControl<c::GridView>(reopened,L"IconLibraryGrid").Items().Size()!=0||reopened.IsPrimaryButtonEnabled())throw std::runtime_error("picker reused recommendations from an earlier target");
            PreviewControl<c::ComboBox>(reopened,L"IconLibraryGroups").SelectedIndex(0); Settle(reopened);
            const auto reopenedGrid=PreviewControl<c::GridView>(reopened,L"IconLibraryGrid");reopenedGrid.SelectedIndex(1);
            PumpUntil([&]{
                bool originalLoaded=false;Walk(reopenedGrid,[&](const x::DependencyObject& node){
                    if(const auto image=node.try_as<c::Image>();image&&x::Automation::AutomationProperties::GetAutomationId(image)==L"IconLibraryPreview-"+winrt::to_hstring(entries[1].id))
                        if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==entries[1].width)originalLoaded=true;
                });return originalLoaded&&reopened.IsPrimaryButtonEnabled();
            });
            c::Button reopenedUse{nullptr};Walk(reopened,[&](const x::DependencyObject& node){if(const auto button=node.try_as<c::Button>();button&&button.Name()==L"PrimaryButton")reopenedUse=button;});
            if(!reopenedUse)throw std::runtime_error("reopened picker use button missing");
            InvokePreview(reopenedUse);PumpUntil([&]{return pickerRequests->size()==2;});
            if(pickerRequests->back().action!="apply"||pickerRequests->back().ids!=std::vector<std::string>{entries[1].id})throw std::runtime_error("reopened picker failed to apply original preview asset");
            if(!shell->Navigate(SettingsRoute::ForPage(SettingsPage::IconLibrary)))throw std::runtime_error("return to library route failed");
            const auto currentGrid=PreviewControl<c::GridView>(shell->FindName(L"PageCards").as<c::StackPanel>(),L"IconLibraryGrid");
            PumpUntil([&] {
                unsigned loaded=0;Walk(currentGrid,[&](const x::DependencyObject& node) {
                    if(const auto image=node.try_as<c::Image>();image&&!winrt::unbox_value_or<bool>(image.Tag(),false))
                        if(const auto bitmap=image.Source().try_as<x::Media::Imaging::BitmapImage>();bitmap&&bitmap.PixelWidth()==96)++loaded;
                });return currentGrid.Items().Size()==7&&loaded==6;
            });
            const auto pageGroups=PreviewControl<c::ComboBox>(shell->FindName(L"PageCards").as<c::StackPanel>(),L"IconLibraryGroups");
            pageGroups.SelectedIndex(2);Settle(shell.as<x::FrameworkElement>());
            if(currentGrid.Items().Size()!=4)throw std::runtime_error("library reentry fixture did not select a partial group");
            if(!shell->Navigate(SettingsRoute::ForPage(SettingsPage::General))||
                !shell->Navigate(SettingsRoute::ForPage(SettingsPage::IconLibrary)))throw std::runtime_error("library leave/return route failed");
            Settle(shell.as<x::FrameworkElement>());
            const auto returnedCards=shell->FindName(L"PageCards").as<c::StackPanel>();
            if(PreviewControl<c::ComboBox>(returnedCards,L"IconLibraryGroups").SelectedIndex()!=0||
                PreviewControl<c::GridView>(returnedCards,L"IconLibraryGrid").Items().Size()!=7)
                throw std::runtime_error("library page did not restore all icons after leaving and returning");
            std::ofstream(output / L"icon-library.tsv") << "stream-image-load\tgroup-filter\tstable-selection\tsearch\tempty-hint-removed\tdefault-all\trecommendations\treopen-all\ttarget-refresh\trecommended-first\tpage-default-all\tpage-reentry-all\tpage-poll-retention\npassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\tpassed\n";
        }
        if (page == SettingsPage::WidgetBehavior && expanded)
        {
            unsigned delays = 0;
            Walk(cards, [&](const x::DependencyObject& node) {
                const auto slider = node.try_as<c::Slider>();
                if (!slider) return;
                const auto label = x::Automation::AutomationProperties::GetName(slider);
                if (label != _LW("settings.widgetBehavior.tabHoverDelay") &&
                    label != _LW("app.settings.popup_hover_delay")) return;
                ++delays;
                if (slider.Value() != 250.0 || slider.StepFrequency() != 50.0)
                    throw std::runtime_error("hover default was rounded away from 250 ms");
            });
            if (delays != 2) throw std::runtime_error("hover delay editors are not mounted");
            std::ofstream(output / L"hover-delays.tsv") << "tabs\tpopup\n250\t250\n";
        }
        if (page == SettingsPage::DesktopPages)
        {
            c::ListView list{nullptr}; c::TextBox name{nullptr};
            c::NumberBox columns{nullptr}, gridRows{nullptr}; c::Button cancel{nullptr};
            Walk(cards, [&](const x::DependencyObject& node) {
                const auto element = node.try_as<x::FrameworkElement>();
                if (!element) return;
                const auto label = x::Automation::AutomationProperties::GetName(element);
                if (label == _LW("settings.pages.manage")) list = node.try_as<c::ListView>();
                if (label == _LW("settings.pages.name")) if (auto box = node.try_as<c::TextBox>()) name = box;
                if (label == _LW("settings.pages.columns")) if (auto box = node.try_as<c::NumberBox>()) columns = box;
                if (label == _LW("settings.pages.rows")) if (auto box = node.try_as<c::NumberBox>()) gridRows = box;
                if (const auto button = node.try_as<c::Button>(); button &&
                    winrt::unbox_value_or<winrt::hstring>(button.Content(), winrt::hstring{}) == _LW("app.settings.cancel")) cancel = button;
            });
            if (!list || !name || !columns || !gridRows || !cancel || list.Items().Size() < 2)
                throw std::runtime_error("page editor fixture incomplete");
            for (const int index : {1, 0, 1})
            {
                list.SelectedIndex(index); Settle(shell.as<x::FrameworkElement>());
                if (!list.IsEnabled() || !columns.IsEnabled() || !gridRows.IsEnabled())
                    throw std::runtime_error("selecting a page incorrectly entered rename state");
            }
            const auto original = name.Text();
            name.Text(original + L" edited"); Settle(shell.as<x::FrameworkElement>());
            if (list.IsEnabled() || columns.IsEnabled() || gridRows.IsEnabled())
                throw std::runtime_error("unsaved rename did not protect selected page");
            name.Text(original); Settle(shell.as<x::FrameworkElement>());
            if (!list.IsEnabled() || !columns.IsEnabled() || !gridRows.IsEnabled())
                throw std::runtime_error("restoring original name did not unlock grid");
            name.Text(original + L" draft"); Settle(shell.as<x::FrameworkElement>());
            InvokePreview(cancel); Settle(shell.as<x::FrameworkElement>());
            if (name.Text() != original || !columns.IsEnabled() || !gridRows.IsEnabled())
                throw std::runtime_error("cancel rename did not restore grid editing");
            std::ofstream(output / L"page-selection.tsv") << "selection\tedit\trevert\tcancel\npassed\tpassed\tpassed\tpassed\n";
        }
        if (page == SettingsPage::Calendar && !calendarScenario.empty())
        {
            const auto list = PreviewControl<c::ListView>(cards, L"CalendarSchedules");
            const int index = calendarScenario == "calendar-weekly" ? 4 : calendarScenario == "calendar-monthly" ? 5 : 3;
            list.SelectedIndex(index); Settle(shell.as<x::FrameworkElement>());
            const auto mode = PreviewControl<c::ComboBox>(cards, L"CalendarDateMode");
            if (mode.SelectedIndex() != index - 2) throw std::runtime_error("calendar series mode mismatch");
            if (calendarScenario == "calendar-dates" &&
                PreviewControl<c::CalendarView>(cards, L"CalendarMultipleDates").SelectedDates().Size() != 3)
                throw std::runtime_error("multiple-date selection was not restored");
            auto occurrence = PreviewControl<c::CalendarDatePicker>(cards, L"CalendarOccurrenceDate");
            if (!occurrence.Date()) throw std::runtime_error("series has no selectable occurrence");
            winrt::Windows::Globalization::Calendar original;
            original.SetDateTime(occurrence.Date().Value());
            if ((index == 4 && (original.Month() != 10 || original.Day() != 19)) ||
                (index == 5 && (original.Month() != 5 || original.Day() != 31)))
                throw std::runtime_error("series editor selected a non-occurrence date");
            if (calendarScenario == "calendar-occurrence")
            {
                original.Day(3);
                occurrence.Date(winrt::box_value(original.GetDateTime()).as<winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::DateTime>>());
                InvokePreview(PreviewControl<c::Button>(cards, L"CalendarEditOccurrence"));
                Settle(shell.as<x::FrameworkElement>());
                const auto scope = PreviewControl<c::ComboBox>(cards, L"CalendarScope");
                const auto actual = PreviewControl<c::CalendarDatePicker>(cards, L"CalendarDate");
                original.SetDateTime(actual.Date().Value());
                if (scope.SelectedIndex() != 0 || original.Day() != 4)
                    throw std::runtime_error("moved occurrence did not keep its original identity");
            }
            std::ofstream(output / L"calendar-mode.tsv") << calendarScenario << "\tpassed\n";
        }
        else if (page == SettingsPage::Calendar && custom)
        {
            Walk(cards, [](const x::DependencyObject& node) {
                if (const auto list = node.try_as<c::ListView>(); list && list.Items().Size() > 0)
                    list.SelectedIndex(0);
            });
            Settle(shell.as<x::FrameworkElement>());
        }
        if (expanded)
        {
            for (int pass = 0; pass < 4; ++pass)
            {
                Walk(cards, [](const x::DependencyObject& node) {
                    if (const auto expander = node.try_as<c::Expander>(); expander && expander.Visibility() == x::Visibility::Visible)
                        expander.IsExpanded(true);
                });
                Settle(shell.as<x::FrameworkElement>());
            }
        }
        if (page == SettingsPage::Widgets &&
            (focusId == "package-inline-layout" || focusId == "package-inline-layout-old"))
            InspectPackageInlineLayout(shell.as<x::FrameworkElement>(), output,
                focusId == "package-inline-layout-old");
        if (page == SettingsPage::Debug && expanded)
        {
            const auto list = PreviewControl<c::ListView>(cards, L"PerformanceCaptureInstances");
            if (list.SelectedItems().Size() != 1 ||
                winrt::unbox_value<winrt::hstring>(list.SelectedItems().GetAt(0).as<c::ListViewItem>().Tag()) != L"preview-clock")
                throw std::runtime_error("recording capture must retain the host-selected instance after layout");
            std::ofstream(output / L"performance-selection.tsv") << "selected-instance\tcount\npreview-clock\t1\n";
        }
        if (secretInput && page == SettingsPage::WidgetSettings)
            ExerciseSecretInput(shell.as<x::FrameworkElement>(), widgets, output);
        if (controlsGallery && page == SettingsPage::WidgetSettings)
        {
            InspectWidgetControls(shell.as<x::FrameworkElement>(), widgets, output);
            if (focusId == "exercise") ExerciseWidgetControls(shell.as<x::FrameworkElement>(), widgets, output);
        }
        const unsigned gradientDetailsInvoked = expanded
            ? ExpandGradientDetails(cards, shell.as<x::FrameworkElement>()) : 0;
        unsigned removeStopButtons = 0;
        Walk(cards, [&](const x::DependencyObject& node) {
            if (const auto button = node.try_as<c::Button>(); button && IsPreviewElementVisible(button) &&
                winrt::unbox_value_or<winrt::hstring>(button.Content(), winrt::hstring{}) ==
                    winrt::hstring{_LW("panelGradient.removeStop")}) ++removeStopButtons;
        });
        actions << key << '\t' << gradientDetailsInvoked << '\t' << removeStopButtons << '\n';
        const auto scroll = shell->FindName(L"PageScrollViewer").as<c::ScrollViewer>();
        Walk(cards, [&](const x::DependencyObject& node) {
            const auto row = node.try_as<c::Grid>();
            if (!row || row.ActualHeight() <= 0 || row.Visibility() != x::Visibility::Visible ||
                !row.Resources().HasKey(winrt::box_value(L"SnowDesktopSettingRow")) || row.Children().Size() < 2) return;
            const auto text = row.Children().GetAt(0).try_as<c::StackPanel>();
            const auto editor = row.Children().GetAt(1).try_as<x::FrameworkElement>();
            if (!text || text.Children().Size() == 0 || !editor) return;
            c::TextBlock label{nullptr};
            Walk(text, [&](const x::DependencyObject& child) {
                if (!label) label = child.try_as<c::TextBlock>();
            });
            if (!label) return;
            auto title = winrt::to_string(label.Text());
            std::replace(title.begin(), title.end(), '\t', ' '); std::replace(title.begin(), title.end(), '\n', ' ');
            const auto bounds = row.TransformToVisual(cards).TransformBounds({0, 0, static_cast<float>(row.ActualWidth()), static_cast<float>(row.ActualHeight())});
            const auto controls = editor.TransformToVisual(row).TransformBounds({0, 0, static_cast<float>(editor.ActualWidth()), static_cast<float>(editor.ActualHeight())});
            if (const auto host = editor.try_as<c::ContentControl>())
                if (const auto combo = host.Content().try_as<c::ComboBox>())
                {
                    const auto choice = combo.TransformToVisual(row).TransformBounds({0, 0,
                        static_cast<float>(combo.ActualWidth()), static_cast<float>(combo.ActualHeight())});
                    choices << key << '\t' << title << '\t' << choice.X + choice.Width << '\t'
                        << controls.X + controls.Width << '\t' << choice.Width << '\n';
                }
            const auto padding = row.Padding();
            rows << key << '\t' << title << '\t' << bounds.X << '\t' << bounds.Y << '\t' << bounds.Width << '\t' << bounds.Height
                << '\t' << padding.Left << '\t' << padding.Top << '\t' << padding.Right << '\t' << padding.Bottom
                << '\t' << controls.X << '\t' << controls.Y << '\t' << controls.Width << '\t' << controls.Height << '\n';
        });
        const double maximum = scroll.ScrollableHeight();
        if (restore)
        {
            const auto originalSize = shell->ActualSize();
            const auto originalRoute = shell->CurrentRoute();
            const auto originalGeneration = shell->CurrentGeneration();
            const auto originalRevision = shell->CurrentRevision();
            const auto originalIsland = host.runtime.IslandWindow();
            const auto navigation = shell->FindName(L"NavigationRoot").as<c::NavigationView>();
            const auto originalSelection = navigation.SelectedItem();
            std::vector<x::UIElement> originalCards;
            for (const auto& card : cards.Children()) originalCards.push_back(card);
            if (maximum > 0)
            {
                (void)scroll.ChangeView(nullptr, winrt::box_value(maximum / 2).as<winrt::Windows::Foundation::IReference<double>>(), nullptr, true);
                Settle(shell.as<x::FrameworkElement>());
            }
            const double originalOffset = scroll.VerticalOffset();
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                ShowWindow(host.window, SW_SHOWMINNOACTIVE);
                PumpUntil([&] { return IsIconic(host.window) != FALSE; });
                // Include WinUI's background resource timeout. Observe native
                // restoration without forcing a layout pass in this probe.
                const auto resumeAt = GetTickCount64() + (cycle == 0 ? 2300 : 100);
                PumpUntil([&] { return GetTickCount64() >= resumeAt; });
                ShowWindow(host.window, SW_SHOWNOACTIVATE);
                PumpUntil([&] { return IsIconic(host.window) == FALSE; });
                const auto settledAt = GetTickCount64() + 180;
                PumpUntil([&] { return GetTickCount64() >= settledAt; });
                if (shell->ActualSize() != originalSize || !host.runtime.IsAttached())
                    throw std::runtime_error("restore changed the Island bounds or detached its content");
                if (shell->CurrentRoute() != originalRoute || navigation.SelectedItem() != originalSelection)
                    throw std::runtime_error("restore changed the active settings page");
                if (shell->CurrentGeneration() != originalGeneration || shell->CurrentRevision() != originalRevision)
                    throw std::runtime_error("restore restarted or refreshed the settings session");
                if (host.runtime.IslandWindow() != originalIsland || cards.Children().Size() != originalCards.size())
                    throw std::runtime_error("restore replaced the retained settings view");
                for (std::uint32_t index = 0; index < originalCards.size(); ++index)
                    if (cards.Children().GetAt(index) != originalCards[index])
                        throw std::runtime_error("restore rebuilt a settings card");
                if (std::abs(scroll.VerticalOffset() - originalOffset) > .5)
                    throw std::runtime_error("restore changed the settings scroll position");
            }
            std::ofstream(output / L"restore.tsv") << "cycles\tretained-layout\tpage\tcontrols\tscroll\tsession\n3\tpassed\tpassed\tpassed\tpassed\tpassed\n";
        }
        const double step = std::max(100., scroll.ViewportHeight() - 48.);
        for (unsigned tile = 0; tile < 64; ++tile)
        {
            const double offset = std::min(maximum, tile * step);
            (void)scroll.ChangeView(nullptr, winrt::box_value(offset).as<winrt::Windows::Foundation::IReference<double>>(), nullptr, true);
            Settle(shell.as<x::FrameworkElement>());
            x::Media::Imaging::RenderTargetBitmap bitmap;
            const double raster = shell->XamlRoot().RasterizationScale();
            Await(bitmap.RenderAsync(shell.as<x::UIElement>(), static_cast<int>(std::lround(width / raster)), static_cast<int>(std::lround(height / raster))));
            const auto buffer = Await(bitmap.GetPixelsAsync());
            const int pixelWidth = bitmap.PixelWidth(), pixelHeight = bitmap.PixelHeight();
            if (pixelWidth <= 0 || pixelHeight <= 0 || std::abs(pixelWidth - width) > 1 ||
                std::abs(pixelHeight - height) > 1 || buffer.Length() != static_cast<unsigned>(pixelWidth * pixelHeight * 4))
                throw std::runtime_error("settings bitmap dimensions or pixel buffer mismatch");
            BYTE* bytes = nullptr;
            winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
            const std::string image = key + "-" + std::to_string(tile) + ".png";
            std::string error;
            if (!preview_png::Save(output / image, bitmap.PixelWidth(), bitmap.PixelHeight(),
                {reinterpret_cast<const std::uint32_t*>(bytes), static_cast<std::size_t>(buffer.Length() / 4)}, error))
                throw std::runtime_error(error);
            manifest << key << '\t' << image << '\t' << width << '\t' << height << '\t' << scroll.VerticalOffset() << '\t' << maximum << '\n';
            manifest.flush(); rows.flush();
            if (offset >= maximum) break;
        }
        if (page == SettingsPage::DesktopCategories && focusId.empty())
        {
            c::Expander editor{nullptr};
            c::HyperlinkButton addLink{nullptr};
            const winrt::hstring addText{_LW("app.settings.add_category")};
            Walk(cards, [&](const x::DependencyObject& node) {
                if (const auto section = node.try_as<c::Expander>(); section)
                    if (const auto heading = section.Header().try_as<c::TextBlock>(); heading && heading.Text() == addText)
                        editor = section;
                if (const auto link = node.try_as<c::HyperlinkButton>(); link &&
                    winrt::unbox_value_or<winrt::hstring>(link.Content(), winrt::hstring{}) == addText)
                    addLink = link;
            });
            if (!editor || !addLink || editor.Visibility() != x::Visibility::Collapsed)
                throw std::runtime_error("new category editor must be hidden on entry");
            const auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(addLink);
            if (!peer) throw std::runtime_error("add category link automation peer is missing");
            peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke)
                .as<x::Automation::Provider::IInvokeProvider>().Invoke();
            Settle(shell.as<x::FrameworkElement>());
            if (editor.Visibility() != x::Visibility::Visible || !editor.IsExpanded())
                throw std::runtime_error("add category link did not reveal the editor");
            for (const auto destination : {SettingsPage::General, SettingsPage::DesktopCategories})
            {
                snapshot.revision = ++revision;
                snapshot.domainRevisions = {revision, revision, revision, revision, revision, revision, revision};
                snapshot.route = SettingsRoute::ForPage(destination);
                if (!shell->ApplySnapshot(snapshot)) throw std::runtime_error("category revisit route refused");
                Settle(shell.as<x::FrameworkElement>());
            }
            if (editor.Visibility() != x::Visibility::Collapsed || editor.IsExpanded())
                throw std::runtime_error("new category editor must hide after leaving and returning");
            std::ofstream(output / L"category-lifecycle.tsv", std::ios::app)
                << "hidden-on-entry\tlink-revealed\thidden-on-reentry\n";
        }
    }
    shell->Close(); host.runtime.Detach(); shell = nullptr;
    if (!rendered) throw std::runtime_error("unknown settings page key");
}

// ---------------------------------------------------------------------------
// Notice catalog: every InfoBar / inline hint that is hidden in the default
// settings snapshot, rendered with the production localization strings,
// severity, close button and action button. This is an isolated XAML gallery,
// not a backend-driven state trace; the General page notices are additionally
// rendered in place through `general-notices` so their real card layout can be
// checked there.
// ---------------------------------------------------------------------------
struct NoticeCatalogSpec
{
    const wchar_t* page;
    const wchar_t* name;
    const wchar_t* slug;
    c::InfoBarSeverity severity = c::InfoBarSeverity::Informational;
    std::wstring title;
    std::wstring message;
    std::wstring action;
    bool closable = true;
    double width = 940.0;
    double inlineOpacity = 0.0;
    double inlineFontSize = 0.0;
};

std::wstring NoticeLocaleText(const char* key)
{
    return std::wstring(_LW(key));
}

std::wstring NoticeFormatFirst(std::wstring text, std::wstring_view marker,
    std::wstring_view value)
{
    const auto position = text.find(marker);
    if (position != std::wstring::npos) text.replace(position, marker.size(), value);
    return text;
}

const wchar_t* NoticeSeverityLabel(c::InfoBarSeverity severity)
{
    switch (severity)
    {
    case c::InfoBarSeverity::Error: return L"Error";
    case c::InfoBarSeverity::Warning: return L"Warning";
    case c::InfoBarSeverity::Success: return L"Success";
    default: return L"Information";
    }
}

std::vector<NoticeCatalogSpec> BuildNoticeCatalog()
{
    std::vector<NoticeCatalogSpec> notices;
    // Settings shell level notices.
    notices.push_back({L"Settings shell", L"Error status", L"shell-status-error",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.status.error"),
        NoticeLocaleText("settings.status.saveFailed"), {}, true, 1180.0});
    notices.push_back({L"Settings shell", L"Advanced features locked", L"shell-advanced-locked",
        c::InfoBarSeverity::Informational, NoticeLocaleText("settings.general.advancedFeatures"),
        NoticeLocaleText("settings.general.advancedFeatures.unlockRequired"), {}, true, 1180.0});
    notices.push_back({L"Settings shell", L"Focus prerequisite", L"shell-focus-prerequisite",
        c::InfoBarSeverity::Informational, {},
        NoticeFormatFirst(NoticeLocaleText("settings.search.prerequisite.enable"), L"{0}", L"Dock"),
        {}, true, 1000.0});
    // Personalization.
    notices.push_back({L"Personalization", L"Restart to apply font", L"personalization-font-restart",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("font.restartRequired"),
        NoticeLocaleText("font.restartNow"), false, 940.0});
    // Components page feedback bars (main, developer and debug share layout).
    notices.push_back({L"Components", L"Operation error", L"components-feedback-error",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.status.error"),
        NoticeLocaleText("app.settings.widgets_error_state_read"), {}, true, 940.0});
    notices.push_back({L"Components", L"Developer operation error", L"components-developer-error",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.status.error"),
        NoticeLocaleText("app.settings.widgets_error_state_read"), {}, true, 940.0});
    notices.push_back({L"Components", L"Debug operation error", L"components-debug-error",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.status.error"),
        NoticeLocaleText("app.settings.widgets_error_state_read"), {}, true, 940.0});
    // Dock and taskbar runtime notices.
    notices.push_back({L"Dock", L"Taskbar hiding failed", L"dock-suppress-failed",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("settings.dock.suppressTaskbar.failed"),
        {}, false, 940.0});
    notices.push_back({L"Dock", L"Taskbar connecting", L"dock-suppress-connecting",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("app.settings.taskbar_connecting"),
        {}, false, 940.0});
    notices.push_back({L"Dock", L"Icon operation failed", L"dock-icon-status-error",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("iconLibrary.stale"), {}, true, 940.0});
    notices.push_back({L"Dock", L"Tray target invalid", L"dock-tray-target-invalid",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("settings.dock.trayInvalidTarget"),
        {}, false, 940.0});
    notices.push_back({L"Taskbar", L"Connecting", L"taskbar-connecting",
        c::InfoBarSeverity::Informational, NoticeLocaleText("settings.taskbar.defaultAppearance"),
        NoticeLocaleText("app.settings.taskbar_connecting"), {}, true, 940.0});
    notices.push_back({L"Taskbar", L"Unsupported system", L"taskbar-unsupported",
        c::InfoBarSeverity::Warning, NoticeLocaleText("settings.taskbar.defaultAppearance"),
        NoticeLocaleText("app.settings.taskbar_unsupported"), {}, true, 940.0});
    notices.push_back({L"Taskbar", L"Connect failed", L"taskbar-connect-failed",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.taskbar.defaultAppearance"),
        NoticeLocaleText("app.settings.taskbar_connect_failed"), {}, true, 940.0});
    // Calendar notices (always Warning and not closable in production).
    notices.push_back({L"Calendar", L"Edit conflict", L"calendar-conflict",
        c::InfoBarSeverity::Warning, {}, NoticeLocaleText("settings.calendar.conflict"), {}, false, 940.0});
    notices.push_back({L"Calendar", L"Unsaved draft", L"calendar-unsaved",
        c::InfoBarSeverity::Warning, {}, NoticeLocaleText("settings.calendar.unsavedDraft"), {}, false, 940.0});
    notices.push_back({L"Calendar", L"Invalid values", L"calendar-invalid",
        c::InfoBarSeverity::Warning, {}, NoticeLocaleText("settings.calendar.invalid"), {}, false, 940.0});
    // Pages and grid result notices.
    notices.push_back({L"Pages & grid", L"Apply failed", L"pages-apply-failed",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.pages.status.error"),
        NoticeLocaleText("settings.pages.status.failed"), {}, true, 940.0});
    notices.push_back({L"Pages & grid", L"Stale layout", L"pages-stale",
        c::InfoBarSeverity::Warning, NoticeLocaleText("settings.pages.status.changed"),
        NoticeLocaleText("settings.pages.status.stale"), {}, true, 940.0});
    notices.push_back({L"Pages & grid", L"Applied", L"pages-applied",
        c::InfoBarSeverity::Success, NoticeLocaleText("settings.pages.status.success"),
        NoticeLocaleText("settings.pages.added"), {}, true, 940.0});
    // Backup and data result notice.
    notices.push_back({L"Backup & data", L"Operation failed", L"backup-operation-error",
        c::InfoBarSeverity::Error, NoticeLocaleText("settings.nav.backup"),
        NoticeLocaleText("settings.backup.error.dispatcherUnavailable"), {}, true, 940.0});
    // Icon library status variants.
    notices.push_back({L"Icon library", L"Working", L"icon-library-working",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("iconLibrary.working"), {}, true, 940.0});
    notices.push_back({L"Icon library", L"Workshop item published", L"icon-library-published",
        c::InfoBarSeverity::Informational, {},
        NoticeLocaleText("iconWorkshop.openItem") +
            L": https://steamcommunity.com/sharedfiles/filedetails/?id=3801234567",
        {}, true, 940.0});
    notices.push_back({L"Icon library", L"Operation error", L"icon-library-error",
        c::InfoBarSeverity::Error, {},
        NoticeLocaleText("iconLibrary.unavailable") +
            L"\nSample_Icon_01.png\nSample_Icon_02.png\n… (3)",
        {}, true, 940.0});
    // Large icon errors.
    notices.push_back({L"Large icon", L"Locked", L"large-icon-locked",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("largeIcon.locked"), {}, false, 940.0});
    notices.push_back({L"Large icon", L"Save failed", L"large-icon-save-failed",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("largeIcon.saveFailed"), {}, false, 940.0});
    // Theme library feedback and save validation.
    notices.push_back({L"Theme manager", L"Operation failed", L"theme-operation-failed",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("themeLibrary.operationFailed"), {}, true, 940.0});
    notices.push_back({L"Theme manager", L"Name required", L"theme-name-required",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("themeLibrary.nameRequired"), {}, true, 940.0});
    // Icon appearance page feedback: the presenter now marks failures as
    // errors and clears the banner on a successful poll.
    notices.push_back({L"Icon appearance", L"Operation error", L"icon-appearance-error",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("iconLibrary.unavailable"), {}, true, 940.0});
    // Status bar device-control feedback.
    notices.push_back({L"Status bar", L"Device operation failed", L"system-control-failed",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("controlCenter.failed"), {}, true, 940.0});
    notices.push_back({L"Status bar", L"Device access denied", L"system-control-access-denied",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("controlCenter.accessDenied"), {}, true, 940.0});
    // Quick navigation configuration validation.
    notices.push_back({L"Quick navigation", L"Invalid configuration", L"quicknav-invalid",
        c::InfoBarSeverity::Error, {}, NoticeLocaleText("quickNav.search.invalidConfiguration"),
        {}, false, 940.0});
    // Advanced-feature inline usage hints under the large-icon description.
    notices.push_back({L"General", L"Large icon usage", L"inline-advanced-usage",
        c::InfoBarSeverity::Informational, {},
        NoticeLocaleText("settings.general.advancedFeatures.largeIconsUsage"), {}, true, 940.0, 0.68, 12.0});
    notices.push_back({L"General", L"Advanced features unavailable", L"inline-advanced-unavailable",
        c::InfoBarSeverity::Informational, {},
        NoticeLocaleText("settings.general.advancedFeatures.availableAfterUnlock"), {}, true, 940.0, 0.68, 12.0});
    // Context menu empty/error state text.
    notices.push_back({L"Context menu", L"Load failed", L"inline-context-menu-failed",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("settings.contextMenu.failed"), {}, true, 940.0, 1.0});
    notices.push_back({L"Context menu", L"Nothing available", L"inline-context-menu-empty",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("settings.contextMenu.empty"), {}, true, 940.0, 1.0});
    // Font import error text on the personalization font card.
    notices.push_back({L"Personalization", L"Font import failed", L"inline-font-import-failed",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("font.importFailed"), {}, true, 940.0, 1.0});
    // Status bar Wi-Fi location denial text.
    notices.push_back({L"Status bar", L"Wi-Fi location denied", L"inline-wifi-location-denied",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("controlCenter.locationDenied"), {}, true, 940.0, 1.0});
    // Inline (non-InfoBar) conditional hints on settings pages.
    notices.push_back({L"Large icon", L"Batch edit hint", L"inline-large-icon-batch-hint",
        c::InfoBarSeverity::Informational, {},
        NoticeFormatFirst(NoticeLocaleText("largeIcon.batchHint"), L"{count}", L"3"), {}, true, 940.0, 1.0});
    notices.push_back({L"Widget settings", L"Field validation", L"inline-widget-validation",
        c::InfoBarSeverity::Informational, {},
        L"Value must be between 1 and 100. / 数值必须在 1 到 100 之间。", // l10n-allow: isolated bilingual preview fixture, not production UI text.
        {}, true, 940.0, 0.86});
    notices.push_back({L"Widget settings", L"Unknown field diagnostic", L"inline-widget-diagnostic",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("app.settings.widget.unknown_type"),
        {}, true, 940.0, 0.72});
    notices.push_back({L"Widget settings", L"Reference unavailable", L"inline-widget-reference",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("iconAppearance.unavailable"),
        {}, true, 940.0, 1.0, 12.0});
    notices.push_back({L"Widget settings", L"Search has no results", L"inline-widget-search-empty",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("app.nav.no_results"), {}, true, 940.0, 1.0});
    notices.push_back({L"Icon library", L"No matches", L"inline-icon-library-no-matches",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("iconLibrary.noMatches"), {}, true, 940.0, 1.0});
    notices.push_back({L"Pages & grid", L"Monitor mapping hint", L"inline-pages-mapping",
        c::InfoBarSeverity::Informational, {}, NoticeLocaleText("settings.pages.mapping.message"),
        {}, true, 940.0, 0.68, 12.0});
    return notices;
}

void ExportNoticeCatalog(const std::filesystem::path& output, bool dark,
    const std::string& locale, int width, int height)
{
    Locale::Instance().Init((std::filesystem::path(GetExecutableDirectoryPath()) / L"lang").c_str());
    if (!Locale::Instance().HasLanguage(locale)) throw std::runtime_error("preview locale is not installed");
    Locale::Instance().SetLanguage(locale.c_str());
    Apartment apartment; winrt::check_hresult(apartment.result);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RenderHost host;
    if (!host.runtime.Initialize()) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    WNDCLASSW previewClass{};
    previewClass.lpfnWndProc = RenderHost::Procedure;
    previewClass.hInstance = GetModuleHandleW(nullptr);
    previewClass.lpszClassName = L"SnowDesktop.SettingsRenderHost";
    if (!RegisterClassW(&previewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) winrt::throw_last_error();
    host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        previewClass.lpszClassName, L"SnowDesktop offline notice catalog", WS_POPUP,
        -32000, -32000, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), &host.runtime);
    if (!host.window) winrt::throw_last_error();
    ShowWindow(host.window, SW_SHOWNOACTIVATE);
    const double scale = GetDpiForWindow(host.window) / 96.;
    SetWindowPos(host.window, nullptr, 0, 0, static_cast<int>(std::ceil(width * scale)),
        static_cast<int>(std::ceil(height * scale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    auto cardStyle = x::Markup::XamlReader::Load(
        LR"(<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="Border">
              <Setter Property="Background" Value="{ThemeResource CardBackgroundFillColorDefaultBrush}"/>
              <Setter Property="BorderBrush" Value="{ThemeResource CardStrokeColorDefaultBrush}"/>
              <Setter Property="BorderThickness" Value="1"/>
              <Setter Property="CornerRadius" Value="4"/>
              <Setter Property="Padding" Value="16"/>
            </Style>)").as<x::Style>();
    auto root = x::Markup::XamlReader::Load(
        LR"(<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                   xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
                   Background="{ThemeResource ApplicationPageBackgroundThemeBrush}">
              <ScrollViewer x:Name="NoticeScroll" VerticalScrollBarVisibility="Auto" HorizontalScrollBarVisibility="Disabled">
                <StackPanel x:Name="NoticeList" Spacing="28" Margin="24,24,24,48" HorizontalAlignment="Center"/>
              </ScrollViewer>
            </Grid>)").as<c::Grid>();
    root.RequestedTheme(dark ? x::ElementTheme::Dark : x::ElementTheme::Light);
    const auto scroll = root.FindName(L"NoticeScroll").as<c::ScrollViewer>();
    const auto list = root.FindName(L"NoticeList").as<c::StackPanel>();

    const auto catalog = BuildNoticeCatalog();
    if (catalog.empty()) throw std::runtime_error("notice catalog is empty");
    std::vector<c::StackPanel> wrappers;
    wrappers.reserve(catalog.size());
    for (const auto& spec : catalog)
    {
        c::StackPanel wrapper;
        wrapper.Spacing(6.0);
        wrapper.Width(spec.width);
        wrapper.HorizontalAlignment(x::HorizontalAlignment::Center);
        c::TextBlock caption;
        caption.FontSize(12.0);
        caption.Opacity(0.7);
        caption.TextWrapping(x::TextWrapping::Wrap);
        std::wstring captionText = spec.page;
        captionText += L" · ";
        captionText += spec.name;
        if (spec.inlineOpacity <= 0.0)
        {
            captionText += L"  [";
            captionText += NoticeSeverityLabel(spec.severity);
            captionText += L"]";
        }
        else
        {
            captionText += L"  [inline hint]";
        }
        caption.Text(captionText);
        c::Border card;
        card.Style(cardStyle);
        if (spec.inlineOpacity > 0.0)
        {
            c::TextBlock hint;
            hint.Text(spec.message);
            hint.TextWrapping(x::TextWrapping::Wrap);
            hint.Opacity(spec.inlineOpacity);
            if (spec.inlineFontSize > 0.0) hint.FontSize(spec.inlineFontSize);
            card.Child(hint);
        }
        else
        {
            c::InfoBar bar;
            bar.Severity(spec.severity);
            bar.IsClosable(spec.closable);
            bar.IsOpen(true);
            if (!spec.title.empty()) bar.Title(spec.title);
            bar.Message(spec.message);
            if (!spec.action.empty())
            {
                c::Button action;
                action.Content(winrt::box_value(spec.action));
                bar.ActionButton(action);
            }
            card.Child(bar);
        }
        wrapper.Children().Append(caption);
        wrapper.Children().Append(card);
        list.Children().Append(wrapper);
        wrappers.push_back(std::move(wrapper));
    }
    if (!host.runtime.Attach(host.window, root)) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
    PumpUntil([&] { root.UpdateLayout(); return root.ActualWidth() > 0; });
    Settle(root);
    std::ofstream manifest(output / L"notice-manifest.tsv");
    manifest << "index\tslug\tpage\tname\tsurface\tseverity\tclosable\taction\twidthDip\theightDip\twidthPx\theightPx\n";
    for (std::size_t index = 0; index < wrappers.size(); ++index)
    {
        const auto& spec = catalog[index];
        const auto& wrapper = wrappers[index];
        const double offset = wrapper.TransformToVisual(list).TransformPoint({0, 0}).Y;
        (void)scroll.ChangeView(nullptr,
            winrt::box_value(std::max(0.0, offset - 24.0)).as<winrt::Windows::Foundation::IReference<double>>(),
            nullptr, true);
        Settle(root);
        x::Media::Imaging::RenderTargetBitmap bitmap;
        Await(bitmap.RenderAsync(wrapper));
        const auto pixels = Await(bitmap.GetPixelsAsync());
        BYTE* bytes = nullptr;
        winrt::check_hresult(pixels.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
        std::wstring fileStamp = index + 1 < 10 ? L"0" : L"";
        fileStamp += std::to_wstring(index + 1);
        std::wstring fileName = fileStamp + L"-" + spec.slug + L".png";
        std::string error;
        if (!preview_png::Save(output / fileName, bitmap.PixelWidth(), bitmap.PixelHeight(),
            {reinterpret_cast<const std::uint32_t*>(bytes), pixels.Length() / 4}, error))
            throw std::runtime_error(error);
        std::wstring message = spec.message;
        std::replace(message.begin(), message.end(), L'\t', L' ');
        std::replace(message.begin(), message.end(), L'\n', L' ');
        manifest << index + 1 << '\t' << winrt::to_string(winrt::hstring{spec.slug}) << '\t'
                 << winrt::to_string(winrt::hstring{spec.page}) << '\t'
                 << winrt::to_string(winrt::hstring{spec.name}) << '\t'
                 << (spec.inlineOpacity > 0.0 ? "inline" : "infobar") << '\t'
                 << winrt::to_string(winrt::hstring{NoticeSeverityLabel(spec.severity)}) << '\t'
                 << (spec.closable ? 1 : 0) << '\t'
                 << winrt::to_string(winrt::hstring{spec.action}) << '\t'
                 << spec.width << '\t' << wrapper.ActualHeight() << '\t'
                 << bitmap.PixelWidth() << '\t' << bitmap.PixelHeight() << '\t'
                 << winrt::to_string(winrt::hstring{message}) << '\n';
    }
    manifest.flush();
    host.runtime.Detach();
}
}

std::optional<int> TryRunSettingsVisualPreview()
{
    int count = 0;
    const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return std::nullopt;
    struct FreeArgs { LPWSTR* value; ~FreeArgs() { LocalFree(value); } } freeArgs{args};
    if (count < 2 || std::wstring_view(args[1]) != L"--settings-visual-preview") return std::nullopt;
    // Internal diagnostic only: output, page key/all, light/dark, locale,
    // supported window width/height in DIP, disclosure state, optional focus ID.
    if (count != 9 && count != 10) return ERROR_BAD_ARGUMENTS;
    std::filesystem::path output(args[2]);
    try
    {
        if (!output.is_absolute()) throw std::runtime_error("absolute output directory required");
        std::filesystem::create_directories(output);
        const int width = std::stoi(args[6]), height = std::stoi(args[7]);
        if (width < 840 || width > 2400 || height < 520 || height > 1800) throw std::runtime_error("unsupported preview size");
        const std::wstring_view theme(args[4]), state(args[8]);
        if (theme != L"light" && theme != L"dark") throw std::runtime_error("preview supports light or dark XAML surfaces");
        const bool calendarState = state == L"calendar-dates" || state == L"calendar-weekly" ||
            state == L"calendar-monthly" || state == L"calendar-occurrence";
        const bool notices = state == L"notices";
        const bool generalNotices = state == L"general-notices";
        if (!notices && !generalNotices && state != L"expanded" && state != L"collapsed" && state != L"custom" && state != L"disabled" && state != L"scenarios" && state != L"searching" && state != L"restore" && state != L"secret-input" && !calendarState) throw std::runtime_error("invalid disclosure state");
        if (notices)
            ExportNoticeCatalog(output, theme == L"dark", winrt::to_string(args[5]), width, height);
        else
            Export(output, winrt::to_string(args[3]), theme == L"dark", winrt::to_string(args[5]), width, height,
                state == L"expanded" || state == L"custom" || state == L"scenarios",
                state == L"custom" || state == L"scenarios", state == L"disabled", state == L"scenarios",
                count == 10 ? winrt::to_string(args[9]) : std::string{}, calendarState ? winrt::to_string(state) : std::string{},
                state == L"searching", state == L"restore", state == L"secret-input", generalNotices);
        std::ofstream(output / L"result.txt")
            << "passed\n"
            << (notices
                    ? "Isolated production XAML notice catalog; representative localized strings, no backend state trace.\n"
                    : "Isolated production XAML, example data; no desktop material or interaction acceptance.\n");
        return 0;
    }
    catch (const winrt::hresult_error& error)
    {
        std::ofstream(output / L"result.txt") << "failed\n" << winrt::to_string(error.message()) << '\n';
    }
    catch (const std::exception& error)
    {
        std::ofstream(output / L"result.txt") << "failed\n" << error.what() << '\n';
    }
    return ERROR_INVALID_DATA;
}
}
