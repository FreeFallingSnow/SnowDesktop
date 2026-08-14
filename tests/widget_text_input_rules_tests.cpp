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
    using snowdesktop::widget_runtime::Utf8BytesForHostText;
    Check(Utf8BytesForHostText(L"note") == 4,
        "ASCII host text uses one byte per code point");
    Check(Utf8BytesForHostText(L"便笺") == 6,
        "CJK host text uses three bytes per code point");
    Check(Utf8BytesForHostText(L"\U0001F4DD") == 4,
        "supplementary characters use one four-byte UTF-8 sequence");
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
        "a zero limit preserves the API v1 unlimited compatibility path");
}
}

int main()
{
    TestUtf8Counting();
    TestBoundedReplacement();
    if (failures != 0)
    {
        std::cerr << failures << " widget text-input rule checks failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "widget text-input rule checks passed\n";
    return EXIT_SUCCESS;
}
