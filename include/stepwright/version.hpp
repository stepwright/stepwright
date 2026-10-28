#pragma once

/// Compile-time major version; safe in preprocessor conditions.
#define STEPWRIGHT_VERSION_MAJOR 0
/// Compile-time minor version (the compatibility series while major is zero).
#define STEPWRIGHT_VERSION_MINOR 2
/// Compile-time patch version within the minor series.
#define STEPWRIGHT_VERSION_PATCH 0

namespace stepwright {

/// Plain semantic-version value; concurrent reads are safe if not mutated.
struct version_info {
    int major; ///< Major version; zero denotes an evolving API.
    int minor; ///< Minor version; compatibility boundary while major is zero.
    int patch; ///< Patch version within the minor series.
};

/// Immutable library version with static lifetime; safe to read from any thread.
inline constexpr version_info version{.major = STEPWRIGHT_VERSION_MAJOR,
                                      .minor = STEPWRIGHT_VERSION_MINOR,
                                      .patch = STEPWRIGHT_VERSION_PATCH};

} // namespace stepwright
