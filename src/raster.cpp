/*
 * Polygon rasteriser: clipping against the view rectangle, edge scanning
 * and the three span fillers (flat, Gouraud, textured).
 *
 * A polygon is built in render.poly_buf[0] (screen x, y and up to two 16-bit
 * attributes per vertex).  Drawing clips it against the four sides of the
 * clip rectangle, ping-ponging between poly_buf[0] and poly_buf[1], walks
 * its edges into the per-scanline tables render.span_x / render.span_a / render.span_b and then
 * fills every scanline between span_top and span_bottom.
 */
#include <utility>
#include "terep.hpp"


int      render_scale = 1;
Segments segments;


/* Clear the frame buffer to colour 0xff.  orig 1000:2b98 */
void clear_screen()
{
    memset(screen(), 0xff, (size_t)screen_w() * screen_h());
}

/* orig 1000:3f98 */
void put_pixel(int16_t x, int16_t y, uint8_t color)
{
    if (x < render.clip_left || x > render.clip_right || y < render.clip_top || y > render.clip_bottom)
        return;
    screen()[(uint32_t)(y * screen_w() + x)] = color;
}

/* One pixel of the original resolution: a square at higher scales. */
void put_block(int16_t x, int16_t y, uint8_t color)
{
    for (int16_t j = 0; j < render_scale; j++)
        for (int16_t i = 0; i < render_scale; i++)
            put_pixel((int16_t)(x + i), (int16_t)(y + j), color);
}

/* The value at distance 0 on the line through (da, va) and (db, vb), where
 * da and db are signed distances to a clipping plane.  orig 1000:3f7a
 * (1000:4680 is an identical copy). */
int16_t edge_lerp(int16_t da, int16_t va, int16_t db, int16_t vb)
{
    if (va == vb)
        return va;
    int32_t num = (int32_t)db * va - (int32_t)da * vb;
    return (int16_t)(num / (int16_t)(db - da));
}

/* ------------------------------------------------------------------------
 * Clipping against the view rectangle
 * ---------------------------------------------------------------------- */

enum ClipEdge { CLIP_LEFT, CLIP_RIGHT, CLIP_TOP, CLIP_BOTTOM };

/* Signed distance of a vertex to a clip edge, positive inside. */
static int16_t clip_distance(const PolyVertex &v, ClipEdge edge)
{
    switch (edge) {
    case CLIP_LEFT:   return (int16_t)(v.sx - render.clip_left);
    case CLIP_RIGHT:  return (int16_t)(render.clip_right - v.sx);
    case CLIP_TOP:    return (int16_t)(v.sy - render.clip_top);
    default:          return (int16_t)(render.clip_bottom - v.sy);
    }
}

/* How many attributes the polygon carries: 0 flat, 1 shade, 2 texture. */
static void copy_vertex(PolyVertex &dst, const PolyVertex &src, int attrs)
{
    dst.sx = src.sx;
    dst.sy = src.sy;
    if (attrs >= 1) dst.a = src.a;
    if (attrs >= 2) dst.b = src.b;
}

/* Point where the edge p -> c crosses the clip edge. */
static void clip_intersect(PolyVertex &out, const PolyVertex &p, const PolyVertex &c,
                           ClipEdge edge, int attrs)
{
    bool horizontal = edge == CLIP_LEFT || edge == CLIP_RIGHT;
    int16_t bound = edge == CLIP_LEFT ? render.clip_left : edge == CLIP_RIGHT ? render.clip_right
                  : edge == CLIP_TOP ? render.clip_top : render.clip_bottom;
    /* distances measured along the clipped axis, as the original does */
    int16_t dc = (int16_t)((horizontal ? c.sx : c.sy) - bound);
    int16_t dp = (int16_t)((horizontal ? p.sx : p.sy) - bound);

    if (horizontal) {
        out.sx = bound;
        out.sy = edge_lerp(dc, c.sy, dp, p.sy);
    } else {
        out.sx = edge_lerp(dc, c.sx, dp, p.sx);
        out.sy = bound;
    }
    if (attrs == 1) {
        out.a = (uint16_t)edge_lerp(dc, (int16_t)c.a, dp, (int16_t)p.a);
    } else if (attrs == 2) {
        /* texture coordinates are unsigned: interpolated at half precision */
        out.a = (uint16_t)(edge_lerp(dc, (int16_t)(c.a >> 1), dp, (int16_t)(p.a >> 1)) << 1);
        out.b = (uint16_t)(edge_lerp(dc, (int16_t)(c.b >> 1), dp, (int16_t)(p.b >> 1)) << 1);
    }
}

