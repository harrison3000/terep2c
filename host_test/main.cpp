/*
 * Test host (headless): replays recordings made with the Win16 port and
 * compares the frames (replay() below).
 *
 *   terep2-headless [-s scale] [-E option]... -R|-C recdir mapdir
 *
 * -R: replay recdir/record.txt on mapdir, at 1x, writing the frames it
 * names as name_new.pgm; -C: the same, only comparing.
 * -s scale: render at that scale; the frames are compared by similarity
 * (block_difference) and the hashes with the port's own (hashes_Nx.txt).
 * -E option, --extra-options option: turn on an extra option (see
 * extra_option_list), such as a fix of a bug of the original, or "all".
 */
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include "../host/host.hpp"

// A PGM (P5, 320 x 200) from the recording; false if it cannot be read.
static bool read_pgm(const char *path, std::vector<uint8_t> *pixels)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char line[256];
    int fields[3], n = 0;                   // width, height, maxval
    bool ok = fgets(line, sizeof line, f) && !strncmp(line, "P5", 2);
    while (ok && n < 3 && fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        int used;
        for (char *p = line; n < 3 && sscanf(p, "%d%n", &fields[n], &used) == 1; p += used)
            n++;
    }
    ok = ok && n == 3 && fields[0] == 320 && fields[1] == 200;
    pixels->resize(320 * 200);
    ok = ok && fread(pixels->data(), 1, pixels->size(), f) == pixels->size();
    fclose(f);
    return ok;
}

// FNV-1a, 64 bits.
static uint64_t fnv1a_64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3;
    return h;
}

/* How far a frame at a higher scale is from the original's 1x frame orig:
 * the average colour of each 8 x 8 block (in 1x pixels), compared channel by
 * channel; the mean of the largest channel difference of every block.  The
 * finer projection moves edges and texels by less than a pixel, which this
 * mostly ignores: 1 to 10 for the same picture (a camera grazing the
 * ground changes the most), over 30 for another. */
constexpr double SIMILAR_MAX = 15;
static double block_difference(const uint8_t *orig)
{
    const int s = render_scale, B = 8;
    double total = 0;
    for (int by = 0; by < 200; by += B)
        for (int bx = 0; bx < 320; bx += B) {
            double a[3] = {}, b[3] = {};
            for (int y = by; y < by + B; y++)
                for (int x = bx; x < bx + B; x++) {
                    const uint8_t *o = host_palette(orig[y * 320 + x]);
                    for (int c = 0; c < 3; c++)
                        a[c] += o[c] * (double)s * s;
                    for (int yy = y * s; yy < (y + 1) * s; yy++)
                        for (int xx = x * s; xx < (x + 1) * s; xx++) {
                            const uint8_t *m = host_palette(screen()[yy * screen_w() + xx]);
                            for (int c = 0; c < 3; c++)
                                b[c] += m[c];
                        }
                }
            double worst = 0;
            for (int c = 0; c < 3; c++)
                worst = std::max(worst, std::abs(a[c] - b[c]) / (B * B * s * s));
            total += worst;
        }
    return total / ((320 / B) * (200 / B));
}

// Writes the frame buffer as recdir/name_new.pgm (P5, as the port does).
static void save_frame(const char *dir, const char *name, int renders)
{
    std::string mine = std::string(dir) + "/" + name;
    size_t dot = mine.rfind('.');
    mine.insert(dot != std::string::npos && dot > mine.rfind('/') ? dot : mine.size(), "_new");
    FILE *out = fopen(mine.c_str(), "wb");
    if (!out) {
        perror(mine.c_str());
        return;
    }
    fprintf(out, "P5\n#frame %d\n%d %d\n255\n", renders, screen_w(), screen_h());
    fwrite(screen(), 1, (size_t)screen_w() * screen_h(), out);
    fclose(out);
}

/* Replays recdir/record.txt, written by the Win16 port, on mapdir: one
 * action per line, told apart by its first letter, called exactly as recorded:
 *   (empty), # ...     nothing; the only lines allowed before the first init
 *   init               new data segment, game_init in mapdir
 *   P                  physics_tick
 *   R                  game_frame
 *   K: 0xcd            game_key (scancode, | 0x80 = released)
 *   frame: name.pgm    writes the frame buffer as recdir/name_new.pgm (not if
 *                      !save) and compares it with recdir/name.pgm
 *   frame_fnv1a_64: 0xcb3dbe0bb5f1a8b3
 *                      compares the frame buffer's FNV-1a hash with that one
 * Returns 0 if every frame matched, 1 if not (or on errors). */
