#include "npu/vehicle_wrong_way_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

float boxAnchorX(const NpuBox& box) { return box.x + box.w * 0.5f; }
float boxAnchorY(const NpuBox& box) { return box.y + box.h; }

float boxDiagonal(const NpuBox& box)
{
    return std::max(1.0f, std::sqrt(box.w * box.w + box.h * box.h));
}

float boxIou(const NpuBox& a, const NpuBox& b)
{
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.w, b.x + b.w);
    const float y2 = std::min(a.y + a.h, b.y + b.h);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float unionArea = std::max(1.0f, a.w * a.h + b.w * b.h - intersection);
    return intersection / unionArea;
}

bool pointInQuad(float x,
                 float y,
                 const std::array<VehicleDirectionPointF, 4>& quad)
{
    bool inside = false;
    for (int i = 0, j = 3; i < 4; j = i++) {
        const VehicleDirectionPointF& pi = quad[static_cast<std::size_t>(i)];
        const VehicleDirectionPointF& pj = quad[static_cast<std::size_t>(j)];
        const bool crosses = ((pi.y > y) != (pj.y > y)) &&
                (x < (pj.x - pi.x) * (y - pi.y) /
                 ((pj.y - pi.y) + ((pj.y == pi.y) ? 1e-9f : 0.0f)) + pi.x);
        if (crosses) inside = !inside;
    }
    return inside;
}

float median(std::vector<float> values)
{
    if (values.empty()) return 1.0f;
    const std::size_t index = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return std::max(1.0f, values[index]);
}

struct MotionEstimate
{
    float normalizedMotion = 0.0f;
    float directionCosine = 0.0f;
    std::uint64_t spanMs = 0;
};

bool estimateMotion(const VehicleWrongWayEngine::Track& track,
                    const VehicleDirectionConfig& config,
                    MotionEstimate* estimate)
{
    if (!estimate || track.laneId < 0 ||
        track.laneId >= static_cast<int>(config.lanes.size()) ||
        track.history.size() < 3) return false;

    const std::uint64_t endMs = track.history.back().timeMs;
    const std::uint64_t beginLimit = endMs > static_cast<std::uint64_t>(config.directionWindowMs)
            ? endMs - static_cast<std::uint64_t>(config.directionWindowMs) : 0;
    std::size_t begin = 0;
    while (begin + 2 < track.history.size() &&
           track.history[begin].timeMs < beginLimit) ++begin;
    const std::size_t count = track.history.size() - begin;
    if (count < 3) return false;

    double meanT = 0.0;
    double meanX = 0.0;
    double meanY = 0.0;
    std::vector<float> diagonals;
    diagonals.reserve(count);
    for (std::size_t i = begin; i < track.history.size(); ++i) {
        const auto& sample = track.history[i];
        const double t = static_cast<double>(sample.timeMs - track.history[begin].timeMs);
        meanT += t;
        meanX += sample.x;
        meanY += sample.y;
        diagonals.push_back(sample.diagonal);
    }
    meanT /= count;
    meanX /= count;
    meanY /= count;

    double denominator = 0.0;
    double numeratorX = 0.0;
    double numeratorY = 0.0;
    for (std::size_t i = begin; i < track.history.size(); ++i) {
        const auto& sample = track.history[i];
        const double t = static_cast<double>(sample.timeMs - track.history[begin].timeMs);
        const double dt = t - meanT;
        denominator += dt * dt;
        numeratorX += dt * (sample.x - meanX);
        numeratorY += dt * (sample.y - meanY);
    }
    if (denominator < 1e-6) return false;
    const std::uint64_t spanMs = endMs - track.history[begin].timeMs;
    const float dx = static_cast<float>(numeratorX / denominator * spanMs);
    const float dy = static_cast<float>(numeratorY / denominator * spanMs);
    const float norm = std::sqrt(dx * dx + dy * dy);
    const VehicleDirectionPointF& direction = config.lanes[static_cast<std::size_t>(track.laneId)].direction;
    estimate->spanMs = spanMs;
    estimate->normalizedMotion = norm / median(diagonals);
    estimate->directionCosine = norm > 1e-6f
            ? (dx * direction.x + dy * direction.y) / norm : 0.0f;
    return true;
}

