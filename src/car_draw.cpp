/*
 * Drawing a car: its vertices are transformed to camera space (with a
 * shadow point on the ground under each one), then the car's model, a
 * small bytecode program stored in the car file, draws polygons using
 * those points.
 *
 * Projected points, in render.car_proj:
 *   [2i]     vertex i
 *   [2i + 1] the ground under vertex i
 *   [2n]     vertex 0 moved along the steered front axle
 *   [2n + 1] vertex 1 moved the other way
 */
#include <map>
#include "terep.hpp"

/* cos of the borders between the nine wheel sprites (22.5 deg apart), Q14 */
static const int16_t wheel_sector_limits[8] = { -16069, -13623, -9102, -3196, 3196, 9102, 13623, 16069 };

static void transform_point(Pos p, ProjVertex *out)
{
    view_transform_fine(p - camera.eye, out);
}

/* Half of the front axle, turned by the steering angle (16.16, length
 * 0x80 map units).  orig 1000:11f0 */
static Vec3 steered_axle(Car &car)
{
    Vec3 ax = car.axis_x(), ay = car.axis_y();
    int32_t s = fx_sin(car.steer_used).q31(), c = fx_cos(car.steer_used).q31();
    auto turn = [&](int32_t y, int32_t x) { return (int32_t)((uint32_t)(mul_hi32(y, s) + mul_hi32(x, c)) << 7); };
    return { turn(ay.x, ax.x), turn(ay.y, ax.y), turn(ay.z, ax.z) };
}

/* orig 1000:1347 */
static void transform_car(Car &car)
{
    CarVertex *v = car.vertices();
    uint16_t n = car.vertex_count();
    ProjVertex *out = render.car_proj;

    for (uint16_t i = 0; i < n; i++) {
        Pos p = v[i].pos;
        transform_point(p, out++);
        p.z = Coord::from_units((uint16_t)terrain_height(p.x.units(), p.y.units()));
        transform_point(p, out++);
    }
    Vec3 a = steered_axle(car);
    transform_point(v[0].pos + a, out++);
    transform_point(v[1].pos - a, out++);
}

/* ------------------------------------------------------------------------
 * Model bytecode
 *
 * The model is a small program in the car file.  A program counter is an
 * offset in the model's bytes (in the original, a data segment offset); all
 * operands are 16-bit little-endian unless noted.  Point operands index
 * car_proj.  "hidden" is the result of the last facing test (shared with
 * the terrain code).  Only the low 5 bits of an opcode count.
 * ---------------------------------------------------------------------- */

enum ModelOpcode : uint8_t {
    OP_END = 0,         /* also 11..15: end of (sub)program */
    OP_PIXEL = 1,       /* p, color             plot a pixel */
    OP_NEARER = 2,      /* p, q, -              hidden = p is nearer than q */
    OP_FACING = 3,      /* p, q, r              hidden = triangle p q r faces away */
    OP_FLAT = 4,        /* n:8, p*(n+1), color  flat polygon (the last point closes it) */
    OP_NOP = 5,
    OP_SHADED = 6,      /* n:8, (p, shade)*(n+1)        Gouraud polygon */
    OP_SHADED_TAB = 7,  /* n:8, base, (p, idx)*(n+1)    Gouraud, shade = word at car_uv_table+idx, + base */
    OP_TEXTURED = 8,    /* n:8, (p, uv:32)*(n+1)        textured polygon */
    OP_SPRITE = 9,      /* p, size, uv:32*4     camera-facing square */
    OP_WHEEL = 10,      /* p, q, -, 9 x (w0, w1, uv:32*4)  wheel seen from the side: one of
                         * nine sprites chosen by the viewing angle */
    OP_SORT2 = 16,      /* a, b                 call pc+a and pc+b, nearest first (b first if hidden) */
    OP_JUMP = 17,       /* rel */
    OP_CALL = 18,       /* rel */
    OP_JUMP_VISIBLE = 19,   /* rel              jump if not hidden */
    OP_JUMP_HIDDEN = 20,    /* rel              jump if hidden */
};

/* ------------------------------------------------------------------------
 * Decoding, at load time
 *
 * Every instruction reachable from pc 0 is decoded into a ModelOp, and the
 * list is kept in program order, so falling through goes to the next
 * entry; jump, call and SORT2 targets become indices.  A target in the
 * middle of an instruction (code that overlaps itself) is refused.
 * ---------------------------------------------------------------------- */

namespace {

struct Decoder {
    const std::vector<uint8_t> &code;

    /* Reads past the end give 0 (the original would read whatever follows
     * the car in the data segment). */
    uint8_t  rd8(uint32_t pc) const { return pc < code.size() ? code[pc] : 0; }
    uint16_t rd16(uint32_t pc) const { return (uint16_t)(rd8(pc) | rd8(pc + 1) << 8); }
    TexCoord rd_uv(uint32_t pc) const { return { rd16(pc), rd16(pc + 2) }; }

