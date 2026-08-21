#include "npu/pedestrian_violation_engine.h"
#include "npu/pedestrian_rule_logic.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace {

int clampInt(int value, int low, int high)
{
    return std::max(low, std::min(high, value));
}

float smoothValue(float previous, float current, float alpha)
{
    return previous * alpha + current * (1.0f - alpha);
}

} // namespace

PedestrianViolationEngine::PedestrianViolationEngine()
    : m_config(sanitizeViolationRuleConfig(ViolationRuleConfig()))
{
}

void PedestrianViolationEngine::reset()
{
    m_lightVotes.clear();
    m_stableLight = NpuTrafficLightState::Unknown;
    m_lastKnownLightMs = 0;
    m_tracks.clear();
    m_nextTrackId = 1;
    m_nextEventId = 1;
    m_hasRecentEvent = false;
    m_recentEvent = NpuViolationEvent();
}

void PedestrianViolationEngine::setConfig(const ViolationRuleConfig& config)
{
    const ViolationRuleConfig clean = sanitizeViolationRuleConfig(config);
    if (clean.revision != m_appliedRevision) {
        // 区域、灯色模式或行人阈值变化后清空当前投票和轨迹状态，
        // 但保留本次运行期间已经产生的历史事件计数。
        m_lightVotes.clear();
        m_stableLight = NpuTrafficLightState::Unknown;
        m_lastKnownLightMs = 0;
        m_tracks.clear();
        m_nextTrackId = 1;
        m_appliedRevision = clean.revision;
    }
    m_config = clean;
}

ViolationRuleConfig PedestrianViolationEngine::config() const
{
    return m_config;
}

PedestrianViolationEngine::LightObservation
PedestrianViolationEngine::detectLight(const std::uint8_t* rgb565,
                                        std::size_t bytes,
                                        int width,
                                        int height) const
{
    LightObservation observation;
    if (m_config.lightMode == ViolationLightMode::ForceRed) {
        observation.state = NpuTrafficLightState::Red;
        observation.redScore = 1.0f;
        return observation;
    }
    if (m_config.lightMode == ViolationLightMode::ForceGreen) {
        observation.state = NpuTrafficLightState::Green;
        observation.greenScore = 1.0f;
        return observation;
    }
    if (!m_config.lightRoiValid || !rgb565 || width <= 0 || height <= 0) {
        return observation;
    }

    const std::size_t required = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 2u;
    if (bytes < required) {
        return observation;
    }

    const int x = clampInt(static_cast<int>(std::lround(m_config.lightX * width)),
                           0, width - 1);
    const int y = clampInt(static_cast<int>(std::lround(m_config.lightY * height)),
                           0, height - 1);
    const int w = clampInt(static_cast<int>(std::lround(m_config.lightW * width)),
                           1, width - x);
    const int h = clampInt(static_cast<int>(std::lround(m_config.lightH * height)),
                           1, height - y);
    if (w < 2 || h < 2) {
        return observation;
    }

    cv::Mat bgr(h, w, CV_8UC3);
    for (int row = 0; row < h; ++row) {
        const std::uint8_t* source = rgb565 +
                (static_cast<std::size_t>(y + row) * width + x) * 2u;
        std::uint8_t* destination = bgr.ptr<std::uint8_t>(row);
        for (int col = 0; col < w; ++col) {
            const std::uint16_t pixel = static_cast<std::uint16_t>(source[col * 2]) |
                    (static_cast<std::uint16_t>(source[col * 2 + 1]) << 8);
            const std::uint8_t red = static_cast<std::uint8_t>(
                        ((pixel >> 11) & 0x1f) * 255 / 31);
            const std::uint8_t green = static_cast<std::uint8_t>(
                        ((pixel >> 5) & 0x3f) * 255 / 63);
            const std::uint8_t blue = static_cast<std::uint8_t>(
                        (pixel & 0x1f) * 255 / 31);
            destination[col * 3 + 0] = blue;
            destination[col * 3 + 1] = green;
            destination[col * 3 + 2] = red;
        }
    }

    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    cv::Mat redLow;
    cv::Mat redHigh;
    cv::Mat greenMask;
    cv::inRange(hsv, cv::Scalar(0, 100, 100),
                cv::Scalar(10, 255, 255), redLow);
    cv::inRange(hsv, cv::Scalar(160, 100, 100),
                cv::Scalar(180, 255, 255), redHigh);
    cv::inRange(hsv, cv::Scalar(35, 80, 80),
                cv::Scalar(90, 255, 255), greenMask);

    const int area = w * h;
    const int redCount = cv::countNonZero(redLow) + cv::countNonZero(redHigh);
    const int greenCount = cv::countNonZero(greenMask);
    observation.redScore = area > 0
            ? static_cast<float>(redCount) / static_cast<float>(area) : 0.0f;
    observation.greenScore = area > 0
            ? static_cast<float>(greenCount) / static_cast<float>(area) : 0.0f;

    const int minimumCount = std::max(10, area / 30);
    if (redCount >= minimumCount &&
        redCount > static_cast<int>(greenCount * 1.2f)) {
        observation.state = NpuTrafficLightState::Red;
    } else if (greenCount >= minimumCount &&
               greenCount > static_cast<int>(redCount * 1.2f)) {
        observation.state = NpuTrafficLightState::Green;
    }
    return observation;
}