const char* classLabel(int classId)
{
    switch (classId) {
    case 0: return "car";
    case 1: return "bus";
    case 2: return "van";
    case 3: return "truck";
    default: return "vehicle";
    }
}

} // namespace

void VehicleWrongWayEngine::setConfig(const VehicleDirectionConfig& config)
{
    m_config = config;
}

void VehicleWrongWayEngine::reset()
{
    m_tracks.clear();
    m_nextTrackId = 1;
    m_nextEventId = 1;
    m_eventCount = 0;
    m_hasRecentEvent = false;
}

void VehicleWrongWayEngine::resetForContext(std::uint64_t configRevision,
                                            int sourceChannel,
                                            std::uint64_t sourceRevision)
{
    if (m_contextConfigRevision != configRevision ||
        m_contextSourceChannel != sourceChannel ||
        m_contextSourceRevision != sourceRevision) {
        reset();
        m_contextConfigRevision = configRevision;
        m_contextSourceChannel = sourceChannel;
        m_contextSourceRevision = sourceRevision;
    }
}

int VehicleWrongWayEngine::laneForPoint(float x, float y, int preferredLane) const
{
    if (preferredLane >= 0 && preferredLane < static_cast<int>(m_config.lanes.size()) &&
        pointInQuad(x, y, m_config.lanes[static_cast<std::size_t>(preferredLane)].quad)) {
        return preferredLane;
    }
    for (std::size_t i = 0; i < m_config.lanes.size(); ++i) {
        if (pointInQuad(x, y, m_config.lanes[i].quad)) return static_cast<int>(i);
    }
    return -1;
}

