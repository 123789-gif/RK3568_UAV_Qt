#pragma once

#include <cstdint>

enum class VideoRoute : std::uint8_t
{
    Overview = 0,
    Channel1 = 1,
    Channel2 = 2,
    Channel3 = 3,
    Channel4 = 4,
    Channel5 = 5,
    Channel6 = 6,
    Channel7 = 7,
    Channel8 = 8,
    Channel9 = 9
};

struct VideoTileRoi
{
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
};

inline VideoRoute sanitizeVideoRoute(int value)
{
    if (value < 1 || value > 9) {
        return VideoRoute::Overview;
    }
    return static_cast<VideoRoute>(value);
}

inline bool isSingleChannelRoute(VideoRoute route)
{
    const int channel = static_cast<int>(route);
    return channel >= 1 && channel <= 9;
}

inline int videoRouteChannel(VideoRoute route)
{
    return isSingleChannelRoute(route) ? static_cast<int>(route) : 0;
}

inline VideoTileRoi videoRouteRoi(VideoRoute route)
{
    if (!isSingleChannelRoute(route)) {
        return {0, 0, 1920, 1080};
    }

    const int index = static_cast<int>(route) - 1;
    const int column = index % 3;
    const int row = index / 3;
    return {column * 640, row * 360, 640, 360};
}

inline const char *videoRouteKey(VideoRoute route)
{
    switch (route) {
    case VideoRoute::Channel1: return "CH1";
    case VideoRoute::Channel2: return "CH2";
    case VideoRoute::Channel3: return "CH3";
    case VideoRoute::Channel4: return "CH4";
    case VideoRoute::Channel5: return "CH5";
    case VideoRoute::Channel6: return "CH6";
    case VideoRoute::Channel7: return "CH7";
    case VideoRoute::Channel8: return "CH8";
    case VideoRoute::Channel9: return "CH9";
    case VideoRoute::Overview:
    default:
        return "ALL";
    }
}
