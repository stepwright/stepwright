#pragma once

// Compile-time semantic version components; safe in preprocessor conditions.
#define STEPWRIGHT_VERSION_MAJOR 0
#define STEPWRIGHT_VERSION_MINOR 1
#define STEPWRIGHT_VERSION_PATCH 0

namespace stepwright {

/// Plain semantic-version value; concurrent reads are safe if not mutated.
struct version_info {
    int major;
    int minor;
    int patch;
};

/// Immutable library version with static lifetime; safe to read from any thread.
inline constexpr version_info version{.major = STEPWRIGHT_VERSION_MAJOR,
                                      .minor = STEPWRIGHT_VERSION_MINOR,
                                      .patch = STEPWRIGHT_VERSION_PATCH};

} // namespace stepwright
