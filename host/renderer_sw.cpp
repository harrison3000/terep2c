// The software renderer: the game draws its own frame (game_frame), which
// goes through the palette into a texture shown as large as the window
// allows, keeping its shape.
#include <algorithm>
#include <vector>
#include "game_renderer.hpp"
#include "gui.hpp"
#include "host.hpp"

static std::vector<uint32_t> rgba;
static GuiTexture *picture;

bool software_start()
{
    picture = new GuiTexture;
    return true;
}

bool software_frame(int win_w, int win_h, PictureRect *where)
{
    if (game_frame())
        return false;
    const int w = screen_w(), h = screen_h();         // the scale may change (Esc menu)
    rgba.resize((size_t)w * h);
    const uint8_t *scr = screen();
    for (int i = 0; i < w * h; i++) {
        const uint8_t *c = host_palette(scr[i]);
        rgba[i] = 0xff000000u | (uint32_t)c[2] << 16 | (uint32_t)c[1] << 8 | c[0];
    }
    picture->upload(rgba.data(), w, h, false);
    const float k = std::min((float)win_w / w, (float)win_h / h);
    *where = { (win_w - w * k) / 2, (win_h - h * k) / 2, k };
    ImGui::GetBackgroundDrawList()->AddImage(picture->id(), ImVec2(where->x0, where->y0),
                                             ImVec2(where->x0 + w * k, where->y0 + h * k));
    return true;
}

void software_stop()
{
    delete picture;
    picture = nullptr;
    rgba.clear();
}
