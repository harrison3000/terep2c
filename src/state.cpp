/*
 * The global state (terep.hpp) and its initial values, which the original
 * had in its data segment image (data.bin).  What game_init sets anyway
 * (gravity, viewport, spring limits) is not repeated here.
 */
#include "terep.hpp"

World      world;
Camera     camera;
Render     render;
MapTables  map_tables;
Contact    contact;

void state_reset()
{
    world = World();
    camera = Camera();
    render = Render();
    map_tables = MapTables();
    contact = Contact();

    /* cameras: chase camera (F4) in both views, at the middle of the map */
    camera.cam_mode[0] = camera.cam_mode[1] = 3;
    const Pos centre = { Coord::from_units(0x8080), Coord::from_units(0x9080), Coord::from_units(0x0400) };
    camera.chase_cam[0].pos = camera.chase_cam[1].pos = camera.eye = centre;
    camera.chase_height = 0x100;
    camera.chase_distance = 0x400;
    render.near_clip = 30;

    render.fog_start = 0x600;
}