/* One Sutherland-Hodgman pass.  The input's first vertex is copied past
 * its last one to close the loop.  When an edge leaves through the left or
 * top side from a vertex lying exactly on it, no extra vertex is added.
 * orig 1000:2df2/2eaf/2f6c/302d (flat), 324f/3376/34a2/35cf (shade),
 * 390a/3aa3/3c3c/3ddb (texture) */
static void clip_pass(PolyBuf &src, PolyBuf &dst, ClipEdge edge, int attrs)
{
    PolyVertex *in = src.v;             /* may run past v[9], like the original */
    PolyVertex *out = dst.v;
    uint16_t n = src.count, count = 0;

    if (n) {
        copy_vertex(in[n], in[0], attrs);
        bool prev_in = clip_distance(in[0], edge) >= 0;
        if (prev_in)
            copy_vertex(out[count++], in[0], attrs);
        for (uint16_t i = 1; i <= n; i++) {
            const PolyVertex &p = in[i - 1], &c = in[i];
            bool cur_in = clip_distance(c, edge) >= 0;
            if (prev_in && !cur_in) {
                bool on_edge = (edge == CLIP_LEFT || edge == CLIP_TOP) && clip_distance(p, edge) == 0;
                if (!on_edge)
                    clip_intersect(out[count++], p, c, edge, attrs);
            } else if (!prev_in && cur_in) {
                clip_intersect(out[count++], p, c, edge, attrs);
            }
            if (cur_in && i < n)
                copy_vertex(out[count++], c, attrs);
            prev_in = cur_in;
        }
    }
    dst.count = count;
}

/* Clip poly_buf[0] in place; returns the number of vertices left. */
static uint16_t clip_polygon(int attrs)
{
    PolyBuf &a = render.poly_buf[0], &b = render.poly_buf[1];
    clip_pass(a, b, CLIP_LEFT, attrs);
    clip_pass(b, a, CLIP_RIGHT, attrs);
    clip_pass(a, b, CLIP_TOP, attrs);
    clip_pass(b, a, CLIP_BOTTOM, attrs);
    return a.count;
}

/* ------------------------------------------------------------------------
 * Edge scanning
 * ---------------------------------------------------------------------- */

/* Record the x of an edge on every scanline it covers, into the left table
 * for downward edges and the right one for upward edges.  16.16 DDA; the
 * last row gets the accumulated value, not x1.  orig 1000:2c4b */
static void scan_edge_x(int16_t x0, int16_t y0, int16_t x1, int16_t y1)
{
    int side = 0;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        side = 1;
    }
    if (y0 < render.span_top) render.span_top = y0;
    if (y1 > render.span_bottom) render.span_bottom = y1;

    int16_t dy = (int16_t)(y1 - y0);
    if (!dy)
        return;
    int32_t step = (int32_t)((uint32_t)(uint16_t)(x1 - x0) << 16) / dy;
    uint32_t x = (uint32_t)(uint16_t)x0 << 16;
    int16_t *row = side ? &render.span_x[0].right : &render.span_x[0].left;
    row += 2 * y0;
    for (int16_t i = 0; i < dy; i++, row += 2) {
        *row = (int16_t)(x >> 16);
        x += (uint32_t)step;
    }
    *row = (int16_t)(x >> 16);
}

/* Same for one attribute.  orig 1000:317d */
static void scan_edge_attr(int16_t *table, int16_t a0, int16_t y0, int16_t a1, int16_t y1)
{
    int side = 0;
    if (y0 > y1) {
        std::swap(a0, a1);
        std::swap(y0, y1);
        side = 1;
    }
    int16_t dy = (int16_t)(y1 - y0);
    if (!dy)
        return;
    int32_t step = (int32_t)((uint32_t)(uint16_t)(a1 - a0) << 16) / dy;
    uint32_t a = (uint32_t)(uint16_t)a0 << 16;
    int16_t *row = table + side + 2 * y0;
    for (int16_t i = 0; i < dy; i++, row += 2) {
        *row = (int16_t)(a >> 16);
        a += (uint32_t)step;
    }
    *row = (int16_t)(a >> 16);
}

