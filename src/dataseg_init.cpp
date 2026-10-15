/*
 * Initial contents of the data segment.  The original program loaded them
 * from its data segment image (data.bin); here they are written by code,
 * giving byte for byte the same 64 KB, so runs still match the original
 * frame by frame.  What game_init sets anyway (gravity, viewport, handles,
 * spring limits) is not repeated here.
 */
#include <cmath>
#include "terep.hpp"

/* The font: src/font.pbm (plain PBM), 5 pixels wide, one 5x5 glyph below
 * the other in memory order; a glyph is as wide as its rightmost lit column.
 * font_chars lists the character of each glyph; every other character
 * shows glyph 0 ('A'). */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"    /* #embed in C++ (g++ 15+ accepts it quietly) */
#endif
static const char font_pbm[] = {
#embed "font.pbm"
    , 0
};
#ifdef __clang__
#pragma clang diagnostic pop
#endif
static const char font_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!\"#$%&'()*+,-./0123456789:;<=>?{|}~";

/* AdLib instruments (11 bytes each, see opl_set_patch). */
static const uint8_t engine_patch[0x21] = {
    0x20, 0x00, 0xfb, 0x05, 0x00, 0x20, 0x00, 0xfb, 0x05, 0x00, 0x0a,
    0x20, 0x00, 0xfb, 0x21, 0x00, 0x20, 0x00, 0x9f, 0x05, 0x00, 0x06,
    0x64, 0x00, 0x76, 0x37, 0x00, 0x21, 0x00, 0x79, 0x00, 0x00, 0x0c,
};
static const uint8_t hit_patch[0x3e] = {
    0x00, 0x00, 0xf7, 0xa8, 0x00, 0x00, 0x00, 0xf3, 0x6a, 0x00, 0x02, 0x80, 0x21, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xf0, 0x13, 0x00, 0x05, 0x00, 0xf2, 0x1d, 0x00, 0x06, 0x80, 0x27, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xf0, 0x13, 0x00, 0x05, 0x00, 0xf2, 0x1d, 0x00, 0x06, 0x80, 0x2f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x00,
};

/* Keys of each car: accelerate, brake, left, right, handbrake, unused. */
static const uint8_t default_keys[5][6] = {
    { 0x48, 0x50, 0x4b, 0x4d, 0x4c, 0x52 },     /* arrows, keypad 5 (keypad 0) */
    { 0x11, 0x2c, 0x1e, 0x20, 0x1f, 0x2d },     /* W Z A D S (X) */
    { 0x17, 0x32, 0x24, 0x26, 0x25, 0x27 },     /* I M J L K (;) */
    { 0x14, 0x2f, 0x21, 0x23, 0x22, 0x30 },     /* T V F H G (B) */
    { 0x47, 0x49, 0x4f, 0x51, 0x53, 0x55 },     /* keypad 7 9 1 3 . */
};

/* Per surface type (colour-map byte >> 4). */
static const uint16_t surface_grip[16] = { 16, 25, 20, 40, 30, 25, 16, 20, 40, 25, 25, 25, 16, 25, 16, 16 };
static const uint8_t surface_damp_shift[16] = { 12, 9, 10, 10, 10, 10, 8, 10, 10, 10, 10, 10, 8, 6, 10, 10 };
static const int32_t surface_max_load[16] = {
    0x30000, 0x18000, 0x20000, 0xc000, 0x20000, 0x20000, 0x20000, 0x20000,
    0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x10000, 0x10000, 0x10000,
};

/* Triangles of car vertices that form the collision hull. */
static const uint16_t hull_triangles[7][3] = {
    { 4, 5, 9 }, { 7, 6, 24 }, { 6, 9, 5 }, { 8, 7, 25 }, { 0, 2, 3 }, { 0, 1, 2 }, { 15, 14, 13 },
};

/* Which smoke quad each particle kind (>> 4) uses. */
static const uint8_t smoke_quad_of_kind[16] = { 0, 0, 1, 2, 0, 0, 1, 3, 2, 0, 0, 0, 3, 3, 0, 0 };

template <size_t N>
static void set_text(char (&dst)[N], const char *s)
{
    strncpy(dst, s, N);
}

/* font.pbm is built in, so it is trusted: no error checks. */
static void init_font()
{
    const char *p = strchr(font_pbm, '\n') + 1;      /* after "P1" */
    while (*p == '#')                                   /* comments */
        p = strchr(p, '\n') + 1;
    int n;
    sscanf(p, "%*d %*d%n", &n);                         /* size: 5 x 5 per glyph */
    p += n;

    uint16_t out = 0;
    for (const char *c = font_chars; *c; c++) {
        uint8_t columns[5] = {};
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++) {
                int bit;
                sscanf(p, " %1d%n", &bit, &n);
                p += n;
                columns[x] |= (uint8_t)(bit << y);
            }
        G->font_offset[(uint8_t)*c] = out;
        int w = 5;
        while (w > 0 && !columns[w - 1])
            w--;
        memcpy(&G->font_glyphs[out], columns, w);
        out = (uint16_t)(out + w);
        G->font_glyphs[out++] = 0;
    }
}