static int replay(const char *rec, const char *mapdir, bool save, int scale)
{
    char dir[PATH_MAX], map[PATH_MAX];       // absolute: init changes to the map's directory
    if (!realpath(rec, dir) || !realpath(mapdir, map)) {
        perror(realpath(rec, dir) ? mapdir : rec);
        return 1;
    }
    std::string record = std::string(dir) + "/record.txt";
    FILE *in = fopen(record.c_str(), "r");
    if (!in) {
        perror(record.c_str());
        return 1;
    }
    render_scale = scale;                   // 1: the original's frame buffer
    /* At higher scales the recorded hashes (of 1x frames) do not apply: the
     * port's own, in recdir/hashes_Nx.txt, are written there (-R) or compared
     * with it (-C, if it exists), one per frame_fnv1a_64 line. */
    FILE *hashes = nullptr;
    if (scale > 1) {
        std::string hp = std::string(dir) + "/hashes_" + std::to_string(scale) + "x.txt";
        hashes = fopen(hp.c_str(), save ? "w" : "r");
        if (!hashes && save)
            perror(hp.c_str());
    }
    int renders = 0, same = 0, differ = 0, missing = 0, line_no = 0;
    std::string first_diff;
    bool started = false;
    char line[512];
    while (fgets(line, sizeof line, in)) {
        line_no++;
        line[strcspn(line, "\r\n")] = 0;
        unsigned key;
        unsigned long long hash;
        char name[400];
        if (!*line || *line == '#')         // empty or a comment
            continue;
        if (*line == 'i') {                 // init
            if (started)
                host_free_game();
            if (!host_init_dataseg() || chdir(map) != 0 || game_init() != 0) {
                fprintf(stderr, "%s: init failed\n", map);
                return 1;
            }
            started = true;
            continue;
        }
        if (!started) {
            fprintf(stderr, "%s before init\n", line);
            return 1;
        }
        if (*line == 'P') {
            physics_tick();
        } else if (*line == 'R') {
            game_frame();
            renders++;
        } else if (sscanf(line, "K: %x", &key) == 1) {
            game_key((uint8_t)key);
        } else if (sscanf(line, "frame_fnv1a_64: %llx", &hash) == 1) {
            if (scale > 1) {
                uint64_t mine = fnv1a_64(screen(), (size_t)screen_w() * screen_h());
                unsigned long long saved;
                if (!hashes)
                    continue;
                if (save) {
                    fprintf(hashes, "0x%016llx\n", (unsigned long long)mine);
                    continue;
                }
                if (fscanf(hashes, "%llx", &saved) == 1 && saved == mine) {
                    same++;
                    continue;
                }
                printf("record.txt:%d, after %d renders: fnv1a 0x%016llx, saved 0x%016llx\n", line_no, renders,
                       (unsigned long long)mine, saved);
                if (!differ++)
                    first_diff = "at record.txt:" + std::to_string(line_no);
                continue;
            }
            uint64_t mine = fnv1a_64(screen(), 320 * 200);
            if (mine == hash) {
                same++;
                continue;
            }
            printf("record.txt:%d, after %d renders: fnv1a 0x%016llx, recorded 0x%016llx\n", line_no, renders,
                   (unsigned long long)mine, hash);
            if (!differ++)
                first_diff = "at record.txt:" + std::to_string(line_no);
        } else if (sscanf(line, "frame: %399s", name) == 1) {
            const uint8_t *scr = screen();
            if (save)
                save_frame(dir, name, renders);
            std::vector<uint8_t> orig;
            if (!read_pgm((std::string(dir) + "/" + name).c_str(), &orig)) {
                printf("%s: original missing or unreadable\n", name);
                missing++;
                continue;
            }
            if (scale > 1) {
                double d = block_difference(orig.data());
                bool ok = d <= SIMILAR_MAX;
                printf("%s: %s (block difference %.1f)\n", name, ok ? "similar" : "NOT SIMILAR", d);
                if (ok)
                    same++;
                else if (!differ++)
                    first_diff = name;
                continue;
            }
            int diff = 0, x0 = 320, y0 = 200, x1 = -1, y1 = -1;
            for (int y = 0; y < 200; y++)
                for (int x = 0; x < 320; x++)
                    if (scr[y * 320 + x] != orig[y * 320 + x]) {
                        diff++;
                        x0 = std::min(x0, x), x1 = std::max(x1, x);
                        y0 = std::min(y0, y), y1 = std::max(y1, y);
                    }
            if (!diff) {
                printf("%s: same\n", name);
                same++;
            } else {
                printf("%s: %d pixels differ (%.2f%%), in x %d..%d, y %d..%d\n", name, diff, diff * 100.0 / 64000,
                       x0, x1, y0, y1);
                if (!differ++)
                    first_diff = name;
            }
        } else {
            printf("unhandled line: %s\n", line);
        }
    }
    fclose(in);
    if (hashes)
        fclose(hashes);
    printf("%d frames: %d same, %d differ%s%s, %d without original\n", same + differ + missing, same, differ,
           differ ? ", first " : "", first_diff.c_str(), missing);
    return differ || missing ? 1 : 0;
}

int main(int argc, char **argv)
{
    int a = 1;
    const char *rec = nullptr;
    bool save = true;
    int scale = 1;
    for (; a + 1 < argc && argv[a][0] == '-'; a += 2) {
        std::string_view opt = argv[a];
        if (opt == "-R" || opt == "-C") {
            rec = argv[a + 1];
            save = opt == "-R";
        } else if (opt == "-s") {
            scale = atoi(argv[a + 1]);
        } else if (opt != "-E" && opt != "--extra-options") {
            break;
        } else if (!extra_option_enable(argv[a + 1])) {
            fprintf(stderr, "unknown option %s; known:", argv[a + 1]);
            for (int i = 0; i < extra_option_count; i++)
                fprintf(stderr, " %s", extra_option_list[i].name);
            fprintf(stderr, " all\n");
            return 2;
        }
    }
    if (!rec || argc - a != 1 || scale < 1 || scale > MAX_RENDER_SCALE) {
        fprintf(stderr, "usage: %s [-s 1..%d] [-E option]... -R|-C recdir mapdir\n", argv[0], MAX_RENDER_SCALE);
        return 2;
    }
    return replay(rec, argv[a], save, scale);
}
