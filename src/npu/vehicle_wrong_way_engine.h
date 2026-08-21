#pragma once

#include "npu/npu_result_types.h"
#include "npu/vehicle_direction_config.h"

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

class VehicleWrongWayEngine
{
public:
    void setConfig(const VehicleDirectionConfig& config);
    void reset();

    void process(std::uint64_t frameId,
                 std::uint64_t monotonicMs,
                 std::uint64_t wallClockMs,
                 int frameWidth,
                 int frameHeight,
                 int sourceChannel,
                 std::uint64_t sourceRevision,
                 std::vector<NpuBox>* vehicles,
                 NpuFrameResult* result);

public: // 仅供实现文件中的无状态运动估计辅助函数访问。
    struct Sample
    {
        std::uint64_t timeMs = 0;
        float x = 0.0f;
        float y = 0.0f;
        float diagonal = 1.0f;
    };

    struct Track
    {
        int id = -1;
        int classId = -1;
        std::map<int, int> classVotes;
        NpuBox raw;
        NpuBox display;
        float anchorX = 0.0f;
        float anchorY = 0.0f;
        float velocityX = 0.0f;
        float velocityY = 0.0f;
        std::uint64_t firstSeenMs = 0;
        std::uint64_t lastSeenMs = 0;
        int laneId = -1;
        int overviewTileId = -1;
        int pendingLaneId = -1;
        int pendingLaneCount = 0;
        int reverseEvidenceMs = 0;
        int clearEvidenceMs = 0;
        float normalizedMotion = 0.0f;
        float directionCosine = 0.0f;
        NpuVehicleDirectionState state = NpuVehicleDirectionState::Unknown;
        std::deque<Sample> history;
    };

private:
    int laneForPoint(float x, float y, int preferredLane) const;
    void resetForContext(std::uint64_t configRevision,
                         int sourceChannel,
                         std::uint64_t sourceRevision);

    VehicleDirectionConfig m_config;
    std::vector<Track> m_tracks;
    int m_nextTrackId = 1;
    std::uint64_t m_nextEventId = 1;
    std::uint64_t m_eventCount = 0;
    bool m_hasRecentEvent = false;
    NpuWrongWayEvent m_recentEvent;
    std::uint64_t m_contextConfigRevision = 0;
    int m_contextSourceChannel = -1;
    std::uint64_t m_contextSourceRevision = 0;
};
