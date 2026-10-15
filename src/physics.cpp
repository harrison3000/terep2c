/*
 * Soft-body car physics, run at a fixed rate (twice per rendered frame in
 * the original timer setup).
 *
 * Every car is a set of mass points (CarVertex) joined by springs.  Each
 * tick: read the controls, spin the wheels, relax the springs, integrate
 * the points against gravity and the ground, then resolve car-vs-car
 * contacts and move the particles.
 *
 */
#include <math.h>
#include "terep.hpp"

/* Keys of each car: accelerate, brake, left, right, handbrake, unused. */
static const uint8_t car_keys[5][6] = {
    { 0x48, 0x50, 0x4b, 0x4d, 0x4c, 0x52 },     /* arrows, keypad 5 (keypad 0) */
    { 0x11, 0x2c, 0x1e, 0x20, 0x1f, 0x2d },     /* W Z A D S (X) */
    { 0x17, 0x32, 0x24, 0x26, 0x25, 0x27 },     /* I M J L K (;) */
    { 0x14, 0x2f, 0x21, 0x23, 0x22, 0x30 },     /* T V F H G (B) */
    { 0x47, 0x49, 0x4f, 0x51, 0x53, 0x55 },     /* keypad 7 9 1 3 . */
};

/* Per surface type (colour-map byte >> 4). */
static const uint16_t surface_grip[16] = { 16, 25, 20, 40, 30, 25, 16, 20, 40, 25, 25, 25, 16, 25, 16, 16 };
static const uint8_t surface_damp_shift[16] = { 12, 9, 10, 10, 10, 10, 8, 10, 10, 10, 10, 10, 8, 6, 10, 10 };
static const int32_t surface_max_load[16] = {
    0x30000, 0x18000, 0x20000, 0xc000, 0x20000, 0x20000, 0x20000, 0x20000,
    0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x10000, 0x10000, 0x10000,
};

/* Triangles of car vertices that form the collision hull. */
static const uint16_t hull_triangles[7][3] = {
    { 4, 5, 9 }, { 7, 6, 24 }, { 6, 9, 5 }, { 8, 7, 25 }, { 0, 2, 3 }, { 0, 1, 2 }, { 15, 14, 13 },
};

int broke_some_ice = 0;

/* ------------------------------------------------------------------------
 * Controls
 * ---------------------------------------------------------------------- */

static bool pressed(uint8_t scancode) { return !(keys[scancode] & 0x80); }

/* orig 1000:0d2a */
void Car::read_controls(const uint8_t binding[6])
{
    bool accel = pressed(binding[0]), brake = pressed(binding[1]);
    int16_t step = 0x32;

    if (accel && throttle <= 0x2000) {
        if (throttle < 0)
            step <<= 2;                 /* reversing direction is quicker */
        throttle += step;
    }
    if (brake && throttle >= -0x2000) {
        if (throttle >= 0)
            step <<= 2;                 /* note: step may already be x4 */
        throttle -= step;
    }
    if (!accel && !brake && throttle != 0)
        throttle -= throttle > 0 ? 300 : -300;     /* no clamp at 0: it wobbles */

    bool left = pressed(binding[2]), right = pressed(binding[3]);

    /* Steering gets slower with speed.  The original meant to use the
     * absolute value of the speed but negates the low word only and then
     * keeps the high word, so this is really 0x40 - (speed >> 16). */
    uint32_t speed = (uint32_t)(wheel_speed[0] + wheel_speed[1]);
    step = (int16_t)(0x40 - (uint16_t)(speed >> 16));

    if (left && steer >= -DEG45) {
        if (steer >= Angle{})
            step <<= 2;
        steer -= Angle{ step };
    }
    if (right && steer <= DEG45) {
        if (steer < Angle{})
            step <<= 2;
        steer += Angle{ step };
    }
    if (!left && !right && steer != Angle{})
        steer -= Angle{ (int16_t)(steer > Angle{} ? 0x50 : -0x50) };

    handbrake = pressed(binding[4]);
}

static inline uint32_t rol32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

