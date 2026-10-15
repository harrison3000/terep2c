/*
 * SDL2 host: a window, the keyboard and the original timing (physics at
 * 120 Hz, display at 60 Hz, as in the Win16 port).
 *
 *   terep2vibed [-s scale] [-E option]... [-n] [-m] [-r renderer] [mapdir]
 *
 * Without mapdir a chooser opens first (launcher.cpp), starting in the
 * current directory, with the scale and the extra options as given here.
 *
 * scale: render at 320*scale x 200*scale (1 = the original picture).
 * -E option, --extra-options option: turn on an extra option (see extra_option_list),
 * such as a fix of a bug of the original, or "all".
 *
 * -n, --no-sound: no sound (the AdLib emulator is not started).
 * -m, --minimap: the map and the cars in the bottom right corner.
 * -r, --renderer name: software (the default) or opengl (if compiled in).
 *
 * Esc quits, or goes back to the chooser if the game was started from it.
 */
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>
#include <SDL.h>
#include "host.hpp"
#include "game_renderer.hpp"
#include "gui.hpp"
#include "launcher.hpp"
#include "minimap.hpp"
#include "opl_sound.hpp"

constexpr int MIN_ZOOM = 3;                // a 320x200 picture opens at least this many times bigger

// Initial window size: the picture at the chosen scale, enlarged by a whole
// factor while it is smaller than 320x200 x MIN_ZOOM, then reduced by a
// whole factor while it does not fit on the screen.
static void window_size(int *w, int *h)
{
    int zoom = (MIN_ZOOM + render_scale - 1) / render_scale, div = 1;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
        auto fits = [&](int z, int d) { return screen_w() * z / d <= usable.w && screen_h() * z / d <= usable.h; };
        while (zoom > 1 && !fits(zoom, 1))
            zoom--;
        while (!fits(zoom, div) && div < render_scale)
            div++;
    }
    *w = screen_w() * zoom / div;
    *h = screen_h() * zoom / div;
}

