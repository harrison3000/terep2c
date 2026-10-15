/*
 * The map chooser.  Left: the directories of the current one ("..",
 * then the others; maps are marked).  Right: the selected map turning
 * in a voxel preview, and below it the render scale and the extra options.
 * A click selects a directory, a double click enters it or, if it is a
 * map, starts the game, like the Play button.
 */
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>
#include "imgui.h"
#include "game_renderer.hpp"
#include "gui.hpp"
#include "host.hpp"
#include "launcher.hpp"
#include "voxel_preview.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int PREVIEW_MIN_W = 320, PREVIEW_MAX_W = 960;    // rendered size; shown larger if needed
constexpr float LIST_SHARE = 0.36f;                         // of the window width, for the directories

struct Entry {
    std::string name;
    bool map;                     // has what the preview needs (map.pcx, col.pcx)
};

// What a directory holds: enough to preview it, and to play it (the game
// also needs at least car1.dat).
bool has_preview_files(const std::string &dir)
{
    return !find_file_nocase(dir, "map.pcx").empty() && !find_file_nocase(dir, "col.pcx").empty();
}

bool playable(const std::string &dir)
{
    return has_preview_files(dir) && !find_file_nocase(dir, "car1.dat").empty();
}

class Launcher {
public:
    // Starts where the last map was chosen, with it selected; else in the current directory.
    Launcher(HostOptions *host, const std::string &last) : host(host)
    {
        if (last.empty()) {
            enter(fs::current_path());
            return;
        }
        fs::path map(last);
        enter(map.parent_path());
        for (int i = 0; i < (int)entries.size(); i++)
            if (entries[i].name == map.filename().string())
                select(i);
    }

    // One frame of the UI; true once a map was chosen.
    bool frame(std::string *map);

private:
    HostOptions *host;
    GuiTexture preview_tex;
    std::vector<uint32_t> pixels;
    fs::path cwd;
    std::vector<Entry> entries;
    int selected = -1;
    VoxelPreview preview;
    bool can_play = false;
    float angle = 0;

    void enter(const fs::path &dir);
    void select(int i);
    fs::path selected_path() const { return selected < 0 ? cwd : cwd / entries[selected].name; }
};

void Launcher::enter(const fs::path &dir)
{
    std::error_code ec;
    fs::path p = fs::canonical(dir, ec);
    if (ec)
        return;
    cwd = p;
    entries.clear();
    if (cwd.has_parent_path() && cwd != cwd.root_path())
        entries.push_back({ "..", false });
    std::vector<Entry> dirs;
    for (fs::directory_iterator it(cwd, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || e2)
            continue;
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.')
            continue;
        dirs.push_back({ name, has_preview_files(it->path().string()) });
    }
    std::sort(dirs.begin(), dirs.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    selected = -1;
    preview.load(cwd.string());     // the directory itself may be a map
    can_play = playable(cwd.string());
}

void Launcher::select(int i)
{
    selected = i;
    std::string dir = selected_path().string();
    preview.load(dir);
    can_play = playable(dir);
}

bool Launcher::frame(std::string *map)
{
    bool chosen = false;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Terep2", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // the preview fills the width on the right, 16:9, leaving room for the options below it
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float show_w = avail.x * (1 - LIST_SHARE) - spacing;
    float show_h = show_w * 9 / 16;
    const float options_h = 300;
    if (show_h > avail.y - options_h) {
        show_h = avail.y - options_h;
        show_w = show_h * 16 / 9;
    }
    if (show_w < PREVIEW_MIN_W) {
        show_w = PREVIEW_MIN_W;
        show_h = show_w * 9 / 16;
    }

    // left: the directories
    ImGui::BeginChild("dirs", ImVec2(avail.x - show_w - spacing, 0), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", cwd.string().c_str());
    ImGui::Separator();
    fs::path go;
    for (int i = 0; i < (int)entries.size(); i++) {
        const Entry &e = entries[i];
        std::string label = (e.map ? "[map] " : "      ") + e.name;
        if (ImGui::Selectable(label.c_str(), selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            if (i != selected)
                select(i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.name != ".." && can_play)
                    chosen = true;
                else
                    go = cwd / e.name;
            }
        }
    }
    ImGui::EndChild();
    if (!go.empty())
        enter(go);

    // right: preview and options
    ImGui::SameLine();
    ImGui::BeginGroup();
    angle += ImGui::GetIO().DeltaTime * 0.35f;
    int w = (int)show_w < PREVIEW_MAX_W ? (int)show_w : PREVIEW_MAX_W, h = w * 9 / 16;
    pixels.resize((size_t)w * h);
    preview.render(pixels.data(), w, h, angle);
    preview_tex.upload(pixels.data(), w, h, false);
    ImGui::Image(preview_tex.id(), ImVec2(show_w, show_h));
    std::string name = selected_path().filename().string();
    if (preview.loaded())
        ImGui::Text("%s%s", name.c_str(), can_play ? "" : "  (no car1.dat: cannot be played)");
    else
        ImGui::TextDisabled("%s: no map.pcx and col.pcx", name.c_str());

    ImGui::SeparatorText("Render scale");
    ImGui::SetNextItemWidth(show_w * 0.5f);
    ImGui::SliderInt("##scale", &render_scale, 1, MAX_RENDER_SCALE, "%dx");
    ImGui::SameLine();
    ImGui::Text("%d x %d%s", screen_w(), screen_h(), render_scale == 1 ? "  (original)" : "");

    ImGui::SeparatorText("Renderer");
    ImGui::SetNextItemWidth(show_w * 0.5f);
    if (ImGui::BeginCombo("##renderer", game_renderers[host->renderer].name)) {
        for (int i = 0; i < game_renderer_count; i++) {
            const GameRenderer &r = game_renderers[i];
            ImGui::BeginDisabled(!r.available);
            if (ImGui::Selectable(r.name, host->renderer == i))
                host->renderer = i;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s%s", r.what, r.available ? "" : "\n(compiled without it: make sdl GLR=1 ...)");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + show_w * 0.5f);
    ImGui::TextDisabled("%s", game_renderers[host->renderer].what);
    ImGui::PopTextWrapPos();

    ImGui::SeparatorText("Misc options");
    float wrap = ImGui::GetCursorPosX() + show_w;
    ImGui::Checkbox("Minimap", &host->minimap);
    ImGui::SameLine();
    ImGui::TextDisabled("the map and the cars, numbered, in the bottom right corner");
    ImGui::Checkbox("Disable sound", &host->no_sound);
    ImGui::SameLine();
    ImGui::TextDisabled("no AdLib emulation at all (Nuked-OPL3)");
    for (int i = 0; i < extra_option_count; i++) {
        ImGui::Checkbox(extra_option_list[i].name, &(extra_options.*extra_option_list[i].flag));
        ImGui::SameLine();
        ImGui::PushTextWrapPos(wrap);
        ImGui::TextDisabled("%s", extra_option_list[i].what);
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!can_play);
    if (ImGui::Button("Play", ImVec2(120, 0)))
        chosen = true;
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::End();

    if (chosen)
        *map = selected_path().string();
    return chosen;
}

}  // namespace

bool launcher_run(std::string *map, HostOptions *host)
{
    bool chosen = false, quit = false;
    {
        Launcher ui(host, *map);
        while (!chosen && !quit) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                gui_event(e);
                bool esc = e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !e.key.repeat;
                if (e.type == SDL_QUIT || esc)
                    quit = true;
            }
            gui_begin();
            chosen = ui.frame(map);
            gui_end();
        }
    }
    return chosen;
}