    /* The n + 1 corners of a polygon at pc; attr_size: 0, 2 or 4 bytes. */
    void outline(ModelOp &o, uint32_t pc, uint8_t n, int attr_size) const
    {
        for (int i = 0; i <= n; i++, pc += 2 + attr_size) {
            ModelOp::Vertex v = { rd16(pc), {} };
            if (attr_size == 2)
                v.attr = rd16(pc + 2);
            else if (attr_size == 4)
                v.attr = rd_uv(pc + 2);
            o.outline.push_back(v);
        }
    }

    /* The instruction at pc; returns its length.  Targets are pcs here.
     * *falls is set when the next instruction runs after it. */
    uint32_t decode(uint32_t pc, ModelOp &o, bool *falls) const
    {
        uint8_t raw = rd8(pc++) & 0x1f;
        o.op = (ModelOpcode)(raw <= OP_WHEEL || (raw >= OP_SORT2 && raw <= OP_JUMP_HIDDEN) ? raw : (uint8_t)OP_END);
        *falls = o.op != OP_END && o.op != OP_JUMP;
        switch (o.op) {
        case OP_PIXEL:
            o.p[0] = rd16(pc);
            o.word = rd16(pc + 2);
            return 5;
        case OP_NEARER:
        case OP_FACING:
            for (int i = 0; i < 3; i++)
                o.p[i] = rd16(pc + 2 * i);
            return 7;
        case OP_FLAT: {
            uint8_t n = rd8(pc);
            outline(o, pc + 1, n, 0);
            o.word = rd16(pc + 1 + (n + 1) * 2);
            return 1 + 1 + (n + 1) * 2 + 2;
        }
        case OP_SHADED:
        case OP_TEXTURED: {
            uint8_t n = rd8(pc);
            int attr_size = o.op == OP_SHADED ? 2 : 4;
            outline(o, pc + 1, n, attr_size);
            return 1 + 1 + (n + 1) * (2 + attr_size);
        }
        case OP_SHADED_TAB: {
            uint8_t n = rd8(pc);
            o.word = rd16(pc + 1);
            outline(o, pc + 3, n, 2);
            return 1 + 3 + (n + 1) * 4;
        }
        case OP_SPRITE:
            o.p[0] = rd16(pc);
            o.sprite.size = (int16_t)rd16(pc + 2);
            for (int i = 0; i < 4; i++)
                o.sprite.uv[i] = rd_uv(pc + 4 + 4 * i);
            return 21;
        case OP_WHEEL:
            o.p[0] = rd16(pc);
            o.p[1] = rd16(pc + 2);
            for (int v = 0; v < 9; v++) {
                uint32_t sp = pc + 6 + v * 20;
                ModelOp::WheelView w = { (int16_t)rd16(sp), (int16_t)rd16(sp + 2), {} };
                for (int i = 0; i < 4; i++)
                    w.uv[i] = rd_uv(sp + 4 + 4 * i);
                o.views.push_back(w);
            }
            return 1 + 0xba;
        case OP_SORT2:
            o.target[0] = (uint16_t)(pc + rd16(pc));
            o.target[1] = (uint16_t)(pc + rd16(pc + 2));
            return 5;
        case OP_JUMP:
        case OP_CALL:
        case OP_JUMP_VISIBLE:
        case OP_JUMP_HIDDEN:
            o.target[0] = (uint16_t)(pc + 2 + rd16(pc));
            return 3;
        default:                        /* OP_NOP, OP_END */
            return 1;
        }
    }
};

}  // namespace

std::vector<ModelOp> model_decode(const std::vector<uint8_t> &code)
{
    Decoder d = { code };
    std::map<uint32_t, std::pair<ModelOp, uint32_t>> at;    /* pc -> instruction, length */
    std::vector<uint32_t> work = { 0 };
    while (!work.empty()) {
        uint32_t pc = work.back();
        work.pop_back();
        if (at.count(pc))
            continue;
        ModelOp o;
        bool falls;
        uint32_t len = d.decode(pc, o, &falls);
        if (pc + len >= 0x10000) {
            fprintf(stderr, "car model: instruction at %u runs past 64 KB, not drawn\n", pc);
            return {};
        }
        if (falls)
            work.push_back(pc + len);
        if (o.op == OP_SORT2)
            work.push_back(o.target[1]);
        if (o.op == OP_SORT2 || o.op == OP_JUMP || o.op == OP_CALL || o.op == OP_JUMP_VISIBLE || o.op == OP_JUMP_HIDDEN)
            work.push_back(o.target[0]);
        at.emplace(pc, std::make_pair(std::move(o), len));
    }

    std::map<uint32_t, uint32_t> index;     /* pc -> index in the list */
    uint32_t end = 0;
    for (const auto &e : at) {
        if (e.first < end) {
            fprintf(stderr, "car model: a jump lands at %u, inside another instruction; not drawn\n", e.first);
            return {};
        }
        index[e.first] = (uint32_t)index.size();
        end = e.first + e.second.second;
    }
    std::vector<ModelOp> ops;
    for (auto &e : at) {
        ModelOp &o = e.second.first;
        o.target[0] = index[o.target[0]];
        o.target[1] = index[o.target[1]];
        ops.push_back(std::move(o));
    }
    return ops;
}