/* Wheel friction and engine torque.  orig 1000:1004 */
void Car::update_wheels()
{
    int32_t *w = wheel_speed;

    steer_used = steer;
    throttle_used = throttle;

    /* the original runs over wheel_speed and spin as one array of 16 */
    auto decay = [](int32_t *v, int n, int shift) {
        for (int i = 0; i < n; i++)
            v[i] -= v[i] >> shift;
    };
    decay(wheel_speed, 4, 7);
    decay(spin, 12, 7);
    if (handbrake & 1) {
        decay(wheel_speed, 4, 2);
        decay(spin, 12, 2);
    }

    /* The torque is scaled with ROL rather than SHL, which for negative
     * values drags sign bits into the bottom; kept as is. */
    uint32_t torque = (uint32_t)(int32_t)(int16_t)(((int32_t)throttle_used * 0x4000) >> 16);
    switch (drive) {
    case Car::REAR:
        torque = rol32(torque, 4);
        w[2] += (int32_t)torque;
        w[3] += (int32_t)torque;
        break;
    case Car::FRONT:
        torque = rol32(torque, 4);
        w[0] += (int32_t)torque;
        w[1] += (int32_t)torque;
        break;
    default:
        torque = rol32(torque, 3);
        for (int i = 0; i < 4; i++)
            w[i] += (int32_t)torque;
        break;
    }
}

/* ------------------------------------------------------------------------
 * Car orientation
 * ---------------------------------------------------------------------- */

/* Unit vector across the car (vertex 6 -> vertex 7).  orig 1000:1091 */
static Vec3 axis_x(const CarVertex *v) { return vec3_normalize(v[7].pos - v[6].pos); }

/* Unit vector along the car ((4 - 6) + (5 - 7)).  orig 1000:10b6 */
static Vec3 axis_y(const CarVertex *v)
{
    Vec3 a = v[4].pos - v[6].pos, b = v[5].pos - v[7].pos;
    return vec3_normalize(a + b);
}

Vec3 Car::axis_x() { return ::axis_x(vertices()); }
Vec3 Car::axis_y() { return ::axis_y(vertices()); }

/* Longitudinal axis turned by the steering angle.  orig 1000:1136 */
void Car::compute_steered_dir()
{
    Vec3 ax = axis_x(), ay = axis_y();
    int32_t s = fx_sin(steer_used).q31(), c = fx_cos(steer_used).q31();

    wheel_dirs_cache.straight = ay;
    wheel_dirs_cache.steered = { (mul_hi32(ay.x, c) - mul_hi32(ax.x, s)) << 1,
                           (mul_hi32(ay.y, c) - mul_hi32(ax.y, s)) << 1,
                           (mul_hi32(ay.z, c) - mul_hi32(ax.z, s)) << 1 };
}

/* Rolling direction of wheel i (vertices 0..3).  orig 1000:0e69
 * prev_edx stands for the value EDX happened to hold: the original returns
 * it unchanged for vertices that are not wheels 0..3. */
Vec3 Car::wheel_direction(uint16_t i, int32_t *speed, int32_t prev_edx)
{
    switch (i) {
    case 0:
    case 1:
        if (!(wheel_dirs_cache.valid & 1)) {
            compute_steered_dir();
            wheel_dirs_cache.valid |= 1;
        }
        *speed = wheel_speed[i];
        return wheel_dirs_cache.steered;
    case 2:
    case 3:
        if (!(wheel_dirs_cache.valid & 2)) {
            wheel_dirs_cache.straight = axis_y();
            wheel_dirs_cache.valid |= 2;
        }
        *speed = wheel_speed[i];
        return wheel_dirs_cache.straight;
    default:
        *speed = prev_edx;
        return { 0, 0, 0 };
    }
}

/* Feed the ground speed back into the wheel: free-rolling wheels follow
 * the ground completely, driven ones only by a quarter.  orig 1000:0f67 */
void Car::wheel_set_speed(uint16_t i, int32_t ground_speed)
{
    bool driven;
    switch (i) {
    case 0: case 1: driven = drive != Car::REAR; break;
    case 2: case 3: driven = drive & 1; break;
    default: return;
    }
    int32_t w = wheel_speed[i];
    wheel_speed[i] = driven ? ((w - ground_speed) >> 2) + ground_speed : ground_speed;
}

