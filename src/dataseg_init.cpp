/*
 * Initial state of the data segment: what the original's data segment
 * image (data.bin) held besides constants, which now live with the code
 * that uses them.  What game_init sets anyway (gravity, viewport, handles,
 * spring limits) is not repeated here.
 */
#include "terep.hpp"

void dataseg_init()
{
    memset(G, 0, sizeof *G);

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

    G->fog_start = 0x600;
    G->vis_hi = 0xff;
    G->vis_max = 0xff;
    G->poly_color = 0x0f0f;
}
