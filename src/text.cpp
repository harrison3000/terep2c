/*
 * Bitmap font text.  Each glyph is a list of column bytes (bit 0 = top
 * row) ended by a zero byte, read at startup from src/font.pbm.
 */
#include <cstdio>
#include "terep.hpp"

/* The font: src/font.pbm (plain PBM), 5 pixels wide, one 5x5 glyph below
 * the other in memory order; a glyph is as wide as its rightmost lit column.
 * font_chars lists the character of each glyph; every other character
 * shows glyph 0 ('A'). */
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"    /* #embed in C++ (g++ 15+ accepts it quietly) */
#endif
static const char font_pbm[] = {
#embed "font.pbm"
    , 0
};
#ifdef __clang__
#pragma clang diagnostic pop
#endif
static const char font_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!\"#$%&'()*+,-./0123456789:;<=>?{|}~";

/* font.pbm is built in, so it is trusted: no error checks. */
static const struct Font {
    uint16_t offset[128];           /* of each character's glyph in glyphs */
    uint8_t  glyphs[0x169];
} font = [] {
    Font f = {};
    const char *p = strchr(font_pbm, '\n') + 1;      /* after "P1" */
    while (*p == '#')                                   /* comments */
        p = strchr(p, '\n') + 1;
    int n;
    sscanf(p, "%*d %*d%n", &n);                         /* size: 5 x 5 per glyph */
    p += n;

    uint16_t out = 0;
    for (const char *c = font_chars; *c; c++) {
        uint8_t columns[5] = {};
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++) {
                int bit;
                sscanf(p, " %1d%n", &bit, &n);
                p += n;
                columns[x] |= (uint8_t)(bit << y);
            }
        f.offset[(uint8_t)*c] = out;
        int w = 5;
        while (w > 0 && !columns[w - 1])
            w--;
        memcpy(&f.glyphs[out], columns, w);
        out = (uint16_t)(out + w);
        f.glyphs[out++] = 0;
    }
    return f;
}();

/* Characters >= 128 take the glyph of c & 0x7f.  In the original their
 * 16-bit offsets ran past the table and wrapped around the data segment;
 * only its jump bug (fixed here, see Car::jump) put such characters in
 * the title. */
const uint8_t *font_glyph(uint8_t c)
{
    return &font.glyphs[font.offset[c & 0x7f]];
}

/* Control bytes: 0 or 13 end the string, 9 is a tab (20 pixels),
 * 27 followed by a byte changes the colour, space is 5 pixels wide.
 * orig 1000:5940 */
void draw_text(const char *s, int16_t x, int16_t y, uint8_t color)
{
    for (;;) {
        uint8_t ch = (uint8_t)*s++;
        if (ch == 0 || ch == 13)
            return;
        if (ch == 9) {
            x = (int16_t)(x + 20);
            continue;
        }
        if (ch == 27) {
            color = (uint8_t)*s++;
            continue;
        }
        if (ch == ' ') {
            x = (int16_t)(x + 5);
            continue;
        }
        const uint8_t *col = font_glyph(ch);
        for (; *col; col++, x++) {
            int16_t row = y;
            for (uint8_t bits = *col; bits; bits >>= 1, row++)
                if (bits & 1)
                    put_block((int16_t)(x * render_scale), (int16_t)(row * render_scale), color);
        }
        x++;                            /* one column of spacing */
    }
}