/* Middle of the body, used by the cameras.  orig 1000:0e28 */
void Car::update_mid()
{
    CarVertex *v = vertices();
    mid[0] = (int16_t)((v[4].pos.x.units() >> 1) + (v[6].pos.x.units() >> 1));
    mid[1] = (int16_t)((v[4].pos.y.units() >> 1) + (v[6].pos.y.units() >> 1));
    mid[2] = (int16_t)((v[4].pos.z.units() >> 1) + (v[6].pos.z.units() >> 1));
    wheel_dirs_cache.valid = 0;
}

/* ------------------------------------------------------------------------
 * Springs
 * ---------------------------------------------------------------------- */

/* CVTSS2SI: round to nearest, value is clamped, if its a NaN, God help us. */
static int32_t float_to_i32(double f)
{
    f = fmin(f, 2147483647.0);
    f = fmax(f, -2147483648.0);

    return (int32_t)lrint(f);
}

/* Split the impulse between both ends; the high byte of the flags makes
 * the spring act more on one end than the other. */
static void apply_spring(CarVertex &a, CarVertex &b, Vec3 d, int32_t force, int32_t len, int shift)
{
    int32_t f64 = force << 6;
    int64_t r64 = (int64_t)(len << 6);
    auto impulse = [&](int32_t dk) { return (int32_t)(((int64_t)dk * f64) / r64); };
    Vec3 f = { impulse(d.x), impulse(d.y), impulse(d.z) };
    Vec3 part = f >> shift;
    a.vel += part;
    b.vel -= f - part;
}

/* orig 1000:4e0a (the length used SSE in the Win16 port) */
static void update_spring(CarVertex &a, CarVertex &b, CarSpring &s)
{
    Vec3 d = a.pos - b.pos;

    /* length one step ahead: positions plus relative velocity */
    Vec3 ahead = d;
    ahead += a.vel - b.vel;
    double fx = (double)ahead.x, fy = (double)ahead.y, fz = (double)ahead.z;
    fx *= fx;
    fy *= fy;
    fz *= fz;
    int32_t len = float_to_i32(sqrt((fy + fx) + fz)) >> 10;

    int32_t rest = s.rest, force;
    if (rest < 0) {                     /* first tick: take the current length */
        s.rest = s.last = (int16_t)len;
        return;
    }

    if (s.type() == CarSpring::ELASTIC) {
        force = rest - len;
        if (force == 0)
            return;
        if ((int16_t)force > world.spring_break_stretch) {
            /* overstretched: the rest length gives way a little */
            force >>= 4;
            s.rest = s.last = (int16_t)(rest - force);
        } else if ((int16_t)force < world.spring_break_squash) {
            s.flags |= CarSpring::BROKEN;
            return;
        }
    } else if (len > s.max_len || len < s.min_len) {
        int32_t limit = len > s.max_len ? s.max_len : s.min_len;
        force = (limit - len) >> 1;
        if (force == 0)
            return;
        s.last = (int16_t)len;
    } else if (s.type() == CarSpring::LIMITS) {
        return;
    } else {
        /* damper: pull to the rest length and resist the change since the
         * last tick */
        force = (rest - len) / (int32_t)(s.type() >> 1);
        force += ((int32_t)(uint16_t)s.last - len) >> 1;
        s.last = (int16_t)len;
    }
    apply_spring(a, b, d, force, len, s.softness());
}

void Car::update_springs()
{
    CarVertex *v = vertices();
    for (CarSpring &s : spring_list)
        update_spring(v[s.a], v[s.b], s);
}

/* ------------------------------------------------------------------------
 * Ground contact
 * ---------------------------------------------------------------------- */

/* The contact frame: n1 = (sin a, cos a), n2 = (sin b, cos b) as Q31.
 * The surface normal is (-n1.sin, -n2.sin, n1.cos * n2.cos). */
static void set_contact_frame(Q15 s1, Q15 c1, Q15 s2, Q15 c2)
{
    /* only the high words are ever written; the low words stay 0 */
    phys.ground_n1[0] = s1.q31();
    phys.ground_n1[1] = c1.q31();
    phys.ground_n2[0] = s2.q31();
    phys.ground_n2[1] = c2.q31();
}

