#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "common/Frame.hpp"

struct ThumbnailConfig {
    bool enabled = false;
    int width = 320;
    int height = 180;
    bool on_detection_only = true;
};
struct ImageProcessingConfig {
    ThumbnailConfig thumbnail;
};
struct Thumbnail {
    int width = 0, height = 0;
    std::string format = "nv12";
    std::vector<uint8_t> data;
};
