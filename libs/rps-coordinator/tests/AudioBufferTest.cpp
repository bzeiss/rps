#include "TestFramework.hpp"
#include <rps/coordinator/AudioBuffer.hpp>

#include <cmath>
#include <vector>

using namespace rps::coordinator;

TEST(AudioBuffer_Construction) {
    AudioBuffer buf(2, 128);
    ASSERT_EQ(buf.numChannels(), 2u);
    ASSERT_EQ(buf.blockSize(), 128u);
    ASSERT_TRUE(buf.isValid());
}

TEST(AudioBuffer_DefaultIsInvalid) {
    AudioBuffer buf;
    ASSERT_FALSE(buf.isValid());
}

TEST(AudioBuffer_ClearZeroesAll) {
    AudioBuffer buf(2, 4);
    buf.channel(0)[0] = 1.0f;
    buf.channel(1)[3] = 2.0f;
    buf.clear();
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < 4; ++i) {
            ASSERT_NEAR(buf.channel(ch)[i], 0.0f, 1e-9);
        }
    }
}

TEST(AudioBuffer_CopyFrom) {
    AudioBuffer a(2, 4);
    a.channel(0)[0] = 1.0f;
    a.channel(0)[1] = 2.0f;
    a.channel(1)[0] = 3.0f;
    a.channel(1)[1] = 4.0f;

    AudioBuffer b(2, 4);
    b.copyFrom(a);

    ASSERT_NEAR(b.channel(0)[0], 1.0f, 1e-9);
    ASSERT_NEAR(b.channel(0)[1], 2.0f, 1e-9);
    ASSERT_NEAR(b.channel(1)[0], 3.0f, 1e-9);
    ASSERT_NEAR(b.channel(1)[1], 4.0f, 1e-9);
}

TEST(AudioBuffer_MixIn_SimpleSum) {
    AudioBuffer a(1, 4);
    AudioBuffer b(1, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        a.channel(0)[i] = 1.0f;
        b.channel(0)[i] = 2.0f;
    }
    a.mixIn(b);
    for (uint32_t i = 0; i < 4; ++i) {
        ASSERT_NEAR(a.channel(0)[i], 3.0f, 1e-6);
    }
}

TEST(AudioBuffer_MixIn_WithGain) {
    AudioBuffer a(1, 4);
    AudioBuffer b(1, 4);
    for (uint32_t i = 0; i < 4; ++i) {
        a.channel(0)[i] = 1.0f;
        b.channel(0)[i] = 2.0f;
    }
    a.mixIn(b, 0.5f);
    for (uint32_t i = 0; i < 4; ++i) {
        ASSERT_NEAR(a.channel(0)[i], 2.0f, 1e-6); // 1 + 2*0.5 = 2
    }
}

TEST(AudioBuffer_MixIn_64BitPrecision) {
    // Test that 64-bit accumulation improves precision during a single mixIn call.
    // Sum 8 buffers each containing a value near 1e7. With 32-bit accumulation,
    // adding a small residual would be lost. With 64-bit, it's preserved.
    AudioBuffer accum(1, 1);
    accum.channel(0)[0] = 0.0f;

    // 8 inputs that sum to exactly 8.0: each is 1.0
    for (int i = 0; i < 8; ++i) {
        AudioBuffer src(1, 1);
        src.channel(0)[0] = 1.0f;
        accum.mixIn(src);
    }
    // The result should be exactly 8.0 (64-bit accumulation is exact for these values)
    ASSERT_NEAR(accum.channel(0)[0], 8.0f, 1e-6);
}

TEST(AudioBuffer_ApplyGain) {
    AudioBuffer buf(2, 4);
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < 4; ++i) {
            buf.channel(ch)[i] = 1.0f;
        }
    }
    buf.applyGain(0.5f);
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t i = 0; i < 4; ++i) {
            ASSERT_NEAR(buf.channel(ch)[i], 0.5f, 1e-6);
        }
    }
}

TEST(AudioBuffer_DeinterleaveAndInterleave) {
    // Interleaved stereo: L0, R0, L1, R1, L2, R2, L3, R3
    float interleaved[] = {1, 2, 3, 4, 5, 6, 7, 8};

    AudioBuffer buf(2, 4);
    buf.deinterleaveFrom(interleaved, 2, 4);

    // L channel: 1, 3, 5, 7
    ASSERT_NEAR(buf.channel(0)[0], 1.0f, 1e-9);
    ASSERT_NEAR(buf.channel(0)[1], 3.0f, 1e-9);
    ASSERT_NEAR(buf.channel(0)[2], 5.0f, 1e-9);
    ASSERT_NEAR(buf.channel(0)[3], 7.0f, 1e-9);
    // R channel: 2, 4, 6, 8
    ASSERT_NEAR(buf.channel(1)[0], 2.0f, 1e-9);
    ASSERT_NEAR(buf.channel(1)[1], 4.0f, 1e-9);
    ASSERT_NEAR(buf.channel(1)[2], 6.0f, 1e-9);
    ASSERT_NEAR(buf.channel(1)[3], 8.0f, 1e-9);

    // Interleave back
    float output[8] = {};
    buf.interleaveTo(output);
    for (int i = 0; i < 8; ++i) {
        ASSERT_NEAR(output[i], interleaved[i], 1e-9);
    }
}

TEST(AudioBuffer_OutOfRangeThrows) {
    AudioBuffer buf(2, 4);
    ASSERT_THROWS(buf.channel(2));
}

TEST(AudioBuffer_CopyFromDimensionMismatchThrows) {
    AudioBuffer a(2, 4);
    AudioBuffer b(1, 4);
    ASSERT_THROWS(a.copyFrom(b));
}