static int32_t n1s() { return phys.ground_n1[0]; }
static int32_t n1c() { return phys.ground_n1[1]; }
static int32_t n2s() { return phys.ground_n2[0]; }
static int32_t n2c() { return phys.ground_n2[1]; }

static inline int32_t mulq31(int32_t a, int32_t b) { return mul_hi32(a, b) << 1; }


/* Friction limit for this surface.  Returns true while the tyre/body
 * grips, false when the tangential speed is beyond what the surface holds
 * (the point slides).  *div scales the slide down in that case.
 * orig 1000:4c68 */
static bool contact_grips(int32_t *div)
{
    int32_t a = phys.contact.slide[0] < 0 ? -phys.contact.slide[0] : phys.contact.slide[0];
    int32_t b = phys.contact.slide[1] < 0 ? -phys.contact.slide[1] : phys.contact.slide[1];
    int32_t slide = a + b;
    unsigned surf = phys.ground_surface >> 4;
    int32_t grip = surface_grip[surf];
    int32_t load = -phys.contact.penetration;
    if (load >= surface_max_load[surf])
        load = surface_max_load[surf];
    int32_t limit = load / grip;
    *div = grip >> 1;
    return limit > (slide >> 6);
}

/* Spawn a particle (dust, smoke) where a point slides.  orig 1000:4d96 */
static void spawn_particle(const CarVertex &v)
{
    if (world.particle_count >= 200)
        return;
    Particle &p = world.particles[world.particle_count];
    p.pos = v.pos;
    p.pos.z -= (int32_t)Coord::from_units((uint16_t)v.radius).raw;
    p.vel = { mulq31(phys.contact.slide[0], n1c()), mulq31(phys.contact.slide[1], n2c()), v.vel.z };
    p.kind = phys.ground_surface;
    p.life = 100;
    world.particle_count++;
}

/* Move a point by the slide that the friction did not absorb. */
static void slide_point(CarVertex &v, int32_t sx, int32_t sy)
{
    int32_t d = mul_hi32(sx, n1c());
    v.vel.x -= d;
    v.pos.x -= d;
    d = mul_hi32(sy, n2c());
    v.vel.y -= d;
    v.pos.y -= d;
}

/* Body point: plain friction.  orig 1000:4cc3 */
static void body_friction(CarVertex &v)
{
    int32_t sx = phys.contact.slide[0], sy = phys.contact.slide[1], div;
    if (!contact_grips(&div)) {
        spawn_particle(v);
        sy = phys.contact.slide[1] / div;
        sx = phys.contact.slide[0] / div;
    }
    slide_point(v, sx, sy);
}

/* Wheel: friction across the wheel, rolling along it.  orig 1000:4d0e */
static void wheel_friction(CarVertex &v)
{
    int32_t div;
    if (!contact_grips(&div)) {
        spawn_particle(v);
        int32_t sy = phys.contact.slide[1] / div;
        int32_t sx = phys.contact.slide[0] / div;
        phys.contact.slide[0] = sx;
        phys.contact.slide[1] = sy;
    }
    /* the part of the slide along the wheel goes into the wheel */
    int32_t along = mul_hi32(phys.contact.slide[0], phys.contact.slip_sin)
                  - mul_hi32(phys.contact.slide[1], phys.contact.slip_cos);
    phys.contact.wheel_speed -= along;
    phys.contact.slide[0] += mul_hi32(along, phys.contact.slip_sin);
    phys.contact.slide[1] -= mul_hi32(along, phys.contact.slip_cos);
    slide_point(v, phys.contact.slide[0], phys.contact.slide[1]);
}

/* Heading of the wheel in the contact plane.  orig 1000:4c26 */
static Angle wheel_heading(Vec3 dir)
{
    int16_t a = (int16_t)(-(int16_t)mul_hi32(dir.x >> 4, n1c()) - (int16_t)mul_hi32(dir.z >> 4, n1s()));
    int16_t b = (int16_t)((int16_t)mul_hi32(dir.y >> 4, n2c()) + (int16_t)mul_hi32(dir.z >> 4, n2s()));
    return fx_atan2(a, b);
}

