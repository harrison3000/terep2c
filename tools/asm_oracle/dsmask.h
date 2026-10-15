/*
 * CRC of the data segment for comparing runs, ignoring:
 *  - segment handles (their values depend on the allocator),
 *  - write-only leftovers and scratch variables of the original that the
 *    port keeps in locals instead (they never carry state between ticks).
 *
 * Used by run.py, which parses the ranges below (it was shared with the
 * old CRC tests of the C++ port, hence the C header).
 */
#pragma once
#include <cstdint>
#include <cstring>

static const uint16_t ds_mask_ranges[][2] = {   /* [first, last] */
    { 0x006f, 0x0072 },     /* sound init leftovers */
    { 0x1a45, 0x1a4c },     /* segment handles */
    { 0xdb10, 0xdb11 },
    { 0x5ac1, 0x5ad8 },     /* terrain_height scratch */
    { 0xd100, 0xd10b },     /* build_matrix scratch */
    { 0xe9b0, 0xe9dd },     /* ground contact scratch */
    { 0xe9f8, 0xea05 },     /* spring scratch */
    { 0xea0c, 0xea23 },     /* ground_probe scratch */
    { 0x05d6, 0x05dd },     /* wheel sprite scratch */
    { 0x05fc, 0x05fe },     /* terrain traversal scratch */
    { 0x1d4d, 0x1d50 },     /* terrain texture scratch */
    { 0xe528, 0xe529 },     /* clipper scratch */
    { 0xe990, 0xe997 },     /* near-plane clipper scratch */
};

/* Polygon attributes the original sometimes leaves as leftovers of a
 * register (they never reach the screen): a/b of every poly_buf vertex. */
inline bool ds_is_poly_attr(uint16_t ofs)
{
    if (ofs >= 0xdb16 && ofs < 0xdb66) return (ofs - 0xdb16) % 8 >= 4;
    if (ofs >= 0xdb68 && ofs < 0xdbb8) return (ofs - 0xdb68) % 8 >= 4;
    return false;
}

inline bool ds_is_masked(uint16_t ofs)
{
    if (ds_is_poly_attr(ofs))
        return true;
    for (unsigned i = 0; i < sizeof ds_mask_ranges / sizeof ds_mask_ranges[0]; i++)
        if (ofs >= ds_mask_ranges[i][0] && ofs <= ds_mask_ranges[i][1])
            return true;
    return false;
}

/* Plain CRC-32 (same as zlib's crc32). */
inline unsigned long crc32_buf(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    uint32_t c = 0xffffffffu;
    while (n--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

inline unsigned long ds_masked_crc(const uint8_t *ds)
{
    static uint8_t copy[0xf000];
    memcpy(copy, ds, sizeof copy);
    for (unsigned i = 0; i < sizeof ds_mask_ranges / sizeof ds_mask_ranges[0]; i++)
        memset(copy + ds_mask_ranges[i][0], 0, ds_mask_ranges[i][1] - ds_mask_ranges[i][0] + 1u);
    for (unsigned o = 0xdb16; o < 0xdbb8; o++)
        if (ds_is_poly_attr((uint16_t)o))
            copy[o] = 0;
    int16_t ncars = *(const int16_t *)(ds + 0x5bba);
    for (int c = 0; c < ncars && c < 10; c++) {
        uint16_t o = (uint16_t)(*(const uint16_t *)(ds + 0x5bbc + 2 * c) + 0x1e);
        copy[o] = copy[o + 1] = 0;          /* car texture handle */
    }
    return crc32_buf(copy, sizeof copy);
}
