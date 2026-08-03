#pragma once

#include <cstddef>

namespace prosched {
    constexpr int kMaxProcesses = 10;           // limit for scheduler-test
    constexpr int kNumPrintInstructions = 100;  // default prints per process
    constexpr int kTickDurationMs = 0;         // duration of a single tick in milliseconds
    constexpr long kMinProcessMemoryBytes = 64;    // 2^6
    constexpr long kMaxProcessMemoryBytes = 65536; // 2^16
    constexpr std::size_t kMinUserInstructions = 1;
    constexpr std::size_t kMaxUserInstructions = 50;
    constexpr std::string kMemoryUnit = "KB";
}