/* Both texture coordinates at once.  orig 1000:379b */
static void scan_edge_uv(int16_t y0, int16_t y1, uint16_t u0, uint16_t v0, uint16_t u1, uint16_t v1)
{
    int side = 0;
    if (y0 > y1) {
        std::swap(y0, y1);
        std::swap(u0, u1);
        std::swap(v0, v1);
        side = 1;
    }
    int16_t dy = (int16_t)(y1 - y0);
    if (!dy)
        return;
    int32_t du = (int32_t)((uint32_t)(uint16_t)(u1 - u0) << 16) / dy;
    int32_t dv = (int32_t)((uint32_t)(uint16_t)(v1 - v0) << 16) / dy;
    uint32_t u = (uint32_t)u0 << 16, v = (uint32_t)v0 << 16;
    int16_t *ru = &render.span_a[0].left + side + 2 * y0;
    int16_t *rv = &render.span_b[0].left + side + 2 * y0;
    for (int16_t i = 0; i < dy; i++, ru += 2, rv += 2) {
        *ru = (int16_t)(u >> 16);
        *rv = (int16_t)(v >> 16);
        u += (uint32_t)du;
        v += (uint32_t)dv;
    }
    *ru = (int16_t)(u >> 16);
    *rv = (int16_t)(v >> 16);
}

/* Walk the polygon outline into render.span_x.  Returns 0 when nothing is left
 * after clipping. */
static bool scan_outline(int attrs)
{
    uint16_t n = clip_polygon(attrs);
    if (n < 3)
        return false;
    const PolyVertex *v = render.poly_buf[0].v;
    render.span_top = render.clip_bottom;
    render.span_bottom = render.clip_top;
    for (uint16_t i = 0; i + 1 < n; i++)
        scan_edge_x(v[i].sx, v[i].sy, v[i + 1].sx, v[i + 1].sy);
    scan_edge_x(v[n - 1].sx, v[n - 1].sy, v[0].sx, v[0].sy);
    return true;
}

/* Left end and width of a scanline; the tables may hold the ends in either
 * order.  A few degenerate polygons give spans outside the clip rectangle:
 * the original writes them anyway, wrapping around its 64 KB screen
 * segment (see pixel_mask), higher scales cut them to the rectangle. */
static uint32_t span_extent(int16_t y, int16_t *x0, int16_t *w)
{
    int16_t l = render.span_x[y].left;
    int16_t d = (int16_t)(render.span_x[y].right - l);
    if (d < 0) {
        l = (int16_t)(l + d);
        d = (int16_t)-d;
    }
    if (render_scale > 1) {
        int16_t r = (int16_t)(l + d);
        if (l < render.clip_left) l = render.clip_left;
        if (r > render.clip_right) r = render.clip_right;
        d = (int16_t)(r < l ? -1 : r - l);
    }
    *x0 = l;
    *w = (int16_t)(d + 1);
    return (uint32_t)(y * screen_w() + l);
}

/* Frame buffer offsets wrap at 64 KB at scale 1, like the original's. */
static uint32_t pixel_mask() { return render_scale > 1 ? 0xffffffffu : 0xffffu; }

/* ------------------------------------------------------------------------
 * Span fillers
 * ---------------------------------------------------------------------- */

/* Flat polygon in poly_color.  A colour word >= 0xf0f0 is translucent:
 * its high byte - 0xf0 picks a row of the shade table.  Otherwise the two
 * bytes of the word are written alternately (a dither pattern when they
 * differ).  orig 1000:2bec / 1000:2d61 */
void draw_flat()
{
    if (!scan_outline(0))
        return;
    int16_t rows = (int16_t)(render.span_bottom - render.span_top);
    if (!rows)
        return;
    uint8_t *scr = screen();
    const uint32_t m = pixel_mask();
    uint16_t color = render.poly_color;
    for (int16_t y = render.span_top; rows >= 0; rows--, y++) {
        int16_t x0, w;
        uint32_t o = span_extent(y, &x0, &w);
        if (color >= 0xf0f0) {
            const uint8_t *shade = map_tables.shade_table[(uint8_t)((color >> 8) - 0xf0)];
            for (uint16_t i = 0; i < (uint16_t)w; i++, o++)
                scr[o & m] = shade[scr[o & m]];
        } else {
            for (uint16_t i = 0; i < (uint16_t)w; i++, o++)
                scr[o & m] = (i & 1) ? (uint8_t)(color >> 8) : (uint8_t)color;
        }
    }
}

