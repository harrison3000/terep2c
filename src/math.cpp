/*
 * Table-driven trigonometry, vector length and the 3x3 view rotation.
 */
#include <array>
#include <cmath>
#include "terep.hpp"

/* sin of 0..90 deg in 1024 steps, Q15 (1.0 clamped to 0x7fff) */
static const std::array<int16_t, 1025> sin_table = [] {
    std::array<int16_t, 1025> t;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i <= 1024; i++) {
        long v = std::lround(32768.0 * std::sin(i * pi / 2048.0));
        t[i] = (int16_t)(v > 0x7fff ? 0x7fff : v);
    }
    return t;
}();

/* atan(i / 256), i = 0..256, in binary angle units (0x10000 = 360 deg).
 * Pi is wrong here so the table is identical to the original one
 * Claude got very near the right wrong pi, but not quite, bateu na trave
 * Lets be honest, the errors probably have another origin and not a wrongly typed pi
 * my guess would be a single precision math library was used*/
static const std::array<uint16_t, 257> atan_table = [] {
    std::array<uint16_t, 257> t;
    const double m = 32768.0 / 3.141688765;
    for (int i = 0; i <= 256; i++)
        t[i] = (uint16_t)std::lround(std::atan(i / 256.0) * m);
    return t;
}();

/* orig 1000:2aad */
Q15 fx_sin(Angle a)
{
    uint16_t t = a.bits() & 0x7fff;
    if (t & 0x4000)
        t = 0x8000 - t;                 /* mirror the second quadrant */
    Q15 v = { sin_table[t >> 4] };
    return (a.bits() & 0x8000) ? -v : v;
}

/* orig 1000:2ad8 */
Q15 fx_cos(Angle a)
{
    uint16_t t = a.bits() & 0x7fff;
    if (t & 0x4000)
        t -= 0x4000;
    else
        t = 0x4000 - t;
    Q15 v = { sin_table[t >> 4] };
    uint16_t quadrant = a.bits() >> 14;
    return (quadrant == 1 || quadrant == 2) ? -v : v;
}

/* atan(num/den) for 0 <= num < den, orig 1000:2b3b */
static Angle atan_ratio(uint16_t num, uint16_t den)
{
    uint16_t q = (uint16_t)(((uint32_t)num << 16) / den);
    return { (int16_t)atan_table[q >> 8] };
}

/* both arguments >= 0, orig 1000:2b2d */
static Angle atan_first_quadrant(int16_t y, int16_t x)
{
    if (y > x)
        return DEG90 - atan_ratio(x, y);
    if (y < x)
        return atan_ratio(y, x);
    return DEG45;
}

/* y >= 0, orig 1000:2b1f */
static Angle atan_upper_half(int16_t y, int16_t x)
{
    if (x < 0)
        return DEG180 - atan_first_quadrant(y, (int16_t)~x);
    if (x == 0)
        return DEG90;
    return atan_first_quadrant(y, x);
}

/* Angle of the vector (x, y), orig 1000:2b08.  Negative coordinates are
 * folded with NOT rather than NEG, as in the original. */
Angle fx_atan2(int16_t y, int16_t x)
{
    if (y < 0)
        return -atan_upper_half((int16_t)~y, x);
    if (y == 0)
        return x < 0 ? DEG180 : Angle{};
    return atan_upper_half(y, x);
}

/* Length of (b, a) computed as a/sin or b/cos of its angle, whichever is
 * better conditioned.  orig 1000:26dd */
int32_t vec2_length(int16_t a, int16_t b)
{
    Angle angle = fx_atan2(a, b);
    uint8_t octant_bits = (angle.bits() >> 8) & 0x60;
    if (octant_bits == 0x20 || octant_bits == 0x40)
        return ((int32_t)a << 15) / fx_sin(angle).raw;
    return ((int32_t)b << 15) / fx_cos(angle).raw;
}

/* orig 1000:271d */
int32_t vec3_length(int16_t a, int16_t b, int16_t c)
{
    return vec2_length((int16_t)vec2_length(a, b), c);
}

/* Divide a 16.16 vector by its length (+1): the result has 1.0 == 0x10000.
 * orig 1000:2726 */
Vec3 vec3_normalize(Vec3 v)
{
    int32_t len = vec3_length(fx_int(v.x), fx_int(v.y), fx_int(v.z)) + 1;
    return { v.x / len, v.y / len, v.z / len };
}

/* Perspective divide, orig 1000:2760.  depth is the camera-space y.  The
 * original divides with 16-bit IDIV and crashes (divide overflow) when the
 * result does not fit; here it wraps around instead (see the README). */
void project(int16_t x, int16_t depth, int16_t z, int16_t *sx, int16_t *sz)
{
    *sx = (int16_t)(((int32_t)x << 8) / depth);
    *sz = (int16_t)(((int32_t)z << 8) / depth);
}

/* Row vector times matrix, every product rounded to Q15 on its own.
 * orig 1000:277e */
void rotate(const Q15 m[9], int16_t x, int16_t y, int16_t z, int16_t out[3])
{
    out[0] = (int16_t)(x * m[0] + y * m[3] + z * m[6]);
    out[1] = (int16_t)(x * m[1] + y * m[4] + z * m[7]);
    out[2] = (int16_t)(x * m[2] + y * m[5] + z * m[8]);
}

/* Rotation matrix from roll, pitch and yaw.  orig 1000:2989 */
void build_matrix(Angles a, Q15 m[9])
{
    Q15 s0 = fx_sin(a.roll), c0 = fx_cos(a.roll);
    Q15 s1 = fx_sin(a.pitch), c1 = fx_cos(a.pitch);
    Q15 s2 = -fx_sin(a.yaw), c2 = fx_cos(a.yaw);

    Q15 s0s1 = s0 * s1;
    Q15 c0c2 = c0 * c2;
    Q15 s2c0 = s2 * c0;

    m[0] = c0c2 + (s0s1 * s2).neg_sat();
    m[1] = (s0s1 * c2 + s2c0).neg_sat();
    m[2] = (s0 * c1).neg_sat();
    m[3] = s2 * c1;
    m[4] = c2 * c1;
    m[5] = s1.neg_sat();
    m[6] = s2c0 * s1 + c2 * s0;
    m[7] = c0c2 * s1 + (s0 * s2).neg_sat();
    m[8] = c0 * c1;
}

/* orig 1000:27f1 */
void transpose(Q15 m[9])
{
    Q15 t;
    t = m[1]; m[1] = m[3]; m[3] = t;
    t = m[2]; m[2] = m[6]; m[6] = t;
    t = m[5]; m[5] = m[7]; m[7] = t;
}
