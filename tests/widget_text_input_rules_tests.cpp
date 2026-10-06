#include "widget/view/widget_text_input_rules.h"
#include "ui/input/text_input_state.h"

#include <dwrite.h>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <wrl/client.h>

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

void TestContextMenuState()
{
    using snowdesktop::widget_runtime::
        ResolveHostInputContextMenuState;

    const auto editableSelection = ResolveHostInputContextMenuState(
        8, 6, 2, true, false, true);
    Check(editableSelection.canSelectAll &&
            editableSelection.canCut &&
            editableSelection.canCopy &&
            editableSelection.canPaste,
        "editable selected text exposes every context-menu command");

    const auto caretOnly = ResolveHostInputContextMenuState(
        8, 4, 4, true, false, true);
    Check(caretOnly.canSelectAll &&
            !caretOnly.canCut &&
            !caretOnly.canCopy &&
            caretOnly.canPaste,
        "copy and cut require a selection while paste uses the caret");

    const auto readOnly = ResolveHostInputContextMenuState(
        8, 8, 0, true, true, true);
    Check(readOnly.canSelectAll &&
            !readOnly.canCut &&
            readOnly.canCopy &&
            !readOnly.canPaste,
        "read-only inputs allow selection and copy but reject mutations");

    const auto emptyClipboard = ResolveHostInputContextMenuState(
        8, 8, 0, true, false, false);
    Check(!emptyClipboard.canPaste,
        "paste is disabled when the clipboard has no Unicode text");

    const auto emptyInput = ResolveHostInputContextMenuState(
        0, 9, 4, true, false, true);
    Check(!emptyInput.canSelectAll &&
            !emptyInput.canCut &&
            !emptyInput.canCopy &&
            emptyInput.canPaste,
        "empty inputs expose only an available paste operation");
}

void TestSubmittedInputFocusScope()
{
    using snowdesktop::widget_runtime::HostInputSubmitFocusScope;
    const HostInputSubmitFocusScope* active = nullptr;
    {
        HostInputSubmitFocusScope submit(active, L"reminders", "new-task", "desktop");
        Check(active && active->AllowsFocus(
                L"reminders", "new-task", "desktop", true, false, true),
            "a live submit can restore its own editor without outer widget selection");
        Check(!active->AllowsFocus(L"other", "new-task", "desktop", true, false, true) &&
                !active->AllowsFocus(L"reminders", "other", "desktop", true, false, true) &&
                !active->AllowsFocus(L"reminders", "new-task", "panel", true, false, true),
            "submit focus cannot transfer to another widget, editor or surface");
        Check(!active->AllowsFocus(L"reminders", "new-task", "desktop", false, false, true) &&
                !active->AllowsFocus(L"reminders", "new-task", "desktop", true, true, true) &&
                !active->AllowsFocus(L"reminders", "new-task", "desktop", true, false, false),
            "invalid, preview and hidden editors cannot be restored");
        {
            HostInputSubmitFocusScope nested(active, L"reminders", "panel-input", "panel");
            Check(active->AllowsFocus(L"reminders", "panel-input", "panel", true, false, true) &&
                    !active->AllowsFocus(L"reminders", "new-task", "desktop", true, false, true),
                "a nested submit temporarily owns the focus grant");
        }
        Check(active == &submit && active->AllowsFocus(
                L"reminders", "new-task", "desktop", true, false, true),
            "a nested submit returns the grant to its still-active caller");
    }
    Check(active == nullptr,
        "the submit grant expires before a later render or timer can request focus");
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

void TestVerticalExtents()
{
    using snowdesktop::widget_runtime::ResolveHostInputVerticalExtents;

    const auto fractionalViewport = ResolveHostInputVerticalExtents(
        48.4f, 40.0f);
    Check(fractionalViewport.viewport == 48 &&
            fractionalViewport.content == 48,
        "fractional viewport height does not create one pixel of false overflow");

    const auto fittingFractionalContent = ResolveHostInputVerticalExtents(
        48.4f, 48.2f);
    Check(fittingFractionalContent.content ==
            fittingFractionalContent.viewport,
        "content that fits before pixel rounding remains non-scrollable");

    const auto overflowingContent = ResolveHostInputVerticalExtents(
        48.4f, 48.6f);
    Check(overflowingContent.viewport == 48 &&
            overflowingContent.content == 49,
        "measured content beyond the viewport retains real overflow");
}

void TestWrappedLineVerticalCaretMovement()
{
    using Microsoft::WRL::ComPtr;
    using snowdesktop::widget_runtime::
        HostInputVerticalDirection;
    using snowdesktop::widget_runtime::
        ResolveHostInputVerticalCaretPosition;

    ComPtr<IDWriteFactory> factory;
    const HRESULT factoryResult = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_ISOLATED,
        __uuidof(IDWriteFactory), &factory);
    Check(SUCCEEDED(factoryResult) && factory,
        "DirectWrite must be available for wrapped caret navigation");
    if (!factory)
        return;

    ComPtr<IDWriteTextFormat> format;
    factory->CreateTextFormat(L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-CN", &format);
    Check(format != nullptr,
        "wrapped caret navigation needs a text format");
    if (!format)
        return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

    const std::wstring text =
        L"自动换行文本需要使用视觉行移动插入点";
    ComPtr<IDWriteTextLayout> layout;
    factory->CreateTextLayout(text.c_str(),
        static_cast<UINT32>(text.size()), format.Get(),
        48.0f, 10000.0f, &layout);
    DWRITE_TEXT_METRICS textMetrics{};
    if (layout)
        layout->GetMetrics(&textMetrics);
    Check(layout && textMetrics.lineCount > 1,
        "regression text must wrap without explicit newlines");
    if (!layout || textMetrics.lineCount <= 1)
        return;

    const std::size_t initialCursor = 1;
    float initialX = 0.0f, initialY = 0.0f;
    DWRITE_HIT_TEST_METRICS initialHit{};
    Check(SUCCEEDED(layout->HitTestTextPosition(
            static_cast<UINT32>(initialCursor), FALSE,
            &initialX, &initialY, &initialHit)),
        "the initial caret must have a measurable visual position");
    const auto down = ResolveHostInputVerticalCaretPosition(
        layout.Get(), text, initialCursor,
        HostInputVerticalDirection::Down);
    Check(down.has_value(), "Down must resolve a visual caret position");
    if (!down)
        return;
    float downX = 0.0f, downY = 0.0f;
    DWRITE_HIT_TEST_METRICS downHit{};
    Check(SUCCEEDED(layout->HitTestTextPosition(
            static_cast<UINT32>(*down), FALSE, &downX, &downY, &downHit)) &&
            std::abs(downY - (initialY + initialHit.height)) < 0.1f &&
            std::abs(downX - initialX) < 0.1f,
        "Down must retain the horizontal caret position on the adjacent visual line");

    const auto up = ResolveHostInputVerticalCaretPosition(
        layout.Get(), text, *down,
        HostInputVerticalDirection::Up);
    Check(up && *up == initialCursor,
        "Up must return to the original visual position in equal-width wrapped text");

    const auto firstLineUp = ResolveHostInputVerticalCaretPosition(
        layout.Get(), text, 0,
        HostInputVerticalDirection::Up);
    const auto lastLineDown = ResolveHostInputVerticalCaretPosition(
        layout.Get(), text, text.size(),
        HostInputVerticalDirection::Down);
    Check(firstLineUp && *firstLineUp == 0 &&
            lastLineDown && *lastLineDown == text.size(),
        "vertical movement stays at the first and last visual line boundaries");
}
}

