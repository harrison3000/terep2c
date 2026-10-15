/*
 * The five cameras (F1..F5; F6 cycles the second player's):
 *   0  mounted on the car, following its roll and pitch
 *   1  mounted on the car, pitch and roll smoothed from the terrain ahead
 *   2  standing still where the chase camera was left, looking at the car
 *   3  chase camera following behind the car
 *   4  looking at the middle of all cars from behind
 * Each one sets camera.eye and camera.eye_angles.
 */
#include "terep.hpp"

/* Middle 16 bits of a 16.16 unit vector component (1.0 = 0x100). */
static int16_t mid16(int32_t v)
{
    return (int16_t)((uint32_t)v >> 8);
}

/* The eye at the car's reference vertex; returns the car's axes (across,
 * along).  The original kept them in its data segment (00e0, 00ec). */
struct CarAxes {
    Vec3 x, y;
};
static CarAxes eye_at_vertex(Car &car)
{
    camera.eye = car.ref_vertex().pos;
    return { car.axis_x(), car.axis_y() };
}

/* orig 1000:0693 */
static void camera_on_car(Car &car)
{
    CarAxes axes = eye_at_vertex(car);
    int16_t xx = mid16(axes.x.x), xy = mid16(axes.x.y), xz = mid16(axes.x.z);
    int16_t yx = mid16(axes.y.x), yy = mid16(axes.y.y), yz = mid16(axes.y.z);

    Angle yaw = fx_atan2((int16_t)-yx, yy);
    camera.eye_angles.yaw = yaw;

    /* roll: the side axis against the horizontal, signed by whether that
     * axis points left or right of the heading */
    int16_t side_len = (int16_t)vec2_length(xx, xy);
    Angle side_dir = fx_atan2((int16_t)-xx, xy);
    if (yaw - side_dir < Angle{})
        side_len = (int16_t)-side_len;
    camera.eye_angles.roll = -fx_atan2(xz, side_len);

    int16_t fwd_len = (int16_t)vec2_length(yx, yy);
    camera.eye_angles.pitch = -fx_atan2(yz, fwd_len);
}

/* orig 1000:073f */
static void camera_on_car_smooth(Car &car)
{
    CarAxes axes = eye_at_vertex(car);
    uint16_t ex = camera.eye.x.units(), ey = camera.eye.y.units(), ez = camera.eye.z.units();

    Angle yaw = fx_atan2((int16_t)-mid16(axes.y.x), mid16(axes.y.y));
    camera.eye_angles.yaw = yaw;

    /* the ground at (sin, -cos) of an angle >> shift from the eye */
    auto height_towards = [&](Angle a, int shift) {
        return terrain_height((uint16_t)((fx_sin(a).raw >> shift) + ex),
                              (uint16_t)(((-fx_cos(a)).raw >> shift) + ey));
    };

    /* pitch towards the ground 1/32 of the cos/sin unit ahead */
    int16_t ahead = height_towards(yaw, 5);
    Angle pitch = fx_atan2((int16_t)(ahead - ez), 0x3ff);
    camera.eye_angles.pitch += (pitch - camera.eye_angles.pitch) >> 2;

    /* roll from the height difference ahead-left / ahead-right */
    int16_t h1 = height_towards(yaw + DEG45, 6);
    int16_t h2 = height_towards(yaw - DEG45, 6);
    Angle roll = fx_atan2((int16_t)(h2 - h1), 0x1ff);
    camera.eye_angles.roll += (roll - camera.eye_angles.roll) >> 2;
}

/* Heading towards a target. */
static Angle heading_to(int16_t dx, int16_t dy)
{
    return fx_atan2(dx, (int16_t)-dy);
}