// SDL scancode -> PC keyboard scancode (set 1), what the game expects.
static uint8_t pc_scancode(SDL_Scancode s)
{
    static const struct { SDL_Scancode sdl; uint8_t pc; } map[] = {
        { SDL_SCANCODE_ESCAPE, 0x01 },
        { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 }, { SDL_SCANCODE_3, 0x04 },
        { SDL_SCANCODE_4, 0x05 }, { SDL_SCANCODE_5, 0x06 }, { SDL_SCANCODE_6, 0x07 },
        { SDL_SCANCODE_7, 0x08 }, { SDL_SCANCODE_8, 0x09 }, { SDL_SCANCODE_9, 0x0a },
        { SDL_SCANCODE_0, 0x0b }, { SDL_SCANCODE_MINUS, 0x0c }, { SDL_SCANCODE_EQUALS, 0x0d },
        { SDL_SCANCODE_BACKSPACE, 0x0e }, { SDL_SCANCODE_TAB, 0x0f },
        { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 },
        { SDL_SCANCODE_R, 0x13 }, { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 },
        { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 }, { SDL_SCANCODE_O, 0x18 },
        { SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1a }, { SDL_SCANCODE_RIGHTBRACKET, 0x1b },
        { SDL_SCANCODE_RETURN, 0x1c }, { SDL_SCANCODE_LCTRL, 0x1d }, { SDL_SCANCODE_RCTRL, 0x1d },
        { SDL_SCANCODE_A, 0x1e }, { SDL_SCANCODE_S, 0x1f }, { SDL_SCANCODE_D, 0x20 },
        { SDL_SCANCODE_F, 0x21 }, { SDL_SCANCODE_G, 0x22 }, { SDL_SCANCODE_H, 0x23 },
        { SDL_SCANCODE_J, 0x24 }, { SDL_SCANCODE_K, 0x25 }, { SDL_SCANCODE_L, 0x26 },
        { SDL_SCANCODE_SEMICOLON, 0x27 }, { SDL_SCANCODE_APOSTROPHE, 0x28 }, { SDL_SCANCODE_GRAVE, 0x29 },
        { SDL_SCANCODE_LSHIFT, 0x2a }, { SDL_SCANCODE_BACKSLASH, 0x2b },
        { SDL_SCANCODE_Z, 0x2c }, { SDL_SCANCODE_X, 0x2d }, { SDL_SCANCODE_C, 0x2e },
        { SDL_SCANCODE_V, 0x2f }, { SDL_SCANCODE_B, 0x30 }, { SDL_SCANCODE_N, 0x31 },
        { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 }, { SDL_SCANCODE_PERIOD, 0x34 },
        { SDL_SCANCODE_SLASH, 0x35 }, { SDL_SCANCODE_KP_DIVIDE, 0x35 }, { SDL_SCANCODE_RSHIFT, 0x36 },
        { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 },
        { SDL_SCANCODE_F1, 0x3b }, { SDL_SCANCODE_F2, 0x3c }, { SDL_SCANCODE_F3, 0x3d },
        { SDL_SCANCODE_F4, 0x3e }, { SDL_SCANCODE_F5, 0x3f }, { SDL_SCANCODE_F6, 0x40 },
        { SDL_SCANCODE_F7, 0x41 }, { SDL_SCANCODE_F8, 0x42 }, { SDL_SCANCODE_F9, 0x43 },
        { SDL_SCANCODE_F10, 0x44 },
        { SDL_SCANCODE_KP_7, 0x47 }, { SDL_SCANCODE_HOME, 0x47 },
        { SDL_SCANCODE_KP_8, 0x48 }, { SDL_SCANCODE_UP, 0x48 },
        { SDL_SCANCODE_KP_9, 0x49 }, { SDL_SCANCODE_PAGEUP, 0x49 },
        { SDL_SCANCODE_KP_MINUS, 0x4a },
        { SDL_SCANCODE_KP_4, 0x4b }, { SDL_SCANCODE_LEFT, 0x4b },
        { SDL_SCANCODE_KP_5, 0x4c },
        { SDL_SCANCODE_KP_6, 0x4d }, { SDL_SCANCODE_RIGHT, 0x4d },
        { SDL_SCANCODE_KP_PLUS, 0x4e },
        { SDL_SCANCODE_KP_1, 0x4f }, { SDL_SCANCODE_END, 0x4f },
        { SDL_SCANCODE_KP_2, 0x50 }, { SDL_SCANCODE_DOWN, 0x50 },
        { SDL_SCANCODE_KP_3, 0x51 }, { SDL_SCANCODE_PAGEDOWN, 0x51 },
        { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_INSERT, 0x52 },
        { SDL_SCANCODE_KP_PERIOD, 0x53 }, { SDL_SCANCODE_DELETE, 0x53 },
    };
    for (const auto &m : map)
        if (m.sdl == s)
            return m.pc;
    return 0;
}

enum class Ending { BACK, QUIT, FAILED };   // Esc, window closed, could not start

// The game loop: physics at 120 Hz, the picture at 60 Hz.
static Ending run(const GameRenderer &renderer, bool minimap)
{
    if (!renderer.start()) {
        fprintf(stderr, "the %s renderer could not start\n", renderer.name);
        return Ending::FAILED;
    }
    Minimap *mini = minimap ? new Minimap() : nullptr;

    const uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t next_tick = SDL_GetPerformanceCounter(), next_frame = next_tick;
    Ending end = Ending::BACK;
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            gui_event(e);
            if (e.type == SDL_QUIT) {
                end = Ending::QUIT;
                running = false;
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                uint8_t sc = pc_scancode(e.key.keysym.scancode);
                if (sc == 0x01 && e.type == SDL_KEYDOWN)
                    running = false;
                else if (sc)
                    game_key(e.type == SDL_KEYUP ? sc | 0x80 : sc);
            }
        }

        uint64_t now = SDL_GetPerformanceCounter();
        while (now >= next_tick) {          // 120 Hz physics
            physics_tick();
            next_tick += freq / 120;
        }
        if (now >= next_frame) {            // 60 Hz display
            next_frame += freq / 60;
            if (now > next_frame + freq / 4)
                next_frame = now;           // do not try to catch up after a stall
            gui_begin();
            const ImVec2 win = ImGui::GetIO().DisplaySize;
            PictureRect at;
            if (!renderer.frame((int)win.x, (int)win.y, &at))
                running = false;
            if (mini)
                mini->draw(ImGui::GetForegroundDrawList(), at.x0, at.y0, at.k, screen_w(), screen_h());
            gui_end();
        } else {
            SDL_Delay(1);
        }
    }
    delete mini;
    renderer.stop();
    return end;
}

