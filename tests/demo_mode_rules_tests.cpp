#include "demo_mode_rules.h"
#include "demo_collection_rules.h"

#include <array>
#include <iostream>
#include <set>
#include <vector>

namespace rules = snowdesktop::demo_mode_rules;
namespace collectionRules = snowdesktop::demo_collection_rules;

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}
} // namespace

int main()
{
    Check(rules::ShouldMaskApplication(true, true),
        "enabled demo mode must mask application shortcuts");
    Check(!rules::ShouldMaskApplication(false, true),
        "disabled demo mode must preserve application shortcuts");
    Check(!rules::ShouldMaskApplication(true, false),
        "files and folders must remain unmasked");

    const std::wstring_view identity = L"desktop:item:editor";
    const auto first = rules::VisualIdentityIndex(identity);
    Check(first == rules::VisualIdentityIndex(identity),
        "a stable identity must keep the same visual mapping");
    Check(first < rules::kVisualIdentities.size(),
        "the visual mapping index must stay in range");

    constexpr std::array<std::wstring_view, 8> samples{
        L"desktop:item:editor", L"desktop:item:mail",
        L"desktop:item:calendar", L"desktop:item:music",
        L"desktop:item:photos", L"desktop:item:archive",
        L"desktop:item:browser", L"desktop:item:terminal",
    };
    std::set<std::size_t> variants;
    for (const auto sample : samples)
    {
        const auto& visual = rules::ResolveVisualIdentity(sample);
        variants.insert(rules::VisualIdentityIndex(sample));
        Check(visual.titleKey != nullptr && visual.titleKey[0] != '\0' &&
                !visual.glyph.empty(),
            "every demo identity must provide a title and glyph");
    }
    Check(variants.size() >= 4,
        "representative identities should not collapse to one visual");

    Check(rules::kDemoIconAssetCount == 28,
        "the embedded demo icon pool must include category visuals");
    const std::vector<std::wstring> creativeItems{
        L"Photoshop.lnk", L"DaVinci Resolve.lnk", L"Blender.lnk" };
    const auto& creative = collectionRules::ResolveCategory(
        L"", L"creative", creativeItems);
    Check(creative.id == L"creative",
        "creative collections must be inferred from title and contents");
    const std::vector<std::wstring> officeItems{
        L"Word.lnk", L"PowerPoint.lnk", L"Excel.lnk" };
    const auto& office = collectionRules::ResolveCategory(
        L"", L"office", officeItems);
    Check(office.id == L"office",
        "office collections must be inferred from title and contents");
    const auto& manual = collectionRules::ResolveCategory(
        L"gaming", L"office", officeItems);
    Check(manual.id == L"gaming",
        "an explicit category binding must override inference");
    Check(collectionRules::VisualIndex(creative, L"one") <
            rules::kDemoIconAssetCount &&
        collectionRules::VisualIndex(creative, L"one") ==
            collectionRules::VisualIndex(creative, L"one"),
        "category visual mappings must be stable and in range");

    if (failures != 0)
    {
        std::cerr << failures << " demo mode rule test(s) failed\n";
        return 1;
    }
    std::cout << "Demo mode rule tests passed\n";
    return 0;
}