void VehicleWrongWayEngine::process(std::uint64_t frameId,
                                    std::uint64_t monotonicMs,
                                    std::uint64_t wallClockMs,
                                    int frameWidth,
                                    int frameHeight,
                                    int sourceChannel,
                                    std::uint64_t sourceRevision,
                                    std::vector<NpuBox>* vehicles,
                                    NpuFrameResult* result)
{
    if (!vehicles || !result || frameWidth <= 0 || frameHeight <= 0) return;
    resetForContext(m_config.revision, sourceChannel, sourceRevision);
    result->wrongWayConfig = m_config;
    result->wrongWayRegionsConfigured = m_config.regionsValid();
    if (!m_config.regionsValid()) {
        // 无车道配置时只关闭CPU规则，不能吞掉车辆模型的原始检测结果。
        return;
    }

    struct Association { int track; int observation; float cost; };
    std::vector<int> observationLane(vehicles->size(), -1);
    for (std::size_t oi = 0; oi < vehicles->size(); ++oi) {
        const float anchorX = boxAnchorX((*vehicles)[oi]);
        const float anchorY = boxAnchorY((*vehicles)[oi]);
        observationLane[oi] = laneForPoint(anchorX / frameWidth,
                                           anchorY / frameHeight,
                                           -1);
    }

    std::vector<Association> associations;
    for (std::size_t ti = 0; ti < m_tracks.size(); ++ti) {
        const Track& track = m_tracks[ti];
        const std::uint64_t dtMs = monotonicMs > track.lastSeenMs
                ? monotonicMs - track.lastSeenMs : 1;
        if (dtMs > static_cast<std::uint64_t>(m_config.trackTtlMs)) continue;
        const float predictedX = track.anchorX + track.velocityX * dtMs;
        const float predictedY = track.anchorY + track.velocityY * dtMs;
        for (std::size_t oi = 0; oi < vehicles->size(); ++oi) {
            const NpuBox& observation = (*vehicles)[oi];
            const float dx = boxAnchorX(observation) - predictedX;
            const float dy = boxAnchorY(observation) - predictedY;
            const float scale = std::max(boxDiagonal(track.raw), boxDiagonal(observation));
            const float distance = std::sqrt(dx * dx + dy * dy) / scale;
            const float iou = boxIou(track.raw, observation);
            if (distance > 1.4f && iou < 0.05f) continue;
            const bool crossLane = track.laneId >= 0 && observationLane[oi] >= 0 &&
                    track.laneId != observationLane[oi];
            Association item;
            item.track = static_cast<int>(ti);
            item.observation = static_cast<int>(oi);
            item.cost = 0.55f * std::min(2.0f, distance) + 0.35f * (1.0f - iou) +
                    0.10f * (track.classId == observation.classId ? 0.0f : 1.0f) +
                    (crossLane ? 0.35f : 0.0f);
            associations.push_back(item);
        }
    }
    std::sort(associations.begin(), associations.end(),
              [](const Association& a, const Association& b) { return a.cost < b.cost; });
    std::vector<bool> trackUsed(m_tracks.size(), false);
    std::vector<bool> observationUsed(vehicles->size(), false);
    std::vector<int> observationTrack(vehicles->size(), -1);
    for (const Association& item : associations) {
        if (trackUsed[static_cast<std::size_t>(item.track)] ||
            observationUsed[static_cast<std::size_t>(item.observation)]) continue;
        trackUsed[static_cast<std::size_t>(item.track)] = true;
        observationUsed[static_cast<std::size_t>(item.observation)] = true;
        observationTrack[static_cast<std::size_t>(item.observation)] = item.track;
    }

    for (std::size_t oi = 0; oi < vehicles->size(); ++oi) {
        NpuBox& observation = (*vehicles)[oi];
        const float anchorX = boxAnchorX(observation);
        const float anchorY = boxAnchorY(observation);
        int trackIndex = observationTrack[oi];
        if (trackIndex < 0) {
            Track track;
            track.id = m_nextTrackId++;
            track.classId = observation.classId;
            track.classVotes[observation.classId] = 1;
            track.raw = observation;
            track.display = observation;
            track.anchorX = anchorX;
            track.anchorY = anchorY;
            track.firstSeenMs = monotonicMs;
            track.lastSeenMs = monotonicMs;
            track.laneId = observationLane[oi];
            track.history.push_back({monotonicMs, anchorX, anchorY, boxDiagonal(observation)});
            m_tracks.push_back(track);
            observationTrack[oi] = static_cast<int>(m_tracks.size() - 1);
            continue;
        }

        Track& track = m_tracks[static_cast<std::size_t>(trackIndex)];
        const std::uint64_t dtMs64 = monotonicMs > track.lastSeenMs
                ? monotonicMs - track.lastSeenMs : 1;
        const float dtMs = static_cast<float>(std::max<std::uint64_t>(1, dtMs64));
        const float measuredVx = (anchorX - track.anchorX) / dtMs;
        const float measuredVy = (anchorY - track.anchorY) / dtMs;
        track.velocityX = 0.6f * track.velocityX + 0.4f * measuredVx;
        track.velocityY = 0.6f * track.velocityY + 0.4f * measuredVy;
        track.anchorX = anchorX;
        track.anchorY = anchorY;
        track.raw = observation;
        const float alpha = 1.0f - std::pow(1.0f - m_config.boxEma,
                                             std::max(0.25f, dtMs / 66.7f));
        track.display.x += alpha * (observation.x - track.display.x);
        track.display.y += alpha * (observation.y - track.display.y);
        track.display.w += alpha * (observation.w - track.display.w);
        track.display.h += alpha * (observation.h - track.display.h);
        track.display.score = observation.score;
        track.lastSeenMs = monotonicMs;

        ++track.classVotes[observation.classId];
        int bestVotes = -1;
        for (const auto& vote : track.classVotes) {
            if (vote.second > bestVotes) {
                track.classId = vote.first;
                bestVotes = vote.second;
            }
        }

        const int observedLane = observationLane[oi];
        if (observedLane == track.laneId) {
            track.pendingLaneId = -1;
            track.pendingLaneCount = 0;
        } else if (track.pendingLaneId != observedLane) {
            track.pendingLaneId = observedLane;
            track.pendingLaneCount = 1;
        } else if (++track.pendingLaneCount >= 2) {
            track.laneId = observedLane;
            track.pendingLaneId = -1;
            track.pendingLaneCount = 0;
            track.reverseEvidenceMs = 0;
            track.clearEvidenceMs = 0;
            track.state = NpuVehicleDirectionState::Unknown;
            track.history.clear();
        }

        if (track.laneId >= 0) {
            track.history.push_back({monotonicMs, anchorX, anchorY, boxDiagonal(observation)});
            const std::uint64_t historyLimit = static_cast<std::uint64_t>(
                        std::max(m_config.directionWindowMs, m_config.stationaryHoldMs) + 500);
            while (track.history.size() > 3 &&
                   monotonicMs - track.history.front().timeMs > historyLimit) {
                track.history.pop_front();
            }
            MotionEstimate motion;
            if (estimateMotion(track, m_config, &motion)) {
                track.normalizedMotion = motion.normalizedMotion;
                track.directionCosine = motion.directionCosine;
                const bool windowReady = motion.spanMs >=
                        static_cast<std::uint64_t>(m_config.directionWindowMs * 2 / 3);
                const bool moving = windowReady && motion.normalizedMotion >= m_config.motionThreshold;
                const bool reverse = moving && motion.directionCosine <= -m_config.directionCosine;
                const bool correct = moving && motion.directionCosine >= m_config.directionCosine;
                const int evidenceMs = static_cast<int>(std::min<std::uint64_t>(dtMs64, 250));
                if (reverse) {
                    track.reverseEvidenceMs += evidenceMs;
                    track.clearEvidenceMs = 0;
                    if (track.reverseEvidenceMs >= m_config.wrongWayHoldMs &&
                        track.state != NpuVehicleDirectionState::WrongWay) {
                        track.state = NpuVehicleDirectionState::WrongWay;
                        NpuWrongWayEvent event;
                        event.eventId = m_nextEventId++;
                        event.frameId = frameId;
                        event.wallClockMs = wallClockMs;
                        event.trackId = track.id;
                        event.classId = track.classId;
                        event.laneId = track.laneId;
                        event.normalizedMotion = track.normalizedMotion;
                        event.directionCosine = track.directionCosine;
                        m_recentEvent = event;
                        m_hasRecentEvent = true;
                        ++m_eventCount;
                    }
                } else if (correct) {
                    track.reverseEvidenceMs = 0;
                    track.clearEvidenceMs += evidenceMs;
                    if (track.state == NpuVehicleDirectionState::WrongWay) {
                        if (track.clearEvidenceMs >= m_config.wrongWayClearMs)
                            track.state = NpuVehicleDirectionState::Normal;
                    } else {
                        track.state = NpuVehicleDirectionState::Normal;
                    }
                } else {
                    // 中性/停车证据不能跨窗口累加为逆行或恢复，避免非连续证据误触发。
                    track.reverseEvidenceMs = 0;
                    track.clearEvidenceMs = 0;
                    if (track.state == NpuVehicleDirectionState::Unknown &&
                        monotonicMs - track.firstSeenMs >=
                            static_cast<std::uint64_t>(m_config.stationaryHoldMs) &&
                        motion.normalizedMotion < m_config.motionThreshold) {
                        track.state = NpuVehicleDirectionState::Stopped;
                    }
                }
            }
        }
    }

    std::vector<NpuBox> stable;
    stable.reserve(vehicles->size());
    int activeWrong = 0;
    for (std::size_t oi = 0; oi < vehicles->size(); ++oi) {
        const int trackIndex = observationTrack[oi];
        if (trackIndex < 0 || trackIndex >= static_cast<int>(m_tracks.size())) continue;
        const Track& track = m_tracks[static_cast<std::size_t>(trackIndex)];
        NpuBox box = track.display;
        box.classId = track.classId;
        box.label = classLabel(track.classId);
        box.trackId = track.id;
        box.laneId = track.laneId;
        box.normalizedMotion = track.normalizedMotion;
        box.directionCosine = track.directionCosine;
        box.vehicleDirectionState = track.state;
        if (track.state == NpuVehicleDirectionState::WrongWay) ++activeWrong;
        stable.push_back(box);
    }
    vehicles->swap(stable);

    for (std::size_t i = m_tracks.size(); i > 0; --i) {
        if (monotonicMs > m_tracks[i - 1].lastSeenMs &&
            monotonicMs - m_tracks[i - 1].lastSeenMs >
                static_cast<std::uint64_t>(m_config.trackTtlMs)) {
            m_tracks.erase(m_tracks.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }
    }

    result->activeVehicleTrackCount = static_cast<int>(m_tracks.size());
    result->activeWrongWayCount = activeWrong;
    result->wrongWayEventCount = m_eventCount;
    result->hasRecentWrongWay = m_hasRecentEvent;
    if (m_hasRecentEvent) result->recentWrongWay = m_recentEvent;
}
