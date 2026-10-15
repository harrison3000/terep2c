/*
 * Loading a track directory: the map layers (PCX images), the fog and
 * translucency tables, and up to five cars.
 *
 * File names are the ones stored in the data segment; the host opens them
 * (data_fopen, case-insensitive on systems that care).
 */
#include <cstdlib>
#include "terep.hpp"

/* ------------------------------------------------------------------------
 * PCX (8-bit, single plane, RLE)
 * ---------------------------------------------------------------------- */

/* Read the 128-byte header.  Like the original, the check only rejects
 * fewer than 8 bits per pixel, or 8 bits with no planes.  orig 1000:5a95 */
static bool pcx_read_header(FILE *f)
{
    fread(&G->pcx_header, 1, sizeof G->pcx_header, f);
    G->pcx_width = (uint16_t)(G->pcx_header.xmax - G->pcx_header.xmin + 1);
    G->pcx_height = (uint16_t)(G->pcx_header.ymax - G->pcx_header.ymin + 1);
    uint8_t bpp = G->pcx_header.bits_per_pixel;
    return !(bpp < 8 || (bpp == 8 && G->pcx_header.planes == 0));
}

/* Decode the image into a segment, at most 256 x 256 pixels: wider
 * images are cropped (rows are packed, not padded to 256).  The file is
 * read through G->pcx_buffer in 768-byte chunks.  orig 1000:5acf */
static void pcx_decode(FILE *f, uint8_t *dst)
{
    uint16_t in = sizeof G->pcx_buffer;     /* buffer empty */
    uint16_t out = 0;
    uint8_t value = 0, repeat = 0;
    int16_t rows = (int16_t)G->pcx_height > 0x100 ? 0x100 : (int16_t)G->pcx_height;
    uint16_t row = (uint16_t)rows;

    do {
        uint16_t x = 0;
        do {
            if (repeat) {
                repeat--;
            } else {
                if (in == sizeof G->pcx_buffer) {
                    fread(G->pcx_buffer, 1, sizeof G->pcx_buffer, f);
                    in = 0;
                }
                value = G->pcx_buffer[in++];
                if ((value & 0xc0) == 0xc0) {
                    repeat = (uint8_t)((value & 0x3f) - 1);   /* a count of 0 gives 255 */
                    if (in == sizeof G->pcx_buffer) {
                        fread(G->pcx_buffer, 1, sizeof G->pcx_buffer, f);
                        in = 0;
                    }
                    value = G->pcx_buffer[in++];
                }
            }
            if (x < 0x100)
                dst[out++] = value;
            x++;
        } while (x < G->pcx_width);
    } while (--row);
}

/* orig 1000:5a60 */
static bool pcx_load(const char *name, uint8_t *dst)
{
    FILE *f = data_fopen(name);
    if (!f)
        return false;
    if (!pcx_read_header(f)) {
        fclose(f);
        return false;
    }
    fseek(f, 0x80, SEEK_SET);
    pcx_decode(f, dst);
    fclose(f);
    return true;
}

static void read_table(const char *name, void *dst, size_t size)
{
    FILE *f = data_fopen(name);
    if (!f)
        return;
    fread(dst, 1, size, f);
    fclose(f);
}

/* Height map, terrain textures, colour map with the palette, fog and
 * translucency tables.  Stops at the first missing image (the original
 * goes on with whatever is in memory).  orig 1000:24c0 */