int main()
{
    using namespace snowdesktop::text_input;
    const std::wstring clusters = L"a\u0301\U0001f469\u200d\U0001f4bb\U0001f1e8\U0001f1f3Z";
    const auto accentEnd = NextBoundary(clusters, 0);
    const auto emojiEnd = NextBoundary(clusters, accentEnd);
    const auto flagEnd = NextBoundary(clusters, emojiEnd);
    Check(accentEnd == 2 && PreviousBoundary(clusters, accentEnd) == 0,
        "combining marks must move and delete with their base character");
    Check(emojiEnd == 7 && PreviousBoundary(clusters, emojiEnd) == accentEnd,
        "a surrogate-pair ZWJ emoji must remain a single editing unit");
    Check(flagEnd == 11 && NextBoundary(clusters, flagEnd) == clusters.size(),
        "regional indicator pairs must remain a single editing unit");
    Check(NextBoundary(L"\r\nx", 0) == 2 && SnapBoundary(clusters, 4) == accentEnd,
        "CRLF and positions inside a cluster must resolve to stable boundaries");
    History history;
    std::wstring draft = L"draft"; std::size_t cursor = 5, anchor = 0;
    const Snapshot initial{draft, cursor, anchor};
    draft = L"中文"; cursor = anchor = 2;
    history.Record(initial, {draft, cursor, anchor});
    history.Record({draft, cursor, anchor}, {draft, 0, 0});
    Check(history.Undo(draft, cursor, anchor) && draft == L"draft" && cursor == 5 && anchor == 0,
        "one completed input must undo together with its original selection");
    Check(history.Redo(draft, cursor, anchor) && draft == L"中文" && cursor == 2 && anchor == 2,
        "redo must restore the completed input and caret");
    history.Undo(draft, cursor, anchor);
    history.Record({draft, cursor, anchor}, {L"replacement", 11, 11});
    Check(!history.CanRedo(), "a new edit after undo must retire the old redo branch");
    TestUtf8Counting();
    TestBoundedReplacement();
    TestReadOnlyMutationGate();
    TestContextMenuState();
    TestSubmittedInputFocusScope();
    TestDeferredFocusRequest();
    TestCaretVisibilityRequest();
    TestVerticalExtents();
    TestWrappedLineVerticalCaretMovement();
    if (failures != 0)
    {
        std::cerr << failures << " widget text-input rule checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget text-input rule checks passed\n";
    return EXIT_SUCCESS;
}