/* Push a point out of the ground (or a map wall) along the contact normal
 * and apply friction.  orig 1000:4a71 */
void Car::ground_contact(CarVertex &v, uint16_t index)
{
    int32_t vn = mulq31(mulq31(v.vel.z, n1c()), n2c())
               - mulq31(v.vel.x, n1s())
               - mulq31(v.vel.y, n2s());
    if (vn >= 0)
        return;                         /* moving away from the surface */

    phys.contact.penetration = vn;
    if (vn <= -0x60000)
        sfx_hit(0);

    int32_t bounce = vn + (vn >> 2);
    int32_t d = mul_hi32(n1s(), bounce);
    v.vel.x += d;
    v.pos.x += d << 1;
    d = mul_hi32(n2s(), bounce);
    v.vel.y += d;
    v.pos.y += d << 1;
    d = mul_hi32(mulq31(bounce, n1c()), n2c());
    v.vel.z -= d;
    v.pos.z -= d << 1;

    phys.contact.slide[0] = mulq31(v.vel.x, n1c()) + mulq31(v.vel.z, n1s());
    phys.contact.slide[1] = mulq31(v.vel.y, n2c()) + mulq31(v.vel.z, n2s());

    if (!v.is_wheel()) {
        body_friction(v);
        return;
    }

    int32_t speed;
    Vec3 dir = wheel_direction(index, &speed, mulq31(v.vel.z, n2s()));
    phys.contact.wheel_speed = -speed;
    Angle heading = wheel_heading(dir);
    phys.contact.slip_cos = fx_cos(heading).q31();
    phys.contact.slip_sin = fx_sin(heading).q31();
    int32_t w2 = phys.contact.wheel_speed << 1;
    phys.contact.slide[0] += mul_hi32(w2, phys.contact.slip_sin);
    phys.contact.slide[1] -= mul_hi32(w2, phys.contact.slip_cos);
    wheel_friction(v);
    wheel_set_speed(index, -phys.contact.wheel_speed);
}

/* Integrate every point of the car.  orig 1000:48db */
void Car::update_points()
{
    update_mid();
    CarVertex *verts = vertices();
    uint16_t n = vertex_count();

    for (uint16_t i = 0; i < n; i++) {
        CarVertex &v = verts[i];
        v.vel.z -= world.gravity;

        int16_t ground = (int16_t)(ground_probe(v.pos.x.units(), v.pos.y.units()) + v.radius);
        if ((int16_t)(ground - v.pos.z.units_signed()) >= 0) {
            /* below the ground: damp by surface, then collide */
            int shift = surface_damp_shift[phys.ground_surface >> 4];
            if (shift)
                v.vel -= v.vel >> shift;
            Angle a = fx_atan2(phys.ground_slope_x, 0x100);
            Angle b = fx_atan2(phys.ground_slope_y, 0x100);
            set_contact_frame(fx_sin(a), fx_cos(a), fx_sin(b), fx_cos(b));
            ground_contact(v, i);
        }

        /* invisible walls half a cell inside the map border */
        if ((uint16_t)(v.pos.x.units() - v.radius) < 0x80) {
            set_contact_frame({ INT16_MIN }, { 0 }, { 0 }, { 0x7f00 });
            phys.ground_surface = 0;
            ground_contact(v, i);
        }
        if ((uint16_t)(v.pos.y.units() - v.radius) < 0x80) {
            set_contact_frame({ 0 }, { 0x7fff }, { INT16_MIN }, { 0 });
            phys.ground_surface = 0;
            ground_contact(v, i);
        }
        if ((uint16_t)(v.pos.x.units() + v.radius) > 0xfe80) {
            set_contact_frame({ 0x7fff }, { 0 }, { 0 }, { 0x7fff });
            phys.ground_surface = 0;
            ground_contact(v, i);
        }
        if ((uint16_t)(v.pos.y.units() + v.radius) > 0xfe80) {
            set_contact_frame({ 0 }, { 0x7fff }, { 0x7fff }, { 0 });
            phys.ground_surface = 0;
            ground_contact(v, i);
        }

        v.pos += v.vel;
    }
}

