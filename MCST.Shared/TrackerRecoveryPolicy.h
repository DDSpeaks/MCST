#pragma once

#include <cstddef>
#include <cstdint>

namespace mcst
{
    struct TrackerRecoveryPolicyInput
    {
        unsigned int failureStreak = 0;
    };

    struct TrackerRecoveryPolicyDecision
    {
        const char* name = "hint-targeted-full";
        std::uint64_t retryCooldownMs = 30ull * 1000ull;
        std::uint64_t targetedTimeBudgetMs = 100;
        std::size_t targetedByteBudget = 8ull * 1024ull * 1024ull;
        std::uint64_t processWideTimeBudgetMs = 3000;
        std::size_t processWideByteBudget = 512ull * 1024ull * 1024ull;
    };

    inline bool TrackerRecoveryCooldownElapsed(
        std::uint64_t nowTickMs,
        std::uint64_t previousTickMs,
        std::uint64_t cooldownMs) noexcept
    {
        return previousTickMs == 0 ||
            nowTickMs < previousTickMs ||
            nowTickMs - previousTickMs >= cooldownMs;
    }

    /** Pure retry/load policy shared by the live Bridge and LogicTests. */
    inline TrackerRecoveryPolicyDecision SelectTrackerRecoveryPolicy(
        const TrackerRecoveryPolicyInput& input) noexcept
    {
        TrackerRecoveryPolicyDecision decision;
        if (input.failureStreak >= 10)
            decision.retryCooldownMs = 5ull * 60ull * 1000ull;
        else if (input.failureStreak >= 3)
            decision.retryCooldownMs = 60ull * 1000ull;
        return decision;
    }
}
