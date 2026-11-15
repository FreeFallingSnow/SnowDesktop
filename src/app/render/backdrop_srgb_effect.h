#pragma once

#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>

#include <utility>
#include <vector>

namespace snowdesktop::backdrop_srgb
{
namespace wf = winrt::Windows::Foundation;
namespace graphics = winrt::Windows::Graphics::Effects;
namespace abi = ABI::Windows::Graphics::Effects;

struct Effect : winrt::implements<Effect,
    graphics::IGraphicsEffect, graphics::IGraphicsEffectSource,
    abi::IGraphicsEffectD2D1Interop>
{
    Effect(GUID effectId, std::vector<wf::IInspectable> effectProperties,
        std::vector<graphics::IGraphicsEffectSource> effectInputs)
        : id(effectId), properties(std::move(effectProperties)), inputs(std::move(effectInputs))
    {
    }

    HRESULT STDMETHODCALLTYPE GetEffectId(GUID* output) noexcept override
    {
        if (!output) return E_INVALIDARG;
        *output = id;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR, UINT*,
        abi::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = static_cast<UINT>(properties.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
        ABI::Windows::Foundation::IPropertyValue** value) noexcept override try
    {
        if (!value) return E_INVALIDARG;
        if (index >= properties.size()) return E_BOUNDS;
        *value = properties[index].as<ABI::Windows::Foundation::IPropertyValue>().detach();
        return S_OK;
    }
    catch (...) { return winrt::to_hresult(); }
    HRESULT STDMETHODCALLTYPE GetSource(UINT index, abi::IGraphicsEffectSource** source) noexcept override
    {
        if (!source) return E_INVALIDARG;
        if (index >= inputs.size()) return E_BOUNDS;
        winrt::copy_to_abi(inputs[index], *reinterpret_cast<void**>(source));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
    {
        if (!count) return E_INVALIDARG;
        *count = static_cast<UINT>(inputs.size());
        return S_OK;
    }
    winrt::hstring Name() const { return name; }
    void Name(const winrt::hstring& value) { name = value; }

    const GUID id;
    const std::vector<wf::IInspectable> properties;
    const std::vector<graphics::IGraphicsEffectSource> inputs;
    winrt::hstring name;
};

inline winrt::com_ptr<Effect> SignMask(const graphics::IGraphicsEffectSource& input)
{
    using wf::PropertyValue;
    std::vector<wf::IInspectable> properties;
    properties.reserve(13);
    for (int channel = 0; channel < 3; ++channel)
    {
        properties.push_back(PropertyValue::CreateSingle(.5f));
        properties.push_back(PropertyValue::CreateSingle(4096.f));
        properties.push_back(PropertyValue::CreateBoolean(false));
    }
    properties.push_back(PropertyValue::CreateSingle(1.f));
    properties.push_back(PropertyValue::CreateSingle(0.f));
    properties.push_back(PropertyValue::CreateBoolean(false));
    properties.push_back(PropertyValue::CreateBoolean(true));
    return winrt::make_self<Effect>(CLSID_D2D1LinearTransfer, std::move(properties),
        std::vector{input});
}

inline winrt::com_ptr<Effect> Gamma(const graphics::IGraphicsEffectSource& input, float exponent)
{
    using wf::PropertyValue;
    std::vector<wf::IInspectable> properties;
    properties.reserve(17);
    for (int channel = 0; channel < 3; ++channel)
    {
        properties.push_back(PropertyValue::CreateSingle(1.f));
        properties.push_back(PropertyValue::CreateSingle(exponent));
        properties.push_back(PropertyValue::CreateSingle(0.f));
        properties.push_back(PropertyValue::CreateBoolean(false));
    }
    properties.push_back(PropertyValue::CreateSingle(1.f));
    properties.push_back(PropertyValue::CreateSingle(1.f));
    properties.push_back(PropertyValue::CreateSingle(0.f));
    properties.push_back(PropertyValue::CreateBoolean(true));
    properties.push_back(PropertyValue::CreateBoolean(false));
    return winrt::make_self<Effect>(CLSID_D2D1GammaTransfer, std::move(properties),
        std::vector{input});
}

// The 2.4 power coordinate gives the same source-over result as the sRGB
// power segment: its outer affine scale and offset cancel in the blend.
// Use a signed extension for scRGB and approximate the tiny linear toe.
// The sign mask has alpha one, so 2 * gamma * mask - gamma retains input alpha.
template<typename MakeInput>
winrt::com_ptr<Effect> Transfer(MakeInput&& makeInput, bool encode)
{
    const auto magnitude = Gamma(makeInput(), encode ? 1.f / 2.4f : 2.4f);
    const auto sign = SignMask(makeInput());
    const float coefficients[]{2.f, -1.f, 0.f, 0.f};
    std::vector<wf::IInspectable> properties{wf::PropertyValue::CreateSingleArray(coefficients),
        wf::PropertyValue::CreateBoolean(false)};
    return winrt::make_self<Effect>(CLSID_D2D1ArithmeticComposite, std::move(properties),
        std::vector<graphics::IGraphicsEffectSource>{*magnitude, *sign});
}
}
