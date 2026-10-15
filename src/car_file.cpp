/*
 * Car files (carN.dat): reading them into a Car.
 *
 *   0x00  uint16 vertices    offset of: uint16 count, CarVertex[count] (0x1c bytes each)
 *   0x02  uint16 springs     offset of: uint16 count, CarSpring[count] (0x0e bytes each)
 *   0x04  uint16 model       offset of the drawing program (to the end of the file)
 *   0x06  uint16 ?
 *   0x08  uint16 drive       0 rear, 1 front, 2 all wheels
 *   0x0a  ...    the game's own state for the car (zero in the files):
 *         steer, throttle, handbrake, mid[3], steer_used, throttle_used,
 *         cell, ?, texture segment, ref_vertex (offset), lowest_vertex (DS
 *         offset), 0x1e bytes ?, wheel_speed[4], spin[12], ?
 *   0x84  the sections, normally in the order above
 *
 * The original copied the file into the data segment and used it in place,
 * so the header doubles as the car's state.
 */
#include "terep.hpp"

Car cars[MAX_CARS];

namespace {

/* Little-endian reads that stay inside the file. */
struct Reader {
    const uint8_t *p;
    size_t size;

    uint16_t u16(size_t at) const { return at + 2 <= size ? (uint16_t)(p[at] | p[at + 1] << 8) : 0; }
    int16_t  i16(size_t at) const { return (int16_t)u16(at); }
    uint32_t u32(size_t at) const { return (uint32_t)u16(at) | (uint32_t)u16(at + 2) << 16; }
    int32_t  i32(size_t at) const { return (int32_t)u32(at); }
};

CarVertex read_vertex(const Reader &r, size_t at)
{
    CarVertex v;
    v.pos = { { r.u32(at) }, { r.u32(at + 4) }, { r.u32(at + 8) } };
    v.vel = { r.i32(at + 12), r.i32(at + 16), r.i32(at + 20) };
    v.radius = r.i16(at + 24);
    v.wheel = r.u16(at + 26);
    return v;
}

CarSpring read_spring(const Reader &r, size_t at)
{
    CarSpring s;
    s.a = r.u16(at);
    s.b = r.u16(at + 2);
    s.rest = r.i16(at + 4);
    s.last = r.i16(at + 6);
    s.flags = r.u16(at + 8);
    s.min_len = r.u16(at + 10);
    s.max_len = r.u16(at + 12);
    return s;
}

}  // namespace

/* Read a car file.  Returns its size as the original counts it (at most
 * MAX_SIZE bytes are read), 0 when it cannot be read. */
uint16_t car_read(Car &car, FILE *f)
{
    std::vector<uint8_t> file(carfile::MAX_SIZE);
    size_t size = fread(file.data(), 1, file.size(), f);
    if (size == 0)
        return 0;
    Reader r = { file.data(), size };

    car = Car();
    size_t vo = r.u16(0), so = r.u16(2), mo = r.u16(4);
    car.drive = r.u16(8);
    car.steer = { r.i16(0x0a) };
    car.throttle = r.i16(0x0c);
    car.handbrake = r.u16(0x0e);
    for (int i = 0; i < 3; i++)
        car.mid[i] = r.i16(0x10 + 2 * i);
    car.steer_used = { r.i16(0x16) };
    car.throttle_used = r.i16(0x18);
    car.cell = r.u16(0x1a);
    for (int i = 0; i < 4; i++)
        car.wheel_speed[i] = r.i32(0x42 + 4 * i);
    for (int i = 0; i < 12; i++)
        car.spin[i] = r.i32(0x52 + 4 * i);

    for (uint16_t i = 0, n = r.u16(vo); i < n && vo + 2 + (i + 1) * carfile::VERTEX <= size; i++)
        car.points.push_back(read_vertex(r, vo + 2 + i * carfile::VERTEX));
    for (uint16_t i = 0, n = r.u16(so); i < n && so + 2 + (i + 1) * carfile::SPRING <= size; i++)
        car.spring_list.push_back(read_spring(r, so + 2 + i * carfile::SPRING));
    if (mo < size)
        car.model_ops = model_decode(std::vector<uint8_t>(file.begin() + (ptrdiff_t)mo, file.begin() + (ptrdiff_t)size));
    return (uint16_t)size;
}
