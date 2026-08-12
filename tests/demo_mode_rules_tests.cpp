#include "demo_mode_rules.h"

#include <array>
#include <iostream>
#include <set>

namespace rules = snowdesktop::demo_mode_rules;

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

    if (failures != 0)
    {
        std::cerr << failures << " demo mode rule test(s) failed\n";
        return 1;
    }
    std::cout << "Demo mode rule tests passed\n";
    return 0;
}