/* ------------------------------------------------------------------------
 * Car vs car
 * ---------------------------------------------------------------------- */

static int32_t hull_normal_component(int32_t a0, int32_t a1, int32_t b0, int32_t b1)
{
    return (a0 >> 14) * (b0 >> 14) - (a1 >> 14) * (b1 >> 14);
}

static int hull_planes() { return phys.hull_plane_count; }

/* Build the collision planes of a car from the hull triangles: for each
 * triangle a point and an (unnormalised) normal.  orig 1000:5091 */
void Car::build_hull()
{
    CarVertex *v = vertices();
    phys.hull_plane_count = 0;
    for (int t = 0; t < (int)(sizeof hull_triangles / sizeof hull_triangles[0]); t++) {
        Pos p = v[hull_triangles[t][0]].pos;
        Vec3 pq = p - v[hull_triangles[t][1]].pos;
        Vec3 pr = p - v[hull_triangles[t][2]].pos;
        int k = hull_planes();
        /* cross product of (p - q) and (p - r) */
        phys.hull_normal[k] = { hull_normal_component(pq.y, pr.y, pr.z, pq.z),
                           -hull_normal_component(pr.z, pq.z, pq.x, pr.x),
                           hull_normal_component(pr.x, pq.x, pq.y, pr.y) };
        phys.hull_point[k] = p;
        phys.hull_plane_count++;
    }
}

/* Push the points of car b that are inside this car's hull.  orig 1000:51bd */
void Car::push_points_of(Car &b)
{
    CarVertex *v = b.vertices();
    uint16_t n = b.vertex_count();

    do {
        int best = 0;
        int32_t best_d = INT32_MIN;
        bool inside = true;
        for (int k = 0; k < hull_planes(); k++) {
            Vec3 rel = v->pos - phys.hull_point[k], nrm = phys.hull_normal[k];
            int32_t d = (rel.x >> 16) * nrm.x + (rel.y >> 16) * nrm.y + (rel.z >> 16) * nrm.z;
            if (d >= 0) {
                inside = false;
                break;
            }
            if (d > best_d) {
                best_d = d;
                best = k;
            }
        }
        if (inside) {
            Vec3 nrm = phys.hull_normal[best];
            v->vel += nrm;

            /* The reaction goes to the point of this car with the smallest
             * |x|+|y|+|z|, the integer parts taken as signed 16-bit (so
             * nearest to a corner of the map, not to v).  Surely meant to
             * be the closest point to v, but that's what it does. */
            CarVertex *av = vertices(), *target = av;
            int16_t best_l1 = 0x7fff;
            for (uint16_t j = vertex_count(); j; j--, av++) {
                int16_t l1 = (int16_t)(abs16(av->pos.x.units_signed()) + abs16(av->pos.y.units_signed())
                                       + abs16(av->pos.z.units_signed()));
                if (l1 < best_l1) {
                    best_l1 = l1;
                    target = av;
                }
            }
            Vec3 half = nrm >> 1;
            target->vel.x -= half.x;
            v->vel.x += half.x;
            target->vel.y -= half.y;
            v->vel.y += half.y;
            target->vel.z -= half.z;
            v->vel.z += half.z;
            spawn_particle(*v);
        }
        v++;
    } while (--n);
}

/* orig 1000:500b */
static void cars_collide()
{
    phys.ground_surface = 0;
    for (int i = 0; i < world.num_cars; i++) {
        Car &a = car_get(i);
        a.build_hull();
        for (int j = 0; j < world.num_cars; j++) {
            Car &b = car_get(j);
            if (&b == &a)
                continue;
            Pos pa = a.vertices()->pos, pb = b.vertices()->pos;
            int16_t dx = (int16_t)(pa.x.units() - pb.x.units());
            int16_t dy = (int16_t)(pa.y.units() - pb.y.units());
            int16_t dz = (int16_t)(pa.z.units() - pb.z.units());
            if (dx > 0x200 || dx < -0x200 || dy > 0x200 || dy < -0x200 || dz > 0x200 || dz < -0x200)
                continue;
            a.push_points_of(b);
        }
    }
}