/* orig 1000:0828 */
static void camera_fixed(Car &car, const ChaseCam &cam)
{
    uint16_t ex = cam.pos.x.units(), ey = cam.pos.y.units();
    int16_t ground = terrain_height(ex, ey);
    camera.eye.x = cam.pos.x;
    camera.eye.y = cam.pos.y;
    camera.eye.z = Coord::from_units((uint16_t)(ground + camera.chase_height));

    Pos ref = car.ref_vertex().pos;
    int16_t dx = (int16_t)(ref.x.units() - ex);
    int16_t dy = (int16_t)(ref.y.units() - ey);
    camera.eye_angles.yaw = heading_to(dx, dy);
    int16_t dist = (int16_t)vec2_length(dx, (int16_t)-dy);
    camera.eye_angles.pitch = fx_atan2((int16_t)(ref.z.units() - camera.eye.z.units()), dist);
    camera.eye_angles.roll = {};
}

/* Put the camera chase_distance behind target along heading, and smooth
 * its height towards chase_height above the ground (never less than 40
 * above it). */
static Pos chase_follow(uint16_t tx, uint16_t ty, Angle heading, Pos p)
{
    int16_t d2 = (int16_t)(camera.chase_distance << 1);
    int16_t bx = mul_hi16(fx_sin(heading).raw, d2);
    int16_t by = mul_hi16(fx_cos(heading).raw, d2);
    p.x.set_units((uint16_t)(tx - bx));
    p.y.set_units((uint16_t)(ty + by));

    int16_t ground = terrain_height(p.x.units(), p.y.units());
    uint16_t min_z = (uint16_t)(ground + 0x28);
    int16_t cz = p.z.units_signed();
    cz = (int16_t)(cz + ((int16_t)(ground + camera.chase_height - cz) >> 3));
    if (min_z > (uint16_t)cz)
        cz = (int16_t)min_z;
    p.z.set_units((uint16_t)cz);
    return p;
}

static Angle smooth_pitch(Angle current, int16_t dz)
{
    Angle target = fx_atan2(dz, camera.chase_distance);
    return current + ((target - current) >> 2);
}

/* orig 1000:0893 */
static void camera_chase(Car &car, ChaseCam &cam)
{
    Pos ref = car.ref_vertex().pos;
    uint16_t rx = ref.x.units(), ry = ref.y.units();
    cam.yaw = heading_to((int16_t)(rx - cam.pos.x.units()), (int16_t)(ry - cam.pos.y.units()));
    cam.pos = chase_follow(rx, ry, cam.yaw, cam.pos);
    cam.pitch = smooth_pitch(cam.pitch, (int16_t)(ref.z.units() - cam.pos.z.units()));
    cam.roll = {};

    camera.eye_angles = { cam.roll, cam.pitch, cam.yaw };
    camera.eye = cam.pos;
}

/* orig 1000:0948 */
static void camera_all_cars()
{
    uint32_t sx = 0, sy = 0, sz = 0;
    uint32_t n = (uint32_t)(uint16_t)world.num_cars;
    for (int i = 0; i < world.num_cars; i++) {
        Pos ref = car_get(i).ref_vertex().pos;
        sx += ref.x.units();
        sy += ref.y.units();
        sz += ref.z.units();
    }
    /* the average position of the cars (orig DS 00c8) */
    uint16_t cx = (uint16_t)(int16_t)(sx / n), cy = (uint16_t)(int16_t)(sy / n);
    int16_t cz = (int16_t)((int32_t)sz / (int32_t)n);

    camera.eye_angles.yaw = heading_to((int16_t)(cx - camera.eye.x.units()), (int16_t)(cy - camera.eye.y.units()));
    camera.eye = chase_follow(cx, cy, camera.eye_angles.yaw, camera.eye);
    camera.eye_angles.pitch = smooth_pitch(camera.eye_angles.pitch, (int16_t)(cz - camera.eye.z.units()));
    camera.eye_angles.roll = {};
}

/* Place the eye for one view.  orig f_cam_select */
void camera_update(int view)
{
    Car &car = car_get(world.player_car[view]);
    ChaseCam &cam = camera.chase_cam[view];
    switch (camera.cam_mode[view] & 7) {
    case 0: camera_on_car(car); break;
    case 1: camera_on_car_smooth(car); break;
    case 2: camera_fixed(car, cam); break;
    case 3: camera_chase(car, cam); break;
    case 4: camera_all_cars(); break;
    default: break;                     /* the original crashes (UD2) */
    }
}