// One game on map, from a fresh state, until Esc or the window is
// closed; everything it used is freed again.
static Ending play(const std::string &map, const HostOptions &host)
{
    char here[PATH_MAX];
    if (!getcwd(here, sizeof here))
        return Ending::FAILED;
    if (!host.no_sound)
        opl_sound_start();                          // before game_init, which sets up the AdLib
    Ending end = Ending::FAILED;
    if (!host_init_state()) {
        opl_sound_stop();
        return end;
    }
    if (chdir(map.c_str()) != 0)
        perror(map.c_str());
    else if (game_init() != 0 || world.num_cars <= 0)
        fprintf(stderr, "could not load the map in %s (no car1.dat?)\n", map.c_str());
    else
        end = run(game_renderers[host.renderer], host.minimap);
    opl_sound_stop();
    host_free_game();
    if (chdir(here) != 0)
        perror(here);
    return end;
}

int main(int argc, char **argv)
{
    std::string map;
    int a = 1;
    bool scale_given = false;
    HostOptions host;
    for (; a < argc && argv[a][0] == '-'; a++) {
        std::string_view opt = argv[a];
        if (opt == "-n" || opt == "--no-sound") {
            host.no_sound = true;
        } else if (opt == "-m" || opt == "--minimap") {
            host.minimap = true;
        } else if ((opt == "-r" || opt == "--renderer") && a + 1 < argc) {
            int r = game_renderer_find(argv[++a]);
            if (r < 0 || !game_renderers[r].available) {
                fprintf(stderr, "%s renderer %s; known:", r < 0 ? "unknown" : "not compiled in:", argv[a]);
                for (int i = 0; i < game_renderer_count; i++)
                    if (game_renderers[i].available)
                        fprintf(stderr, " %s", game_renderers[i].name);
                fprintf(stderr, "\n");
                return 2;
            }
            host.renderer = r;
        } else if (opt == "-s" && a + 1 < argc) {
            render_scale = atoi(argv[++a]);
            scale_given = true;
        } else if ((opt == "-E" || opt == "--extra-options") && a + 1 < argc) {
            if (!extra_option_enable(argv[++a])) {
                fprintf(stderr, "unknown option %s; known:", argv[a]);
                for (int i = 0; i < extra_option_count; i++)
                    fprintf(stderr, " %s", extra_option_list[i].name);
                fprintf(stderr, " all\n");
                return 2;
            }
        } else {
            break;
        }
    }
    if (a < argc)
        map = argv[a];
    if (render_scale < 1 || render_scale > MAX_RENDER_SCALE) {
        fprintf(stderr, "scale must be 1..%d\n", MAX_RENDER_SCALE);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    const bool chooser = map.empty();
    int win_w = 320 * MIN_ZOOM, win_h = 200 * MIN_ZOOM;
    if (!chooser)
        window_size(&win_w, &win_h);
    SDL_Window *win = gui_open(win_w, win_h);
    if (!win)
        return 1;
    if (chooser) {
        if (!scale_given)
            render_scale = 3;                       // the chooser starts at 3x
        extra_options.fix_ice_bug = true;           // offered on there; the game alone stays faithful
    }

    // Esc in a game started from the chooser goes back to it, for another map
    for (;;) {
        if (chooser) {
            SDL_SetWindowSize(win, 320 * MIN_ZOOM, 200 * MIN_ZOOM);
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            if (!launcher_run(&map, &host))
                break;
            window_size(&win_w, &win_h);            // for the scale chosen there
            SDL_SetWindowSize(win, win_w, win_h);
            SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        Ending end = play(map, host);
        if (end == Ending::FAILED && !chooser) {
            gui_close();
            SDL_Quit();
            return 1;
        }
        if (end == Ending::QUIT || !chooser)
            break;
    }
    gui_close();
    SDL_Quit();
    return 0;
}

