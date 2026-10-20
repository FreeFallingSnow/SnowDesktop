#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_host_control_projection.h"
#include "widget/view/widget_view_lua.h"
#include "widget/view/widget_view_accessibility.h"

extern "C" {
#include <lauxlib.h>
}
#include <algorithm>
#include <stdexcept>

namespace {
using namespace snowdesktop::widget_runtime;
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
ViewNode Parse(const char* source)
{
    auto* state = luaL_newstate();
    Require(state != nullptr, "projection Lua state");
    struct Close { lua_State* state; ~Close() { lua_close(state); } } close{state};
    lua_newtable(state);
    lua_pushcfunction(state, LuaViewColumn); lua_setfield(state, -2, "column");
    lua_pushcfunction(state, LuaViewTextInput); lua_setfield(state, -2, "textInput");
    lua_setglobal(state, "view");
    Require(luaL_dostring(state, source) == LUA_OK, "projection Lua evaluation");
    ViewNode root;
    std::string error;
    if(!ParseLuaViewTree(state,-1,root,error))
        throw std::runtime_error("projection actual view parse: "+error);
    return root;
}
void Collect(const ViewNode& root, std::string surface,
    std::vector<LuaWidget::HostControl>& controls)
{
    std::vector<ViewInputControl> inputs;
    std::string error;
    Require(CollectViewInputControls(root, inputs, error), "actual input collection");
    for (const auto& input : inputs)
    {
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Input;
        control.id = input.key;
        control.surface = surface;
        control.controlled = true;
        control.controlledText = input.value;
        control.readOnly = input.readOnly;
        control.rect = {static_cast<LONG>(input.frame.x),
            static_cast<LONG>(input.frame.y),
            static_cast<LONG>(input.frame.x + input.frame.width),
            static_cast<LONG>(input.frame.y + input.frame.height)};
        controls.push_back(std::move(control));
    }
}
struct Focus {
    bool active = true;
    std::wstring widgetId = L"widget";
    std::string id = "query", surface = "panel:settings";
    std::wstring text = L"private panel editing";
    std::size_t cursor = 7, selectionAnchor = 2;
};
std::string EncodeText(const std::wstring& value)
{
    if(value.empty())return {};
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,
        value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    Require(bytes>0,"projection UTF-8 size");
    std::string result(static_cast<std::size_t>(bytes),'\0');
    Require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),result.data(),bytes,nullptr,nullptr)==bytes,
        "projection UTF-8 conversion");
    return result;
}
}