NpuTrafficLightState PedestrianViolationEngine::stabilizeLight(
        NpuTrafficLightState raw,
        std::uint64_t nowMs)
{
    if (m_config.lightMode == ViolationLightMode::ForceRed ||
        m_config.lightMode == ViolationLightMode::ForceGreen) {
        m_lightVotes.clear();
        m_stableLight = raw;
        m_lastKnownLightMs = nowMs;
        return m_stableLight;
    }

    m_lightVotes.push_back(raw);
    while (static_cast<int>(m_lightVotes.size()) > m_config.lightVoteWindow) {
        m_lightVotes.pop_front();
    }

    int red = 0;
    int green = 0;
    for (NpuTrafficLightState state : m_lightVotes) {
        red += state == NpuTrafficLightState::Red ? 1 : 0;
        green += state == NpuTrafficLightState::Green ? 1 : 0;
    }

    NpuTrafficLightState voted = NpuTrafficLightState::Unknown;
    if (red >= m_config.lightVoteMin && red > green) {
        voted = NpuTrafficLightState::Red;
    } else if (green >= m_config.lightVoteMin && green > red) {
        voted = NpuTrafficLightState::Green;
    }

    if (voted != NpuTrafficLightState::Unknown) {
        m_stableLight = voted;
        m_lastKnownLightMs = nowMs;
    } else if (pedestrianLightKnown(raw) &&
               pedestrianLightKnown(m_stableLight) &&
               raw != m_stableLight) {
        // 灯态切换尚未达到投票门限时进入未知态，不能沿用旧灯态继续
        // 产生违法或把一次绿色误识别当成立即合法。
        return NpuTrafficLightState::Unknown;
    } else if (m_stableLight != NpuTrafficLightState::Unknown &&
               nowMs > m_lastKnownLightMs +
                       static_cast<std::uint64_t>(m_config.unknownHoldMs)) {
        m_stableLight = NpuTrafficLightState::Unknown;
    }
    return m_stableLight;
}

float PedestrianViolationEngine::footCrosswalkSignedDistance(
        const NpuBox& box,
        int width,
        int height) const
{
    if (!m_config.crosswalkValid || width <= 0 || height <= 0) {
        return -1000000.0f;
    }

    std::vector<cv::Point2f> polygon;
    polygon.reserve(4);
    for (const ViolationPointF& point : m_config.crosswalk) {
        polygon.emplace_back(point.x * width, point.y * height);
    }

    const cv::Point2f foot(box.x + box.w * 0.5f, box.y + box.h);
    return static_cast<float>(cv::pointPolygonTest(polygon, foot, true));
}

float PedestrianViolationEngine::trackIou(const NpuBox& a, const NpuBox& b) const
{
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.w, b.x + b.w);
    const float bottom = std::min(a.y + a.h, b.y + b.h);
    const float intersection = std::max(0.0f, right - left) *
                               std::max(0.0f, bottom - top);
    const float unionArea = a.w * a.h + b.w * b.h - intersection;
    return unionArea > 0.0f ? intersection / unionArea : 0.0f;
}

float PedestrianViolationEngine::trackCenterDistanceNormalized(
        const NpuBox& a, const NpuBox& b) const
{
    const float ax = a.x + a.w * 0.5f;
    const float ay = a.y + a.h * 0.5f;
    const float bx = b.x + b.w * 0.5f;
    const float by = b.y + b.h * 0.5f;
    const float dx = ax - bx;
    const float dy = ay - by;
    const float diagonal = std::sqrt(std::max(1.0f, a.w * a.w + a.h * a.h));
    return std::sqrt(dx * dx + dy * dy) / diagonal;
}

float PedestrianViolationEngine::trackFootDistanceNormalized(
        const NpuBox& a, const NpuBox& b) const
{
    const float ax = a.x + a.w * 0.5f;
    const float ay = a.y + a.h;
    const float bx = b.x + b.w * 0.5f;
    const float by = b.y + b.h;
    const float dx = ax - bx;
    const float dy = ay - by;
    const float scale = std::max(1.0f, a.h);
    return std::sqrt(dx * dx + dy * dy) / scale;
}

