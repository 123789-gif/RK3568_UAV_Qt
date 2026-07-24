#pragma once

class QString;

#include <array>
#include <cstdint>

struct ViolationPointF
{
    float x = 0.0f;
    float y = 0.0f;
};

enum class ViolationLightMode : std::uint8_t
{
    Auto = 0,
    ForceRed = 1,
    ForceGreen = 2
};

struct ViolationRuleConfig
{
    bool lightRoiValid = false;
    float lightX = 0.0f;
    float lightY = 0.0f;
    float lightW = 0.0f;
    float lightH = 0.0f;

    bool crosswalkValid = false;
    std::array<ViolationPointF, 4> crosswalk {};

    ViolationLightMode lightMode = ViolationLightMode::Auto;
    int lightVoteWindow = 5;
    int lightVoteMin = 3;
    int unknownHoldMs = 300;
    int violationHoldMs = 600;
    bool requireRedEntry = true;
    int trackTtlMs = 1000;
    float trackIouThreshold = 0.20f;
    float boxEma = 0.50f;
    std::uint64_t revision = 1;

    bool regionsValid() const
    {
        return lightRoiValid && crosswalkValid;
    }
};

ViolationRuleConfig sanitizeViolationRuleConfig(ViolationRuleConfig config);

bool loadViolationRuleConfig(const QString& path,
                             ViolationRuleConfig* config,
                             QString* errorMessage = nullptr);

bool saveViolationRuleConfig(const QString& path,
                             const ViolationRuleConfig& config,
                             QString* errorMessage = nullptr);

QString violationLightModeKey(ViolationLightMode mode);
ViolationLightMode violationLightModeFromKey(const QString& key);
