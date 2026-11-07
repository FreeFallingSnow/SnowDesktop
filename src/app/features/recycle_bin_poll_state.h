#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>

struct RecycleBinPollState {
    std::atomic<int64_t> hasItems{ -1 };   ///< -1 未知，0 空，1 非空
    std::atomic<bool> queryInFlight{ false };
    std::atomic<DWORD> lastQueryDurationMs{ 0 };
};
