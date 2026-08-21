#pragma once

#include <cstdint>
#include <string>

namespace plate_temporal_policy {

inline constexpr int kInitialTextVoteMin = 2;
inline constexpr int kTrackTtlMs = 600;
inline constexpr int kDisplayMissTolerance = 1;
inline constexpr double kIdentitySignatureDiff = 20.0;

inline bool identityChanged(bool stableTextValid,
                            const std::string& stableText,
                            bool candidateTextValid,
                            const std::string& candidateText,
                            double signatureDifference)
{
    return stableTextValid && candidateTextValid &&
            candidateText != stableText &&
            signatureDifference > kIdentitySignatureDiff;
}

inline bool shouldDisplay(int hitCount,
                          int missedInferenceCount,
                          std::int64_t ageMs,
                          bool stableTextValid)
{
    return hitCount >= kInitialTextVoteMin &&
            missedInferenceCount <= kDisplayMissTolerance &&
            ageMs >= 0 && ageMs <= kTrackTtlMs && stableTextValid;
}

}  // namespace plate_temporal_policy