/* ------------------------------------------------------------------------
 * Running the decoded model
 * ---------------------------------------------------------------------- */

/* Projected point i; the original read any point index from the data
 * segment, here those past car_proj give a point at the origin. */
static const ProjVertex &point(uint16_t i)
{
    static const ProjVertex none = {};
    return i < sizeof render.car_proj / sizeof render.car_proj[0] ? render.car_proj[i] : none;
}

/* The shade table of OP_SHADED_TAB: the word at byte idx of what was
 * car_uv_table in the original, the 0xc8 bytes after the first 100
 * projected points (never written there by the demo's cars, which have
 * fewer than 50 vertices); the original read past it, here that is 0. */
static uint16_t shade_tab(uint16_t idx)
{
    const ProjVertex *table = &render.car_proj[100];
    const int16_t *words = &table[0].x;     /* ProjVertex is five int16_t */
    if ((uint32_t)idx + 1 >= 20 * sizeof(ProjVertex))
        return 0;
    if (idx & 1) {                          /* an odd byte offset: two halves */
        uint16_t lo = (uint16_t)words[idx / 2], hi = (uint16_t)words[idx / 2 + 1];
        return (uint16_t)(lo >> 8 | hi << 8);
    }
    return (uint16_t)words[idx / 2];
}

/* Feeds the outline of a polygon to the clipper.  Returns the number of
 * points that survived the near plane. */
static uint16_t draw_outline(const ModelOp &o)
{
    const size_t n = o.outline.size() - 1;
    uint16_t count = 0;
    for (size_t i = 0; i <= n; i++) {
        const ProjVertex &p = point(o.outline[i].point);
        VertexAttr attr = o.outline[i].attr;
        if (o.op == OP_FLAT)
            attr = {};
        else if (o.op == OP_SHADED_TAB)
            attr = (uint16_t)(shade_tab(attr.a) + o.word);
        if (i == 0)
            poly_begin(p, attr);
        else if (i < n)
            poly_add(p, attr);
        else
            count = poly_end(p, attr);
    }
    return count;
}

/* Front-facing and not cut away by the near plane? */
static bool outline_visible(uint16_t count)
{
    if ((int8_t)count < 3)
        return false;
    poly_hidden = poly_backfacing();
    return !poly_hidden;
}

/* Wheel sprite (OP_WHEEL).  The angle between the line of sight and the
 * axle p->q picks one of nine views; the sprite is rotated to the axle's
 * direction on screen. */
static void draw_wheel(const ModelOp &o, const uint8_t *tex)
{
    const ProjVertex &a = point(o.p[0]), &b = point(o.p[1]);

    int16_t axle = (int16_t)vec3_length((int16_t)(a.x - b.x), (int16_t)(a.y - b.y), (int16_t)(a.z - b.z));
    int32_t sight = vec3_length(a.x, a.y, a.z);
    int32_t lengths = (int32_t)((uint32_t)sight * (uint32_t)(int32_t)axle);
    uint32_t dot = (uint32_t)((int32_t)(int16_t)(b.x - a.x) * a.x)
                 + (uint32_t)((int32_t)(int16_t)(b.y - a.y) * a.y)
                 + (uint32_t)((int32_t)(int16_t)(b.z - a.z) * a.z);
    int16_t cosine = (int16_t)(((int32_t)(dot << 6) / (lengths >> 9)) >> 1);

    int view = 0;
    for (int i = 0; i < 8; i++) {
        if (cosine < wheel_sector_limits[i]) {
            view = 8 - i;
            break;
        }
    }
    const ModelOp::WheelView &w = o.views[view];

    Angle angle = fx_atan2((int16_t)(b.sx - a.sx), (int16_t)(b.sy - a.sy));
    Q15 s = fx_sin(angle), c = fx_cos(angle);
    int16_t depth = a.y;
    auto size = [&](int16_t w, Q15 t) { return screen_clamp(w * t * render_scale / depth); };
    int16_t c0 = size(w.w0, c), s0 = size(w.w0, s);
    int16_t c1 = size(w.w1, c), s1 = size(w.w1, s);

    PolyVertex *out = render.poly_buf[0].v;
    int16_t X = a.sx, Y = a.sy;
    out[0].sx = (int16_t)(X - c0 - s1); out[0].sy = (int16_t)(Y + s0 - c1);
    out[1].sx = (int16_t)(X - c0 + s1); out[1].sy = (int16_t)(Y + s0 + c1);
    out[2].sx = (int16_t)(X + c0 + s1); out[2].sy = (int16_t)(Y - s0 + c1);
    out[3].sx = (int16_t)(X + c0 - s1); out[3].sy = (int16_t)(Y - s0 - c1);
    for (int i = 0; i < 4; i++) {
        out[i].a = w.uv[i].u;
        out[i].b = w.uv[i].v;
    }
    render.poly_buf[0].count = 4;
    draw_textured(tex);
}