void TestHostControlProjection()
{
    using namespace snowdesktop::widget_runtime;
    auto accepted = Parse(R"lua(return view.column({key="accepted",children={
        view.textInput({key="query",value="accepted desktop",readOnly=true,
            accessibility={label="Desktop query"}})}}))lua");
    auto panel = Parse(R"lua(return view.textInput({key="query",value="panel",readOnly=false,action={id="panel.change"},
        accessibility={label="Panel query"}}))lua");
    std::string error;
    if(!ValidateAndLayoutViewTree(accepted,240,100,error) ||
        !ValidateAndLayoutViewTree(panel,240,100,error))
        throw std::runtime_error("actual accepted layout: "+error);
    std::vector<LuaWidget::HostControl> controls;
    Collect(accepted,"desktop",controls);
    Collect(panel,"panel:settings",controls);
    auto lookup = [&]() { return &controls; };
    // Simulate the production render callback registering an immediate control
    // before returning an invalid retained scene. The complete frame rolls back.
    for(int failure=0;failure<3;++failure)
    {
        bool rejected=false;
        {
            HostControlFrame<LuaWidget::HostControl> frame(lookup,"desktop");
            controls.push_back({LuaWidget::HostControl::Type::Input,"immediate"});
            if(failure<2)
            {
                try { (void)Parse(failure==0?"return missing_callback()":"return 7"); }
                catch(const std::runtime_error&){rejected=true;}
            }
            else
            {
                auto invalid = Parse(R"lua(return view.column({key="rejected",children={
                    view.textInput({key="duplicate",value="bad",action={id="one.change"},accessibility={label="One"}}),
                    view.textInput({key="duplicate",value="also bad",action={id="two.change"},accessibility={label="Two"}})}}))lua");
                rejected=!ValidateAndLayoutViewTree(invalid,240,100,error);
            }
        }
        Require(rejected && controls.size()==2 && controls[0].id=="query" &&
            controls[0].controlledText=="accepted desktop" &&
            controls[1].surface=="panel:settings", "Lua error, parse error and invalid layout each retain accepted input frame");
    }
    Require(controls.size()==2 && controls[0].id=="query" &&
        controls[0].controlledText=="accepted desktop" &&
        controls[1].surface=="panel:settings", "rejected frame retains all accepted input controls");
    std::vector<ViewAccessibilityNode> nodes;
    Require(CollectViewAccessibilityNodes(accepted,"",nodes,error),
        "actual retained scene accessibility collection");
    auto node=std::find_if(nodes.begin(),nodes.end(),[](const auto& n){return n.key=="query";});
    Require(node!=nodes.end(),"accepted input has accessibility node");
    Focus focus;
    auto project=[&]() {
        return ProjectHostInputAccessibility(controls,focus,L"widget","query","desktop",
            [](const auto&){return std::string("stored");},
            EncodeText);
    };
    auto input=project();
    Require(input && !input->focused && input->valueText=="accepted desktop" &&
        input->control->readOnly && input->cursor==0 && input->anchor==0,
        "desktop snapshot does not expose same-key panel value, selection or writable state");
    node->valueText=input->valueText; node->focused=input->focused;
    node->valueReadOnly=input->control->readOnly;
    Require(node->valueText=="accepted desktop" && node->valueReadOnly && !node->focused,
        "accepted accessibility snapshot agrees with accepted controls");
    focus.surface="desktop"; focus.text=L"desktop editing";
    input=project();
    Require(input && input->focused && input->valueText=="desktop editing" &&
        input->cursor==7 && input->anchor==2,"matching desktop focus exposes its own edit and selection");
    {
        HostControlFrame<LuaWidget::HostControl> frame(lookup,"desktop");
        auto next=Parse(R"lua(return view.textInput({key="next",value="next frame",action={id="next.change"},
            accessibility={label="Next query"}}))lua");
        Require(ValidateAndLayoutViewTree(next,240,100,error),"next layout accepts");
        Collect(next,"desktop",controls); frame.Commit();
    }
    Require(controls.size()==2 && controls[0].surface=="panel:settings" &&
        controls[1].id=="next", "accepted frame replaces only its own surface");
    struct Owner { std::vector<LuaWidget::HostControl> controls; };
    std::vector<Owner> owners;
    owners.push_back({controls});
    const auto originalOwnerAddress=reinterpret_cast<std::uintptr_t>(&owners.front());
    int lookups=0;
    const auto currentOwner=[&]() -> std::vector<LuaWidget::HostControl>* {
        ++lookups;
        return owners.empty()?nullptr:&owners.front().controls;
    };
    {
        HostControlFrame<LuaWidget::HostControl> frame(currentOwner,"desktop");
        owners.reserve(owners.capacity()+1);
        Require(reinterpret_cast<std::uintptr_t>(&owners.front())!=originalOwnerAddress,
            "fixture actually relocates the owner object and its vector member");
        owners.front().controls.push_back({LuaWidget::HostControl::Type::Input,"rejected"});
    }
    Require(lookups==2 && owners.front().controls.size()==2 &&
        owners.front().controls[0].surface=="panel:settings" &&
        owners.front().controls[1].id=="next", "rollback re-resolves relocated owner storage");
    {
        HostControlFrame<LuaWidget::HostControl> frame(currentOwner,"desktop");
        owners.clear();
    }
    Require(lookups==4 && owners.empty(),"rollback tolerates destruction of the frame owner");
}
