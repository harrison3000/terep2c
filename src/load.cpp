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
    if (!pcx_load(fname_map, heightmap()))
        return;
    if (!pcx_load(fname_maptex, segments.maptex.data()))
        return;

    FILE *f = data_fopen(fname_col);
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

    read_table(fname_fog, G->fog_table, sizeof G->fog_table);
    read_table(fname_smoke, G->shade_table, sizeof G->shade_table);
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
 * Without it the car uses textures.pcx. */
static void load_car_skin(Car &car, const char *name)
{
    FILE *f = data_fopen(name);
    if (!f)
        return;
    pcx_read_header(f);
    car.skin.assign(SEGMENT_SIZE, 0);
    pcx_decode(f, car.skin.data());
    fclose(f);
}

static const char *const car_dat_names[MAX_CARS] = { "car1.dat", "car2.dat", "car3.dat", "car4.dat", "car5.dat" };
static const char *const car_pcx_names[MAX_CARS] = { "car1.pcx", "car2.pcx", "car3.pcx", "car4.pcx", "car5.pcx" };
const char *car_dat_name(int i) { return car_dat_names[i]; }
const char *car_pcx_name(int i) { return car_pcx_names[i]; }

/* Cars are loaded until a carN.dat is missing, lined up along y = 0x7a00
 * every 256 units of x, at most 5 (the original's name tables hold 5; a
 * sixth car took its name from the bytes after them). */
static void load_cars()
{
    G->num_cars = 0;
    for (int i = 0; i < MAX_CARS; i++) {
        const char *dat = car_dat_name(i), *pcx = car_pcx_name(i);
        Car &car = cars[i];
        uint16_t size = load_car(car, dat, (uint16_t)(0x8000 - i * 256), 0x7a00);
        if (!size || car.points.empty())
            break;
        find_ref_vertex(car);
        load_car_skin(car, pcx);
        G->num_cars++;
    }
}

/* ------------------------------------------------------------------------ */

/* The segments and the cars, freed. */
void game_free()
{
    segments = Segments();
    for (Car &c : cars)
        c = Car();
}

/* Returns 0 on success.  orig f_init */
int game_init()
{
    /* everything outside the data segment as at the start of the program */
    memset(keys, 0xff, sizeof keys);
    game_free();
    game_frame_reset();

    G->num_cars = -2;
    G->gravity = 0x1800;
    G->spring_break_stretch = 0x800;
    G->spring_break_squash = (int16_t)0xf000;
    G->clip_left = 0;
    G->screen_cx = 160;
    G->clip_right = 319;
    G->clip_top = 0;
    G->screen_cy = 80;
    G->clip_bottom = 199;

    FILE *cfg = data_fopen(fname_cfg);   /* optional spring limits */
    if (cfg) {
        fread(&G->spring_break_stretch, 1, 2, cfg);
        fread(&G->spring_break_squash, 1, 2, cfg);
        fclose(cfg);
    }

    for (auto *s : { &segments.heightmap, &segments.colormap, &segments.textures, &segments.maptex, &segments.screen })
        s->assign(SEGMENT_SIZE, 0);
    load_map();
    pcx_load(fname_textures, segments.textures.data());      /* orig 1000:255c */
    load_cars();
    if (render_scale > 1)
        segments.hires_screen.assign((size_t)screen_w() * screen_h(), 0);
    sound_init();
    G->sim_running = 1;
    return 0;
}