namespace {

/* A decoded model being run. */
class Program {
public:
    Program(const std::vector<ModelOp> &ops, const uint8_t *texture) : ops(ops), tex(texture) {}
    void run(uint32_t i);           /* from instruction i to its OP_END */

private:
    const std::vector<ModelOp> &ops;
    const uint8_t *tex;             /* the car's skin, or textures.pcx */
};

/* orig 1000:1408 */
void Program::run(uint32_t i)
{
    for (;;) {
        const ModelOp &o = ops[i++];
        switch (o.op) {
        case OP_PIXEL: {                /* FUN_1000_1408.LAB_LOC_6 */
            const ProjVertex &p = point(o.p[0]);
            if (p.y >= render.near_clip)
                put_block(p.sx, p.sy, (uint8_t)o.word);
            break;
        }
        case OP_NEARER:                 /* FUN_1000_1408.LAB_LOC_7 */
            poly_hidden = (int16_t)(point(o.p[0]).y - point(o.p[1]).y) < 0;
            break;
        case OP_FACING: {               /* FUN_1000_1408.LAB_LOC_8 */
            const ProjVertex &p = point(o.p[0]);
            poly_begin(p, {});
            poly_add(point(o.p[1]), {});
            poly_add(point(o.p[2]), {});
            poly_end(p, {});
            poly_hidden = poly_backfacing();    /* even if nothing is left */
            break;
        }
        case OP_FLAT: {                 /* FUN_1000_1408.LAB_LOC_9 */
            uint16_t count = draw_outline(o);
            if (outline_visible(count))
                draw_flat(o.word);
            break;
        }
        case OP_NOP:                    /* FUN_1000_1408.LAB_LOC_11 */
            break;
        case OP_SHADED:                 /* FUN_1000_1408.LAB_LOC_12 */
            if (outline_visible(draw_outline(o)))
                draw_shaded();
            break;
        case OP_SHADED_TAB:             /* FUN_1000_1408.LAB_LOC_14 */
            if (outline_visible(draw_outline(o)))
                draw_shaded();
            break;
        case OP_TEXTURED:               /* FUN_1000_1408.LAB_LOC_16 */
            if (outline_visible(draw_outline(o)))
                draw_textured(tex);
            break;
        case OP_SPRITE: {               /* FUN_1000_1408.LAB_LOC_18 */
            const ProjVertex &p = point(o.p[0]);
            if (p.y >= render.near_clip)
                draw_sprite(p.sx, p.sy, p.y, o.sprite, tex);
            break;
        }
        case OP_WHEEL:                  /* FUN_1000_1408.LAB_LOC_20 */
            if (point(o.p[0]).y >= render.near_clip)
                draw_wheel(o, tex);
            break;
        case OP_SORT2:                  /* FUN_1000_1408.LAB_LOC_24 */
            if (!poly_hidden) {
                run(o.target[0]);
                run(o.target[1]);
            } else {                    /* FUN_1000_1408.LAB_LOC_25 */
                run(o.target[1]);
                run(o.target[0]);
            }
            break;
        case OP_JUMP:                   /* FUN_1000_1408.LAB_LOC_2 */
            i = o.target[0];
            break;
        case OP_CALL:                   /* FUN_1000_1408.LAB_LOC_3 */
            run(o.target[0]);
            break;
        case OP_JUMP_VISIBLE:           /* FUN_1000_1408.LAB_LOC_4 */
            if (!poly_hidden)
                i = o.target[0];
            break;
        case OP_JUMP_HIDDEN:            /* FUN_1000_1408.LAB_LOC_5 */
            if (poly_hidden)
                i = o.target[0];
            break;
        default:                        /* OP_END, also 11..15: FUN_1000_1408.LAB_LOC_1;
                                         * 21..31: FUN_1000_1408.LAB_RUIM (UD2, a crash) */
            return;
        }
    }
}

}  // namespace

/* orig 1000:1323 */
void draw_car(Car &car)
{
    transform_car(car);
    if (!car.model_ops.empty()) {
        Program program(car.model_ops, (car.skin.empty() ? segments.textures : car.skin).data());
        program.run(0);
    }
}
