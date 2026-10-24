#include <stepwright/stepwright.hpp>

#include <gtest/gtest.h>

TEST(Smoke, VersionIsConsistent) {
    static_assert(stepwright::version.major == STEPWRIGHT_VERSION_MAJOR);
    EXPECT_EQ(stepwright::version.minor, STEPWRIGHT_VERSION_MINOR);
    EXPECT_EQ(stepwright::version.patch, STEPWRIGHT_VERSION_PATCH);
}
