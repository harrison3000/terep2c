/*
 * AdLib (OPL2) sound: one engine voice per view plus a "hit" effect.
 * The register writes are kept; the host decides what opl_write does
 * (the SDL host plays them through an OPL3 emulator, host/opl_sound.cpp;
 * the test host ignores them).
 */
#include "terep.hpp"

/* AdLib instruments (11 bytes each, see opl_set_patch). */
static const uint8_t engine_patch[0x21] = {
    0x20, 0x00, 0xfb, 0x05, 0x00, 0x20, 0x00, 0xfb, 0x05, 0x00, 0x0a,
    0x20, 0x00, 0xfb, 0x21, 0x00, 0x20, 0x00, 0x9f, 0x05, 0x00, 0x06,
    0x64, 0x00, 0x76, 0x37, 0x00, 0x21, 0x00, 0x79, 0x00, 0x00, 0x0c,
};
static const uint8_t hit_patch[0x3e] = {
    0x00, 0x00, 0xf7, 0xa8, 0x00, 0x00, 0x00, 0xf3, 0x6a, 0x00, 0x02, 0x80, 0x21, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xf0, 0x13, 0x00, 0x05, 0x00, 0xf2, 0x1d, 0x00, 0x06, 0x80, 0x27, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xf0, 0x13, 0x00, 0x05, 0x00, 0xf2, 0x1d, 0x00, 0x06, 0x80, 0x2f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x00,
};

/* operator offset of each OPL2 channel (orig 1000:592c) */
static const uint8_t opl_op_offset[9] = { 0, 1, 2, 8, 9, 0x0a, 0x10, 0x11, 0x12 };

void (*opl_write_hook)(uint8_t reg, uint8_t value);

void opl_write(uint8_t reg, uint8_t value)      /* orig 1000:58fc */
{
    if (opl_write_hook)
        opl_write_hook(reg, value);
}

/* Load an 11-byte instrument into a channel.  orig 1000:589b
 * (the feedback/connection register is addressed with the operator offset
 * instead of the channel number; harmless for the channels used.) */
static void opl_set_patch(uint8_t channel, const uint8_t *patch)
{
    static const uint8_t regs[11] = { 0x20, 0x40, 0x60, 0x80, 0xe0, 0x23, 0x43, 0x63, 0x83, 0xe3, 0xc0 };
    uint8_t op = opl_op_offset[channel];
    for (int i = 0; i < 11; i++)
        opl_write((uint8_t)(regs[i] + op), patch[i]);
}

/* orig 1000:57e0 */
void sound_init()
{
    opl_write(0x01, 0x20);              /* enable waveform select */
    opl_write(0x08, 0x00);
    opl_write(0xbd, 0xc0);
    for (uint8_t r = 0xb0; r <= 0xb8; r++)
        opl_write(r, 0x00);             /* all notes off */
    opl_set_patch(0, engine_patch);
    opl_set_patch(1, engine_patch);
    opl_write(0x44, 0x3f);
    opl_write(0xa1, 0x00);
    opl_write(0xb1, 0x2f);
}

/* Crash/bump sound on channel 8.  orig 1000:5864 */
void sfx_hit(int n)
{
    const uint8_t *p = &hit_patch[n * 16];
    opl_write(0xb8, 0x00);
    opl_set_patch(8, p);
    opl_write(0xa8, p[11]);
    opl_write(0xb8, p[12]);
}

/* Engine note: freq is the 10-bit F-number plus block, level 0..0x3f.
 * orig 1000:5831 */
static void engine_note(uint8_t channel, uint16_t freq, uint8_t level)
{
    opl_write((uint8_t)(0xa0 + channel), (uint8_t)freq);
    opl_write((uint8_t)(0xb0 + channel), (uint8_t)((freq >> 8) | 0x20));
    uint8_t att = (uint8_t)(0x3f - level);
    if (att > 0x3f)
        att = 0x3f;
    opl_write((uint8_t)(0x43 + opl_op_offset[channel]), att);
}

/* Engine pitch follows the driven wheels, volume follows the throttle.
 * orig f_do_the_sound_thing (was 1000:0653..) */
void engine_sound(int car_index, uint8_t channel)
{
    Car &car = car_get(car_index);
    int32_t front = (car.wheel_speed[0] + car.wheel_speed[1]) >> 14;
    int32_t rear = (car.wheel_speed[2] + car.wheel_speed[3]) >> 14;
    int32_t speed;
    switch (car.drive) {
    case Car::REAR:  speed = rear; break;
    case Car::FRONT: speed = front; break;
    default:         speed = (front + rear) >> 1; break;
    }

    uint16_t pitch = (uint16_t)abs16((int16_t)speed) + 0x1030;
    uint16_t level = (uint16_t)abs16((int16_t)(car.throttle >> 9)) + 0x2c;
    engine_note(channel, pitch, (uint8_t)level);
}
