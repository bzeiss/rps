#include "TestFramework.hpp"
#include <rps/coordinator/ChannelFormat.hpp>

using namespace rps::coordinator;

TEST(ChannelFormat_MonoHasOneChannel) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Mono), 1u);
}

TEST(ChannelFormat_StereoHasTwoChannels) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Stereo), 2u);
}

TEST(ChannelFormat_Surround71HasEightChannels) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Surround_7_1), 8u);
}

TEST(ChannelFormat_Atmos512AlsoHasEightChannels) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Atmos_5_1_2), 8u);
}

TEST(ChannelFormat_Atmos714HasTwelveChannels) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Atmos_7_1_4), 12u);
}

TEST(ChannelFormat_AmbisonicsTOAHasSixteenChannels) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Ambisonics_TOA), 16u);
}

TEST(ChannelFormat_CustomReturnsZero) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Custom), 0u);
}

TEST(ChannelFormat_UnknownReturnsZero) {
    ASSERT_EQ(getChannelCount(ChannelFormat::Unknown), 0u);
}

TEST(ChannelFormat_UniqueEnumValues) {
    // Ensure Surround_7_1 and Atmos_5_1_2 have DIFFERENT enum values
    // even though they share the same channel count.
    ASSERT_NE(static_cast<uint32_t>(ChannelFormat::Surround_7_1),
              static_cast<uint32_t>(ChannelFormat::Atmos_5_1_2));
}

TEST(ChannelFormat_StringRoundTrip) {
    auto str = channelFormatToString(ChannelFormat::Atmos_7_1_4);
    ASSERT_EQ(str, "7.1.4");
    auto fmt = channelFormatFromString("7.1.4");
    ASSERT_EQ(static_cast<uint32_t>(fmt), static_cast<uint32_t>(ChannelFormat::Atmos_7_1_4));
}

TEST(ChannelFormat_StereoStringRoundTrip) {
    auto str = channelFormatToString(ChannelFormat::Stereo);
    ASSERT_EQ(str, "stereo");
    auto fmt = channelFormatFromString("stereo");
    ASSERT_EQ(static_cast<uint32_t>(fmt), static_cast<uint32_t>(ChannelFormat::Stereo));
}

TEST(ChannelFormat_UnknownStringReturnsUnknown) {
    auto fmt = channelFormatFromString("nonexistent");
    ASSERT_EQ(static_cast<uint32_t>(fmt), static_cast<uint32_t>(ChannelFormat::Unknown));
}

TEST(ChannelLayout_EffectiveChannelCount_NamedFormat) {
    ChannelLayout l{ChannelFormat::Surround_5_1, 0};
    ASSERT_EQ(l.effectiveChannelCount(), 6u);
}

TEST(ChannelLayout_EffectiveChannelCount_Custom) {
    ChannelLayout l{ChannelFormat::Custom, 10};
    ASSERT_EQ(l.effectiveChannelCount(), 10u);
}

TEST(ChannelFormat_LayoutCompatibility_Same) {
    ChannelLayout a{ChannelFormat::Stereo, 2};
    ChannelLayout b{ChannelFormat::Stereo, 2};
    ASSERT_TRUE(areLayoutsCompatible(a, b));
}

TEST(ChannelFormat_LayoutCompatibility_Mismatch) {
    ChannelLayout stereo{ChannelFormat::Stereo, 2};
    ChannelLayout mono{ChannelFormat::Mono, 1};
    ASSERT_FALSE(areLayoutsCompatible(stereo, mono));
}

TEST(ChannelFormat_LayoutCompatibility_71and512_SameCount) {
    // Both have 8 channels — compatible for direct connection
    ChannelLayout a{ChannelFormat::Surround_7_1, 8};
    ChannelLayout b{ChannelFormat::Atmos_5_1_2, 8};
    ASSERT_TRUE(areLayoutsCompatible(a, b));
}
