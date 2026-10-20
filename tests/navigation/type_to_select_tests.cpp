#include "navigation/type_to_select.h"

#include <iostream>
#include <vector>

namespace
{
int failures = 0;
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

// The real Session/MatchesPrefix receive view-order candidates. Only the host's
// window, selection and clock boundaries are replaced; expected indices are
// literal. No test edits real desktop files or launches the application.
struct View
{
    snowdesktop::type_to_select::Session session;
    std::vector<snowdesktop::type_to_select::Candidate> files;

    std::optional<size_t> Type(wchar_t ch, std::uint64_t tick,
        std::wstring_view scope = L"desktop:one",
        const snowdesktop::type_to_select::InputContext& context = {},
        bool repeated = false)
    {
        const auto index = session.Input(ch, tick, scope, files, context, repeated);
        if (index)
        {
            for (auto& file : files) file.selected = false;
            files[*index].selected = true;
        }
        return index;
    }
};
}

int RunTypeToSelectTests()
{
    using namespace snowdesktop::type_to_select;
    Check(MatchesPrefix(L"工作计划.xlsx", L"gongzuo"), "full pinyin locates a Chinese file");
    Check(MatchesPrefix(L"工作计划.xlsx", L"GZJH"), "case-insensitive initials locate a Chinese file");
    Check(MatchesPrefix(L"工作计划.xlsx", L"工作"), "committed Chinese text locates a literal prefix");
    Check(MatchesPrefix(L"ReadMe.txt", L"rea"), "Latin filenames ignore case");
    Check(MatchesPrefix(L"École.txt", L"éC"), "literal prefix uses ordinal Unicode case folding");
    Check(MatchesPrefix(L"项目2026说明.txt", L"xm2026"), "initials preserve digits in mixed names");
    Check(MatchesPrefix(L"女儿.txt", L"nver"), "the existing umlaut pinyin convention is reused");
    Check(MatchesPrefix(L"_工作.txt", L"_工"), "literal punctuation is preserved");
    Check(!MatchesPrefix(L"年度工作计划.xlsx", L"gzjh"), "typing must not jump to an arbitrary substring");
    Check(!MatchesPrefix(L"工作.txt", L"!"), "punctuation alone must not match every romanized filename");
    Check(!MatchesPrefix(L"Alpha.txt", L"a!"), "punctuation in a query must not be stripped into a false match");
    Check(!MatchesPrefix(L"工作.txt", L""), "empty input must not select an arbitrary file");

    View prefix{{}, {{L"工作计划.xlsx"}, {L"工作总结.docx"}, {L"个人照片.jpg"}}};
    Check(prefix.Type(L'g', 0) == 0, "first key starts from the first item in view order");
    Check(prefix.Type(L'z', 100) == 0, "continuation retains a matching selected file");
    Check(prefix.Type(L'z', 200) == 1, "additional initials narrow selection to the correct file");
    Check(!prefix.Type(L'x', 300) && prefix.files[1].selected,
        "no match preserves the last selection instead of clearing it");
    Check(prefix.Type(L'g', 1300) == 2, "exact timeout resets the query and advances from selection");

    View cycle{{}, {{L"Garden.txt"}, {L"工作.txt"}, {L"个人.txt"}}};
    Check(cycle.Type(L'g', 10) == 0 && cycle.Type(L'G', 20) == 1 &&
        cycle.Type(L'g', 30) == 2 && cycle.Type(L'g', 40) == 0,
        "deliberate repeated letters cycle and wrap in visual order");
    Check(!cycle.Type(L'g', 50, L"desktop:one", {}, true) && cycle.files[0].selected,
        "holding a key must not race through the selection");
    Check(cycle.Type(L'z', 60) == 1, "a letter after cycling continues from the single-letter prefix");

    View doubled{{}, {{L"Alpha.txt"}, {L"Aaron.txt"}}};
    Check(doubled.Type(L'a', 10) == 0 && doubled.Type(L'a', 20) == 1 &&
        doubled.Type(L'r', 30) == 1,
        "real repeated filename prefixes remain searchable instead of always cycling");
    View chinese{{}, {{L"工作.txt"}, {L"人人.txt"}}};
    Check(chinese.Type(L'人', 10) == 1 && chinese.Type(L'人', 20) == 1,
        "repeated Chinese characters stay literal and do not become letter cycling");

    View scopes{{}, {{L"Alpha.txt"}, {L"Beta.txt"}}};
    Check(scopes.Type(L'a', 10) == 0 && scopes.Type(L'b', 20, L"widget:two") == 1,
        "moving into a different view must not append to the old query");
    scopes.session.Reset();
    Check(scopes.Type(L'a', 30, L"widget:two") == 0,
        "explicit mouse/focus/command reset starts a new query immediately");
    View expand{{}, {{L"工作计划.txt"}, {L"个人.txt"}}};
    Check(expand.Type(L'g', 10, L"widget:one") == 0, "compact collection begins a query");
    expand.session.ContinueInScope(L"popup:widget:one");
    Check(expand.Type(L'z', 20, L"popup:widget:one") == 0,
        "automatic collection expansion preserves the incremental query");

    for (const InputContext context : {InputContext{true}, InputContext{false, true},
            InputContext{false, false, true}, InputContext{false, false, false, true}})
    {
        View protectedView{{}, {{L"Alpha.txt"}, {L"Beta.txt", true}}};
        Check(!protectedView.Type(L'a', 10, L"desktop:one", context) &&
            protectedView.files[1].selected, "editing, overlays, gestures and shortcuts preserve selection");
        Check(protectedView.Type(L'a', 20) == 0, "blocked input does not leak into a subsequent desktop query");
    }
    View empty;
    Check(!empty.Type(L'a', 10), "an empty view cannot produce a selection index");
    View controls{{}, {{L"Alpha.txt", true}, {L"Beta.txt"}}};
    Check(!controls.Type(L'\r', 10) && !controls.Type(0x7f, 20) && controls.files[0].selected,
        "command characters are never filename input");
    return failures;
}
