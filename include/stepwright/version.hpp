#pragma once

#define STEPWRIGHT_VERSION_MAJOR 0
#define STEPWRIGHT_VERSION_MINOR 1
#define STEPWRIGHT_VERSION_PATCH 0

namespace stepwright {

struct version_info {
    int major;
    int minor;
    int patch;
};

inline constexpr version_info version{.major = STEPWRIGHT_VERSION_MAJOR,
                                      .minor = STEPWRIGHT_VERSION_MINOR,
                                      .patch = STEPWRIGHT_VERSION_PATCH};

} // namespace stepwright
