#pragma once

#include "npu/npu_result_types.h"
#include "npu/violation_rule_config.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class PedestrianViolationEngine
{
public:
    PedestrianViolationEngine();

    void reset();
    void setConfig(const ViolationRuleConfig& config);
    ViolationRuleConfig config() const;

    void process(const std::uint8_t* rgb565,
                 std::size_t bytes,
                 int width,
                 int height,
                 std::uint64_t frameId,
                 std::uint64_t steadyTimeMs,
                 std::uint64_t wallClockMs,
                 std::vector<NpuBox>* pedestrians,
                 NpuFrameResult* result);

private:
    struct LightObservation
    {
        NpuTrafficLightState state = NpuTrafficLightState::Unknown;
        float redScore = 0.0f;
        float greenScore = 0.0f;
    };

    struct Track
    {
        int id = -1;
        NpuBox box;
        std::uint64_t lastSeenMs = 0;
        std::uint64_t redInsideStartMs = 0;
        std::uint64_t entryRedEpoch = 0;
        std::uint64_t latchedRedEpoch = 0;
        bool insideInitialized = false;
        bool wasInsideCrosswalk = false;
        bool enteredDuringRed = false;
        bool activeViolation = false;
    };

    LightObservation detectLight(const std::uint8_t* rgb565,
                                 std::size_t bytes,
                                 int width,
                                 int height) const;
    NpuTrafficLightState stabilizeLight(NpuTrafficLightState raw,
                                        std::uint64_t nowMs);
    bool footInsideCrosswalk(const NpuBox& box, int width, int height) const;
    float trackIou(const NpuBox& a, const NpuBox& b) const;
    float trackCenterDistanceNormalized(const NpuBox& a, const NpuBox& b) const;
    float trackFootDistanceNormalized(const NpuBox& a, const NpuBox& b) const;
    void expireTracks(std::uint64_t nowMs);
    void clearRedRuntime(Track* track, bool clearActive);
    void clearAllCurrentViolations();

private:
    ViolationRuleConfig m_config;
    std::uint64_t m_appliedRevision = 0;
    std::deque<NpuTrafficLightState> m_lightVotes;
    NpuTrafficLightState m_stableLight = NpuTrafficLightState::Unknown;
    std::uint64_t m_lastKnownLightMs = 0;
    std::uint64_t m_redEpoch = 0;
    bool m_redRuleWasActive = false;
    std::vector<Track> m_tracks;
    int m_nextTrackId = 1;
    std::uint64_t m_nextEventId = 1;
    bool m_hasRecentEvent = false;
    NpuViolationEvent m_recentEvent;
};
