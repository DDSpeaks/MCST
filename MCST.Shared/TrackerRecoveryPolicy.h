#pragma once

#include <cstddef>
#include <cstdint>

namespace mcst
{
    enum class TrackerRecoveryTier
    {
        Fast,
        Expanded,
        Wide
    };

    struct TrackerRecoveryPolicyInput
    {
        unsigned int failureStreak = 0;
        std::uint64_t nowTickMs = 0;
        std::uint64_t lastExpandedTickMs = 0;
        std::uint64_t lastWideTickMs = 0;
    };

    struct TrackerRecoveryPolicyDecision
    {
        TrackerRecoveryTier tier = TrackerRecoveryTier::Fast;
        const char* name = "fast";
        std::uint64_t timeBudgetMs = 900;
        std::size_t byteBudget = 16ull * 1024ull * 1024ull;
        bool stampExpandedTick = false;
        bool stampWideTick = false;
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

    /**
     * Pure recovery-tier decision shared by the live Bridge and LogicTests.
     * The caller owns state mutation and records the selected heavy-tier tick.
     */
    inline TrackerRecoveryPolicyDecision SelectTrackerRecoveryPolicy(
        const TrackerRecoveryPolicyInput& input) noexcept
    {
        constexpr std::uint64_t expandedCooldownMs = 2ull * 60ull * 1000ull;
        constexpr std::uint64_t wideCooldownMs = 5ull * 60ull * 1000ull;

        if (input.failureStreak >= 12 && TrackerRecoveryCooldownElapsed(
                input.nowTickMs, input.lastWideTickMs, wideCooldownMs))
        {
            TrackerRecoveryPolicyDecision decision;
            decision.tier = TrackerRecoveryTier::Wide;
            decision.name = "wide";
            decision.timeBudgetMs = 2800;
            decision.byteBudget = 128ull * 1024ull * 1024ull;
            decision.stampWideTick = true;
            return decision;
        }

        if (input.failureStreak >= 4 && TrackerRecoveryCooldownElapsed(
                input.nowTickMs, input.lastExpandedTickMs, expandedCooldownMs))
        {
            TrackerRecoveryPolicyDecision decision;
            decision.tier = TrackerRecoveryTier::Expanded;
            decision.name = "expanded";
            decision.timeBudgetMs = 1800;
            decision.byteBudget = 64ull * 1024ull * 1024ull;
            decision.stampExpandedTick = true;
            return decision;
        }

        return {};
    }
}
