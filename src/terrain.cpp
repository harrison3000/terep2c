/*
 * Terrain height queries.
 *
 * The heightmap is a 256x256 grid of bytes (one per cell corner), heights
 * are scaled by 16.  Each cell is split along its anti-diagonal into two
 * triangles and the height is interpolated linearly inside the triangle.
 * Coordinates are 8.8: high byte = cell, low byte = position in the cell.
 */
#include "terep.hpp"

struct TerrainSample {
    uint16_t cell;      /* x | y << 8 */
    int16_t  base;      /* height of the triangle's corner */
    int16_t  dz_u, dz_v;/* height deltas along the two triangle edges */
    uint8_t  u, v;      /* position inside the triangle, 0..128 */
    bool     upper;     /* far triangle (corner at x+1, y+1) */
};

static TerrainSample terrain_sample(uint16_t x, uint16_t y)
{
    TerrainSample s;
    const uint8_t *h = heightmap();
    uint8_t fx = (uint8_t)(x & 0xff) >> 1;
    uint8_t fy = (uint8_t)(y & 0xff) >> 1;

    s.cell = (uint16_t)((y & 0xff00) | (x >> 8));
    s.upper = (uint8_t)(fx + fy) > 0x80;
    if (!s.upper) {
        s.base = (int16_t)(h[s.cell] << 4);
        s.dz_u = (int16_t)((h[(uint16_t)(s.cell + 0x001)] << 4) - s.base);
        s.dz_v = (int16_t)((h[(uint16_t)(s.cell + 0x100)] << 4) - s.base);
        s.u = fx;
        s.v = fy;
    } else {
        s.base = (int16_t)(h[(uint16_t)(s.cell + 0x101)] << 4);
        s.dz_u = (int16_t)((h[(uint16_t)(s.cell + 0x100)] << 4) - s.base);
        s.dz_v = (int16_t)((h[(uint16_t)(s.cell + 0x001)] << 4) - s.base);
        s.u = (uint8_t)(0x80 - fx);
        s.v = (uint8_t)(0x80 - fy);
    }
    return s;
}

/* The original solves a general 2x2 system (orig 1000:256b / 1000:52d4)
 * but always with the same 0x80 x 0x80 basis, so it reduces to this. */
static int16_t terrain_interpolate(const TerrainSample &s)
{
    int32_t num = (int32_t)s.dz_u * (int16_t)(s.u * 0x80)
                + (int32_t)s.dz_v * (int16_t)(s.v * 0x80);
    return (int16_t)(s.base + (int16_t)(num / 0x4000));
}

/* orig 1000:25c5 */
int16_t terrain_height(uint16_t x, uint16_t y)
{
    return terrain_interpolate(terrain_sample(x, y));
}

/* Same as terrain_height, plus what the physics needs about the ground
 * under the point: surface type and slope.  orig 1000:532e */
Ground ground_probe(uint16_t x, uint16_t y)
{
    TerrainSample s = terrain_sample(x, y);
    Ground g;
    g.surface = colormap()[s.cell];
    g.slope_x = s.upper ? (int16_t)-s.dz_u : s.dz_u;
    g.slope_y = s.upper ? (int16_t)-s.dz_v : s.dz_v;
    g.height = terrain_interpolate(s);
    return g;
}
