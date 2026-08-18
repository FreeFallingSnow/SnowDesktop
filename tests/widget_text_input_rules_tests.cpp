#include "widget_text_input_rules.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestUtf8Counting()
{
    using snowdesktop::widget_runtime::HostTextOffsetFromUtf8ByteOffset;
    using snowdesktop::widget_runtime::Utf8ByteOffsetForHostTextOffset;
    using snowdesktop::widget_runtime::Utf8BytesForHostText;
    Check(Utf8BytesForHostText(L"note") == 4,
        "ASCII host text uses one byte per code point");
    Check(Utf8BytesForHostText(L"便笺") == 6,
        "CJK host text uses three bytes per code point");
    Check(Utf8BytesForHostText(L"\U0001F4DD") == 4,
        "supplementary characters use one four-byte UTF-8 sequence");
    const std::wstring mixed = L"A便\U0001F4DDB";
    Check(Utf8ByteOffsetForHostTextOffset(mixed, 0) == 0 &&
            Utf8ByteOffsetForHostTextOffset(mixed, 1) == 1 &&
            Utf8ByteOffsetForHostTextOffset(mixed, 2) == 4 &&
            Utf8ByteOffsetForHostTextOffset(mixed, mixed.size()) == 9,
        "host caret offsets convert to zero-based UTF-8 byte boundaries");
    Check(HostTextOffsetFromUtf8ByteOffset(mixed, 0) == 0 &&
            HostTextOffsetFromUtf8ByteOffset(mixed, 4) == 2 &&
            HostTextOffsetFromUtf8ByteOffset(mixed, 8) ==
                mixed.size() - 1 &&
            HostTextOffsetFromUtf8ByteOffset(mixed, 9) == mixed.size() &&
            !HostTextOffsetFromUtf8ByteOffset(mixed, 2),
        "UTF-8 selection offsets convert only at complete code-point boundaries");
}

void TestBoundedReplacement()
{
    using snowdesktop::widget_runtime::HostTextReplacementFits;
    using snowdesktop::widget_runtime::TryApplyHostTextReplacement;

    std::wstring text = L"1234";
    std::size_t cursor = 4;
    Check(!TryApplyHostTextReplacement(
            text, 4, 4, L"5", 4, cursor) &&
            text == L"1234" && cursor == 4,
        "a rejected insertion leaves text and cursor unchanged");
    Check(TryApplyHostTextReplacement(
            text, 1, 4, L"便", 4, cursor) &&
            text == L"1便" && cursor == 2,
        "replacement evaluates the final UTF-8 byte count atomically");
    Check(!HostTextReplacementFits(text, 2, 2, L"笺", 4),
        "multibyte insertion is rejected above the byte limit");
    Check(TryApplyHostTextReplacement(
            text, 2, 2, L"笺", 0, cursor) &&
            text == L"1便笺",
        "a zero limit keeps text replacement unlimited");
}

void TestReadOnlyMutationGate()
{
    using snowdesktop::widget_runtime::HostInputAllowsMutation;
    Check(HostInputAllowsMutation(true, false),
        "enabled editable controls allow mutations");
    Check(!HostInputAllowsMutation(true, true),
        "read-only controls reject mutations while remaining enabled");
    Check(!HostInputAllowsMutation(false, false) &&
            !HostInputAllowsMutation(false, true),
        "disabled controls always reject mutations");
}

void TestDeferredFocusRequest()
{
    using snowdesktop::widget_runtime::DeferredHostInputFocus;

    DeferredHostInputFocus request;
    Check(!request.Active(), "deferred focus starts empty");
    request.Request("edit-task-1", "desktop");
    Check(request.Active() && request.ControlId() == "edit-task-1",
        "deferred focus retains the requested control");
    Check(request.MatchesSurface("desktop") &&
            !request.MatchesSurface("panel"),
        "deferred focus is scoped to its originating surface");
    request.Request("edit-task-2", "panel");
    Check(request.ControlId() == "edit-task-2" &&
            request.MatchesSurface("panel"),
        "the newest deferred focus request replaces the previous request");
    request.Clear();
    Check(!request.Active() && request.ControlId().empty(),
        "consuming deferred focus clears the request");
}

void TestCaretVisibilityRequest()
{
    using snowdesktop::widget_runtime::HostInputCaretVisibilityRequest;

    HostInputCaretVisibilityRequest request;
    Check(!request.Consume(),
        "caret visibility starts without a pending adjustment");
    request.Request();
    Check(request.Consume() && !request.Consume(),
        "caret visibility is consumed once after focus or editing");
    request.Request();
    request.PreserveManualScroll();
    Check(!request.Consume(),
        "manual multiline scrolling cancels the pending caret adjustment");
    request.Request();
    Check(request.Consume(),
        "later caret movement restores caret-follow scrolling");
}
}

int main()
{
    TestUtf8Counting();
    TestBoundedReplacement();
    TestReadOnlyMutationGate();
    TestDeferredFocusRequest();
    TestCaretVisibilityRequest();
    if (failures != 0)
    {
        std::cerr << failures << " widget text-input rule checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget text-input rule checks passed\n";
    return EXIT_SUCCESS;
}
