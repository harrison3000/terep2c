/*
 * One rendered frame and the keyboard.
 */
#include "terep.hpp"

uint8_t keys[128];                  /* all released (0xff) once game_init ran */
ExtraOptions extra_options;

const ExtraOptionInfo extra_option_list[] = {
    { "fix-ice-bug", &ExtraOptions::fix_ice_bug, "Fix the integer underflow that make some broken ice become a huge pillar" },
    { "hide-banner", &ExtraOptions::hide_banner, "Hide the \"GAMBIARRA FOREVER!\" line added by the Win16 port" },
};
const int extra_option_count = sizeof extra_option_list / sizeof extra_option_list[0];

bool extra_option_enable(const char *name)
{
    bool found = false;
    for (const ExtraOptionInfo &f : extra_option_list) {
        if (!strcmp(name, "all") || !strcmp(name, f.name)) {
            extra_options.*f.flag = true;
            found = true;
        }
    }
    return found;
}

const char title_text[] = "-DEFORMERS-  ATOMIC VEHICLE SIMULATION SYSTEM (C)NAGYMATHE DENES 1996-1997";
const char subtitle_text[] = "WORK-IN-PROGRESS DEMO VERSION";

/* Line added by the Win16 port, drawn in a cycling colour. */
static const char port_banner[] = "GAMBIARRA FOREVER!";
static uint8_t banner_color;

/* What game_frame keeps from one frame to the next outside the data
 * segment, as at the start of the program (called by game_init). */
void game_frame_reset()
{
    banner_color = 0;
    poly_hidden = false;
}

/* In the original's 320x200 coordinates, scaled to the screen. */
static void set_viewport(int16_t top, int16_t center_y, int16_t bottom)
{
    int16_t s = (int16_t)render_scale;
    render.clip_left = 0;
    render.screen_cx = (int16_t)(160 * s);
    render.clip_right = (int16_t)(320 * s - 1);
    render.clip_top = (int16_t)(top * s);
    render.screen_cy = (int16_t)(center_y * s);
    render.clip_bottom = (int16_t)((bottom + 1) * s - 1);
}

static void draw_view(int view)
{
    camera_update(view);
    Angle yaw = camera.eye_angles.yaw;
    camera.yaw_sin7 = (int16_t)(fx_sin(yaw).raw >> 7);
    camera.yaw_cos7 = (int16_t)(fx_cos(yaw).raw >> 7);
    build_matrix(camera.eye_angles, camera.view_matrix);
    transpose(camera.view_matrix);
    if (view == 0)
        clear_screen();
    draw_terrain();
    draw_particles();
    engine_sound(world.player_car[view], (uint8_t)view);
}

static bool held(uint8_t scancode)
{
    return !(keys[scancode] & 0x80);
}

/* Render the view(s) and the text, then handle the keys.  Returns 1 when
 * Esc was pressed.  orig FUN_main_render */
int game_frame()
{
    if (!world.split_screen) {
        set_viewport(0, 0x50, 0xc7);
        draw_view(0);
    } else {
        set_viewport(0, 0x32, 0x62);
        draw_view(0);
        set_viewport(0x64, 0x96, 0xc7);
        draw_view(1);
    }

    set_viewport(0, 0x50, 0xc7);
    draw_text(title_text, 10, 10, 15);
    draw_text(subtitle_text, 100, 190, 15);
    if (!extra_options.hide_banner)
        draw_text(port_banner, 5, 190, (uint8_t)(banner_color >> 2));
    banner_color++;

    /* numeric keypad: chase camera height (+ -) and distance (/ *) */
    if (held(78) && camera.chase_height <= 1000)
        camera.chase_height += 20;
    if (held(74) && camera.chase_height >= 50)
        camera.chase_height -= 20;
    if (held(53) && camera.chase_distance <= 0x1000)
        camera.chase_distance += 40;
    if (held(55) && camera.chase_distance >= 0x100)
        camera.chase_distance -= 40;

    switch (keys[0]) {                  /* last key pressed */
    case 0x01:                          /* Esc */
        return 1;
    case 0x0f:                          /* Tab: next car for view 1 */
        if (++world.player_car[0] >= (uint16_t)world.num_cars)
            world.player_car[0] = 0;
        break;
    case 0x10:                          /* Q: next car for view 2 */
        if (++world.player_car[1] >= (uint16_t)world.num_cars)
            world.player_car[1] = 0;
        break;
    case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:     /* F1..F5 */
        camera.cam_mode[0] = (uint8_t)(keys[0] - 0x3b);
        break;
    case 0x40:                          /* F6: cycle view 2's camera */
        if (++camera.cam_mode[1] >= 5)
            camera.cam_mode[1] = 0;
        break;
    case 0x1a:                          /* [ */
        world.gravity += 50;
        break;
    case 0x1b:                          /* ] */
        world.gravity -= 50;
        break;
    case 0x44:                          /* F10: fog distance */
        render.fog_start ^= 0x600;
        break;
    case 0x43:                          /* F9: split screen */
        world.split_screen ^= 1;
        break;
    default:
        return 0;
    }
    keys[0] = 0;
    return 0;
}

/* Keyboard scancode (bit 7 = release).  keys[sc] has bit 7 clear while
 * the key is down; keys[0] remembers the last key pressed.
 * orig FUN_keyboard_56df (the DOS version's INT 9 handler) */
void game_key(uint8_t scancode)
{
    uint8_t sc = scancode & 0x7f;
    if (scancode & 0x80) {
        keys[sc] = 0xff;
        keys[0] = 0;
    } else {
        keys[sc] &= 0x7f;
        keys[0] = sc;
    }
}

