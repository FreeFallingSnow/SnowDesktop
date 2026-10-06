#include "text_input_accessibility.h"
#include "text_input_state.h"
#include <ole2.h>
#include <UIAutomation.h>
#include <UIAutomationCoreApi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <new>

namespace snowdesktop::text_input
{
using Microsoft::WRL::ComPtr;
namespace
{
std::optional<AccessibleDocument> Read(const std::shared_ptr<TextAccess>& access)
{
    if(!access||!access->document)return {};
    try{return access->document();}catch(...){return {};}
}
std::vector<std::size_t> Boundaries(std::wstring_view text,TextUnit unit)
{
    std::vector<std::size_t> values{0};
    if(unit==TextUnit_Character)
        for(std::size_t at=0;at<text.size();) {at=NextBoundary(text,at);values.push_back(at);}
    else if(unit==TextUnit_Word)
        for(std::size_t at=0;at<text.size();) {at=WordBoundary(text,at,true);if(at==values.back())at=NextBoundary(text,at);values.push_back(at);}
    else if(unit==TextUnit_Line||unit==TextUnit_Paragraph)
        for(std::size_t at=0;at<text.size();++at)if(text[at]==L'\n')values.push_back(at+1);
    if(values.back()!=text.size())values.push_back(text.size());
    return values;
}
std::vector<std::size_t> Boundaries(const std::shared_ptr<TextAccess>& access,std::wstring_view text,TextUnit unit)
{
    if(access->unitBoundaries){auto values=access->unitBoundaries(static_cast<Unit>(unit));if(!values.empty())return values;}
    return Boundaries(text,unit);
}
bool SameDocument(const std::shared_ptr<TextAccess>& a,const std::shared_ptr<TextAccess>& b)
{return a==b||(a->lifetimeIdentity&&a->lifetimeIdentity==b->lifetimeIdentity&&a->elementIdentity==b->elementIdentity);}
class Range final:public ITextRangeProvider
{
public:
    Range(std::shared_ptr<TextAccess> access,IRawElementProviderSimple* owner,std::size_t start,std::size_t end)
        :access_(std::move(access)),owner_(owner),start_(start),end_(end){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_ITextRangeProvider)return E_NOINTERFACE;*out=static_cast<ITextRangeProvider*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider** out) override
    {if(!out)return E_POINTER;*out=new(std::nothrow)Range(access_,owner_.Get(),start_,end_);return *out?S_OK:E_OUTOFMEMORY;}
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider* other,BOOL* out) override
    {if(!out)return E_POINTER;const auto* range=dynamic_cast<Range*>(other);*out=range&&SameDocument(range->access_,access_)&&range->start_==start_&&range->end_==end_;return S_OK;}
    HRESULT STDMETHODCALLTYPE CompareEndpoints(TextPatternRangeEndpoint endpoint,ITextRangeProvider* other,TextPatternRangeEndpoint target,int* out) override
    {if(!out)return E_POINTER;const auto* range=dynamic_cast<Range*>(other);if(!range||!SameDocument(range->access_,access_))return E_INVALIDARG;const auto a=Endpoint(endpoint),b=range->Endpoint(target);*out=a==b?0:a<b?-1:1;return S_OK;}
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) override
    {
        const auto value=Document();if(!value)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto bounds=Boundaries(access_,value->text,unit);
        auto it=std::upper_bound(bounds.begin(),bounds.end(),start_);
        if(it==bounds.end()){start_=bounds.size()>1?bounds[bounds.size()-2]:0;end_=bounds.back();}
        else{end_=*it;start_=*(it-1);}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID,VARIANT,BOOL,ITextRangeProvider** out) override
    {if(!out)return E_POINTER;*out=nullptr;return S_OK;}
    HRESULT STDMETHODCALLTYPE FindText(BSTR query,BOOL backward,BOOL ignoreCase,ITextRangeProvider** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;if(!query)return E_INVALIDARG;
        const auto value=Document();if(!value)return UIA_E_ELEMENTNOTAVAILABLE;
        auto haystack=value->text.substr(start_,end_-start_);std::wstring needle(query,SysStringLen(query));
        if(ignoreCase){for(auto& ch:haystack)ch=static_cast<wchar_t>(std::towlower(ch));for(auto& ch:needle)ch=static_cast<wchar_t>(std::towlower(ch));}
        const auto at=backward?haystack.rfind(needle):haystack.find(needle);
        if(at==std::wstring::npos)return S_OK;
        *out=new(std::nothrow)Range(access_,owner_.Get(),start_+at,start_+at+needle.size());return *out?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID id,VARIANT* out) override
    {
        if(!out)return E_POINTER;VariantInit(out);const auto value=Document();if(!value)return UIA_E_ELEMENTNOTAVAILABLE;
        if(id==UIA_IsReadOnlyAttributeId){out->vt=VT_BOOL;out->boolVal=value->readOnly?VARIANT_TRUE:VARIANT_FALSE;return S_OK;}
        out->vt=VT_UNKNOWN;return UiaGetReservedNotSupportedValue(&out->punkVal);
    }
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;if(!Document())return UIA_E_ELEMENTNOTAVAILABLE;
        const auto boxes=access_->rectangles?access_->rectangles(start_,end_):std::vector<Rectangle>{};
        *out=SafeArrayCreateVector(VT_R8,0,static_cast<ULONG>(boxes.size()*4));if(!*out)return E_OUTOFMEMORY;
        double* data=nullptr;const auto hr=SafeArrayAccessData(*out,reinterpret_cast<void**>(&data));if(FAILED(hr)){SafeArrayDestroy(*out);*out=nullptr;return hr;}
        for(std::size_t i=0;i<boxes.size();++i){data[i*4]=boxes[i].left;data[i*4+1]=boxes[i].top;data[i*4+2]=boxes[i].width;data[i*4+3]=boxes[i].height;}
        return SafeArrayUnaccessData(*out);
    }
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(IRawElementProviderSimple** out) override
    {if(!out)return E_POINTER;*out=nullptr;if(!Document())return UIA_E_ELEMENTNOTAVAILABLE;return owner_.CopyTo(out);}
    HRESULT STDMETHODCALLTYPE GetText(int maximum,BSTR* out) override
    {
        if(!out)return E_POINTER;*out=nullptr;if(maximum<-1)return E_INVALIDARG;
        const auto value=Document();if(!value)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto count=maximum<0?end_-start_:(std::min)(end_-start_,static_cast<std::size_t>(maximum));
        *out=SysAllocStringLen(value->text.data()+start_,static_cast<UINT>(count));return *out?S_OK:E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE Move(TextUnit unit,int count,int* moved) override
    {
        if(!moved)return E_POINTER;*moved=0;const bool nonempty=start_!=end_;
        if(!count)return Document()?S_OK:UIA_E_ELEMENTNOTAVAILABLE;
        if(nonempty){const auto hr=ExpandToEnclosingUnit(unit);if(FAILED(hr))return hr;}
        const auto value=Document();if(!value)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto bounds=Boundaries(access_,value->text,unit);
        const auto index=static_cast<std::ptrdiff_t>(std::upper_bound(bounds.begin(),bounds.end(),start_)-bounds.begin())-1;
        const auto maximum=static_cast<std::ptrdiff_t>(bounds.size())-(nonempty?2:1);
        const auto next=std::clamp(index+static_cast<std::ptrdiff_t>(count),std::ptrdiff_t{0},(std::max)(std::ptrdiff_t{0},maximum));
        *moved=static_cast<int>(next-index);start_=bounds[static_cast<std::size_t>(next)];end_=nonempty&&next+1<static_cast<std::ptrdiff_t>(bounds.size())?bounds[static_cast<std::size_t>(next+1)]:start_;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(TextPatternRangeEndpoint endpoint,TextUnit unit,int count,int* moved) override
    {
        if(!moved)return E_POINTER;*moved=0;const auto value=Document();if(!value)return UIA_E_ELEMENTNOTAVAILABLE;
        if(!count)return S_OK;
        const auto bounds=Boundaries(access_,value->text,unit);const auto at=Endpoint(endpoint);
        const auto it=count<0?std::lower_bound(bounds.begin(),bounds.end(),at):std::upper_bound(bounds.begin(),bounds.end(),at);
        const auto index=count<0?static_cast<std::ptrdiff_t>(it-bounds.begin()):static_cast<std::ptrdiff_t>(it-bounds.begin())-1;
        const auto next=std::clamp(index+static_cast<std::ptrdiff_t>(count),std::ptrdiff_t{0},static_cast<std::ptrdiff_t>(bounds.size()-1));
        *moved=static_cast<int>(next-index);SetEndpoint(endpoint,bounds[static_cast<std::size_t>(next)]);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(TextPatternRangeEndpoint endpoint,ITextRangeProvider* other,TextPatternRangeEndpoint target) override
    {const auto* range=dynamic_cast<Range*>(other);if(!range||!SameDocument(range->access_,access_))return E_INVALIDARG;SetEndpoint(endpoint,range->Endpoint(target));return S_OK;}
    HRESULT STDMETHODCALLTYPE Select() override
    {if(!Document())return UIA_E_ELEMENTNOTAVAILABLE;return access_->select&&access_->select(start_,end_)?S_OK:UIA_E_INVALIDOPERATION;}
    HRESULT STDMETHODCALLTYPE AddToSelection() override{return UIA_E_INVALIDOPERATION;}
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override
    {if(!Document())return UIA_E_ELEMENTNOTAVAILABLE;return access_->select&&access_->select(end_,end_)?S_OK:UIA_E_INVALIDOPERATION;}
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL top) override
    {if(!Document())return UIA_E_ELEMENTNOTAVAILABLE;return access_->reveal&&access_->reveal(top?start_:end_,top!=FALSE)?S_OK:UIA_E_NOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY** out) override
    {if(!out)return E_POINTER;*out=SafeArrayCreateVector(VT_UNKNOWN,0,0);return *out?S_OK:E_OUTOFMEMORY;}
private:
    std::optional<AccessibleDocument> Document()
    {auto value=Read(access_);if(!value||value->password)return {};start_=(std::min)(start_,value->text.size());end_=std::clamp(end_,start_,value->text.size());return value;}
    std::size_t Endpoint(TextPatternRangeEndpoint endpoint) const{return endpoint==TextPatternRangeEndpoint_Start?start_:end_;}
    void SetEndpoint(TextPatternRangeEndpoint endpoint,std::size_t value)
    {if(endpoint==TextPatternRangeEndpoint_Start){start_=value;if(end_<start_)end_=start_;}else{end_=value;if(start_>end_)start_=end_;}}
    std::atomic<ULONG> references_{1};std::shared_ptr<TextAccess> access_;ComPtr<IRawElementProviderSimple> owner_;std::size_t start_,end_;
};
class Provider final:public ITextProvider
{
public:
    Provider(std::shared_ptr<TextAccess> access,IRawElementProviderSimple* owner):access_(std::move(access)),owner_(owner){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override
    {if(!out)return E_POINTER;*out=nullptr;if(iid!=IID_IUnknown&&iid!=IID_ITextProvider)return E_NOINTERFACE;*out=static_cast<ITextProvider*>(this);AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** out) override
    {if(!out)return E_POINTER;*out=nullptr;const auto value=Read(access_);if(!value||value->password)return UIA_E_ELEMENTNOTAVAILABLE;return Array((std::min)(value->cursor,value->anchor),(std::max)(value->cursor,value->anchor),out);}
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY** out) override
    {
        if(!out)return E_POINTER;*out=nullptr;const auto value=Read(access_);if(!value||value->password)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto ranges=value->offscreen?std::vector<std::pair<std::size_t,std::size_t>>{}:
            access_->visibleRanges?access_->visibleRanges():std::vector<std::pair<std::size_t,std::size_t>>{{0,value->text.size()}};
        *out=SafeArrayCreateVector(VT_UNKNOWN,0,static_cast<ULONG>(ranges.size()));if(!*out)return E_OUTOFMEMORY;
        LONG index=0;for(const auto& bounds:ranges)
        {
            ComPtr<ITextRangeProvider> range;range.Attach(new(std::nothrow)Range(access_,owner_.Get(),bounds.first,bounds.second));
            const auto hr=range?SafeArrayPutElement(*out,&index,range.Get()):E_OUTOFMEMORY;
            if(FAILED(hr)){SafeArrayDestroy(*out);*out=nullptr;return hr;}++index;
        }return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RangeFromChild(IRawElementProviderSimple*,ITextRangeProvider** out) override
    {if(!out)return E_POINTER;*out=nullptr;return E_INVALIDARG;}
    HRESULT STDMETHODCALLTYPE RangeFromPoint(UiaPoint point,ITextRangeProvider** out) override
    {if(!out)return E_POINTER;*out=nullptr;const auto value=Read(access_);if(!value||value->password)return UIA_E_ELEMENTNOTAVAILABLE;
        const auto at=access_->hit?access_->hit({point.x,point.y}):std::optional<std::size_t>{};if(!at)return UIA_E_NOTSUPPORTED;
        *out=new(std::nothrow)Range(access_,owner_.Get(),*at,*at);return *out?S_OK:E_OUTOFMEMORY;}
    HRESULT STDMETHODCALLTYPE get_DocumentRange(ITextRangeProvider** out) override
    {if(!out)return E_POINTER;*out=nullptr;const auto value=Read(access_);if(!value||value->password)return UIA_E_ELEMENTNOTAVAILABLE;
        *out=new(std::nothrow)Range(access_,owner_.Get(),0,value->text.size());return *out?S_OK:E_OUTOFMEMORY;}
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(SupportedTextSelection* out) override
    {if(!out)return E_POINTER;*out=SupportedTextSelection_Single;return S_OK;}
private:
    HRESULT Array(std::size_t start,std::size_t end,SAFEARRAY** out)
    {
        if(!out)return E_POINTER;*out=SafeArrayCreateVector(VT_UNKNOWN,0,1);if(!*out)return E_OUTOFMEMORY;
        ComPtr<ITextRangeProvider> range;range.Attach(new(std::nothrow)Range(access_,owner_.Get(),start,end));if(!range){SafeArrayDestroy(*out);*out=nullptr;return E_OUTOFMEMORY;}
        LONG index=0;const auto hr=SafeArrayPutElement(*out,&index,range.Get());if(FAILED(hr)){SafeArrayDestroy(*out);*out=nullptr;}return hr;
    }
    std::atomic<ULONG> references_{1};std::shared_ptr<TextAccess> access_;ComPtr<IRawElementProviderSimple> owner_;
};
}
HRESULT CreateTextProvider(std::shared_ptr<TextAccess> access,IRawElementProviderSimple* owner,ITextProvider** out)
{if(!out)return E_POINTER;*out=new(std::nothrow)Provider(std::move(access),owner);return *out?S_OK:E_OUTOFMEMORY;}
}