void dataseg_init()
{
    memset(G, 0, sizeof *G);

    set_text(G->title_text, "-DEFORMERS-  ATOMIC VEHICLE SIMULATION SYSTEM (C)NAGYMATHE DENES 1996-1997");
    set_text(G->subtitle_text, "WORK-IN-PROGRESS DEMO VERSION");
    G->subtitle_text[sizeof G->subtitle_text - 1] = (char)0xff;     /* as in the original */
    set_text(G->fname_map, "map.pcx");
    set_text(G->fname_col, "col.pcx");
    set_text(G->fname_textures, "textures.pcx");
    set_text(G->fname_maptex, "maptex.pcx");
    set_text(G->fname_fog, "fog.dat");
    set_text(G->fname_smoke, "smoke.dat");
    set_text(G->fname_cfg, "sim.cfg");
    for (int i = 0; i < 5; i++) {       /* "car1.dat".."car5.dat", "car1.pcx".."car5.pcx" */
        char *dat = &G->car_dat_names[9 * i], *pcx = &G->car_pcx_names[9 * i];
        snprintf(dat, 9, "car%d.dat", i + 1);
        snprintf(pcx, 9, "car%d.pcx", i + 1);
        G->car_dat_name[i] = ds_ofs(dat);
        G->car_pcx_name[i] = ds_ofs(pcx);
    }

    /* cameras: chase camera (F4) in both views, at the middle of the map */
    G->cam_mode[0] = G->cam_mode[1] = 3;
    const Pos centre = { Coord::from_units(0x8080), Coord::from_units(0x9080), Coord::from_units(0x0400) };
    G->chase_cam[0].pos = G->chase_cam[1].pos = G->eye = centre;
    G->cars_center[0] = (int16_t)0x8080;
    G->cars_center[1] = (int16_t)0x8080;
    G->cars_center[2] = 0x0400;
    G->chase_height = 0x100;
    G->chase_distance = 0x400;
    G->near_clip = 30;

    /* cos of the borders between the nine wheel sprites (22.5 deg apart), Q14 */
    static const int16_t sector[8] = { -16069, -13623, -9102, -3196, 3196, 9102, 13623, 16069 };
    memcpy(G->wheel_sector_limits, sector, sizeof sector);
    G->view_reach = 0x21;
    G->view_side_a = 0x19;
    G->view_side_b = 0x14;
    G->fog_start = 0x600;
    G->vis_hi = 0xff;
    G->vis_max = 0xff;
    G->poly_color = 0x0f0f;

    /* smoke: 11x11-texel squares stacked down column 243 of textures.pcx */
    for (int k = 0; k < 4; k++) {
        SpriteQuad q;
        q.size = 0x0a00;
        uint32_t v0 = (uint32_t)(0x1000 * k + 0x80) << 16, v1 = v0 + (0xb00u << 16);
        q.uv[0] = v0 | 0xf380;
        q.uv[1] = v0 | 0xfe80;
        q.uv[2] = v1 | 0xfe80;
        q.uv[3] = v1 | 0xf380;
        G->smoke_quad[k] = q;
    }
    for (int i = 0; i < 16; i++)
        G->smoke_sprite[i] = ds_ofs(&G->smoke_quad[smoke_quad_of_kind[i]]);

    memcpy(G->car_keys, default_keys, sizeof default_keys);
    memcpy(G->surface_grip, surface_grip, sizeof surface_grip);
    memcpy(G->surface_damp_shift, surface_damp_shift, sizeof surface_damp_shift);
    memcpy(G->surface_max_load, surface_max_load, sizeof surface_max_load);
    G->hull_tri_count = 7;
    memcpy(G->hull_tri, hull_triangles, sizeof hull_triangles);

    /* sin of 0..90 deg in 1024 steps, Q15 (1.0 clamped to 0x7fff) */
    const double pi = 3.14159265358979323846;
    for (int i = 0; i <= 1024; i++) {
        long v = std::lround(32768.0 * std::sin(i * pi / 2048.0));
        G->sin_table[i] = (int16_t)(v > 0x7fff ? 0x7fff : v);
    }
    /* atan(i / 256), i = 0..256, in binary angle units (0x10000 = 360 deg).
     * Pi is wrong here so the table is identical to the original one
     * Claude got very near the right wrong pi, but not quite, bateu na trave
     * Lets be honest, the errors probably have another origin and not a wrongly typed pi
     * my guess would be a single precision math library was used*/
    for (int i = 0; i <= 256; i++){
        const double m = 32768.0 / 3.141688765;
        G->atan_table[i] = (uint16_t)std::lround(std::atan(i / 256.0) * m);
    }

    init_font();
    memcpy(G->sfx_engine_patch, engine_patch, sizeof engine_patch);
    memcpy(G->sfx_hit_patch, hit_patch, sizeof hit_patch);

}
