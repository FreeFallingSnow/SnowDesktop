#pragma once
#include "operation_feedback.h"
namespace snowdesktop::operation_feedback
{
enum class RecoveryChoice { Cancel, Retry, Continue };
// Only for optional prerequisites. Actual failed operations cannot be forced.
RecoveryChoice ChooseRecovery(HWND owner, const Failure& failure, const wchar_t* continueLabel);
}
