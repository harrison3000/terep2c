/*
 * The minimap: each cell of the map in its colour (col.pcx through the
 * game's palette), darker where it is low, x to the right and y down, as
 * the map is laid out (a camera with yaw 0 looks up the picture).  A car
 * is a square in its own colour at its reference point (where the camera
 * of F1 sits), with its number next to it in the game's font.  All of it
 * goes through an ImGui draw list over the game's picture.
 */
#include <cstdio>
#include "host.hpp"
#include "minimap.hpp"

static const ImU32 CAR_COLOURS[10] = {
    IM_COL32(255, 60, 60, 255), IM_COL32(255, 230, 40, 255), IM_COL32(60, 220, 255, 255), IM_COL32(255, 80, 255, 255),
    IM_COL32(80, 255, 90, 255), IM_COL32(255, 150, 30, 255), IM_COL32(255, 255, 255, 255), IM_COL32(120, 120, 255, 255),
    IM_COL32(180, 255, 180, 255), IM_COL32(255, 180, 200, 255),
};
constexpr int MAP_ALPHA = 150;          // about 60 %

Minimap::Minimap()
{
    static uint32_t pixels[256 * 256];
    for (int i = 0; i < 256 * 256; i++) {
        const uint8_t *rgb = host_palette(colormap()[i]);
        int light = 150 + heightmap()[i] * 106 / 255;          // 150..256: low ground darker
        pixels[i] = 0xff000000u | (uint32_t)(rgb[2] * light >> 8) << 16 |
                    (uint32_t)(rgb[1] * light >> 8) << 8 | (uint32_t)(rgb[0] * light >> 8);
    }
    map.upload(pixels, 256, 256, false);
}

// In the game's own font (font_glyph: columns, bit 0 at the top).
void Minimap::text(ImDrawList *dl, const char *s, float x, float y, float px, ImU32 c) const
{
    for (; *s; s++, x += px) {
        const uint8_t *col = font_glyph((uint8_t)*s);
        for (; *col; col++, x += px)
            for (int row = 0; row < 5; row++)
                if (*col >> row & 1)
                    dl->AddRectFilled(ImVec2(x, y + row * px), ImVec2(x + px, y + (row + 1) * px), c);
    }
}

void Minimap::draw(ImDrawList *dl, float x0, float y0, float k, int w, int h) const
{
    // laid out in the game's pixels, then put on the window
    const int size = h * 3 / 10, margin = h / 50;
    const float ax = x0 + (w - size - margin) * k, ay = y0 + (h - size - margin) * k, as = size * k;
    dl->AddImage(map.id(), ImVec2(ax, ay), ImVec2(ax + as, ay + as), ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, MAP_ALPHA));

    const int mark = size / 80 > 1 ? size / 80 : 1, px = h / 200 > 1 ? h / 200 : 1;   // mark: half the square
    for (int i = 0; i < world.num_cars; i++) {
        const Pos &p = car_get(i).ref_vertex().pos;
        float x = ax + (float)((p.x.raw >> 8) * (uint64_t)size >> 24) * k;  // 8.24 cells -> pixels
        float y = ay + (float)((p.y.raw >> 8) * (uint64_t)size >> 24) * k;
        ImU32 c = CAR_COLOURS[i % 10];
        dl->AddRectFilled(ImVec2(x - (mark + 1) * k, y - (mark + 1) * k), ImVec2(x + (mark + 2) * k, y + (mark + 2) * k),
                          IM_COL32(0, 0, 0, 255));
        dl->AddRectFilled(ImVec2(x - mark * k, y - mark * k), ImVec2(x + (mark + 1) * k, y + (mark + 1) * k), c);
        char n[4];
        snprintf(n, sizeof n, "%d", i + 1);
        text(dl, n, x + (mark + 2 * px) * k, y - 2 * px * k, px * k, c);
    }
}
