#if ENABLE_TEST_XMATH

#include "gtest/gtest.h"
#include "math/xmath.h"

TEST(XMath, ConstantsAndAngles)
{
    EXPECT_NEAR(au::math::PI, 3.14159265358979323846, 1e-15);
    EXPECT_DOUBLE_EQ(au::math::TWO_PI, 2.0 * au::math::PI);
    EXPECT_DOUBLE_EQ(au::math::HALF_PI, au::math::PI / 2.0);
    EXPECT_NEAR(au::math::E, 2.71828182845904523536, 1e-15);
    EXPECT_NEAR(au::math::SQRT2, 1.41421356237309504880, 1e-15);
    EXPECT_FLOAT_EQ(au::math::EPSILON, 1e-6f);
    EXPECT_DOUBLE_EQ(au::math::EPSILOND, 1e-10);

    EXPECT_NEAR(au::math::deg2rad(180.0), au::math::PI, au::math::EPSILOND);
    EXPECT_NEAR(au::math::rad2deg(au::math::PI), 180.0, au::math::EPSILOND);
    EXPECT_DOUBLE_EQ(au::math::deg2rad(0.0), 0.0);
    EXPECT_NEAR(au::math::deg2rad(-180.0), -au::math::PI, au::math::EPSILOND);
    EXPECT_NEAR(au::math::rad2deg(-au::math::PI), -180.0, au::math::EPSILOND);
}

TEST(XMath, MinMaxAndClamp)
{
    EXPECT_EQ(au::math::minOf(3, 5), 3);
    EXPECT_EQ(au::math::maxOf(3, 5), 5);
    EXPECT_EQ(au::math::minOf(3, 1, 5), 1);
    EXPECT_EQ(au::math::maxOf(3, 1, 5), 5);

    EXPECT_EQ(au::math::clampToRange(5, 0, 10), 5);
    EXPECT_EQ(au::math::clampToRange(-1, 0, 10), 0);
    EXPECT_EQ(au::math::clampToRange(15, 0, 10), 10);
    EXPECT_EQ(au::math::clampToUint8(300), 255);
    EXPECT_EQ(au::math::clampToUint8(-5), 0);
    EXPECT_EQ(au::math::clampToUint8(255), 255);
    EXPECT_EQ(au::math::clampToUint16(70000), 65535);
    EXPECT_EQ(au::math::clampToUint16(-1), 0);
    EXPECT_EQ(au::math::clampToUint16(65535), 65535);
}

TEST(XMath, Arithmetic)
{
    EXPECT_EQ(au::math::abs(-7), 7);
    EXPECT_FLOAT_EQ(au::math::abs(-1.5f), 1.5f);
    EXPECT_DOUBLE_EQ(au::math::abs(-3.14), 3.14);
    EXPECT_EQ(au::math::sqr(4), 16);
    EXPECT_EQ(au::math::cube(-3), -27);
    EXPECT_DOUBLE_EQ(au::math::lerp(0.0, 10.0, 0.25), 2.5);
    EXPECT_EQ(au::math::lerp(0, 10, 0.25), 2);

    EXPECT_FLOAT_EQ(au::math::powf(2.0f, 3.0f), 8.0f);
    EXPECT_DOUBLE_EQ(au::math::powd(2.0, 10.0), 1024.0);
    EXPECT_FLOAT_EQ(au::math::sqrtf(16.0f), 4.0f);
    EXPECT_NEAR(au::math::sqrtd(2.0), au::math::SQRT2, au::math::EPSILOND);

    EXPECT_TRUE(au::math::approxEqual(1.0f, 1.0f + au::math::EPSILON / 2.0f));
    EXPECT_FALSE(au::math::approxEqual(1.0f, 1.0f + 10.0f * au::math::EPSILON));
    EXPECT_TRUE(au::math::approxEqual(1.0, 1.0 + au::math::EPSILOND / 2.0));
    EXPECT_FALSE(au::math::approxEqual(1.0, 1.0 + 10.0 * au::math::EPSILOND));
}

TEST(XMath, RangeChecks)
{
    EXPECT_TRUE(au::math::isInRangeCC(0, 0, 10));
    EXPECT_TRUE(au::math::isInRangeCC(10, 0, 10));
    EXPECT_FALSE(au::math::isInRangeOO(0, 0, 10));
    EXPECT_TRUE(au::math::isInRangeOO(5, 0, 10));
    EXPECT_TRUE(au::math::isInRangeOC(10, 0, 10));
    EXPECT_FALSE(au::math::isInRangeOC(0, 0, 10));
    EXPECT_TRUE(au::math::isInRangeCO(0, 0, 10));
    EXPECT_FALSE(au::math::isInRangeCO(10, 0, 10));
}

TEST(XMath, Alignment)
{
    EXPECT_TRUE(au::math::isAlignedTo(64, 16));
    EXPECT_FALSE(au::math::isAlignedTo(65, 16));
    EXPECT_EQ(au::math::ceilTo(13, 8), 16);
    EXPECT_EQ(au::math::floorTo(15, 8), 8);

    EXPECT_TRUE(au::math::isAlignedToOdd(3));
    EXPECT_FALSE(au::math::isAlignedToOdd(4));
    EXPECT_EQ(au::math::ceilToOdd(4), 5);
    EXPECT_EQ(au::math::floorToOdd(4), 3);

    EXPECT_TRUE(au::math::isAlignedTo2(4));
    EXPECT_FALSE(au::math::isAlignedTo2(3));
    EXPECT_TRUE(au::math::isAlignedTo4(8));
    EXPECT_TRUE(au::math::isAlignedTo8(16));
    EXPECT_EQ(au::math::ceilTo2(3), 4);
    EXPECT_EQ(au::math::ceilTo4(5), 8);
    EXPECT_EQ(au::math::ceilTo8(9), 16);
    EXPECT_EQ(au::math::floorTo2(3), 2);
    EXPECT_EQ(au::math::floorTo4(7), 4);
    EXPECT_EQ(au::math::floorTo8(15), 8);

    EXPECT_TRUE(au::math::isPowerOf2(1));
    EXPECT_TRUE(au::math::isPowerOf2(256));
    EXPECT_FALSE(au::math::isPowerOf2(3));
    EXPECT_FALSE(au::math::isPowerOf2(0));
}

TEST(XMath, Trigonometry)
{
    EXPECT_NEAR(au::math::sinf(0.0f), 0.0f, au::math::EPSILON);
    EXPECT_NEAR(au::math::cosf(0.0f), 1.0f, au::math::EPSILON);
    EXPECT_NEAR(au::math::tanf(0.0f), 0.0f, au::math::EPSILON);
}

#endif  // ENABLE_TEST_XMATH