/* Gouraud-shaded polygon, attribute a = colour in 8.8.  orig 1000:30ee / 1000:31d1 */
void draw_shaded()
{
    if (!scan_outline(1))
        return;
    const PolyVertex *v = render.poly_buf[0].v;
    uint16_t n = render.poly_buf[0].count;
    int16_t *tab = &render.span_a[0].left;
    for (uint16_t i = 0; i + 1 < n; i++)
        scan_edge_attr(tab, (int16_t)v[i].a, v[i].sy, (int16_t)v[i + 1].a, v[i + 1].sy);
    scan_edge_attr(tab, (int16_t)v[n - 1].a, v[n - 1].sy, (int16_t)v[0].a, v[0].sy);

    int16_t rows = (int16_t)(render.span_bottom - render.span_top);
    if (!rows)
        return;
    uint8_t *scr = screen();
    const uint32_t m = pixel_mask();
    for (int16_t y = render.span_top; rows >= 0; rows--, y++) {
        int16_t a0 = render.span_a[y].left;
        int16_t da = (int16_t)(render.span_a[y].right - a0);
        if ((int16_t)(render.span_x[y].right - render.span_x[y].left) < 0) {
            a0 = (int16_t)(a0 + da);
            da = (int16_t)-da;
        }
        int16_t x0, w;
        uint32_t o = span_extent(y, &x0, &w);
        int32_t step = ((int32_t)da << 8) / (int32_t)(uint16_t)w;
        uint32_t a = (uint32_t)((int32_t)a0 << 8);
        for (uint16_t i = 0; i < (uint16_t)w; i++, o++) {
            scr[o & m] = (uint8_t)(a >> 16);
            a += (uint32_t)step;
        }
    }
}

/* Texture-mapped polygon (affine), attributes u, v in 8.8 texels of a
 * 256-wide texture.  Texel 0xff is transparent, 0xf0..0xfe darken what is
 * behind through the shade table.  orig 1000:36fe / 1000:3827 */
void draw_textured(const uint8_t *tex)
{
    if (!scan_outline(2))
        return;
    const PolyVertex *v = render.poly_buf[0].v;
    uint16_t n = render.poly_buf[0].count;
    for (uint16_t i = 0; i + 1 < n; i++)
        scan_edge_uv(v[i].sy, v[i + 1].sy, v[i].a, v[i].b, v[i + 1].a, v[i + 1].b);
    scan_edge_uv(v[n - 1].sy, v[0].sy, v[n - 1].a, v[n - 1].b, v[0].a, v[0].b);

    int16_t rows = (int16_t)(render.span_bottom - render.span_top);
    if (!rows)
        return;
    uint8_t *scr = screen();
    const uint32_t m = pixel_mask();
    for (int16_t y = render.span_top; rows >= 0; rows--, y++) {
        int16_t u0 = render.span_a[y].left, du = (int16_t)(render.span_a[y].right - u0);
        int16_t v0 = render.span_b[y].left, dv = (int16_t)(render.span_b[y].right - v0);
        if ((int16_t)(render.span_x[y].right - render.span_x[y].left) < 0) {
            u0 = (int16_t)(u0 + du); du = (int16_t)-du;
            v0 = (int16_t)(v0 + dv); dv = (int16_t)-dv;
        }
        int16_t x0, w;
        uint32_t o = span_extent(y, &x0, &w);
        int32_t ustep = ((int32_t)du << 8) / (int32_t)(uint16_t)w;
        int32_t vstep = ((int32_t)dv << 8) / (int32_t)(uint16_t)w;
        uint32_t u = (uint32_t)(uint16_t)u0 << 8, vv = (uint32_t)(uint16_t)v0 << 8;
        for (uint16_t i = 0; i < (uint16_t)w; i++, o++) {
            uint8_t t = tex[(uint16_t)((u >> 16) + ((vv >> 16) << 8))];
            if (t != 0xff)
                scr[o & m] = t >= 0xf0 ? map_tables.shade_table[t - 0xf0][scr[o & m]] : t;
            u += (uint32_t)ustep;
            vv += (uint32_t)vstep;
        }
    }
}

