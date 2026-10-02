#include "text_input_window.h"
#include "text_input_state.h"
#include "app_font.h"
#include <dwrite.h>
#include <imm.h>
#include <commctrl.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <atomic>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>
#include <vector>

namespace snowdesktop::text_input
{
using Microsoft::WRL::ComPtr;
namespace
{
constexpr UINT_PTR kCaretTimer = 0x54455854;
constexpr wchar_t kClass[] = L"SnowDesktop.DrawnTextInput";
constexpr UINT kAccessQuery = WM_APP + 0x5e1;
MenuHandler menuHandler;
struct State;
void RaiseEvent(State&, EVENTID);
struct AccessQuery
{
    enum class Kind { Document, Select, Rectangles, Hit, Reveal, SetValue, LineBoundaries, VisibleRanges } kind = Kind::Document;
    const void* identity = nullptr;
    AccessibleDocument document;
    std::size_t start=0,end=0;
    Point point{};
    std::vector<Rectangle> rectangles;
    std::vector<std::size_t> boundaries;
    std::vector<std::pair<std::size_t,std::size_t>> visible;
    bool result=false;
};
struct SecureBuffer
{
    std::vector<wchar_t> storage;
    std::size_t length = 0;
    ~SecureBuffer() { Clear(); }
    void Clear() noexcept
    {
        if (!storage.empty()) SecureZeroMemory(storage.data(), storage.size() * sizeof(wchar_t));
        length = 0;
    }
    std::wstring_view View() const noexcept { return {storage.data(), length}; }
    void Assign(std::wstring_view value)
    {
        // Erase the complete allocation before replacing or releasing it.
        Clear(); storage.resize((std::max)(storage.size(), value.size() + 1));
        if (!value.empty()) std::memcpy(storage.data(), value.data(), value.size() * sizeof(wchar_t));
        length = value.size();
    }
    void Replace(std::size_t start, std::size_t end, std::wstring_view value)
    {
        SecureBuffer next;
        next.storage.resize(start + value.size() + length - end + 1);
        if (start) std::memcpy(next.storage.data(), storage.data(), start * sizeof(wchar_t));
        if (!value.empty()) std::memcpy(next.storage.data() + start, value.data(), value.size() * sizeof(wchar_t));
        if (length > end) std::memcpy(next.storage.data() + start + value.size(), storage.data() + end, (length - end) * sizeof(wchar_t));
        next.length = next.storage.size() - 1;
        storage.swap(next.storage); std::swap(length, next.length);
    }
};
struct State
{
    HWND window = nullptr;
    HFONT font = nullptr;
    LOGFONTW fontInfo{};
    Colors colors;
    bool explicitColors = false;
    float radius = 6.f, leftMargin = 8.f, rightMargin = 8.f, verticalMargin = 4.f;
    int width = 1, height = 1;
    float scrollX = 0, scrollY = 0;
    float singleLineOffsetY = 0, caretHeight = 0;
    RECT frame{}, clip{};
    bool embedded = false, shown = true, interactive = true;
    bool password = false, multiline = false, singleLine = true, readOnly = false;
    bool composing = false, dragging = false, caret = true, deferred = false, forwardedPointer = false, editingMenu = false;
    std::size_t cursor = 0, anchor = 0, compositionCursor = 0;
    std::size_t limit = 0;
    wchar_t highSurrogate = 0;
    std::wstring text, composition, pending, cue, duplicateResult;
    std::wstring name;
    std::shared_ptr<TextAccess> accessibility;
    SecureBuffer secret;
    History history;
    std::optional<std::pair<std::size_t, std::size_t>> deferredSelection;
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteTextLayout> layout;
    std::wstring display;
    std::wstring_view Value() const noexcept { return password ? secret.View() : std::wstring_view(text); }
    void Dirty()
    {
        layout.Reset(); caret = true;
        if (!window) return;
        InvalidateRect(embedded ? GetParent(window) : window, nullptr, FALSE);
        RaiseEvent(*this, UIA_Text_TextSelectionChangedEventId);
    }
    void Notify(UINT code)
    {
        if (code == EN_CHANGE && !password) RaiseEvent(*this, UIA_Text_TextChangedEventId);
        if (window) SendMessageW(GetParent(window), WM_COMMAND,
            MAKEWPARAM(GetDlgCtrlID(window), code), reinterpret_cast<LPARAM>(window));
    }
    bool Mutable() const noexcept { return window && interactive && !readOnly && IsWindowEnabled(window); }
    bool Replace(std::wstring_view replacement, const Snapshot* original = nullptr)
    {
        if (!Mutable()) return false;
        const auto value = Value();
        const auto start = SnapBoundary(value, (std::min)(cursor, anchor));
        const auto end = (std::min)((std::max)(cursor, anchor), value.size());
        const auto count = value.size() - (end - start) + replacement.size();
        if (limit && count > limit) return false;
        if (value.substr(start, end - start) == replacement) return false;
        if (password) secret.Replace(start, end, replacement);
        else
        {
            Snapshot before = original ? *original : Snapshot{text, cursor, anchor};
            text.replace(start, end - start, replacement);
            cursor = anchor = start + replacement.size();
            history.Record(std::move(before), {text, cursor, anchor});
        }
        cursor = anchor = start + replacement.size();
        composition.clear(); compositionCursor = 0;
        Dirty(); Notify(EN_UPDATE); Notify(EN_CHANGE); return true;
    }
    void SetText(std::wstring_view value, bool notify = true)
    {
        if (composing && !password) { deferred = Value() != value; pending = deferred ? std::wstring(value) : std::wstring{}; return; }
        if (Value() == value) return;
        if (password) secret.Assign(value); else text = value;
        cursor = SnapBoundary(Value(), cursor); anchor = SnapBoundary(Value(), anchor);
        history.Clear(); Dirty();
        if(notify){Notify(EN_UPDATE);Notify(EN_CHANGE);}else if(!password)RaiseEvent(*this,UIA_Text_TextChangedEventId);
    }
    float FontSize() const { return static_cast<float>(fontInfo.lfHeight ? std::abs(fontInfo.lfHeight) : 12L); }
    float TextY() const { return verticalMargin + singleLineOffsetY - scrollY; }
    void UpdateWindowClip()
    {
        if (!window || embedded) return;
        const int diameter = static_cast<int>(std::lround((std::max)(0.f, radius) * 2.f));
        const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, diameter, diameter);
        if (region && !SetWindowRgn(window, region, TRUE)) DeleteObject(region);
    }
    void Layout()
    {
        if (layout) return;
        if (!factory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(factory.GetAddressOf())))) return;
        display = password ? std::wstring(secret.length, L'\x2022') : text;
        if (!composition.empty()) display.replace((std::min)(cursor, anchor),
            (std::max)(cursor, anchor) - (std::min)(cursor, anchor), composition);
        const auto* family = fontInfo.lfFaceName[0] ? fontInfo.lfFaceName : L"Segoe UI";
        ComPtr<IDWriteTextFormat> format;
        const auto selectedFont = app_fonts::current.load();
        const bool usesAppFont = selectedFont && _wcsicmp(family, selectedFont->family.c_str()) == 0;
        const auto weight = fontInfo.lfWeight >= FW_SEMIBOLD ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
        const auto style = fontInfo.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
        const HRESULT formatResult = usesAppFont
            ? app_fonts::CreateTextFormat(factory, family, weight, style,
                DWRITE_FONT_STRETCH_NORMAL, FontSize(), L"", &format)
            : factory->CreateTextFormat(family, nullptr,
                fontInfo.lfWeight >= FW_SEMIBOLD ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                fontInfo.lfItalic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, FontSize(), L"", &format);
        if (FAILED(formatResult)) return;
        format->SetWordWrapping(multiline ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if ((GetWindowLongPtrW(window, GWL_STYLE) & ES_CENTER) != 0)
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        singleLineOffsetY = 0;
        if (!multiline)
        {
            // Keep line metrics consistent across fallback fonts. Visible ink
            // is centered separately after shaping the actual value or cue.
            ComPtr<IDWriteTextLayout> reference;
            if (SUCCEEDED(factory->CreateTextLayout(L"国H", 2, format.Get(), 1000.f, 1000.f, &reference))) // l10n-allow: fixed font measurement glyphs, never displayed
            {
                DWRITE_LINE_METRICS line{}; UINT32 count = 0;
                if (SUCCEEDED(reference->GetLineMetrics(&line, 1, &count)) && count == 1)
                {
                    format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line.height, line.baseline);
                }
            }
            format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        factory->CreateTextLayout(display.data(), static_cast<UINT32>(display.size()), format.Get(),
            (std::max)(1.f, static_cast<float>(width) - leftMargin - rightMargin),
            multiline ? 100000.f : (std::max)(1.f, static_cast<float>(height) - verticalMargin*2.f), &layout);
        if (!layout) return;
        if (!multiline)
        {
            DWRITE_TEXT_METRICS metrics{}; layout->GetMetrics(&metrics);
            // Center short titles, but keep the entire beginning of an
            // overflowing name reachable by horizontal scrolling.
            if (metrics.widthIncludingTrailingWhitespace > layout->GetMaxWidth())
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            ComPtr<IDWriteTextLayout> cueLayout;
            auto* visibleLayout = layout.Get();
            const auto& visibleText = display.empty() ? cue : display;
            if (display.empty() && !cue.empty() && SUCCEEDED(factory->CreateTextLayout(
                    cue.data(), static_cast<UINT32>(cue.size()), layout.Get(),
                    layout->GetMaxWidth(), layout->GetMaxHeight(), &cueLayout)))
                visibleLayout = cueLayout.Get();
            DWRITE_OVERHANG_METRICS ink{};
            if (visibleText.find_first_not_of(L" \t\r\n") != std::wstring::npos &&
                SUCCEEDED(visibleLayout->GetOverhangMetrics(&ink)))
                singleLineOffsetY = (ink.top - ink.bottom) / 2.f;
            scrollY = 0;
        }
        float x = 0, y = 0; DWRITE_HIT_TEST_METRICS hit{};
        const auto position = composition.empty() ? cursor : (std::min)(cursor, anchor) + compositionCursor;
        layout->HitTestTextPosition(static_cast<UINT32>((std::min)(position, display.size())), FALSE, &x, &y, &hit);
        const float available = (std::max)(1.f, static_cast<float>(width) - leftMargin - rightMargin);
        if (!multiline) scrollX = (std::max)(0.f, x > available - 2.f ? x - available + 2.f : (std::min)(scrollX, x));
        else
        {
            const float viewport = (std::max)(1.f, static_cast<float>(height) - verticalMargin*2.f);
            if (y < scrollY) scrollY = y;
            if (y + hit.height > scrollY + viewport) scrollY = y + hit.height - viewport;
            DWRITE_TEXT_METRICS metrics{}; layout->GetMetrics(&metrics);
            scrollY = std::clamp(scrollY, 0.f, (std::max)(0.f, metrics.height - viewport));
        }
    }
    std::size_t Hit(float x, float y)
    {
        Layout(); if (!layout) return 0;
        BOOL trailing = FALSE, inside = FALSE; DWRITE_HIT_TEST_METRICS metrics{};
        layout->HitTestPoint(x - leftMargin + scrollX, y - TextY(), &trailing, &inside, &metrics);
        return SnapBoundary(Value(), (std::min)(static_cast<std::size_t>(metrics.textPosition + (trailing ? metrics.length : 0)), Value().size()));
    }
    void ImePosition()
    {
        if (!window || password) return;
        Layout(); if (!layout) return;
        float x = 0, y = 0; DWRITE_HIT_TEST_METRICS hit{};
        const auto position = composition.empty() ? cursor : (std::min)(cursor, anchor) + compositionCursor;
        layout->HitTestTextPosition(static_cast<UINT32>((std::min)(position, display.size())), FALSE, &x, &y, &hit);
        POINT point{static_cast<LONG>(std::lround(x + leftMargin - scrollX)), static_cast<LONG>(std::lround(y + TextY()))};
        if (embedded)
        { point.x += frame.left; point.y += frame.top; MapWindowPoints(GetParent(window), window, &point, 1); }
        if (const auto context = ImmGetContext(window))
        {
            COMPOSITIONFORM form{}; form.dwStyle = CFS_POINT; form.ptCurrentPos = point;
            ImmSetCompositionWindow(context, &form);
            CANDIDATEFORM candidate{}; candidate.dwStyle = CFS_EXCLUDE;
            candidate.ptCurrentPos = {point.x, point.y + static_cast<LONG>(std::ceil(hit.height))};
            candidate.rcArea = {point.x, point.y, point.x + 1, candidate.ptCurrentPos.y};
            ImmSetCandidateWindow(context, &candidate); ImmReleaseContext(window, context);
        }
    }
    POINT ScreenOrigin() const
    {
        POINT origin{};
        if(embedded){origin={frame.left,frame.top};ClientToScreen(GetParent(window),&origin);}
        else ClientToScreen(window,&origin);
        return origin;
    }
    void Query(AccessQuery& query)
    {
        if(query.identity!=this||!window)return;
        if(query.kind==AccessQuery::Kind::Document)
        {
            auto& document=query.document;
            document.password=password;document.readOnly=readOnly;document.enabled=interactive&&IsWindowEnabled(window);
            document.focused=GetFocus()==window;document.offscreen=!shown||!(GetWindowLongPtrW(window,GWL_STYLE)&WS_VISIBLE)||!IsWindowVisible(GetParent(window));document.name=name.empty()?cue:name;
            const auto origin=ScreenOrigin();document.bounds={origin.x,origin.y,origin.x+width,origin.y+height};
            if(!password){document.text=text;document.cursor=cursor;document.anchor=anchor;}query.result=true;
        }
        else if(query.kind==AccessQuery::Kind::Select)
        {
            if(password||!interactive)return;
            if(composing){deferredSelection=std::make_pair(query.start,query.end);query.result=true;return;}
            anchor=SnapBoundary(Value(),query.start);cursor=SnapBoundary(Value(),query.end);Dirty();ImePosition();query.result=true;
        }
        else if(query.kind==AccessQuery::Kind::SetValue)
        {if(!password&&Mutable()&&(!limit||query.document.text.size()<=limit)){SetText(query.document.text);query.result=true;}}
        else
        {
            if(password)return;Layout();if(!layout)return;
            ComPtr<IDWriteTextLayout> committedLayout;
            IDWriteTextLayout* documentLayout = layout.Get();
            if (!composition.empty() && SUCCEEDED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                layout.Get(), layout->GetMaxWidth(), layout->GetMaxHeight(), &committedLayout)))
                documentLayout = committedLayout.Get();
            const auto origin=ScreenOrigin();
            if(query.kind==AccessQuery::Kind::LineBoundaries||query.kind==AccessQuery::Kind::VisibleRanges)
            {
                UINT32 count=0;documentLayout->GetLineMetrics(nullptr,0,&count);
                std::vector<DWRITE_LINE_METRICS> lines(count);
                if(count&&SUCCEEDED(documentLayout->GetLineMetrics(lines.data(),count,&count)))
                {
                    DWRITE_TEXT_METRICS metrics{}; documentLayout->GetMetrics(&metrics);
                    std::size_t at=0;float top=TextY()+metrics.top;query.boundaries.push_back(0);
                    for(const auto& line:lines)
                    {
                        const auto end=(std::min)(text.size(),at+line.length);
                        const float visibleTop=embedded?static_cast<float>((std::max)(0L,clip.top-frame.top)):0.f;
                        const float visibleBottom=embedded?static_cast<float>((std::min)(static_cast<LONG>(height),clip.bottom-frame.top)):static_cast<float>(height);
                        if(top+line.height>visibleTop&&top<visibleBottom)
                        {
                            BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS hit{};
                            const float left=embedded?static_cast<float>((std::max)(0L,clip.left-frame.left)):0.f;
                            const float right=embedded?static_cast<float>((std::min)(static_cast<LONG>(width),clip.right-frame.left)):static_cast<float>(width);
                            documentLayout->HitTestPoint(left-leftMargin+scrollX,top-TextY()+line.height/2,&trailing,&inside,&hit);
                            const auto first=SnapBoundary(text,hit.textPosition);
                            documentLayout->HitTestPoint(right-leftMargin+scrollX,top-TextY()+line.height/2,&trailing,&inside,&hit);
                            const auto last=SnapBoundary(text,hit.textPosition+(trailing?hit.length:0));
                            query.visible.emplace_back((std::min)(first,last),(std::max)(first,last));
                        }
                        at=end;top+=line.height;query.boundaries.push_back(end);
                    }
                }
                query.result=true;
            }
            else if(query.kind==AccessQuery::Kind::Hit)
            {BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS hit{};
                documentLayout->HitTestPoint(static_cast<float>(query.point.x-origin.x)-leftMargin+scrollX,
                    static_cast<float>(query.point.y-origin.y)-TextY(),&trailing,&inside,&hit);
                query.start=SnapBoundary(text,hit.textPosition+(trailing?hit.length:0));query.result=true;}
            else if(query.kind==AccessQuery::Kind::Reveal)
            {
                float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};
                documentLayout->HitTestTextPosition(static_cast<UINT32>((std::min)(query.start,text.size())),FALSE,&x,&y,&hit);
                scrollX=multiline?0:(std::max)(0.f,x-static_cast<float>(width)+leftMargin+rightMargin+2.f);
                scrollY=multiline?(std::max)(0.f,y):0;InvalidateRect(embedded?GetParent(window):window,nullptr,FALSE);query.result=true;
            }
            else if(query.kind==AccessQuery::Kind::Rectangles)
            {
                const auto start=(std::min)(query.start,text.size()),end=std::clamp(query.end,start,text.size());
                UINT32 count=0;documentLayout->HitTestTextRange(static_cast<UINT32>(start),static_cast<UINT32>(end-start),
                    static_cast<float>(origin.x)+leftMargin-scrollX,static_cast<float>(origin.y)+TextY(),nullptr,0,&count);
                std::vector<DWRITE_HIT_TEST_METRICS> boxes(count);
                if(count&&SUCCEEDED(documentLayout->HitTestTextRange(static_cast<UINT32>(start),static_cast<UINT32>(end-start),
                    static_cast<float>(origin.x)+leftMargin-scrollX,static_cast<float>(origin.y)+TextY(),boxes.data(),count,&count)))
                    for(const auto& box:boxes)
                    {
                        POINT clipping{clip.left,clip.top};if(embedded)ClientToScreen(GetParent(window),&clipping);
                        const float left=(std::max)(box.left,(std::max)(static_cast<float>(origin.x)+leftMargin,
                            static_cast<float>(embedded?clipping.x:origin.x)));
                        const float top=(std::max)(box.top,static_cast<float>(embedded?(std::max)(origin.y,clipping.y):origin.y));
                        const float right=(std::min)(box.left+box.width,(std::min)(static_cast<float>(origin.x+width)-rightMargin,
                            static_cast<float>(embedded?clipping.x+clip.right-clip.left:origin.x+width)));
                        const float bottom=(std::min)(box.top+box.height,static_cast<float>(embedded?(std::min)(origin.y+height,clipping.y+clip.bottom-clip.top):origin.y+height));
                        if(right>=left&&bottom>top)query.rectangles.push_back({left,top,right-left,bottom-top});
                    }
                query.result=true;
            }
        }
    }
};
bool DispatchAccess(HWND window,const std::weak_ptr<State>& weak,AccessQuery& query)
{
    const auto state=weak.lock();if(!state)return false;query.identity=state.get();
    SendMessageW(window,kAccessQuery,0,reinterpret_cast<LPARAM>(&query));return query.result;
}
std::shared_ptr<TextAccess> MakeAccess(HWND window,const std::shared_ptr<State>& state)
{
    const std::weak_ptr<State> weak=state;auto access=std::make_shared<TextAccess>();
    access->lifetimeIdentity=state.get();
    access->document=[window,weak]()->std::optional<AccessibleDocument>{AccessQuery query;if(!DispatchAccess(window,weak,query))return {};return std::move(query.document);};
    access->select=[window,weak](std::size_t start,std::size_t end){AccessQuery query;query.kind=AccessQuery::Kind::Select;query.start=start;query.end=end;return DispatchAccess(window,weak,query);};
    access->rectangles=[window,weak](std::size_t start,std::size_t end){AccessQuery query;query.kind=AccessQuery::Kind::Rectangles;query.start=start;query.end=end;DispatchAccess(window,weak,query);return query.rectangles;};
    access->hit=[window,weak](Point point)->std::optional<std::size_t>{AccessQuery query;query.kind=AccessQuery::Kind::Hit;query.point=point;if(!DispatchAccess(window,weak,query))return {};return query.start;};
    access->reveal=[window,weak](std::size_t at,bool){AccessQuery query;query.kind=AccessQuery::Kind::Reveal;query.start=at;return DispatchAccess(window,weak,query);};
    access->unitBoundaries=[window,weak](Unit unit){AccessQuery query;if(unit==Unit::Line){query.kind=AccessQuery::Kind::LineBoundaries;DispatchAccess(window,weak,query);}return query.boundaries;};
    access->visibleRanges=[window,weak]{AccessQuery query;query.kind=AccessQuery::Kind::VisibleRanges;DispatchAccess(window,weak,query);return query.visible;};
    return access;
}
class SimpleProvider final:public IRawElementProviderSimple,public IValueProvider
{
public:
    SimpleProvider(HWND window,const std::shared_ptr<State>& state):window_(window),state_(state),access_(state->accessibility){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {if(!out)return E_POINTER;*out=nullptr;if(iid==IID_IUnknown||iid==IID_IRawElementProviderSimple)*out=static_cast<IRawElementProviderSimple*>(this);else if(iid==IID_IValueProvider)*out=static_cast<IValueProvider*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override{if(!out)return E_POINTER;*out=ProviderOptions_ServerSideProvider;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id,IUnknown** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;const auto doc=access_->document();if(!doc)return UIA_E_ELEMENTNOTAVAILABLE;
        if(id==UIA_ValuePatternId)return QueryInterface(IID_IValueProvider,reinterpret_cast<void**>(out));
        if(id==UIA_TextPatternId&&!doc->password){ComPtr<ITextProvider> provider;const auto hr=CreateTextProvider(access_,this,&provider);if(FAILED(hr))return hr;return provider.CopyTo(out);}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id,VARIANT* out) override
    {
        if(!out)return E_POINTER;VariantInit(out);const auto doc=access_->document();if(!doc)return UIA_E_ELEMENTNOTAVAILABLE;
        if(id==UIA_ControlTypePropertyId){out->vt=VT_I4;out->lVal=UIA_EditControlTypeId;}
        else if(id==UIA_NamePropertyId){out->vt=VT_BSTR;out->bstrVal=SysAllocStringLen(doc->name.data(),static_cast<UINT>(doc->name.size()));if(!out->bstrVal)return E_OUTOFMEMORY;}
        else if(id==UIA_IsPasswordPropertyId||id==UIA_IsKeyboardFocusablePropertyId||id==UIA_HasKeyboardFocusPropertyId||
            id==UIA_IsEnabledPropertyId||id==UIA_IsOffscreenPropertyId||id==UIA_IsTextPatternAvailablePropertyId||id==UIA_IsValuePatternAvailablePropertyId)
        {
            out->vt=VT_BOOL;const bool value=id==UIA_IsPasswordPropertyId?doc->password:id==UIA_HasKeyboardFocusPropertyId?doc->focused:
                id==UIA_IsEnabledPropertyId?doc->enabled:id==UIA_IsOffscreenPropertyId?doc->offscreen:id==UIA_IsTextPatternAvailablePropertyId?!doc->password:true;
            out->boolVal=value?VARIANT_TRUE:VARIANT_FALSE;
        }
        else if(id==UIA_BoundingRectanglePropertyId)
        {
            out->vt=VT_ARRAY|VT_R8;out->parray=SafeArrayCreateVector(VT_R8,0,4);if(!out->parray)return E_OUTOFMEMORY;
            double values[]{static_cast<double>(doc->bounds.left),static_cast<double>(doc->bounds.top),
                static_cast<double>(doc->bounds.right-doc->bounds.left),static_cast<double>(doc->bounds.bottom-doc->bounds.top)};
            for(LONG i=0;i<4;++i)SafeArrayPutElement(out->parray,&i,&values[i]);
        }
        else if(id==UIA_ValueValuePropertyId&&!doc->password){out->vt=VT_BSTR;out->bstrVal=SysAllocStringLen(doc->text.data(),static_cast<UINT>(doc->text.size()));if(!out->bstrVal)return E_OUTOFMEMORY;}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override
    {if(!out)return E_POINTER;*out=nullptr;if(!access_->document())return UIA_E_ELEMENTNOTAVAILABLE;return UiaHostProviderFromHwnd(window_,out);}
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override
    {if(!value)return E_INVALIDARG;const auto doc=access_->document();if(!doc)return UIA_E_ELEMENTNOTAVAILABLE;if(doc->password)return E_ACCESSDENIED;
        AccessQuery query;query.kind=AccessQuery::Kind::SetValue;query.document.text=value;return DispatchAccess(window_,state_,query)?S_OK:UIA_E_NOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override
    {if(!out)return E_POINTER;*out=nullptr;const auto doc=access_->document();if(!doc)return UIA_E_ELEMENTNOTAVAILABLE;if(doc->password)return E_ACCESSDENIED;*out=SysAllocStringLen(doc->text.data(),static_cast<UINT>(doc->text.size()));return *out?S_OK:E_OUTOFMEMORY;}
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override
    {if(!out)return E_POINTER;const auto doc=access_->document();if(!doc)return UIA_E_ELEMENTNOTAVAILABLE;*out=doc->readOnly||!doc->enabled;return S_OK;}
private:
    std::atomic<ULONG> references_{1};HWND window_;std::weak_ptr<State> state_;std::shared_ptr<TextAccess> access_;
};
void RaiseEvent(State& state,EVENTID id)
{
    if(!state.window||(state.password&&id!=UIA_AutomationFocusChangedEventId)||!UiaClientsAreListening())return;
    const auto* holder=reinterpret_cast<const std::shared_ptr<State>*>(GetWindowLongPtrW(state.window,GWLP_USERDATA));
    if(!holder||holder->get()!=&state)return;
    ComPtr<IRawElementProviderSimple> provider;provider.Attach(new(std::nothrow)SimpleProvider(state.window,*holder));
    if(provider)UiaRaiseAutomationEvent(provider.Get(),id);
}
std::shared_ptr<State> Get(HWND window)
{
    if (!window) return {};
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (lstrcmpW(name, kClass) != 0) return {};
    const auto* holder = reinterpret_cast<std::shared_ptr<State>*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return holder ? *holder : std::shared_ptr<State>{};
}
D2D1_COLOR_F Color(COLORREF value)
{ return D2D1::ColorF(GetRValue(value)/255.f, GetGValue(value)/255.f, GetBValue(value)/255.f); }
void Render(State& state, ID2D1RenderTarget* target, D2D1_RECT_F frame, float scale, bool drawFrame)
{
    if (!target || !state.shown) return;
    scale = (std::max)(0.01f, scale); state.Layout();
    ComPtr<ID2D1SolidColorBrush> brush; target->CreateSolidColorBrush(Color(state.colors.background), &brush);
    if (!brush) return;
    const auto drawBorder = [&]
    {
        if (!drawFrame) return;
        const float stroke = 1.f / scale;
        const float inset = stroke * .5f;
        const auto borderFrame = D2D1::RectF(frame.left + inset, frame.top + inset,
            frame.right - inset, frame.bottom - inset);
        const float radius = (std::max)(0.f, state.radius / scale - inset);
        brush->SetColor(Color(GetFocus() == state.window ? state.colors.accent : state.colors.border));
        // Keep the entire stroke inside the client area and rounded window
        // region. Drawing on the outer edge loses half the stroke to clipping.
        target->DrawRoundedRectangle(D2D1::RoundedRect(borderFrame, radius, radius), brush.Get(), stroke);
    };
    if (drawFrame)
    {
        const auto rounded = D2D1::RoundedRect(frame, state.radius / scale, state.radius / scale);
        target->FillRoundedRectangle(rounded, brush.Get());
    }
    if (!state.layout) { drawBorder(); return; }
    D2D1_MATRIX_3X2_F old{}; target->GetTransform(&old);
    target->PushAxisAlignedClip(frame, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target->SetTransform(D2D1::Matrix3x2F::Scale(1.f/scale,1.f/scale) *
        D2D1::Matrix3x2F::Translation(frame.left, frame.top) * old);
    target->PushAxisAlignedClip(D2D1::RectF(state.leftMargin, 0.f,
        std::max(state.leftMargin + 1.f, static_cast<float>(state.width) - state.rightMargin),
        static_cast<float>(state.height)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float x = state.leftMargin - state.scrollX, y = state.TextY();
    std::vector<DWRITE_HIT_TEST_METRICS> selectionBoxes;
    const auto ranges = [&](std::size_t start, std::size_t count, bool underline)
    {
        UINT32 actual = 0; state.layout->HitTestTextRange(static_cast<UINT32>(start), static_cast<UINT32>(count), x, y, nullptr, 0, &actual);
        std::vector<DWRITE_HIT_TEST_METRICS> boxes(actual);
        if (actual && SUCCEEDED(state.layout->HitTestTextRange(static_cast<UINT32>(start), static_cast<UINT32>(count), x, y, boxes.data(), actual, &actual)))
            for (const auto& box : boxes)
            {
                target->FillRectangle(D2D1::RectF(box.left, underline ? box.top+box.height-1.f : box.top,
                    box.left+box.width, box.top+box.height), brush.Get());
                if(!underline)selectionBoxes.push_back(box);
            }
    };
    const bool selected = state.composition.empty() && state.cursor != state.anchor;
    if (selected)
    { brush->SetColor(Color(state.colors.accent)); ranges((std::min)(state.cursor,state.anchor), (std::max)(state.cursor,state.anchor)-(std::min)(state.cursor,state.anchor), false); }
    brush->SetColor(Color(state.colors.foreground));
    // The viewport clips both glyphs and selection. The layout's own clip
    // would discard overflowing glyphs before horizontal scrolling reveals them.
    target->DrawTextLayout(D2D1::Point2F(x,y), state.layout.Get(), brush.Get());
    if(selected)
    {
        brush->SetColor(Color(state.colors.selectionText));
        for(const auto& box:selectionBoxes)
        {
            target->PushAxisAlignedClip(D2D1::RectF(box.left,box.top,box.left+box.width,box.top+box.height),D2D1_ANTIALIAS_MODE_ALIASED);
            target->DrawTextLayout(D2D1::Point2F(x,y),state.layout.Get(),brush.Get());
            target->PopAxisAlignedClip();
        }
        brush->SetColor(Color(state.colors.foreground));
    }
    if (!state.composition.empty()) ranges((std::min)(state.cursor,state.anchor), state.composition.size(), true);
    if (state.display.empty() && !state.cue.empty())
    {
        brush->SetColor(Color(state.colors.secondary));
        ComPtr<IDWriteTextLayout> hint;
        state.factory->CreateTextLayout(state.cue.data(),static_cast<UINT32>(state.cue.size()),state.layout.Get(),
            state.layout->GetMaxWidth(),state.layout->GetMaxHeight(),&hint);
        if(hint)target->DrawTextLayout(D2D1::Point2F(state.leftMargin,y),hint.Get(),brush.Get());
    }
    if (GetFocus() == state.window && state.caret && state.interactive)
    {
        float cx = 0, cy = 0; DWRITE_HIT_TEST_METRICS hit{};
        const auto position = state.composition.empty() ? state.cursor : (std::min)(state.cursor,state.anchor)+state.compositionCursor;
        state.layout->HitTestTextPosition(static_cast<UINT32>((std::min)(position,state.display.size())),FALSE,&cx,&cy,&hit);
        brush->SetColor(Color(state.colors.foreground));
        const auto caret = state.caretHeight > 0.f && !state.multiline
            ? CenteredCaretRectangle(D2D1::RectF(0.f,0.f,static_cast<float>(state.width),static_cast<float>(state.height)), x + cx, state.caretHeight, 1.5f)
            : D2D1::RectF(x+cx,y+cy,x+cx+1.5f,y+cy+hit.height);
        target->FillRectangle(caret,brush.Get());
    }
    target->PopAxisAlignedClip(); target->SetTransform(old); target->PopAxisAlignedClip();
    // Selection and caret painting must not cover the frame's outline.
    drawBorder();
}
void Paint(State& state, HDC dc)
{
    if (state.embedded || !dc) return;
    if(!state.explicitColors)
    {
        SetTextColor(dc,state.colors.foreground);SetBkColor(dc,state.colors.background);
        if(SendMessageW(GetParent(state.window),WM_CTLCOLOREDIT,reinterpret_cast<WPARAM>(dc),reinterpret_cast<LPARAM>(state.window)))
        {state.colors.foreground=GetTextColor(dc);state.colors.background=GetBkColor(dc);}
        if(!state.window)return;
    }
    ComPtr<ID2D1Factory> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) return;
    ComPtr<ID2D1DCRenderTarget> target;
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96,96);
    if (FAILED(factory->CreateDCRenderTarget(&properties,&target))) return;
    RECT bounds{0,0,state.width,state.height}; if (FAILED(target->BindDC(dc,&bounds))) return;
    target->BeginDraw(); target->Clear(Color(state.colors.background)); Render(state,target.Get(),D2D1::RectF(0,0,static_cast<float>(state.width),static_cast<float>(state.height)),1,true); target->EndDraw();
}
void Copy(State& state)
{
    if (state.password || state.cursor == state.anchor || !OpenClipboard(state.window)) return;
    const auto selection = state.Value().substr((std::min)(state.cursor,state.anchor),
        (std::max)(state.cursor,state.anchor)-(std::min)(state.cursor,state.anchor));
    const auto memory = GlobalAlloc(GMEM_MOVEABLE,(selection.size()+1)*sizeof(wchar_t));
    if (memory)
    {
        if (auto* destination=static_cast<wchar_t*>(GlobalLock(memory)))
        {
            std::memcpy(destination,selection.data(),selection.size()*sizeof(wchar_t)); destination[selection.size()]=0; GlobalUnlock(memory);
            EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT,memory)) GlobalFree(memory);
        }
        else GlobalFree(memory);
    }
    CloseClipboard();
}
void Paste(State& state)
{
    if (!state.Mutable() || !OpenClipboard(state.window)) return;
    struct ClipboardSession { ~ClipboardSession(){CloseClipboard();} } clipboardSession;
    std::wstring value;
    SecureBuffer protectedValue;
    if (const auto data=GetClipboardData(CF_UNICODETEXT))
    {
        const auto bytes=GlobalSize(data);
        if (const auto* source=static_cast<const wchar_t*>(GlobalLock(data)))
        {
            struct Unlock { HANDLE data; ~Unlock(){GlobalUnlock(data);} } unlock{data};
            const auto capacity=(std::min)(bytes/sizeof(wchar_t),static_cast<SIZE_T>(1048576));
            if(state.password)protectedValue.storage.resize(capacity+1);
            for (SIZE_T i=0;i<capacity&&source[i];++i)
            {
                if(source[i]==L'\r')continue;
                const auto ch=state.singleLine&&(source[i]==L'\n'||source[i]==L'\t')?L' ':source[i];
                if(state.password)protectedValue.storage[protectedValue.length++]=ch;else value.push_back(ch);
            }
        }
    }
    state.Replace(state.password?protectedValue.View():std::wstring_view(value));
}
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wp,LPARAM lp)
{
    if (message==WM_NCCREATE)
    {
        const auto* creation=reinterpret_cast<CREATESTRUCTW*>(lp);
        auto state=std::make_shared<State>();state->window=window;
        state->password=(creation->style&ES_PASSWORD)!=0;state->readOnly=(creation->style&ES_READONLY)!=0;
        state->multiline=(creation->style&ES_MULTILINE)!=0;state->singleLine=!state->multiline;
        const std::wstring_view initial=creation->lpszName?creation->lpszName:L"";
        if (state->password) state->secret.Assign(initial); else state->text=initial;
        state->cursor=state->anchor=state->Value().size();
        state->accessibility=MakeAccess(window,state);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(new std::shared_ptr<State>(std::move(state))));
        return TRUE; // Do not cache the creation text in DefWindowProc, especially for passwords.
    }
    const auto state=Get(window);if(!state)return DefWindowProcW(window,message,wp,lp);
    switch(message)
    {
    case kAccessQuery:if(lp)state->Query(*reinterpret_cast<AccessQuery*>(lp));return 0;
    case WM_GETOBJECT:
        if(static_cast<LONG>(lp)==UiaRootObjectId)
        {ComPtr<IRawElementProviderSimple> provider;provider.Attach(new SimpleProvider(window,state));return UiaReturnRawElementProvider(window,wp,lp,provider.Get());}break;
    case WM_NCDESTROY:
        state->window=nullptr;state->secret.Clear();KillTimer(window,kCaretTimer);
        delete reinterpret_cast<std::shared_ptr<State>*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        SetWindowLongPtrW(window,GWLP_USERDATA,0);return DefWindowProcW(window,message,wp,lp);
    case WM_CREATE: if(state->password)ImmAssociateContext(window,nullptr);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT p{};const auto dc=BeginPaint(window,&p);Paint(*state,dc);EndPaint(window,&p);return 0;}
    case WM_PRINT:case WM_PRINTCLIENT:Paint(*state,reinterpret_cast<HDC>(wp));return 0;
    case WM_SETFONT:
        state->font=reinterpret_cast<HFONT>(wp);state->fontInfo={};
        if(state->font)GetObjectW(state->font,sizeof(state->fontInfo),&state->fontInfo);state->Dirty();return 0;
    case WM_GETFONT:return reinterpret_cast<LRESULT>(state->font);
    case WM_SETTEXT:state->SetText(lp?std::wstring_view(reinterpret_cast<const wchar_t*>(lp)):std::wstring_view{});return TRUE;
    case WM_GETTEXTLENGTH:return static_cast<LRESULT>(state->Value().size());
    case WM_GETTEXT:
        if(wp&&lp){auto* out=reinterpret_cast<wchar_t*>(lp);const auto count=state->password?0:(std::min)(state->text.size(),static_cast<std::size_t>(wp-1));
            if(count)std::memcpy(out,state->text.data(),count*sizeof(wchar_t));out[count]=0;return static_cast<LRESULT>(count);}return 0;
    case WM_SIZE:if(!state->embedded){state->width=LOWORD(lp);state->height=HIWORD(lp);state->UpdateWindowClip();state->Dirty();}return 0;
    case EM_SETLIMITTEXT:state->limit=static_cast<std::size_t>(wp);return 0;
    case EM_GETLIMITTEXT:return static_cast<LRESULT>(state->limit);
    case EM_SETREADONLY:state->readOnly=wp!=0;state->Dirty();return TRUE;
    case EM_GETSEL:
        if(wp)*reinterpret_cast<DWORD*>(wp)=static_cast<DWORD>((std::min)(state->cursor,state->anchor));
        if(lp)*reinterpret_cast<DWORD*>(lp)=static_cast<DWORD>((std::max)(state->cursor,state->anchor));
        return MAKELRESULT(static_cast<WORD>((std::min)(state->cursor,state->anchor)),static_cast<WORD>((std::max)(state->cursor,state->anchor)));
    case EM_SETSEL:
        if(state->composing){state->deferredSelection=std::make_pair(static_cast<INT_PTR>(wp)==-1?state->cursor:static_cast<std::size_t>(wp),lp<0?state->Value().size():static_cast<std::size_t>(lp));return 0;}
        if(static_cast<INT_PTR>(wp)==-1)state->anchor=state->cursor;
        else{state->anchor=SnapBoundary(state->Value(),static_cast<std::size_t>(wp));state->cursor=lp<0?state->Value().size():SnapBoundary(state->Value(),static_cast<std::size_t>(lp));}
        state->Dirty();state->ImePosition();return 0;
    case EM_SETMARGINS:
        if(wp&EC_LEFTMARGIN)state->leftMargin=LOWORD(lp);if(wp&EC_RIGHTMARGIN)state->rightMargin=HIWORD(lp);state->Dirty();return 0;
    case EM_GETRECT:if(lp)*reinterpret_cast<RECT*>(lp)={static_cast<LONG>(state->leftMargin),static_cast<LONG>(state->verticalMargin),state->width-static_cast<LONG>(state->rightMargin),state->height-static_cast<LONG>(state->verticalMargin)};return 0;
    case EM_SETCUEBANNER:
    {
        const std::wstring_view cue=lp?reinterpret_cast<const wchar_t*>(lp):L"";
        if(state->cue!=cue){state->cue=cue;state->Dirty();}
        return TRUE;
    }
    case EM_EMPTYUNDOBUFFER:state->history.Clear();return 0;
    case EM_CANUNDO:return !state->password&&state->history.CanUndo();
    case EM_GETPASSWORDCHAR:return state->password?0x2022:0;
    case EM_GETLINECOUNT:{state->Layout();DWRITE_TEXT_METRICS metrics{};if(state->layout)state->layout->GetMetrics(&metrics);return (std::max)(1L,static_cast<LONG>(metrics.lineCount));}
    case EM_GETFIRSTVISIBLELINE:return static_cast<LRESULT>(state->scrollY/state->FontSize());
    case EM_LINESCROLL:state->scrollY=(std::max)(0.f,state->scrollY+static_cast<float>(lp)*state->FontSize());state->Dirty();return TRUE;
    case EM_SCROLLCARET:state->layout.Reset();state->Layout();state->Dirty();return 0;
    case EM_REPLACESEL:if(lp)state->Replace(reinterpret_cast<const wchar_t*>(lp));return 0;
    case WM_COPY:Copy(*state);return 0;
    case WM_CUT:if(!state->password&&state->Mutable()){Copy(*state);state->Replace(L"");}return 0;
    case WM_PASTE:Paste(*state);return 0;
    case WM_CLEAR:state->Replace(L"");return 0;
    case WM_UNDO:case EM_UNDO:
        if(!state->password&&state->Mutable()&&state->history.Undo(state->text,state->cursor,state->anchor))
        {state->Dirty();state->Notify(EN_UPDATE);state->Notify(EN_CHANGE);return TRUE;}return FALSE;
    case WM_GETDLGCODE:return DLGC_WANTCHARS|DLGC_WANTARROWS|(state->multiline?DLGC_WANTALLKEYS:0);
    case WM_SETFOCUS:state->caret=true;SetTimer(window,kCaretTimer,(std::max)(100u,GetCaretBlinkTime()),nullptr);state->Dirty();state->ImePosition();RaiseEvent(*state,UIA_AutomationFocusChangedEventId);state->Notify(EN_SETFOCUS);return 0;
    case WM_KILLFOCUS:CompleteComposition(window);if(state->window&&state->composing)SendMessageW(window,WM_IME_ENDCOMPOSITION,0,0);KillTimer(window,kCaretTimer);state->dragging=false;if(GetCapture()==window)ReleaseCapture();state->composition.clear();state->composing=false;state->Dirty();state->Notify(EN_KILLFOCUS);return 0;
    case WM_TIMER:if(wp==kCaretTimer){state->caret=!state->caret;InvalidateRect(state->embedded?GetParent(window):window,nullptr,FALSE);return 0;}break;
    case WM_ENABLE:state->Dirty();return 0;
    case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:
    {
        SetFocus(window);POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(state->embedded&&!state->forwardedPointer){GetCursorPos(&point);ScreenToClient(GetParent(window),&point);point.x-=state->frame.left;point.y-=state->frame.top;}
        const auto at=state->Hit(static_cast<float>(point.x),static_cast<float>(point.y));
        if(message==WM_LBUTTONDBLCLK){state->anchor=WordBoundary(state->Value(),at,false);state->cursor=WordBoundary(state->Value(),at,true);}
        else{if(!(GetKeyState(VK_SHIFT)&0x8000))state->anchor=at;state->cursor=at;}
        state->dragging=true;SetCapture(window);state->Dirty();state->ImePosition();return 0;
    }
    case WM_MOUSEMOVE:
        if(state->dragging){POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};if(state->embedded&&!state->forwardedPointer){GetCursorPos(&point);ScreenToClient(GetParent(window),&point);point.x-=state->frame.left;point.y-=state->frame.top;}
            state->cursor=state->Hit(static_cast<float>(point.x),static_cast<float>(point.y));state->Dirty();state->ImePosition();}return 0;
    case WM_LBUTTONUP:state->dragging=false;if(GetCapture()==window)ReleaseCapture();return 0;
    case WM_CAPTURECHANGED:case WM_CANCELMODE:state->dragging=false;return 0;
    case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_IBEAM));return TRUE;
    case WM_CONTEXTMENU:
    {
        if(!menuHandler)return 0;
        CompleteComposition(window);if(!state->window)return 0;
        POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(point.x==-1&&point.y==-1){point=state->ScreenOrigin();point.x+=static_cast<LONG>(state->leftMargin);point.y+=state->height;}
        const bool selected=state->cursor!=state->anchor,editable=state->Mutable();
        const MenuState options{editable&&!state->password&&state->history.CanUndo(),editable&&!state->password&&state->history.CanRedo(),
            editable&&!state->password&&selected,!state->password&&selected,editable&&IsClipboardFormatAvailable(CF_UNICODETEXT),!state->Value().empty()};
        state->editingMenu=true;
        const auto handler=menuHandler;const auto command=handler(window,point,options);
        state->editingMenu=false;if(!state->window)return 0;
        if(GetFocus()!=window)
        {
            SendMessageW(window,WM_KILLFOCUS,reinterpret_cast<WPARAM>(GetFocus()),0);
            if(state->window)SendMessageW(window,WM_ACTIVATE,WA_INACTIVE,reinterpret_cast<LPARAM>(GetForegroundWindow()));
            return 0;
        }
        switch(command)
        {
        case MenuCommand::Undo:SendMessageW(window,WM_UNDO,0,0);break;
        case MenuCommand::Redo:if(editable&&!state->password&&state->history.Redo(state->text,state->cursor,state->anchor))
            {state->Dirty();state->Notify(EN_UPDATE);state->Notify(EN_CHANGE);}break;
        case MenuCommand::Cut:SendMessageW(window,WM_CUT,0,0);break;
        case MenuCommand::Copy:Copy(*state);break;
        case MenuCommand::Paste:Paste(*state);break;
        case MenuCommand::SelectAll:SendMessageW(window,EM_SETSEL,0,-1);break;
        case MenuCommand::None:break;
        }return 0;
    }
    case WM_IME_STARTCOMPOSITION:if(!state->Mutable()||state->password)return 0;state->composing=true;state->composition.clear();state->duplicateResult.clear();state->ImePosition();return 0;
    case WM_IME_COMPOSITION:
    {
        if(state->password||!state->Mutable())return 0;
        if(const auto context=ImmGetContext(window))
        {
            const auto read=[&](DWORD flag){const auto bytes=ImmGetCompositionStringW(context,flag,nullptr,0);std::wstring value(bytes>0?static_cast<std::size_t>(bytes)/sizeof(wchar_t):0,L'\0');if(bytes>0)ImmGetCompositionStringW(context,flag,value.data(),static_cast<DWORD>(bytes));return value;};
            if(lp&GCS_RESULTSTR){auto result=read(GCS_RESULTSTR);state->Replace(result);state->duplicateResult=std::move(result);}
            if(state->window&&(lp&(GCS_COMPSTR|GCS_CURSORPOS))){state->composition=read(GCS_COMPSTR);const auto at=ImmGetCompositionStringW(context,GCS_CURSORPOS,nullptr,0);state->compositionCursor=at<0?0:(std::min)(static_cast<std::size_t>(at),state->composition.size());state->Dirty();}
            ImmReleaseContext(window,context);
        }
        state->ImePosition();return 0;
    }
    case WM_IME_ENDCOMPOSITION:
        state->composing=false;state->composition.clear();state->Dirty();
        if(state->deferred){auto value=std::move(state->pending);state->deferred=false;state->SetText(value,false);}
        if(state->deferredSelection){const auto selection=*state->deferredSelection;state->deferredSelection.reset();state->anchor=SnapBoundary(state->Value(),selection.first);state->cursor=SnapBoundary(state->Value(),selection.second);state->Dirty();}return 0;
    case WM_IME_CHAR:return 0; // Result strings are applied only at GCS_RESULTSTR.
    case WM_KEYDOWN:
    {
        state->duplicateResult.clear();state->highSurrogate=0;
        if(state->composing)return DefWindowProcW(window,message,wp,lp);
        const bool ctrl=(GetKeyState(VK_CONTROL)&0x8000)!=0,shift=(GetKeyState(VK_SHIFT)&0x8000)!=0;
        if(ctrl&&wp=='A'){state->anchor=0;state->cursor=state->Value().size();state->Dirty();return 0;}
        if(ctrl&&(wp=='C'||wp=='X')){SendMessageW(window,wp=='C'?WM_COPY:WM_CUT,0,0);return 0;}
        if(ctrl&&wp=='V'){Paste(*state);return 0;}
        if(ctrl&&(wp=='Z'||wp=='Y'))
        {
            if(!state->password&&state->Mutable())
            {
                const bool changed=(wp=='Y'||shift)?state->history.Redo(state->text,state->cursor,state->anchor):state->history.Undo(state->text,state->cursor,state->anchor);
                if(changed){state->Dirty();state->Notify(EN_UPDATE);state->Notify(EN_CHANGE);}
            }return 0;
        }
        const auto value=state->Value();
        if(wp==VK_BACK||wp==VK_DELETE)
        {
            if(!state->Mutable())return 0;
            std::optional<Snapshot> before;
            if(!state->password)before=Snapshot{state->text,state->cursor,state->anchor};
            if(state->cursor==state->anchor)
                state->anchor=wp==VK_BACK?(ctrl?WordBoundary(value,state->cursor,false):PreviousBoundary(value,state->cursor)):
                    (ctrl?WordBoundary(value,state->cursor,true):NextBoundary(value,state->cursor));
            state->Replace(L"",before?&*before:nullptr);return 0;
        }
        auto next=state->cursor;
        if(wp==VK_LEFT)next=!shift&&state->cursor!=state->anchor?(std::min)(state->cursor,state->anchor):(ctrl?WordBoundary(value,next,false):PreviousBoundary(value,next));
        else if(wp==VK_RIGHT)next=!shift&&state->cursor!=state->anchor?(std::max)(state->cursor,state->anchor):(ctrl?WordBoundary(value,next,true):NextBoundary(value,next));
        else if(wp==VK_HOME){const auto line=next?value.rfind(L'\n',next-1):std::wstring_view::npos;next=!ctrl&&state->multiline&&line!=std::wstring_view::npos?line+1:0;}
        else if(wp==VK_END){const auto line=value.find(L'\n',next);next=!ctrl&&state->multiline&&line!=std::wstring_view::npos?line:value.size();}
        else if((wp==VK_UP||wp==VK_DOWN)&&state->multiline)
        {
            state->Layout();if(state->layout){float x=0,y=0;DWRITE_HIT_TEST_METRICS hit{};state->layout->HitTestTextPosition(static_cast<UINT32>(next),FALSE,&x,&y,&hit);
                BOOL trailing=FALSE,inside=FALSE;state->layout->HitTestPoint(x,y+(wp==VK_UP?-0.5f:1.5f)*hit.height,&trailing,&inside,&hit);next=SnapBoundary(value,hit.textPosition+(trailing?hit.length:0));}
        }
        else return DefWindowProcW(window,message,wp,lp);
        state->cursor=next;if(!shift)state->anchor=next;state->Dirty();state->ImePosition();return 0;
    }
    case WM_CHAR:
    {
        const auto ch=static_cast<wchar_t>(wp);
        if(!state->duplicateResult.empty()&&state->duplicateResult.front()==ch){state->duplicateResult.erase(0,1);return 0;}
        state->duplicateResult.clear();
        if(ch==L'\r'){if(!state->singleLine)state->Replace(L"\n");return 0;}
        if(ch<32||ch==127||state->composing)return 0;
        if(ch>=0xd800&&ch<=0xdbff){state->highSurrogate=ch;return 0;}
        wchar_t units[2]{state->highSurrogate,ch};const bool pair=state->highSurrogate&&ch>=0xdc00&&ch<=0xdfff;state->highSurrogate=0;
        if(!pair&&ch>=0xdc00&&ch<=0xdfff)return 0;
        state->Replace(pair?std::wstring_view(units,2):std::wstring_view(&ch,1));if(state->password)SecureZeroMemory(units,sizeof(units));state->ImePosition();return 0;
    }
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
const wchar_t* WindowClass()
{
    static std::once_flag registered;
    std::call_once(registered,[]{WNDCLASSEXW cls{sizeof(cls)};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Procedure;cls.hCursor=LoadCursorW(nullptr,IDC_IBEAM);cls.lpszClassName=kClass;cls.style=CS_DBLCLKS;RegisterClassExW(&cls);});
    return kClass;
}
void SetColors(HWND window,const Colors& colors,float radius)
{
    if(const auto state=Get(window))
    {
        if(state->explicitColors&&state->colors==colors&&state->radius==radius)return;
        const bool clipChanged=state->radius!=radius;
        state->colors=colors;state->explicitColors=true;state->radius=radius;
        if(clipChanged)state->UpdateWindowClip();
        state->Dirty();
    }
}
bool IsComposing(HWND window) {const auto state=Get(window);return state&&state->composing;}
void SetLogicalSingleLine(HWND window,bool value) {if(const auto state=Get(window))state->singleLine=value;}
void SetEmbeddedPose(HWND window,RECT frame,RECT clip,bool shown,bool interactive)
{
    if(const auto state=Get(window))
    {
        const bool place=!state->embedded||state->shown!=shown;
        const bool changed=!EqualRect(&state->frame,&frame)||!EqualRect(&state->clip,&clip)||state->shown!=shown||state->interactive!=interactive;
        if(!state->embedded)SetWindowRgn(window,nullptr,FALSE);
        state->embedded=true;state->frame=frame;state->clip=clip;state->shown=shown;state->interactive=interactive;
        state->width=(std::max)(1L,frame.right-frame.left);state->height=(std::max)(1L,frame.bottom-frame.top);
        if(place)SetWindowPos(window,nullptr,-32000,-32000,1,1,SWP_NOACTIVATE|SWP_NOZORDER|(shown?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
        if((IsWindowEnabled(window)!=FALSE)!=interactive)EnableWindow(window,interactive);
        if(changed)state->Dirty();if(GetFocus()==window)state->ImePosition();
    }
}
void Draw(HWND window,ID2D1RenderTarget* target,D2D1_RECT_F frame,float scale,bool drawFrame)
{if(const auto state=Get(window))Render(*state,target,frame,scale,drawFrame);}
bool RoutePointer(HWND window,UINT message,WPARAM wp,POINT point)
{
    const auto state=Get(window);if(!state||!state->embedded||!state->shown||!state->interactive)return false;
    if(message==WM_CONTEXTMENU)
    {
        if(point.x==-1&&point.y==-1){if(GetFocus()!=window)return false;SendMessageW(window,message,wp,MAKELPARAM(-1,-1));return true;}
        ScreenToClient(GetParent(window),&point);
        if(!PtInRect(&state->frame,point)||!PtInRect(&state->clip,point))return false;
        ClientToScreen(GetParent(window),&point);SendMessageW(window,message,wp,MAKELPARAM(point.x,point.y));return true;
    }
    if(message!=WM_LBUTTONDOWN&&message!=WM_LBUTTONDBLCLK&&message!=WM_LBUTTONUP&&message!=WM_MOUSEMOVE)return false;
    if(!state->dragging&&(!PtInRect(&state->frame,point)||!PtInRect(&state->clip,point)))return false;
    state->forwardedPointer=true;
    SendMessageW(window,message,wp,MAKELPARAM(point.x-state->frame.left,point.y-state->frame.top));
    state->forwardedPointer=false;return true;
}
int DesiredHeight(HWND window)
{
    const auto state=Get(window);if(!state)return 1;state->Layout();DWRITE_TEXT_METRICS metrics{};if(state->layout)state->layout->GetMetrics(&metrics);
    return static_cast<int>(std::ceil(metrics.height+state->verticalMargin*2.f));
}
void CopySecret(HWND window,wchar_t* destination,std::size_t capacity)
{
    if(!destination||!capacity)return;SecureZeroMemory(destination,capacity*sizeof(wchar_t));
    if(const auto state=Get(window);state&&state->password){const auto count=(std::min)(capacity-1,state->secret.length);if(count)std::memcpy(destination,state->secret.storage.data(),count*sizeof(wchar_t));}
}
void SetAccessibleName(HWND window,std::wstring_view name)
{if(const auto state=Get(window))state->name=name;}
std::shared_ptr<TextAccess> Accessibility(HWND window)
{const auto state=Get(window);return state?state->accessibility:std::shared_ptr<TextAccess>{};}
HRESULT CreateWindowProvider(HWND window,IRawElementProviderSimple** out)
{if(!out)return E_POINTER;*out=nullptr;const auto state=Get(window);if(!state)return UIA_E_ELEMENTNOTAVAILABLE;*out=new(std::nothrow)SimpleProvider(window,state);return *out?S_OK:E_OUTOFMEMORY;}
std::wstring DisplayText(HWND window)
{
    const auto state=Get(window);if(!state||state->password)return {};
    auto text=state->text;
    if(!state->composition.empty())text.replace(std::min(state->cursor,state->anchor),
        std::max(state->cursor,state->anchor)-std::min(state->cursor,state->anchor),state->composition);
    return text;
}
void SetMenuHandler(MenuHandler handler){menuHandler=std::move(handler);}
bool HasEditingMenu(HWND window){const auto state=Get(window);return state&&state->editingMenu;}
void CompleteComposition(HWND window,bool cancel)
{
    const auto state=Get(window);if(!state||!state->composing)return;
    if(const auto context=ImmGetContext(window)){ImmNotifyIME(context,NI_COMPOSITIONSTR,cancel?CPS_CANCEL:CPS_COMPLETE,0);ImmReleaseContext(window,context);}
}
void SetCaretHeight(HWND window,float height)
{if(const auto state=Get(window)){height=std::max(0.f,height);if(state->caretHeight!=height){state->caretHeight=height;state->Dirty();}}}
void SetPadding(HWND window,float horizontal,float vertical)
{
    if(const auto state=Get(window))
    {
        horizontal=std::max(0.f,horizontal);vertical=std::max(0.f,vertical);
        if(state->leftMargin==horizontal&&state->rightMargin==horizontal&&state->verticalMargin==vertical)return;
        state->leftMargin=state->rightMargin=horizontal;state->verticalMargin=vertical;state->Dirty();
    }
}
}
