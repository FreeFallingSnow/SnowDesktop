#include "android_icon_pack.h"
#include "icon_image_codec.h"
#include <windows.h>
#include <objbase.h>
#include <objidl.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace snowdesktop::android_icon_pack
{
namespace
{
constexpr size_t kMaxSvgBytes = 16 * 1024 * 1024;
void Charge(size_t& used, size_t amount, size_t limit)
{
    if (amount > limit || used > limit - amount) throw std::length_error("Android materialization budget");
    used += amount;
}
void CheckSvg(const std::string& value)
{ if (value.size() > kMaxSvgBytes) throw std::length_error("Android SVG budget"); }
// Android resource structures follow AOSP androidfw/ResourceTypes.h. Every
// access is checked against its containing chunk, including compact entries.
struct Bytes
{
    std::string_view data;
    void Check(size_t p, size_t n) const { if (p > data.size() || n > data.size() - p) throw std::runtime_error("truncated Android resource"); }
    unsigned U8(size_t p) const { Check(p,1); return static_cast<unsigned char>(data[p]); }
    unsigned U16(size_t p) const { return U8(p) | U8(p+1)<<8; }
    std::uint32_t U32(size_t p) const { return U16(p) | static_cast<std::uint32_t>(U16(p+2))<<16; }
    Bytes Slice(size_t p, size_t n) const { Check(p,n); return {data.substr(p,n)}; }
};
std::string Utf8(std::wstring_view text)
{
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0 && !text.empty()) throw std::runtime_error("invalid UTF-16");
    std::string out(count,'\0'); if (count) WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),out.data(),count,nullptr,nullptr); return out;
}
std::vector<std::string> Strings(Bytes chunk)
{
    if (chunk.U16(0)!=1 || chunk.U16(2)<28 || chunk.U32(4)!=chunk.data.size()) throw std::runtime_error("invalid Android string pool");
    const auto count=chunk.U32(8), start=chunk.U32(20); const bool utf8=(chunk.U32(16)&256)!=0;
    if (count>200000) throw std::runtime_error("oversized Android string pool");
    chunk.Check(chunk.U16(2),static_cast<size_t>(count)*4);
    const auto length=[&](size_t& q) {
        unsigned n=utf8?chunk.U8(q++):chunk.U16(q); if (!utf8) q+=2;
        if (n & (utf8?128:32768)) { const auto next=utf8?chunk.U8(q++):chunk.U16(q); if (!utf8)q+=2; n=((n&(utf8?127:32767))<<(utf8?8:16))|next; } return n;
    };
    std::vector<std::string> result; result.reserve(count); size_t materialized = 0;
    for (unsigned i=0;i<count;++i)
    {
        size_t q=static_cast<size_t>(start)+chunk.U32(chunk.U16(2)+i*4); auto n=length(q); if (utf8)n=length(q);
        if (n>1000000) throw std::runtime_error("oversized Android string");
        chunk.Check(q,static_cast<size_t>(n)*(utf8?1:2)+(utf8?1:2));
        if (utf8) { Charge(materialized,n,16*1024*1024); result.emplace_back(chunk.data.substr(q,n)); }
        else { std::wstring wide; wide.reserve(n); for (unsigned j=0;j<n;++j)wide+=static_cast<wchar_t>(chunk.U16(q+j*2)); auto value=Utf8(wide); Charge(materialized,value.size(),16*1024*1024); result.push_back(std::move(value)); }
    }
    return result;
}
const std::string& At(const std::vector<std::string>& values, unsigned index)
{ if (index>=values.size()) throw std::runtime_error("invalid Android string reference"); return values[index]; }
struct Value { unsigned type=0; std::uint32_t data=0; std::string text; };
Value Typed(Bytes b,size_t pos,const std::vector<std::string>& strings)
{ b.Check(pos,8); if (b.U16(pos)!=8) throw std::runtime_error("invalid typed Android value"); Value v{b.U8(pos+3),b.U32(pos+4),{}}; if(v.type==3)v.text=At(strings,v.data); return v; }
struct Node { std::string tag; std::map<std::string,Value> attrs; std::vector<Node> children; };
Node PlainXml(std::string_view bytes)
{
    Microsoft::WRL::ComPtr<IStream> stream; Microsoft::WRL::ComPtr<IXmlReader> reader;
    ULONG written=0;
    if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)) || FAILED(stream->Write(bytes.data(),static_cast<ULONG>(bytes.size()),&written)) || written!=bytes.size() ||
        FAILED(stream->Seek({},STREAM_SEEK_SET,nullptr)) || FAILED(CreateXmlReader(__uuidof(IXmlReader),reinterpret_cast<void**>(reader.GetAddressOf()),nullptr)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing,DtdProcessing_Prohibit)) || FAILED(reader->SetInput(stream.Get())))throw std::runtime_error("invalid Android XML stream");
    Node document; std::vector<Node*> stack{&document}; XmlNodeType type{}; HRESULT status=S_OK; unsigned count=0; size_t materialized=0;
    while((status=reader->Read(&type))==S_OK)
    {
        if(type==XmlNodeType_Element)
        {
            if(++count>100000 || stack.size()>64)throw std::runtime_error("oversized Android XML");
            const wchar_t* name=nullptr; reader->GetLocalName(&name,nullptr); Node node; node.tag=Utf8(name?name:L"");
            Charge(materialized,node.tag.size(),32*1024*1024);
            const bool empty=reader->IsEmptyElement()!=FALSE;
            if(reader->MoveToFirstAttribute()==S_OK)do
            {
                const wchar_t *key=nullptr,*value=nullptr;reader->GetLocalName(&key,nullptr);reader->GetValue(&value,nullptr);
                if(!key||!value||node.attrs.size()>256)throw std::runtime_error("invalid Android XML attribute");
                auto attribute=Utf8(key), text=Utf8(value); Charge(materialized,attribute.size()+text.size(),32*1024*1024);
                node.attrs.emplace(std::move(attribute),Value{3,0,std::move(text)});
            }while(reader->MoveToNextAttribute()==S_OK);
            reader->MoveToElement(); stack.back()->children.push_back(std::move(node));
            if(!empty)stack.push_back(&stack.back()->children.back());
        }
        else if(type==XmlNodeType_EndElement){if(stack.size()<2)throw std::runtime_error("invalid Android XML nesting");stack.pop_back();}
    }
    if(FAILED(status)||stack.size()!=1||document.children.size()!=1)throw std::runtime_error("invalid Android XML document");return std::move(document.children[0]);
}
Node Xml(std::string_view bytes)
{
    const auto first=bytes.find_first_not_of(" \t\r\n\xef\xbb\xbf");
    if(first!=bytes.npos && bytes[first]=='<')return PlainXml(bytes);
    Bytes b{bytes}; Node document; std::vector<Node*> stack{&document};
    if (b.U16(0)!=3 || b.U32(4)!=bytes.size()) throw std::runtime_error("invalid compiled Android XML");
    std::vector<std::string> strings; unsigned nodes=0; size_t materialized=0;
    for(size_t p=b.U16(2);p<bytes.size();)
    {
        const auto size=b.U32(p+4); if(size<8)throw std::runtime_error("invalid Android XML chunk");
        const auto chunk=b.Slice(p,size); const auto type=chunk.U16(0);
        if(type==1) strings=Strings(chunk);
        else if(type==0x102)
        {
            if(++nodes>100000 || stack.size()>64)throw std::runtime_error("oversized Android XML tree");
            Node value; value.tag=At(strings,chunk.U32(20)); Charge(materialized,value.tag.size(),32*1024*1024);
            const size_t start=16+chunk.U16(24), stride=chunk.U16(26); const auto count=chunk.U16(28);
            if(stride<20 || count>256)throw std::runtime_error("invalid Android XML attributes");
            chunk.Check(start,static_cast<size_t>(count)*stride);
            for(unsigned i=0;i<count;++i)
            {
                const auto a=start+i*stride; auto v=Typed(chunk,a+12,strings); const auto raw=chunk.U32(a+8);
                if(raw!=0xffffffff){v.type=3;v.text=At(strings,raw);}
                const auto& key=At(strings,chunk.U32(a+4)); Charge(materialized,key.size()+v.text.size(),32*1024*1024);
                value.attrs.emplace(key,std::move(v));
            }
            stack.back()->children.push_back(std::move(value)); stack.push_back(&stack.back()->children.back());
        }
        else if(type==0x103) { if(stack.size()<2 || stack.back()->tag!=At(strings,chunk.U32(20)))throw std::runtime_error("unbalanced Android XML"); stack.pop_back(); }
        p+=size;
    }
    if(stack.size()!=1 || document.children.size()!=1)throw std::runtime_error("invalid Android XML root");
    return std::move(document.children.front());
}
struct Resource { std::string type,name; Value value; int rank=0; };
using Resources=std::unordered_map<std::uint32_t,Resource>;
Resources Table(std::string_view bytes)
{
    Bytes b{bytes}; if(b.U16(0)!=2 || b.U32(4)!=bytes.size())throw std::runtime_error("invalid Android table");
    Resources result; std::vector<std::string> global; size_t materialized=0;
    for(size_t p=b.U16(2);p<bytes.size();)
    {
        const auto size=b.U32(p+4); if(size<8)throw std::runtime_error("invalid Android resource chunk"); const auto chunk=b.Slice(p,size);
        if(chunk.U16(0)==1) global=Strings(chunk);
        if(chunk.U16(0)==0x200)
        {
            const auto package=chunk.U32(8); const auto offset=chunk.U16(2)>=288?chunk.U32(284):0;
            const auto pool=[&](unsigned at) { const auto q=chunk.U32(at); return Strings(chunk.Slice(q,chunk.U32(q+4))); };
            const auto types=pool(268), keys=pool(276);
            for(size_t q=chunk.U16(2);q<chunk.data.size();)
            {
                const auto sz=chunk.U32(q+4); if(sz<8)throw std::runtime_error("invalid Android type chunk"); const auto type=chunk.Slice(q,sz);
                if(type.U16(0)==0x201)
                {
                    const auto id=type.U8(8), flags=type.U8(9), count=type.U32(12), start=type.U32(16); const size_t header=type.U16(2);
                    if(!id || count>65536 || package>255 || id+offset>255)throw std::runtime_error("oversized Android type");
                    type.Check(header,static_cast<size_t>(count)*((flags&2)?2:4));
                    int rank=0; for(size_t c=24;c<header;++c) if(c!=34 && c!=35 && c!=44 && c!=45 && c!=46 && c!=47 && type.U8(c)) rank+=10000;
                    // Prefer the pack's baseline color over Android-version
                    // wallpaper palette aliases, which require an Android
                    // framework and have no corresponding Windows resource.
                    if(header>=48)rank+=static_cast<int>(type.U16(44)*2)*(At(types,id-1)=="color"?1:-1)-static_cast<int>(type.U16(34));
                    for(unsigned i=0;i<count;++i)
                    {
                        unsigned index=i; std::uint32_t at=0;
                        if(flags&1){index=type.U16(header+i*4);at=type.U16(header+i*4+2)*4;}
                        else if(flags&2){at=type.U16(header+i*2);at=at==65535?0xffffffff:at*4;}
                        else at=type.U32(header+i*4);
                        if(at==0xffffffff)continue;
                        const size_t e=static_cast<size_t>(start)+at; const auto ef=type.U16(e+2); if(ef&1)continue;
                        const auto key=(ef&8)?type.U16(e):type.U32(e+4);
                        auto value=(ef&8)?Value{ef>>8,type.U32(e+4),{}}:Typed(type,e+type.U16(e),global);
                        if((ef&8) && value.type==3)value.text=At(global,value.data);
                        const auto rid=(package<<24)|((id+offset)<<16)|index;
                        if(!result.contains(rid)||rank<result[rid].rank)
                        {
                            if(!result.contains(rid)&&result.size()>=100000)throw std::length_error("Android resource count budget");
                            const auto& typeName=At(types,id-1); const auto& keyName=At(keys,key);
                            const auto validName=[](const std::string& name) {
                                return !name.empty() && name.size()<=256 &&
                                    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0)>0;
                            };
                            if(!validName(typeName)||!validName(keyName))throw std::runtime_error("invalid Android resource name");
                            Charge(materialized,typeName.size()+keyName.size()+value.text.size(),64*1024*1024);
                            result[rid]={typeName,keyName,std::move(value),rank};
                        }
                    }
                }
                q+=sz;
            }
        }
        p+=size;
    }
    return result;
}
std::string Escape(std::string_view text)
{ std::string result; for(const char c:text) { if(c=='&')result+="&amp;";else if(c=='\"')result+="&quot;";else if(c=='<')result+="&lt;";else if(c=='>')result+="&gt;";else result+=c; } return result; }
std::string Number(double value)
{ if(!std::isfinite(value)||std::abs(value)>10000000)throw std::runtime_error("invalid Android coordinate");std::ostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(9)<<value;return s.str(); }
std::string Base64(std::string_view bytes)
{
    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve((bytes.size()+2)/3*4);
    for(size_t p=0;p<bytes.size();p+=3)
    {
        const unsigned a=static_cast<unsigned char>(bytes[p]),b=p+1<bytes.size()?static_cast<unsigned char>(bytes[p+1]):0,c=p+2<bytes.size()?static_cast<unsigned char>(bytes[p+2]):0;
        out+=alphabet[a>>2];out+=alphabet[((a&3)<<4)|(b>>4)];out+=p+1<bytes.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';out+=p+2<bytes.size()?alphabet[c&63]:'=';
    }
    return out;
}
struct Converter
{
    const Resources& resources;
    const std::unordered_map<std::string,std::string_view>& files;
    std::unordered_map<std::string,const Resource*> named;
    unsigned nextId=0;
    bool monochrome=false;
    Converter(const Resources& values,const std::unordered_map<std::string,std::string_view>& paths) : resources(values),files(paths)
    { for(const auto& [id,value]:resources){(void)id;named.emplace(value.type+"/"+value.name,&value);} }
    Value Resolve(Value v,unsigned depth=0) const
    {
        if(depth>32)throw std::runtime_error("cyclic Android reference");
        if(v.type==1 || v.type==7){const auto it=resources.find(v.data);if(it==resources.end())throw std::runtime_error("missing Android resource");return Resolve(it->second.value,depth+1);}
        if(v.type==3 && v.text.starts_with('@'))
        { const auto it=named.find(v.text.substr(1));if(it==named.end())throw std::runtime_error("missing named Android resource");return Resolve(it->second->value,depth+1); }
        if(v.type==2)throw std::runtime_error("unresolved Android theme attribute");return v;
    }
    const Value* Attr(const Node& n,const char* key) const {const auto it=n.attrs.find(key);return it==n.attrs.end()?nullptr:&it->second;}
    std::string Text(const Node& n,const char* key,std::string fallback={}) const
    { const auto v=Attr(n,key);if(!v)return fallback;const auto value=Resolve(*v);if(value.type!=3)throw std::runtime_error("expected Android text");return value.text; }
    double Numeric(Value value) const
    {
        value=Resolve(value);if(value.type==4)return std::bit_cast<float>(value.data);
        if(value.type==5||value.type==6)
        {constexpr std::array<double,4> radix{1./256,1./32768,1./8388608,1./2147483648};return static_cast<std::int32_t>(value.data&0xffffff00)*radix[(value.data>>4)&3];}
        if(value.type==16||value.type==17)return static_cast<std::int32_t>(value.data);
        if(value.type==3){size_t used=0;const double n=std::stod(value.text,&used);if(used==value.text.size())return n;if(value.text.substr(used)=="%")return n/100;if(value.text.substr(used)=="dp"||value.text.substr(used)=="dip"||value.text.substr(used)=="px")return n;}
        throw std::runtime_error("unsupported Android number");
    }
    double Num(const Node& n,const char* key,double fallback) const {const auto v=Attr(n,key);return v?Numeric(*v):fallback;}
    std::uint32_t Color(Value value) const
    {
        value=Resolve(value);
        if(value.type>=28&&value.type<=31)return monochrome?value.data&0xff000000:value.data;
        if(value.type==3&&value.text.starts_with('#'))
        {const auto n=value.text.size()-1;const auto color=static_cast<std::uint32_t>(std::stoul(value.text.substr(1),nullptr,16));if(n==6)return monochrome?0xff000000:color|0xff000000;if(n==8)return monochrome?color&0xff000000:color;}
        throw std::runtime_error("unsupported Android color");
    }
    std::string Paint(std::uint32_t color,double opacity,std::optional<std::uint32_t> tint={}) const
    {
        opacity*=double(color>>24)/255.; if(tint) {color=(color&0xff000000)|(*tint&0xffffff);opacity*=double(*tint>>24)/255.;}
        std::ostringstream s;s<<'#'<<std::hex<<std::setfill('0')<<std::setw(6)<<(color&0xffffff);
        return s.str()+"\" opacity=\""+Number(opacity);
    }
    std::string VectorNodes(const Node& node,std::optional<std::uint32_t> tint)
    {
        std::string out;unsigned clips=0;
        for(const auto& child:node.children)
        {
            if(child.tag=="group")
            {
                const double px=Num(child,"pivotX",0),py=Num(child,"pivotY",0);
                out+="<g transform=\"translate("+Number(Num(child,"translateX",0)+px)+" "+Number(Num(child,"translateY",0)+py)+") rotate("+Number(Num(child,"rotation",0))+") scale("+Number(Num(child,"scaleX",1))+" "+Number(Num(child,"scaleY",1))+") translate("+Number(-px)+" "+Number(-py)+")\">"+VectorNodes(child,tint)+"</g>";
            }
            else if(child.tag=="clip-path")
            {
                const auto id="androidClip"+std::to_string(nextId++);
                out+="<defs><clipPath id=\""+id+"\"><path d=\""+Escape(Text(child,"pathData"))+"\" clip-rule=\""+(Num(child,"fillType",0)==1?"evenodd":"nonzero")+"\"/></clipPath></defs><g clip-path=\"url(#"+id+")\">"; ++clips;
            }
            else if(child.tag=="path")
            {
                if(Num(child,"trimPathStart",0)!=0||Num(child,"trimPathEnd",1)!=1||Num(child,"trimPathOffset",0)!=0||!child.children.empty())throw std::runtime_error("unsupported Android path effect");
                const auto fill=Attr(child,"fillColor"),stroke=Attr(child,"strokeColor");
                const auto fillColor=fill?Color(*fill):0,strokeColor=stroke?Color(*stroke):0;
                // Individual paint alpha is separate from the root tint. A
                // transparent fill remains transparent in tinted line icons.
                const auto colorText=[&](std::uint32_t color) {if(tint)color=(color&0xff000000)|(*tint&0xffffff);std::ostringstream text;text<<'#'<<std::hex<<std::setfill('0')<<std::setw(6)<<(color&0xffffff);return text.str();};
                const double tintAlpha=tint?double(*tint>>24)/255:1;
                out+="<path d=\""+Escape(Text(child,"pathData"))+"\" fill=\""+colorText(fillColor)+"\" fill-opacity=\""+Number(double(fillColor>>24)/255*Num(child,"fillAlpha",1)*tintAlpha)+"\" stroke=\""+colorText(strokeColor)+"\" stroke-opacity=\""+Number(double(strokeColor>>24)/255*Num(child,"strokeAlpha",1)*tintAlpha)+"\" stroke-width=\""+Number(Num(child,"strokeWidth",0))+"\" fill-rule=\""+(Num(child,"fillType",0)==1?"evenodd":"nonzero")+"\"";
                const int cap=static_cast<int>(Num(child,"strokeLineCap",0)),join=static_cast<int>(Num(child,"strokeLineJoin",0));
                if(cap<0||cap>2||join<0||join>2)throw std::runtime_error("invalid Android stroke");
                out+=" stroke-linecap=\""+std::string(std::array{"butt","round","square"}[cap])+"\" stroke-linejoin=\""+std::string(std::array{"miter","round","bevel"}[join])+"\" stroke-miterlimit=\""+Number(Num(child,"strokeMiterLimit",4))+"\"/>";
            }
            else throw std::runtime_error("unsupported Android vector node");
            CheckSvg(out);
        }
        while(clips--)out+="</g>";CheckSvg(out);return out;
    }
    Node Document(const Value& value) const
    { const auto path=Resolve(value);if(path.type!=3)throw std::runtime_error("expected Android drawable file");const auto it=files.find(path.text);if(it==files.end())throw std::runtime_error("missing Android drawable file");return Xml(it->second); }
    std::string Layer(const Node& node,unsigned depth=0)
    {
        if(depth>32)throw std::runtime_error("cyclic Android drawable");
        if(node.tag=="item")
            for(const auto key:{"left","top","right","bottom","width","height","gravity"})
                if(Attr(node,key))throw std::runtime_error("unsupported Android layer placement");
        if(node.tag=="bitmap")
        {
            if(Num(node,"gravity",119)!=119 || Num(node,"tileMode",0)!=0 || Attr(node,"tileModeX") || Attr(node,"tileModeY"))
                throw std::runtime_error("unsupported Android bitmap tiling");
            const auto source=Attr(node,"src");if(!source)throw std::runtime_error("missing Android bitmap source");
            Node drawable;drawable.tag="item";drawable.attrs["drawable"]=*source;return Layer(drawable,depth+1);
        }
        if(const auto drawable=Attr(node,"drawable"); drawable && node.tag!="inset")
        {
            const auto value=Resolve(*drawable);
            if(value.type>=28&&value.type<=31)return "<rect width=\"108\" height=\"108\" fill=\""+Paint(Color(value),1)+"\"/>";
            if(value.type==3 && !value.text.ends_with(".xml"))
            {
                const auto file=files.find(value.text);if(file==files.end())throw std::runtime_error("missing Android bitmap layer");
                icon_image::Image image;std::string png,error;
                if(!icon_image::DecodeBytes(file->second,1024,image,error))throw std::runtime_error("invalid Android bitmap layer");
                if(monochrome)for(auto& pixel:image.pixels)pixel&=0xff000000;
                if(!icon_image::EncodePng(image,png,error))throw std::runtime_error("invalid Android bitmap layer");
                return "<image width=\"108\" height=\"108\" preserveAspectRatio=\"none\" xlink:href=\"data:image/png;base64,"+Base64(png)+"\"/>";
            }
            return Layer(Document(value),depth+1);
        }
        if(node.tag=="vector")
        {
            const double width=Num(node,"viewportWidth",0),height=Num(node,"viewportHeight",0);if(width<=0||height<=0)throw std::runtime_error("invalid vector viewport");
            if(Num(node,"tintMode",5)!=5)throw std::runtime_error("unsupported Android tint blend mode");
            std::optional<std::uint32_t> tint; if(const auto value=Attr(node,"tint"))tint=Color(*value);
            return "<g opacity=\""+Number(Num(node,"alpha",1))+"\" transform=\"scale("+Number(108/width)+" "+Number(108/height)+")\">"+VectorNodes(node,tint)+"</g>";
        }
        if(node.tag=="inset")
        {
            const auto inset=[&](const char* key) { const auto attr=Attr(node,key);const auto value=attr?*attr:(Attr(node,"inset")?*Attr(node,"inset"):Value{16,0,{}});return Numeric(value)*(value.type==6||(value.type==3&&value.text.ends_with('%'))?108:1); };
            const double left=inset("insetLeft"),top=inset("insetTop"),right=inset("insetRight"),bottom=inset("insetBottom");
            if(left<0||top<0||left+right>=108||top+bottom>=108)throw std::runtime_error("invalid Android inset");
            Node content=node;content.tag="item";content.attrs.erase("inset");content.attrs.erase("insetLeft");content.attrs.erase("insetTop");content.attrs.erase("insetRight");content.attrs.erase("insetBottom");
            return "<g transform=\"translate("+Number(left)+" "+Number(top)+") scale("+Number((108-left-right)/108)+" "+Number((108-top-bottom)/108)+")\">"+Layer(content,depth+1)+"</g>";
        }
        if(node.tag=="shape")
        {
            int kind=0;const auto shape=Attr(node,"shape");
            if(shape)kind=shape->type==3?(shape->text=="rectangle"?0:shape->text=="oval"?1:-1):static_cast<int>(Numeric(*shape));
            if(kind<0||kind>1)throw std::runtime_error("unsupported Android shape type");
            std::uint32_t fill=0,stroke=0;double width=0,radius=0;
            for(const auto& child:node.children)
            {
                if(child.tag=="solid") {const auto color=Attr(child,"color");if(!color)throw std::runtime_error("missing Android solid color");fill=Color(*color);}
                else if(child.tag=="stroke")
                {const auto color=Attr(child,"color");if(color)stroke=Color(*color);width=Num(child,"width",0);if(Attr(child,"dashWidth")||Attr(child,"dashGap"))throw std::runtime_error("unsupported Android dashed shape stroke");}
                else if(child.tag=="corners")
                {radius=Num(child,"radius",0);for(const auto key:{"topLeftRadius","topRightRadius","bottomLeftRadius","bottomRightRadius"})if(Num(child,key,radius)!=radius)throw std::runtime_error("unsupported asymmetric Android shape corners");}
                else throw std::runtime_error("unsupported Android shape effect");
            }
            if(width<0||width>=108||radius<0)throw std::runtime_error("invalid Android shape geometry");
            const auto paint=[&](std::uint32_t color){std::ostringstream text;text<<'#'<<std::hex<<std::setfill('0')<<std::setw(6)<<(color&0xffffff);return text.str();};
            std::string out=kind==1?"<ellipse cx=\"54\" cy=\"54\" rx=\""+Number(54-width/2)+"\" ry=\""+Number(54-width/2)+"\"":"<rect x=\""+Number(width/2)+"\" y=\""+Number(width/2)+"\" width=\""+Number(108-width)+"\" height=\""+Number(108-width)+"\" rx=\""+Number(std::max(0.,radius-width/2))+"\"";
            return out+" fill=\""+paint(fill)+"\" fill-opacity=\""+Number(double(fill>>24)/255)+"\" stroke=\""+paint(stroke)+"\" stroke-opacity=\""+Number(double(stroke>>24)/255)+"\" stroke-width=\""+Number(width)+"\"/>";
        }
        if(node.tag=="layer-list" || node.tag=="adaptive-icon")
        {std::string out;for(const auto& child:node.children)if(child.tag!="monochrome"){out+=Layer(child,depth+1);CheckSvg(out);}return out;}
        if((node.tag=="item"||node.tag=="background"||node.tag=="foreground"||node.tag=="monochrome")&&node.children.size()==1)return Layer(node.children[0],depth+1);
        throw std::runtime_error("unsupported Android drawable");
    }
    std::string Svg(const Node& node, bool adaptive = false, bool monoVariant = false)
    {
        nextId=0;monochrome=monoVariant;double width=108,height=108;std::string viewport=adaptive?"18 18 72 72":"0 0 108 108",content;
        if(!adaptive&&node.tag=="vector")
        {
            width=Num(node,"width",Num(node,"viewportWidth",108));height=Num(node,"height",Num(node,"viewportHeight",108));
            if(width<=0||height<=0)throw std::runtime_error("invalid Android vector size");
            viewport="0 0 "+Number(width)+" "+Number(height);
            content="<g transform=\"scale("+Number(width/108)+" "+Number(height/108)+")\">"+Layer(node)+"</g>";
        }
        else content=Layer(node);
        auto svg="<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\""+Number(width)+"\" height=\""+Number(height)+"\" viewBox=\""+viewport+"\">"+content+"</svg>";
        CheckSvg(svg);return svg;
    }
};
}
bool Convert(icon_pack::Archive& archive,std::string& error)
{
    try
    {
        std::unordered_map<std::string,std::string_view> files;
        for(const auto& file:archive.documents)files.emplace(file.name,file.bytes);
        for(const auto& file:archive.images)files.emplace(file.name,file.bytes);
        const auto table=files.find("resources.arsc");if(table==files.end())throw std::runtime_error("missing Android table");
        const auto resources=Table(table->second);Converter converter{resources,files};
        std::set<std::string> names;
        for(const auto& path:{"assets/appfilter.xml","assets/drawable.xml"})if(const auto file=files.find(path);file!=files.end())
        {
            const auto tree=Xml(file->second);
            for(const auto& item:tree.children)if(const auto attr=converter.Attr(item,"drawable");attr&&attr->type==3)names.insert(attr->text);
        }
        for(const auto& [id,value]:resources)
        {
            (void)id;if(value.type!="xml" || (value.name!="appfilter"&&value.name!="drawable"))continue;
            const auto tree=converter.Document(value.value);
            for(const auto& item:tree.children)if(const auto attr=converter.Attr(item,"drawable"))
            { const auto text=converter.Resolve(*attr);if(text.type==3)names.insert(text.text); }
        }
        if(names.empty())throw std::runtime_error("missing Android icon mapping");
        std::map<std::string,const Resource*> mapped;
        for(const auto& [id,value]:resources){(void)id;if((value.type=="drawable"||value.type=="mipmap")&&names.contains(value.name))mapped.emplace(value.name,&value);}
        std::vector<icon_pack::Entry> images; size_t convertedBytes=0;
        const auto append=[&](std::string name,std::string bytes) {
            if(images.size()>=50000)throw std::length_error("Android converted asset count budget");
            CheckSvg(bytes);Charge(convertedBytes,bytes.size(),256*1024*1024);
            images.push_back({std::move(name),std::move(bytes)});
        };
        for(const auto& name:names)
        {
            try
            {
                const auto found=mapped.find(name);if(found==mapped.end())throw std::runtime_error("missing mapped Android icon");
                const auto value=converter.Resolve(found->second->value);
                if(value.type!=3)throw std::runtime_error("unsupported mapped Android icon");
                const auto file=files.find(value.text);if(file==files.end())throw std::runtime_error("missing mapped Android file");
                if(!value.text.ends_with(".xml")) {append(name+std::filesystem::path(value.text).extension().string(),std::string(file->second));continue;}
                const auto tree=Xml(file->second);append(name+".svg",converter.Svg(tree,tree.tag=="adaptive-icon"));
                if(tree.tag=="adaptive-icon")for(const auto& child:tree.children)if(child.tag=="monochrome")
                {
                    try { append(name+".monochrome.svg",converter.Svg(child,true,true)); }
                    catch(const std::length_error&) { throw; }
                    catch(const std::exception&) { archive.skipped.push_back(name+".monochrome"); }
                }
            }
            catch(const std::length_error&) { throw; }
            catch(const std::exception&) {archive.skipped.push_back(name);}
        }
        archive.images=std::move(images);archive.documents.clear();
        if(archive.images.empty()){error="iconLibrary.packUnsupported";return false;}return true;
    }
    catch(const std::length_error&){error="iconLibrary.packTooLarge";return false;}
    catch(const std::exception&){error="iconLibrary.packUnsupported";return false;}
}
}
