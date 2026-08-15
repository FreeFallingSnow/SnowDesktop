#include "widget_view_accessibility.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace
{
using namespace snowdesktop::widget_runtime;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

ViewNode Node(ViewNodeType type, const char* key,
    float x, float y, float width, float height)
{
    ViewNode node;
    node.type = type;
    node.key = key;
    node.frame = { x, y, width, height };
    return node;
}

void TestSemanticHierarchyAndState()
{
    ViewNode root = Node(ViewNodeType::Column,
        "root", 0, 0, 240, 180);
    ViewNode title = Node(ViewNodeType::Text,
        "", 8, 8, 180, 24);
    title.text = "System monitor";
    ViewNode button = Node(ViewNodeType::Button,
        "refresh", 8, 40, 100, 32);
    button.text = "Refresh";
    ViewNode slider = Node(ViewNodeType::Slider,
        "volume", 8, 84, 180, 24);
    slider.accessibilityLabel = "Volume";
    slider.value = 0.5f;
    slider.minimum = 0.0f;
    slider.maximum = 1.0f;
    slider.step = 0.1f;
    root.children = { title, button, slider };

    std::vector<ViewAccessibilityNode> nodes;
    std::string error;
    Check(CollectViewAccessibilityNodes(
            root, "volume", nodes, error),
        "a valid laid-out tree must produce a semantic snapshot");
    Check(nodes.size() == 4 &&
            nodes[0].children.size() == 3 &&
            nodes[1].parentIndex == 0 &&
            nodes[2].parentIndex == 0 &&
            nodes[3].parentIndex == 0,
        "semantic hierarchy must preserve view parentage");
    Check(nodes[0].semanticId == "key:root" &&
            nodes[1].semanticId == "path:0/0" &&
            nodes[2].semanticId == "key:refresh",
        "semantic ids must prefer stable keys and fall back to structure paths");
    Check(nodes[1].controlType == "Text" &&
            nodes[1].name == "System monitor" &&
            !nodes[1].focusable,
        "text nodes must expose bounded read-only semantics");
    Check(nodes[2].controlType == "Button" &&
            HasViewAccessibilityPattern(nodes[2].patterns,
                ViewAccessibilityPattern::Invoke) &&
            nodes[2].focusable,
        "buttons must expose Invoke and keyboard focus semantics");
    Check(nodes[3].controlType == "Slider" &&
            HasViewAccessibilityPattern(nodes[3].patterns,
                ViewAccessibilityPattern::RangeValue) &&
            nodes[3].focused && nodes[3].value == 0.5f &&
            nodes[3].minimum == 0.0f && nodes[3].maximum == 1.0f &&
            nodes[3].step == 0.1f && !nodes[3].rangeValueReadOnly,
        "sliders must expose range state and current host focus");
}

void TestClipAndControlledState()
{
    ViewNode scroll = Node(ViewNodeType::Scroll,
        "scroll", 0, 0, 160, 80);
    scroll.orientation = ViewOrientation::Vertical;
    scroll.clipFrame = ViewRect{ 0, 0, 160, 80 };
    scroll.scrollOffset = 40.0f;
    scroll.scrollViewportExtent = 80.0f;
    scroll.scrollContentExtent = 200.0f;
    ViewNode visible = Node(ViewNodeType::Checkbox,
        "visible", 8, 8, 120, 28);
    visible.text = "Visible";
    visible.checked = true;
    ViewNode offscreen = Node(ViewNodeType::TextInput,
        "offscreen", 8, 120, 120, 28);
    offscreen.accessibilityLabel = "Query";
    offscreen.inputValue = "snow";
    offscreen.readOnly = true;
    offscreen.validationMessage = "Query is unavailable";
    scroll.children = { visible, offscreen };

    std::vector<ViewAccessibilityNode> nodes;
    std::string error;
    Check(CollectViewAccessibilityNodes(scroll, {}, nodes, error) &&
            nodes.size() == 3,
        "materialized clipped children must remain in the semantic snapshot");
    Check(nodes[1].checked == true && !nodes[1].offscreen &&
            nodes[2].offscreen && nodes[2].valueText == "snow" &&
            nodes[2].valueReadOnly &&
            nodes[2].helpText == "Query is unavailable",
        "semantic state must retain values, validation help, read-only state, and offscreen status");
    Check(HasViewAccessibilityPattern(nodes[0].patterns,
                ViewAccessibilityPattern::Scroll) &&
            !nodes[0].scrollHorizontal &&
            nodes[0].scrollOffset == 40.0f &&
            nodes[0].scrollViewportExtent == 80.0f &&
            nodes[0].scrollContentExtent == 200.0f,
        "scroll containers must expose the applied host scroll state");
}

void TestImmediateRegionSemantics()
{
    InteractionRegion button;
    button.key = "open";
    button.shape = { InteractionShapeType::RoundedRect,
        8, 8, 80, 32, 6 };
    button.accessibilityRole = "button";
    button.accessibilityLabel = "Open";
    button.events.emplace("click", InteractionAction{ "open", {} });
    InteractionRegion slider;
    slider.key = "level";
    slider.shape = { InteractionShapeType::Rect,
        8, 52, 120, 24, 0 };
    slider.accessibilityRole = "slider";
    slider.accessibilityLabel = "Level";
    slider.controlKind = InteractionControlKind::Slider;
    slider.controlValue = 4.0f;
    slider.minimum = 0.0f;
    slider.maximum = 10.0f;
    slider.step = 0.5f;
    slider.clip = InteractionClipRect{ 140, 0, 20, 20 };
    slider.events.emplace("change", InteractionAction{ "level", {} });

    std::vector<ViewAccessibilityNode> nodes;
    std::string error;
    Check(CollectInteractionAccessibilityNodes({ button, slider },
            160, 100, "open", nodes, error) && nodes.size() == 2,
        "immediate regions must share the declarative semantic model");
    Check(nodes[0].controlType == "Button" && nodes[0].focused &&
            nodes[0].semanticId == "key:open" &&
            HasViewAccessibilityPattern(nodes[0].patterns,
                ViewAccessibilityPattern::Invoke),
        "immediate buttons must expose Invoke and host focus");
    Check(nodes[1].controlType == "Slider" &&
            nodes[1].value == 4.0f && nodes[1].minimum == 0.0f &&
            nodes[1].maximum == 10.0f && nodes[1].step == 0.5f &&
            !nodes[1].rangeValueReadOnly && nodes[1].offscreen,
        "immediate sliders must expose bounded range state and effective clipping");
}

void TestVirtualControlChildren()
{
    ViewNode radio = Node(ViewNodeType::RadioGroup,
        "theme", 0, 0, 200, 40);
    radio.accessibilityLabel = "Theme";
    radio.selectedValue = "dark";
    radio.options = {
        { "light", "light", "Light", true },
        { "dark", "dark", "Dark", true },
    };
    std::vector<ViewAccessibilityNode> nodes;
    std::string error;
    Check(CollectViewAccessibilityNodes(
            radio, "theme/dark", nodes, error) &&
            nodes.size() == 3 && nodes[0].children.size() == 2 &&
            nodes[1].semanticId == "key:theme/light" &&
            nodes[1].controlType == "RadioButton" &&
            nodes[1].checked == false &&
            nodes[2].semanticId == "key:theme/dark" &&
            nodes[2].checked == true && nodes[2].focused &&
            HasViewAccessibilityPattern(nodes[2].patterns,
                ViewAccessibilityPattern::SelectionItem),
        "radio groups must expose stable selectable option children");

    ViewNode select = Node(ViewNodeType::Select,
        "choice", 0, 0, 180, 32);
    select.accessibilityLabel = "Choice";
    select.selectedValue = "b";
    select.options = {
        { "a", "a", "Option A", true },
        { "b", "b", "Option B", true },
    };
    Check(CollectViewAccessibilityNodes(
            select, {}, nodes, error) && nodes.size() == 1,
        "collapsed selects must not expose popup options");
    select.expanded = true;
    Check(CollectViewAccessibilityNodes(
            select, "choice/b", nodes, error) && nodes.size() == 3 &&
            nodes[1].controlType == "ListItem" &&
            nodes[2].key == "choice/b" && nodes[2].checked == true,
        "expanded selects must expose their selectable popup options");

    ViewNode calendar = Node(ViewNodeType::MonthCalendar,
        "calendar", 0, 0, 280, 240);
    calendar.accessibilityLabel = "August 2026";
    calendar.calendarYear = 2026;
    calendar.calendarMonth = 8;
    calendar.calendarSelectedDate = "2026-08-15";
    calendar.calendarTodayDate = "2026-08-15";
    calendar.firstDayOfWeek = 1;
    Check(CollectViewAccessibilityNodes(calendar,
            "calendar/2026-08-15", nodes, error) &&
            nodes.size() == 43 && nodes[0].children.size() == 42,
        "month calendars must expose all materialized date cells");
    const auto selected = std::find_if(nodes.begin(), nodes.end(),
        [](const auto& node) {
            return node.key == "calendar/2026-08-15";
        });
    Check(selected != nodes.end() && selected->checked == true &&
            selected->focused && selected->controlType == "DataItem" &&
            HasViewAccessibilityPattern(selected->patterns,
                ViewAccessibilityPattern::GridItem) &&
            selected->gridRow && selected->gridColumn &&
            nodes[0].gridRowCount == 6 &&
            nodes[0].gridColumnCount == 7,
        "month calendar date cells must expose selection and focus state");

    ViewNode grid = Node(ViewNodeType::GridList,
        "tiles", 0, 0, 200, 100);
    grid.accessibilityLabel = "Tiles";
    grid.columns = 2;
    for (int index = 0; index < 3; ++index)
    {
        ViewNode item = Node(ViewNodeType::ListItem,
            index == 0 ? "tile-a" : index == 1 ? "tile-b" : "tile-c",
            static_cast<float>((index % 2) * 100),
            static_cast<float>((index / 2) * 50), 100, 50);
        item.accessibilityLabel = index == 0
            ? "Tile A" : index == 1 ? "Tile B" : "Tile C";
        grid.children.push_back(std::move(item));
    }
    Check(CollectViewAccessibilityNodes(grid, {}, nodes, error) &&
            nodes.size() == 4 && nodes[0].gridRowCount == 2 &&
            nodes[0].gridColumnCount == 2 &&
            nodes[3].gridRow == 1 && nodes[3].gridColumn == 0 &&
            HasViewAccessibilityPattern(nodes[3].patterns,
                ViewAccessibilityPattern::GridItem),
        "grid collections must expose zero-based row-major item metadata");
}
}

int main()
{
    TestSemanticHierarchyAndState();
    TestClipAndControlledState();
    TestImmediateRegionSemantics();
    TestVirtualControlChildren();
    std::cout << "widget view accessibility tests passed\n";
    return 0;
}
