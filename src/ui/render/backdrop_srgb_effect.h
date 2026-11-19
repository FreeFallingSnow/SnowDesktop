#pragma once

#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Composition.h>

#include <utility>
#include <vector>

namespace snowdesktop::backdrop_srgb
{
namespace wf = winrt::Windows::Foundation;
namespace graphics = winrt::Windows::Graphics::Effects;
namespace abi = ABI::Windows::Graphics::Effects;
namespace composition = winrt::Windows::UI::Composition;

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

inline winrt::com_ptr<Effect> Composite(D2D1_COMPOSITE_MODE mode,
    const graphics::IGraphicsEffectSource& background, const graphics::IGraphicsEffectSource& foreground)
{
    return winrt::make_self<Effect>(CLSID_D2D1Composite,
        std::vector<wf::IInspectable>{wf::PropertyValue::CreateUInt32(mode)},
        std::vector{background, foreground});
}

inline composition::CompositionEffectFactory CreateSceneFactory(const composition::Compositor& compositor)
{
    const auto scene = composition::CompositionEffectSourceParameter(L"scene");
    const auto mixed = Composite(D2D1_COMPOSITE_MODE_SOURCE_OVER,
        composition::CompositionEffectSourceParameter(L"backdrop"), scene);
    // The scene is an 8-bit premultiplied surface. Replace the background
    // only where an actual content pixel reached the scene surface.
    const float mask[]{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 0};
    const auto support = winrt::make_self<Effect>(CLSID_D2D1ColorMatrix,
        std::vector<wf::IInspectable>{wf::PropertyValue::CreateSingleArray(mask),
            wf::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED),
            wf::PropertyValue::CreateBoolean(true)},
        std::vector<graphics::IGraphicsEffectSource>{composition::CompositionEffectSourceParameter(L"scene")});
    const auto output = Composite(D2D1_COMPOSITE_MODE_DESTINATION_IN, *mixed, *support);
    auto factory = compositor.CreateEffectFactory(*output);
    winrt::check_hresult(factory.ExtendedError());
    return factory;
}
}