static void load_map()
{
    if (!pcx_load(G->fname_map, heightmap()))
        return;
    if (!pcx_load(G->fname_maptex, seg_ptr(G->seg_maptex)))
        return;

    FILE *f = data_fopen(G->fname_col);
    if (!f)
        return;
    if (!pcx_read_header(f) || fseek(f, -0x300, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    fread(G->palette, 1, sizeof G->palette, f);
    fseek(f, 0x80, SEEK_SET);
    pcx_decode(f, colormap());
    fclose(f);

    read_table(G->fname_fog, G->fog_table, sizeof G->fog_table);
    read_table(G->fname_smoke, G->shade_table, sizeof G->shade_table);
}

/* ------------------------------------------------------------------------
 * Cars
 * ---------------------------------------------------------------------- */

/* Load a car file and put it on the map at (x, y), 100 units above the
 * ground.  Returns the file size, 0 if it is missing.  orig 1000:2454 */
static uint16_t load_car(Car &car, const char *name, uint16_t x, uint16_t y)
{
    FILE *f = data_fopen(name);
    if (!f)
        return 0;
    uint16_t size = car_read(car, f);
    fclose(f);

    int16_t z = (int16_t)(terrain_height(x, y) + 100);
    Vec3 offset = { (int32_t)Coord::from_units(x).raw, (int32_t)Coord::from_units(y).raw,
                    (int32_t)Coord::from_units((uint16_t)z).raw };
    for (CarVertex &v : car.points)
        v.pos += offset;
    return size;
}

/* The reference vertex (camera anchor) is the one marked 0xffff, else the
 * first.  The original takes its loop count from the low word of the
 * first vertex's x instead of the vertex count (0 counting as 65536), and
 * would go on reading past the vertices; here the search stops at the
 * last vertex.  orig 1000:2431 */
static void find_ref_vertex(Car &car)
{
    uint32_t count = (uint16_t)car.points[0].pos.x.raw;
    if (count == 0)
        count = 0x10000;
    car.ref_index = 0;
    for (uint32_t i = 0; i < count && i < car.points.size(); i++) {
        if (car.points[i].wheel == 0xffff) {
            car.ref_index = (uint16_t)i;
            return;
        }
    }
}

/* The car's skin: a PCX of up to 256 pixels width in its own segment.
 * Without it the car keeps the segment named in its file (0 = textures.pcx). */
static void load_car_skin(Car &car, const char *name)
{
    FILE *f = data_fopen(name);
    if (!f)
        return;
    pcx_read_header(f);
    uint16_t w = (int16_t)G->pcx_width > 0x100 ? 0x100 : G->pcx_width;
    uint16_t paragraphs = (uint16_t)(((uint16_t)(w * G->pcx_height) >> 4) + 1);
    uint16_t seg;
    if (seg_alloc(paragraphs, &seg) == 0) {
        car.texture_seg = seg;
        pcx_decode(f, seg_ptr(seg));
    }
    fclose(f);
}

/* A word of the data segment at a computed offset. */
static uint16_t &ds_word(size_t ofs)
{
    return *reinterpret_cast<uint16_t *>(ds_ptr((uint16_t)ofs));
}

/* Cars are loaded until a carN.dat is missing, lined up along y = 0x7a00
 * every 256 units of x.  The name tables hold 5 entries; a sixth car would
 * take its name from the bytes after them.  car_ofs still records where
 * the original would have put each car in car_pool (back to back), which
 * the test host uses (host_test/car_image.cpp). */
static void load_cars()
{
    G->num_cars = 0;
    G->car_ofs[0] = (uint16_t)offsetof(DataSeg, car_pool);
    for (int i = 0; i < (int)(sizeof cars / sizeof cars[0]); i++) {
        /* read through ds_ptr: i can run past the 5-entry tables */
        const char *dat = (const char *)ds_ptr(ds_word(offsetof(DataSeg, car_dat_name) + 2 * i));
        const char *pcx = (const char *)ds_ptr(ds_word(offsetof(DataSeg, car_pcx_name) + 2 * i));
        Car &car = cars[i];
        uint16_t size = load_car(car, dat, (uint16_t)(0x8000 - i * 256), 0x7a00);
        if (!size || car.points.empty())
            break;
        find_ref_vertex(car);
        load_car_skin(car, pcx);
        G->num_cars++;
        if (i + 1 < (int)(sizeof G->car_ofs / sizeof G->car_ofs[0]))
            G->car_ofs[i + 1] = (uint16_t)(G->car_ofs[i] + size);
    }
}

/* ------------------------------------------------------------------------ */

/* Returns 0 on success.  orig f_init */
int game_init()
{
    /* everything outside the data segment as at the start of the program */
    memset(keys, 0xff, sizeof keys);
    for (Car &c : cars)
        c = Car();
    game_frame_reset();
    free(hires_screen);
    hires_screen = nullptr;
    memset(span_x, 0, sizeof span_x);   /* zero, as the data segment's were */
    memset(span_a, 0, sizeof span_a);
    memset(span_b, 0, sizeof span_b);

    G->num_cars = -2;
    G->unk_ec50 = 0x78;
    G->gravity = 0x1800;
    G->spring_break_stretch = 0x800;
    G->spring_break_squash = (int16_t)0xf000;
    G->clip_left = 0;
    G->screen_cx = 160;
    G->clip_right = 319;
    G->clip_top = 0;
    G->screen_cy = 80;
    G->clip_bottom = 199;

    FILE *cfg = data_fopen(G->fname_cfg);   /* optional spring limits */
    if (cfg) {
        fread(&G->spring_break_stretch, 1, 2, cfg);
        fread(&G->spring_break_squash, 1, 2, cfg);
        fclose(cfg);
    }

    if (seg_alloc(0x1000, &G->seg_heightmap) || seg_alloc(0x1000, &G->seg_colormap) ||
        seg_alloc(0x1000, &G->seg_textures) || seg_alloc(0x1000, &G->seg_maptex))
        return 1;
    load_map();
    pcx_load(G->fname_textures, seg_ptr(G->seg_textures));      /* orig 1000:255c */
    load_cars();

    if (seg_alloc(0xfa0, &G->seg_screen))
        return 1;
    if (render_scale > 1) {
        hires_screen = static_cast<uint8_t *>(calloc((size_t)screen_w(), (size_t)screen_h()));
        if (!hires_screen)
            return 1;
    }
    sound_init();
    G->sim_running = 1;
    return 0;
}
