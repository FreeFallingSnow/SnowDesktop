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
        Check(!visual.title.empty() &&
                !visual.glyph.empty(),
            "every demo identity must provide a title and glyph");
    }
    Check(variants.size() >= 4,
        "representative identities should not collapse to one visual");

    Check(rules::kDemoIconAssetCount == 115,
        "the embedded demo icon pool must include expanded category subjects");
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
    std::set<std::size_t> categoryVisuals;
    std::set<std::wstring_view> demoTitles;
    for (const auto& visual : rules::kVisualIdentities)
        Check(demoTitles.insert(visual.title).second,
            "standalone demo titles must be unique");
    constexpr std::array<std::size_t, 12> minimumVisibleSubjects{
        11, 6, 7, 11, 8, 5, 4, 13, 7, 9, 6, 4 };
    for (std::size_t categoryIndex = 0;
        categoryIndex < collectionRules::kCategories.size();
        ++categoryIndex)
    {
        const auto& category = collectionRules::kCategories[categoryIndex];
        Check(category.visualIndices.size() >=
                minimumVisibleSubjects[categoryIndex],
            "each demo category must cover its visible collection subjects");
        Check(category.identityTitles.size() ==
                collectionRules::kIdentityCountPerCategory,
            "each demo category must expose the full presentation pool");
        std::set<std::wstring_view> categoryTitles;
        for (const auto title : category.identityTitles)
            Check(categoryTitles.insert(title).second,
                "demo presentation titles must be unique within a category");
        for (const auto visualIndex : category.visualIndices)
        {
            Check(visualIndex < rules::kDemoIconAssetCount,
                "category visual index must stay in range");
            Check(categoryVisuals.insert(visualIndex).second,
                "different demo categories must not reuse an icon");
            Check(demoTitles.insert(
                    rules::VisualIdentityAt(visualIndex).title).second,
                "all visible demo titles must be unique");
        }
    }
    std::set<std::pair<std::size_t, std::size_t>> presentations;
    std::set<std::wstring_view> presentationTitles;
    for (std::size_t index = 0;
        index < collectionRules::kIdentityCountPerCategory; ++index)
    {
        const auto presentation =
            collectionRules::PresentationForSlot(creative, index);
        Check(presentation.visualIndex < rules::kDemoIconAssetCount,
            "demo presentation visual index must stay in range");
        Check(presentation.variantIndex <
                collectionRules::kIdentityVariantCount,
            "demo presentation variant index must stay in range");
        Check(presentations.insert({ presentation.visualIndex,
                presentation.variantIndex }).second,
            "presentation variants must be unique before the pool wraps");
        Check(presentationTitles.insert(presentation.title).second,
            "presentation titles must be unique before the pool wraps");
    }
    Check(collectionRules::PresentationForSlot(creative, 20).title ==
            collectionRules::PresentationForSlot(creative, 0).title,
        "large collections must wrap presentation only after twenty entries");
    Check(collectionRules::ExposedItemCount(12, 4, 2) == 11 &&
            collectionRules::ExposedItemCount(13, 1, 1) == 4 &&
            collectionRules::ExposedItemCount(3, 3, 1) == 3,
        "visible subject counts must include the all-button mosaic only on overflow");
    Check(collectionRules::PresentationForSlot(creative, 0, 3).visualIndex ==
            collectionRules::PresentationForSlot(creative, 3).visualIndex,
        "later peer collections must continue from the prior visible subject range");

    if (failures != 0)
    {
        std::cerr << failures << " demo mode rule test(s) failed\n";
        return 1;
    }
    std::cout << "Demo mode rule tests passed\n";
    return 0;
}
