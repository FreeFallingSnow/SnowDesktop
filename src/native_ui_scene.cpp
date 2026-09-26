#include "native_ui_scene.h"
#include <d2d1_1helper.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
#include "native_control_geometry.h"
#include "status_bar_battery.h"

namespace snowdesktop::native_ui
{
using Microsoft::WRL::ComPtr;
namespace
{
bool Contains(D2D1_RECT_F r, D2D1_POINT_2F p)
{ return p.x >= r.left && p.y >= r.top && p.x < r.right && p.y < r.bottom; }
bool HasClip(D2D1_RECT_F r) { return r.right > r.left && r.bottom > r.top; }
}
bool Node::Interactive() const
{ return enabled && !id.empty() && (role == Role::Button || role == Role::Toggle || role == Role::Slider || role == Role::ListItem || role == Role::Icon); }
const Node* Scene::Find(std::string_view id) const
{
    const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const auto& n) { return n.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}
const Node* Scene::Hit(D2D1_POINT_2F p, bool interactiveOnly) const
{
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        if ((!interactiveOnly || it->Interactive()) && Contains(it->bounds, p) && (!HasClip(it->clip) || Contains(it->clip, p))) return &*it;
    return nullptr;
}
namespace
{
std::string Utf8(std::wstring_view text)
{
    const int count=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count,nullptr,nullptr);
    return result;
}
std::string RegionKey(std::string_view id)
{
    // OS device identifiers may exceed the component key length. Keep that
    // identity in the adapter; the common input/UIA layer gets a bounded key.
    std::uint64_t value=14695981039346656037ull;
    for(unsigned char ch:id){value^=ch;value*=1099511628211ull;}
    return "native."+std::to_string(value);
}
bool SameRect(D2D1_RECT_F a,D2D1_RECT_F b)
{return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;}
}
bool Scene::SameContent(const Scene& other)const
{
    if(width!=other.width||height!=other.height||cards.size()!=other.cards.size()||nodes.size()!=other.nodes.size())return false;
    for(std::size_t i=0;i<cards.size();++i)if(!SameRect(cards[i],other.cards[i]))return false;
    for(std::size_t i=0;i<nodes.size();++i)
    {
        const auto& a=nodes[i];const auto& b=other.nodes[i];
        if(a.id!=b.id||a.role!=b.role||!SameRect(a.bounds,b.bounds)||!SameRect(a.clip,b.clip)||
            a.text!=b.text||a.detail!=b.detail||a.glyph!=b.glyph||a.tooltip!=b.tooltip||
            a.fontSize!=b.fontSize||a.value!=b.value||a.enabled!=b.enabled||a.selected!=b.selected||
            a.accent!=b.accent||a.centered!=b.centered||a.bold!=b.bold||a.outlined!=b.outlined||
            a.secondary!=b.secondary||a.charging!=b.charging||bool(a.image)!=bool(b.image)||a.paths.size()!=b.paths.size())return false;
        if(a.image&&a.image!=b.image&&(a.image->width!=b.image->width||a.image->height!=b.image->height||
            a.image->stride!=b.image->stride||a.image->pixels!=b.image->pixels))return false;
        for(std::size_t p=0;p<a.paths.size();++p)
        {
            if(a.paths[p].size()!=b.paths[p].size())return false;
            for(std::size_t q=0;q<a.paths[p].size();++q)
                if(a.paths[p][q].x!=b.paths[p][q].x||a.paths[p][q].y!=b.paths[p][q].y)return false;
        }
    }
    return true;
}
void Input::Sync(const Scene& scene)
{
    regions_.BeginFrame();std::map<std::string,std::string> identities;std::string error;
    for(const auto& n:scene.nodes)
    {
        if(n.id.empty()||n.role==Role::Separator||n.role==Role::Chart||n.id=="scrollbar")continue;
        if(HasClip(n.clip)&&(n.bounds.bottom<=n.clip.top||n.bounds.top>=n.clip.bottom))continue;
        widget_runtime::InteractionRegion r;r.key=RegionKey(n.id);
        if(!identities.emplace(r.key,n.id).second){regions_.AbortFrame();throw std::runtime_error("native scene identity collision");}
        r.shape={widget_runtime::InteractionShapeType::Rect,n.bounds.left,n.bounds.top,n.bounds.right-n.bounds.left,n.bounds.bottom-n.bounds.top,0};
        if(HasClip(n.clip))r.clip=widget_runtime::InteractionClipRect{n.clip.left,n.clip.top,n.clip.right-n.clip.left,n.clip.bottom-n.clip.top};
        r.enabled=n.enabled;r.focusable=n.Interactive();r.tooltip=Utf8(n.tooltip);
        r.accessibilityLabel=Utf8(n.text.empty()?n.tooltip:n.text);
        r.accessibilityValue=Utf8(n.detail);r.accessibilityRole=n.Interactive()?"button":"text";
        if(n.role==Role::Slider)
        {
            r.controlKind=widget_runtime::InteractionControlKind::Slider;r.accessibilityRole="slider";
            r.controlValue=std::clamp(n.value,0.f,1.f);r.step=.02f;
            r.controlStart=n.bounds.left+10;r.controlLength=(std::max)(1.f,n.bounds.right-n.bounds.left-20);
            r.capturePointer=true;r.events["change"].id=r.key;
        }
        else if(n.role==Role::Toggle)
        {
            r.controlKind=widget_runtime::InteractionControlKind::Toggle;r.accessibilityRole="switch";
            r.checked=n.selected;r.events["change"].id=r.key;
        }
        else if(n.Interactive()){r.events["click"].id=r.key;r.events["contextMenu"].id=r.key;r.capturePointer=n.id.starts_with("tray:");}
        if(!regions_.Submit(std::move(r),error)){regions_.AbortFrame();throw std::runtime_error(error);}
    }
    regions_.CommitFrame();identities_=std::move(identities);
    if(!focused_.empty()&&!regions_.IsKeyboardFocusable(RegionKey(focused_)))focused_.clear();
}
std::string Input::Pressed()const
{const auto it=identities_.find(regions_.PressedKey());return it==identities_.end()?std::string{}:it->second;}
std::string Input::Identity(std::string_view key)const
{const auto it=identities_.find(std::string(key));return it==identities_.end()?std::string{}:it->second;}
bool Input::Focus(std::string_view id)
{if(!regions_.IsKeyboardFocusable(RegionKey(id)))return false;focused_=id;return true;}
std::vector<widget_runtime::InteractionRegion> Input::AccessibilityRegions()const{return regions_.AccessibilityRegions();}
InputResult Input::Resolve(const std::optional<widget_runtime::InteractionResolvedAction>& action)const
{
    if(!action)return {};
    if(action->controlValue)return {InputResult::Kind::Value,Identity(action->action.id),*action->controlValue};
    return {InputResult::Kind::Invoke,Identity(action->action.id)};
}
bool Input::Press(const Scene& scene,D2D1_POINT_2F p,bool right)
{
    Cancel();Sync(scene);const auto pressed=regions_.PointerDown(p.x,p.y,right?2:1);
    const auto it=identities_.find(pressed.targetKey);if(it==identities_.end())return false;
    focused_=it->second;origin_=p;right_=right;return true;
}
InputResult Input::Move(const Scene& scene,D2D1_POINT_2F p)
{
    Sync(scene);const auto id=Pressed();const auto* n=scene.Find(id);
    if(!n||!n->Interactive()){Cancel();return {};}
    if(!right_&&n->role==Role::Slider)return Resolve(regions_.ResolveAction(RegionKey(id),"change",p.x,p.y,0));
    if(!right_&&id.starts_with("tray:")&&(std::abs(p.x-origin_.x)>=8||std::abs(p.y-origin_.y)>=8))dragging_=true;
    return {};
}
InputResult Input::Release(const Scene& scene,D2D1_POINT_2F p,bool right)
{
    Sync(scene);const auto id=Pressed();const auto* n=scene.Find(id);const bool dragged=dragging_,matching=right==right_;
    const bool valid=regions_.HasPointerCapture();
    const auto result=regions_.PointerUp(p.x,p.y,right?2:1);dragging_=right_=false;
    if(!n||!n->Interactive()||!matching)return {};
    if(dragged)return valid?InputResult{InputResult::Kind::Drag,id}:InputResult{};
    if(!right&&n->role==Role::Slider)return Resolve(regions_.ResolveAction(RegionKey(id),"change",p.x,p.y,0));
    if(result.clickTargetKey!=RegionKey(id))return {};
    return {right?InputResult::Kind::Context:InputResult::Kind::Invoke,id};
}
InputResult Input::Key(const Scene& scene,unsigned key,bool shift)
{
    Sync(scene);
    if(key==VK_TAB)
    {
        const auto ids=regions_.KeyboardFocusableKeys();if(ids.empty()){focused_.clear();return {};}
        const auto it=std::find(ids.begin(),ids.end(),RegionKey(focused_));
        const auto index=it==ids.end()?(shift?ids.size()-1:0):(static_cast<std::size_t>(it-ids.begin())+(shift?ids.size()-1:1))%ids.size();
        focused_=identities_.at(ids[index]);return {};
    }
    if(!regions_.IsKeyboardFocusable(RegionKey(focused_)))return {};
    if(key==VK_LEFT||key==VK_RIGHT)return Resolve(regions_.ResolveKeyboardStep(RegionKey(focused_),key==VK_LEFT?-1:1));
    if(key==VK_HOME||key==VK_END)return Resolve(regions_.ResolveRangeValue(RegionKey(focused_),key==VK_HOME?0.f:1.f));
    if(key==VK_RETURN||key==VK_SPACE)return {InputResult::Kind::Invoke,focused_};
    if(key==VK_APPS||(key==VK_F10&&shift))return {InputResult::Kind::Context,focused_};
    return {};
}
void Input::Cancel(){regions_.CancelPointerPress();dragging_=right_=false;}