void PedestrianViolationEngine::expireTracks(std::uint64_t nowMs)
{
    m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                                  [&](const Track& track) {
        return nowMs > track.lastSeenMs +
                static_cast<std::uint64_t>(m_config.trackTtlMs);
    }), m_tracks.end());
}

void PedestrianViolationEngine::clearIllegalRuntime(Track* track,
                                                    bool clearEventLatch)
{
    if (!track) {
        return;
    }
    track->illegalStartMs = 0;
    track->illegalTiming = false;
    track->activeViolation = false;
    track->violationReason = NpuPedestrianViolationReason::None;
    if (clearEventLatch) {
        track->eventLatched = false;
    }
}

void PedestrianViolationEngine::process(const std::uint8_t* rgb565,
                                         std::size_t bytes,
                                         int width,
                                         int height,
                                         std::uint64_t frameId,
                                         std::uint64_t steadyTimeMs,
                                         std::uint64_t wallClockMs,
                                         std::vector<NpuBox>* pedestrians,
                                         NpuFrameResult* result)
{
    if (!pedestrians || !result) {
        return;
    }

    result->violationConfig = m_config;
    result->violationRegionsConfigured = m_config.regionsValid();

    const LightObservation observation = detectLight(rgb565, bytes, width, height);
    result->rawTrafficLight = observation.state;
    result->trafficLightRedScore = observation.redScore;
    result->trafficLightGreenScore = observation.greenScore;

    result->trafficLight = stabilizeLight(observation.state, steadyTimeMs);
    result->rawPedestrianCount = static_cast<int>(pedestrians->size());
    result->pedestrianGeometryRejectedCount = 0;
    result->pedestrianConfidenceRejectedCount = 0;
    result->pedestrianUnconfirmedCount = 0;

    expireTracks(steadyTimeMs);
    std::vector<bool> usedTracks(m_tracks.size(), false);
    std::vector<NpuBox> confirmedPedestrians;
    confirmedPedestrians.reserve(pedestrians->size());

    constexpr float kCenterDistanceMax = 0.50f;
    constexpr float kFootDistanceMax = 0.35f;

    for (NpuBox& detection : *pedestrians) {
        if (!pedestrianBoxPassesGeometry(
                    detection,
                    width,
                    height,
                    m_config.pedestrianMinHeightPx,
                    m_config.pedestrianMinHeightWidthRatio,
                    m_config.pedestrianMaxHeightWidthRatio)) {
            ++result->pedestrianGeometryRejectedCount;
            continue;
        }

        int bestIndex = -1;
        float bestScore = -1000000.0f;
        for (std::size_t i = 0; i < m_tracks.size(); ++i) {
            if (usedTracks[i]) {
                continue;
            }

            const float iou = trackIou(m_tracks[i].box, detection);
            const float centerDistance = trackCenterDistanceNormalized(
                        m_tracks[i].box, detection);
            const float footDistance = trackFootDistanceNormalized(
                        m_tracks[i].box, detection);
            if (iou < m_config.trackIouThreshold &&
                centerDistance > kCenterDistanceMax &&
                footDistance > kFootDistanceMax) {
                continue;
            }

            // 斑马线规则依赖脚点，因此在IoU之外优先约束脚点连续性，降低
            // 两个行人靠近或交叉时违法状态串到另一条轨迹的概率。
            const float score = iou * 1.50f -
                    centerDistance * 0.35f - footDistance * 0.45f;
            if (score > bestScore) {
                bestScore = score;
                bestIndex = static_cast<int>(i);
            }
        }

        const float requiredConfidence = bestIndex >= 0
                ? m_config.pedestrianTrackedConfidence
                : m_config.pedestrianNewTrackConfidence;
        if (!std::isfinite(detection.score) ||
            detection.score < requiredConfidence) {
            ++result->pedestrianConfidenceRejectedCount;
            continue;
        }

        bool newTrack = false;
        if (bestIndex < 0) {
            Track track;
            track.id = m_nextTrackId++;
            track.box = detection;
            track.lastSeenMs = steadyTimeMs;
            track.hitStreak = 1;
            track.confirmed = m_config.pedestrianConfirmHits <= 1;
            m_tracks.push_back(track);
            usedTracks.push_back(true);
            bestIndex = static_cast<int>(m_tracks.size()) - 1;
            newTrack = true;
        } else {
            usedTracks[static_cast<std::size_t>(bestIndex)] = true;
        }

        Track& track = m_tracks[static_cast<std::size_t>(bestIndex)];
        const std::uint64_t previousSeenMs = track.lastSeenMs;
        if (!newTrack && previousSeenMs != 0 &&
            steadyTimeMs <= previousSeenMs +
                    static_cast<std::uint64_t>(
                        m_config.pedestrianConfirmMaxGapMs)) {
            ++track.hitStreak;
        } else if (!newTrack && previousSeenMs != steadyTimeMs) {
            track.hitStreak = 1;
        }
        if (!track.confirmed &&
            track.hitStreak >= m_config.pedestrianConfirmHits) {
            track.confirmed = true;
            clearIllegalRuntime(&track, true);
        }

        if (track.lastSeenMs != 0 && track.box.w > 0.0f && track.box.h > 0.0f) {
            const float alpha = m_config.boxEma;
            detection.x = smoothValue(track.box.x, detection.x, alpha);
            detection.y = smoothValue(track.box.y, detection.y, alpha);
            detection.w = smoothValue(track.box.w, detection.w, alpha);
            detection.h = smoothValue(track.box.h, detection.h, alpha);
        }
        track.box = detection;
        track.lastSeenMs = steadyTimeMs;

        if (!track.confirmed) {
            ++result->pedestrianUnconfirmedCount;
            continue;
        }

        const float crosswalkDistance = footCrosswalkSignedDistance(
                    detection, width, height);
        if (!track.insideInitialized) {
            track.insideInitialized = true;
            track.insideCrosswalk = crosswalkDistance >= 0.0f;
        } else if (track.insideCrosswalk) {
            track.insideCrosswalk = crosswalkDistance >=
                    -m_config.crosswalkHysteresisPx;
        } else {
            track.insideCrosswalk = crosswalkDistance >=
                    m_config.crosswalkHysteresisPx;
        }

        double accumulatedMs = 0.0;
        NpuPedestrianState state = NpuPedestrianState::Uncertain;
        NpuPedestrianViolationReason reason =
                NpuPedestrianViolationReason::None;

        const bool ruleReady = result->violationRegionsConfigured &&
                pedestrianLightKnown(result->trafficLight);
        if (!ruleReady) {
            // 灯态未知期间不产生事件，也不延续违法计时；保留已产生事件的
            // latch，避免短时灯态丢失后对同一连续行为重复计数。
            clearIllegalRuntime(&track, false);
        } else if (pedestrianMovementLegal(result->trafficLight,
                                           track.insideCrosswalk)) {
            state = NpuPedestrianState::Legal;
            clearIllegalRuntime(&track, true);
        } else {
            reason = pedestrianViolationReason(result->trafficLight,
                                               track.insideCrosswalk);
            track.violationReason = reason;
            if (!track.illegalTiming) {
                track.illegalTiming = true;
                track.illegalStartMs = steadyTimeMs;
                track.activeViolation = false;
            }
            accumulatedMs = static_cast<double>(
                        steadyTimeMs - track.illegalStartMs);
            if (accumulatedMs >= m_config.violationHoldMs) {
                track.activeViolation = true;
                state = NpuPedestrianState::Illegal;
            } else {
                state = NpuPedestrianState::IllegalAccumulating;
            }

            if (track.activeViolation && !track.eventLatched) {
                track.eventLatched = true;
                m_recentEvent.eventId = m_nextEventId++;
                m_recentEvent.frameId = frameId;
                m_recentEvent.wallClockMs = wallClockMs;
                m_recentEvent.trackId = track.id;
                m_recentEvent.accumulatedMs = accumulatedMs;
                m_recentEvent.reason = reason;
                m_hasRecentEvent = true;
            }
        }

        detection.trackId = track.id;
        detection.pedestrianState = state;
        detection.pedestrianViolationReason = reason;
        detection.stateDurationMs = accumulatedMs;
        track.box = detection;
        confirmedPedestrians.push_back(detection);
    }

    result->activeTrackCount = static_cast<int>(std::count_if(
                m_tracks.begin(), m_tracks.end(),
                [](const Track& track) { return track.confirmed; }));
    result->activeViolationCount = 0;
    for (const Track& track : m_tracks) {
        if (track.confirmed && track.activeViolation &&
            track.lastSeenMs == steadyTimeMs) {
            ++result->activeViolationCount;
        }
    }
    *pedestrians = std::move(confirmedPedestrians);
    result->violationEventCount = m_nextEventId - 1;
    result->hasRecentViolation = m_hasRecentEvent;
    if (m_hasRecentEvent) {
        result->recentViolation = m_recentEvent;
    }
}