/* Is poly_buf[0] (its first three vertices) facing away, i.e. clockwise
 * on screen?  orig 1000:2662 */
bool poly_backfacing()
{
    const PolyVertex *v = render.poly_buf[0].v;
    int32_t a = (int32_t)(int16_t)(v[0].sx - v[1].sx) * (int16_t)(v[2].sy - v[1].sy);
    int32_t b = (int32_t)(int16_t)(v[2].sx - v[1].sx) * (int16_t)(v[0].sy - v[1].sy);
    return (int32_t)((uint32_t)b - (uint32_t)a) < 0;
}

/* ------------------------------------------------------------------------
 * Building polygons from camera-space points, clipped at the near plane
 * ---------------------------------------------------------------------- */


/* Projection of a point lying on the near plane.  The original uses the
 * full-screen centre here even in split screen.  Far above or below the eye
 * the projection overflows 16 bits: the original crashes there, this port
 * wraps around (see project). */
static void near_point(int16_t x, int16_t z, uint32_t attr)
{
    PolyVertex &o = render.poly_buf[0].v[render.near_prev.count++];
    int16_t sx, sy;
    project(x, render.near_clip, z, &sx, &sy);
    o.sx = (int16_t)(screen_clamp(sx * render_scale) + 160 * render_scale);
    o.sy = (int16_t)(100 * render_scale - screen_clamp(sy * render_scale));
    o.a = (uint16_t)attr;
    o.b = (uint16_t)(attr >> 16);
}

static void emit_projected(const ProjVertex &p, uint32_t attr)
{
    PolyVertex &o = render.poly_buf[0].v[render.near_prev.count++];
    o.sx = p.sx;
    o.sy = p.sy;
    o.a = (uint16_t)attr;
    o.b = (uint16_t)(attr >> 16);
}

/* First point of a polygon.  attr is (b << 16 | a).  orig 1000:46a0 */
void poly_begin(const ProjVertex &p, uint32_t attr)
{
    render.near_prev.count = 0;
    render.near_prev.dist = (int16_t)(p.y - render.near_clip);
    if (render.near_prev.dist >= 0)
        emit_projected(p, attr);
    render.near_prev.x = p.x;
    render.near_prev.z = p.z;
}

/* Next point.  Where the outline crosses the near plane a point on the
 * plane is inserted; it takes the attribute of the current point.
 * orig 1000:46d3 */
void poly_add(const ProjVertex &p, uint32_t attr)
{
    int16_t d = (int16_t)(p.y - render.near_clip);
    if (render.near_prev.dist >= 0) {
        if (d >= 0)
            emit_projected(p, attr);
        else if (render.near_prev.dist != 0)
            near_point(edge_lerp(d, p.x, render.near_prev.dist, render.near_prev.x), edge_lerp(d, p.z, render.near_prev.dist, render.near_prev.z), attr);
    } else if (d >= 0) {
        near_point(edge_lerp(render.near_prev.dist, render.near_prev.x, d, p.x), edge_lerp(render.near_prev.dist, render.near_prev.z, d, p.z), attr);
        emit_projected(p, attr);
    }
    render.near_prev.dist = d;
    render.near_prev.x = p.x;
    render.near_prev.z = p.z;
}

/* Closing point (normally the first one again): only the crossing is
 * added.  Sets the vertex count; returns it.  orig 1000:47ec */
uint16_t poly_end(const ProjVertex &p, uint32_t attr)
{
    int16_t d = (int16_t)(p.y - render.near_clip);
    if (render.near_prev.dist >= 0) {
        if (d < 0 && render.near_prev.dist != 0)
            near_point(edge_lerp(d, p.x, render.near_prev.dist, render.near_prev.x), edge_lerp(d, p.z, render.near_prev.dist, render.near_prev.z), attr);
    } else if (d >= 0) {
        near_point(edge_lerp(render.near_prev.dist, render.near_prev.x, d, p.x), edge_lerp(render.near_prev.dist, render.near_prev.z, d, p.z), attr);
    }
    render.poly_buf[0].count = render.near_prev.count;
    return render.near_prev.count;
}