HRESULT Draw(ID2D1DeviceContext* dc, IDWriteFactory* factory, const Scene& scene, const Palette& p,
    std::string_view hovered, std::string_view focused, std::string_view pressed)
{
    if (!dc || !factory) return E_INVALIDARG;
    ComPtr<ID2D1SolidColorBrush> brush; HRESULT hr = dc->CreateSolidColorBrush(p.text, &brush); if (FAILED(hr)) return hr;
    std::map<std::tuple<float,bool,bool,bool>,ComPtr<IDWriteTextFormat>> formats;
    auto color = [&](D2D1_COLOR_F value, bool enabled = true) { if (!enabled) value.a *= .4f; brush->SetColor(value); };
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, float size, D2D1_COLOR_F ink, bool bold, bool center, bool glyph) {
        if (value.empty() || r.right <= r.left || r.bottom <= r.top) return;
        auto& format=formats[{size,bold,center,glyph}];
        if(!format)
        {
        if (FAILED(factory->CreateTextFormat(glyph ? L"Segoe MDL2 Assets" : L"Segoe UI", nullptr,
            bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", &format))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        ComPtr<IDWriteInlineObject> ellipsis; factory->CreateEllipsisTrimmingSign(format.Get(), &ellipsis);
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0}; format->SetTrimming(&trimming, ellipsis.Get());
        }
        color(ink); dc->DrawTextW(value.data(), static_cast<UINT32>(value.size()), format.Get(), r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    for (const auto& n : scene.nodes)
    {
        if (HasClip(n.clip)) dc->PushAxisAlignedClip(n.clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const auto r = n.bounds;
        const bool hot = n.id == hovered && n.Interactive(), down = n.id == pressed && n.Interactive();
        const bool fill = n.role == Role::Button || n.role == Role::Toggle || n.role == Role::Card || n.selected || hot || down;
        if (fill)
        {
            color(n.accent || (n.role == Role::Toggle && n.selected) ? p.accent : hot || down ? p.hover : p.control, n.enabled);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, n.role == Role::Card ? 10.f : 7.f, n.role == Role::Card ? 10.f : 7.f), brush.Get());
        }
        if(n.outlined){color(p.accent);auto outline=r;outline.left+=1;outline.top+=1;outline.right-=1;outline.bottom-=1;dc->DrawRoundedRectangle(D2D1::RoundedRect(outline,7,7),brush.Get(),2);}
        auto ink = (n.accent || (n.role == Role::Toggle && n.selected)) ? p.accentText : n.secondary?p.secondary:p.text;
        if (!n.enabled) ink.a *= .4f;
        if (n.role == Role::Separator) { color(p.stroke); dc->DrawLine({r.left, r.top}, {r.right, r.top}, brush.Get(), 1); }
        else if (n.role == Role::Slider)
        {
            const auto geometry=native_controls::Slider(r,10,4,n.value,false);
            color(p.stroke, n.enabled); dc->FillRoundedRectangle(D2D1::RoundedRect(geometry.track,2,2),brush.Get());
            color(p.accent,n.enabled); dc->FillRoundedRectangle(D2D1::RoundedRect(geometry.fill,2,2),brush.Get());
            color(p.control); dc->FillEllipse(D2D1::Ellipse(geometry.thumb,9,9),brush.Get());
            color(p.accent,n.enabled); dc->FillEllipse(D2D1::Ellipse(geometry.thumb,6,6),brush.Get());
        }
        else if (n.role == Role::Chart)
        {
            color(p.stroke);
            for (int i=0;i<=6;++i) { const float at=r.left+(r.right-r.left)*i/6; dc->DrawLine({at,r.top},{at,r.bottom},brush.Get(),.5f); }
            for (int i=0;i<=4;++i) { const float at=r.top+(r.bottom-r.top)*i/4; dc->DrawLine({r.left,at},{r.right,at},brush.Get(),.5f); }
            color(p.accent);
            for (const auto& path : n.paths) for (std::size_t i=1;i<path.size();++i) dc->DrawLine(path[i-1],path[i],brush.Get(),2);
        }
        else
        {
            D2D1_RECT_F label = r;
            const bool iconOnly = n.text.empty();
            if(n.charging)
            {
                ComPtr<ID2D1Factory> geometryFactory;dc->GetFactory(&geometryFactory);
                if(auto geometry=CreateChargingBatteryGeometry(geometryFactory.Get()))
                {
                    D2D1_MATRIX_3X2_F previous;dc->GetTransform(&previous);
                    dc->SetTransform(D2D1::Matrix3x2F::Translation(r.left+12,(r.top+r.bottom-20)/2)*previous);
                    color(D2D1::ColorF(0x34c759));dc->FillGeometry(geometry.Get(),brush.Get());dc->SetTransform(previous);
                }
                label.left+=44;
            }
            else if (n.image && n.image->width && n.image->height && n.image->stride >= n.image->width * 4 &&
                n.image->pixels.size() * 4 >= static_cast<std::size_t>(n.image->stride) * n.image->height)
            {
                ComPtr<ID2D1Bitmap> bitmap;
                const auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(n.image->width,n.image->height),n.image->pixels.data(),n.image->stride,props,&bitmap)))
                {
                    const float side = n.role == Role::Image ? (std::min)(r.right-r.left,r.bottom-r.top) : 20;
                    const float left = (r.left+r.right-side)/2, top=(r.top+r.bottom-side)/2;
                    dc->DrawBitmap(bitmap.Get(),{left,top,left+side,top+side},n.enabled?1.f:.4f);
                }
            }
            else if (!n.glyph.empty())
            {
                const auto glyphRect = iconOnly ? r : D2D1::RectF(r.left+10,r.top,r.left+38,r.bottom);
                text(n.glyph,glyphRect,18,ink,false,true,true); if (!iconOnly) label.left += 44;
            }
            else if (n.role != Role::Text) label.left += 12;
            if (!iconOnly)
            {
                label.right -= n.role == Role::Text ? 0 : 12;
                if (!n.detail.empty())
                {
                    const float mid=(r.top+r.bottom)/2;
                    text(n.text,{label.left,r.top+4,label.right,mid+3},n.fontSize,ink,n.bold,n.centered,false);
                    auto secondary=p.secondary; if(!n.enabled)secondary.a*=.4f;
                    text(n.detail,{label.left,mid+1,label.right,r.bottom-4},12,secondary,false,n.centered,false);
                }
                else text(n.text,label,n.fontSize,ink,n.bold,n.centered,false);
            }
        }
        if (!n.id.empty() && n.id == focused && n.Interactive())
        { color(p.accent); auto focus=r; focus.left+=2;focus.top+=2;focus.right-=2;focus.bottom-=2; dc->DrawRoundedRectangle(D2D1::RoundedRect(focus,5,5),brush.Get(),2); }
        if (HasClip(n.clip)) dc->PopAxisAlignedClip();
    }
    return S_OK;
}
}