/* ------------------------------------------------------------------------
 * Particles
 * ---------------------------------------------------------------------- */

/* orig 1000:0bb5 */
static void particles_update()
{
    Particle *particles = world.particles;
    uint16_t i = 0;
    while (i < world.particle_count) {
        Particle &p = particles[i];
        p.life -= 2;
        if (p.life < 0) {
            /* dead: move the last particle into this slot and redo it */
            world.particle_count--;
            if (world.particle_count == 0)
                return;
            p = particles[world.particle_count];
            continue;
        }
        p.pos += p.vel;
        if (p.kind == Particle::ICE_CHIP) {
            /* falls until it hits the ground, then chips the ice */
            uint16_t x = p.pos.x.units(), y = p.pos.y.units();
            if ((int16_t)(terrain_height(x, y) - p.pos.z.units_signed()) < 0) {
                p.vel.z -= world.gravity >> 1;
            } else {
                uint8_t &col = colormap()[(uint16_t)((y & 0xff00) | (x >> 8))];
                if (col & 0x0f)
                    col--;
                /* bug of the original (fixed with -E fix-ice-bug): at height 0
                 * the cell wraps to 255, a pillar that throws cars up */
                uint8_t &h = heightmap()[(uint16_t)(((uint16_t)(y + 0x80) & 0xff00) | ((uint16_t)(x + 0x80) >> 8))];
                if (h || !extra_options.fix_ice_bug){
                    h--;
                    broke_some_ice = 1;
                }
                p.kind = Particle::DUST;
                p.vel = { 0, 0, 10000 };
            }
        }
        i++;
    }
}

/* ------------------------------------------------------------------------
 * Jump keys
 * ---------------------------------------------------------------------- */

/* Keys 1 and 2: kick the lowest point of the player cars upwards.
 * orig 1000:0a3b / 1000:0a82 */
void Car::jump(bool keep_point)
{
    CarVertex *v = vertices();
    uint16_t n = vertex_count();
    int32_t kick = (int32_t)((n >> 1) + 1) * world.gravity;

    /* The original had a bug here, fixed in this version: a car that was
     * never kicked has lowest_vertex = 0, a data segment offset, so a key
     * still held after switching to it (Tab/Q) added the kick to the
     * "vel.z" at DS 0x14..0x17, which is the "VEHICLE" in the title text.
     * Such a car now looks for its lowest point as on a fresh press. */
    if (!keep_point || lowest < 0) {
        int low = 0;
        int16_t low_z = v->pos.z.units_signed();       /* compared signed, as the original */
        for (uint16_t i = 1; i < n; i++) {
            if (v[i].pos.z.units_signed() < low_z) {
                low_z = v[i].pos.z.units_signed();
                low = i;
            }
        }
        lowest = low;
        /* quirk: the original searches with AX, which is the low half of the
         * kick, so the low word ends up as the last vertex's height */
        kick = (int32_t)(((uint32_t)kick & 0xffff0000u) | v[n - 1].pos.z.units());
    }
    v[lowest].vel.z += kick;
}

static void jump_keys()
{
    /* the key state goes 0xff (up) -> 0x7f (down) -> 0x3f (held, seen);
     * PF of (state & 0xc0) tells "held" apart from "just pressed" */
    for (int p = 0; p < 2; p++) {
        uint8_t &k = keys[2 + p];
        bool held = parity_even(k & 0xc0);
        if (!(k & 0x80)) {
            k &= 0x3f;
            car_get(world.player_car[p]).jump(held);
        }
    }
}

/* ------------------------------------------------------------------------ */

/* orig 1000:5680 (timer interrupt handler in the DOS version) */
void physics_tick()
{
    if (world.sim_running != 1)
        return;
    for (int i = 0; i < world.num_cars; i++) {
        Car &c = car_get(i);
        c.read_controls(car_keys[i]);
        c.update_wheels();
        c.update_springs();
        c.update_points();
    }
    cars_collide();
    particles_update();
    jump_keys();
}
