#include "widget_view_accessibility.h"

#include <cstdlib>
#include <iostream>

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
        "title", 8, 8, 180, 24);
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
            nodes[3].minimum == 0.0f && nodes[3].maximum == 1.0f,
        "sliders must expose range state and current host focus");
}

void TestClipAndControlledState()
{
    ViewNode scroll = Node(ViewNodeType::Scroll,
        "scroll", 0, 0, 160, 80);
    scroll.clipFrame = ViewRect{ 0, 0, 160, 80 };
    ViewNode visible = Node(ViewNodeType::Checkbox,
        "visible", 8, 8, 120, 28);
    visible.text = "Visible";
    visible.checked = true;
    ViewNode offscreen = Node(ViewNodeType::TextInput,
        "offscreen", 8, 120, 120, 28);
    offscreen.accessibilityLabel = "Query";
    offscreen.inputValue = "snow";
    scroll.children = { visible, offscreen };

    std::vector<ViewAccessibilityNode> nodes;
    std::string error;
    Check(CollectViewAccessibilityNodes(scroll, {}, nodes, error) &&
            nodes.size() == 3,
        "materialized clipped children must remain in the semantic snapshot");
    Check(nodes[1].checked == true && !nodes[1].offscreen &&
            nodes[2].offscreen && nodes[2].valueText == "snow",
        "semantic state must retain controlled values and offscreen status");
}
}

int main()
{
    TestSemanticHierarchyAndState();
    TestClipAndControlledState();
    std::cout << "widget view accessibility tests passed\n";
    return 0;
}
