/* Map screen: OpenStreetMap tiles and node dots, drawn into the XFB.
 *
 * The XFB holds Y'CbCr 4:2:2: one u32 holds two pixels (Y1 Cb Y2 Cr).
 * The map is drawn into a cached canvas first, then copied to the XFB.
 * Tiles come from tile.openstreetmap.org, the same source as the
 * Sonoran web map (MAP_TILE_URL default in sonoran/config.py). */
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <png.h>

#include "https.h"
#include "map.h"
#include "net.h"

#define TILE_HOST "tile.openstreetmap.org"
#define TILE 256
#define CACHE_TILES 40
#define DOT 6

struct tile {
    int z, x, y;
    int state;          /* 0 = empty, 1 = ready, 2 = failed */
    unsigned age;
    u32 *yuv;           /* TILE/2 * TILE words */
};

static struct tile cache[CACHE_TILES];
static unsigned clock_age;
static u32 *xfb_words;
static int fb_w, fb_h, map_y0, map_y1, map_h, pairs_w;
static u32 *canvas;
static int failed;

/* ---- colour ---- */

static void rgb_to_ycbcr(int r, int g, int b, int *y, int *cb, int *cr)
{
    *y = 16 + (66 * r + 129 * g + 25 * b + 128) / 256;
    *cb = 128 + (-38 * r - 74 * g + 112 * b + 128) / 256;
    *cr = 128 + (112 * r - 94 * g - 18 * b + 128) / 256;
}

static u32 pair_of(int r1, int g1, int b1, int r2, int g2, int b2)
{
    int y1, cb1, cr1, y2, cb2, cr2;

    rgb_to_ycbcr(r1, g1, b1, &y1, &cb1, &cr1);
    rgb_to_ycbcr(r2, g2, b2, &y2, &cb2, &cr2);
    return ((u32)y1 << 24) | ((u32)((cb1 + cb2) / 2) << 16) | ((u32)y2 << 8)
           | (u32)((cr1 + cr2) / 2);
}

static u32 solid(int r, int g, int b)
{
    return pair_of(r, g, b, r, g, b);
}

/* ---- PNG ---- */

struct mem_reader {
    const unsigned char *data;
    size_t len, pos;
};

static void png_mem_read(png_structp png, png_bytep out, png_size_t n)
{
    struct mem_reader *m = png_get_io_ptr(png);

    if (m->pos + n > m->len)
        png_error(png, "short PNG");
    memcpy(out, m->data + m->pos, n);
    m->pos += n;
}

/* Decode a 256x256 PNG into YUV pairs. Return 0 on success. */
static int decode_tile(const unsigned char *data, size_t len, u32 *yuv)
{
    png_structp png;
    png_infop info;
    struct mem_reader m = { data, len, 0 };
    unsigned char *rgb = NULL;
    png_bytep rows[TILE];
    int i, x, ok = -1;

    if (len < 8 || png_sig_cmp(data, 0, 8) != 0)
        return -1;
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL)
        return -1;
    info = png_create_info_struct(png);
    if (info == NULL) {
        png_destroy_read_struct(&png, NULL, NULL);
        return -1;
    }
    if (setjmp(png_jmpbuf(png))) {
        free(rgb);
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }
    png_set_read_fn(png, &m, png_mem_read);
    png_read_info(png, info);
    if (png_get_image_width(png, info) != TILE || png_get_image_height(png, info) != TILE)
        png_error(png, "tile is not 256x256");
    png_set_palette_to_rgb(png);
    png_set_expand_gray_1_2_4_to_8(png);
    png_set_gray_to_rgb(png);
    png_set_strip_16(png);
    png_set_strip_alpha(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != TILE * 3)
        png_error(png, "unexpected row size");

    rgb = malloc(TILE * TILE * 3);
    if (rgb == NULL)
        png_error(png, "out of memory");
    for (i = 0; i < TILE; i++)
        rows[i] = rgb + i * TILE * 3;
    png_read_image(png, rows);

    for (i = 0; i < TILE; i++) {
        const unsigned char *p = rows[i];
        u32 *o = yuv + i * (TILE / 2);
        for (x = 0; x < TILE / 2; x++, p += 6)
            o[x] = pair_of(p[0], p[1], p[2], p[3], p[4], p[5]);
    }
    ok = 0;
    free(rgb);
    png_destroy_read_struct(&png, &info, NULL);
    return ok;
}

/* ---- tile cache ---- */

static struct tile *find_tile(int z, int x, int y)
{
    int i;

    for (i = 0; i < CACHE_TILES; i++)
        if (cache[i].state != 0 && cache[i].z == z && cache[i].x == x && cache[i].y == y) {
            cache[i].age = ++clock_age;
            return &cache[i];
        }
    return NULL;
}

static struct tile *load_tile(int z, int x, int y)
{
    struct tile *t = NULL;
    struct http_reply r;
    char path[64];
    int i;

    for (i = 0; i < CACHE_TILES; i++) {
        if (cache[i].state == 0) {
            t = &cache[i];
            break;
        }
        if (t == NULL || cache[i].age < t->age)
            t = &cache[i];
    }
    if (t->yuv == NULL) {
        t->yuv = malloc(TILE / 2 * TILE * sizeof(u32));
        if (t->yuv == NULL)
            return NULL;
    }
    t->z = z;
    t->x = x;
    t->y = y;
    t->age = ++clock_age;
    t->state = 2;

    snprintf(path, sizeof(path), "/%d/%d/%d.png", z, x, y);
    if (https_get(TILE_HOST, path, NULL, &r) == 0 && r.status == 200
        && decode_tile((const unsigned char *)r.body, r.len, t->yuv) == 0)
        t->state = 1;
    else
        dbg("tile %s failed: HTTP %d %s", path, r.status, r.err);
    http_free(&r);
    return t;
}

/* ---- geometry ---- */

static double world_x(double lon, int z)
{
    return (lon + 180.0) / 360.0 * (double)(TILE << z);
}

static double world_y(double lat, int z)
{
    double s = sin(lat * M_PI / 180.0);
    return (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * (double)(TILE << z);
}

static double lon_of(double wx, int z)
{
    return wx / (double)(TILE << z) * 360.0 - 180.0;
}

static double lat_of(double wy, int z)
{
    double n = M_PI - 2.0 * M_PI * wy / (double)(TILE << z);
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

/* Left and top world pixel of the view. Left is even, because a u32
 * holds two pixels. */
static void view_origin(const struct map_view *v, long *left, long *top)
{
    *left = (long)floor(world_x(v->lon, v->zoom)) - fb_w / 2;
    *left &= ~1L;
    *top = (long)floor(world_y(v->lat, v->zoom)) - map_h / 2;
}

/* ---- drawing ---- */

void map_setup(void *xfb, int fb_width, int fb_height, int y0, int y1)
{
    xfb_words = xfb;
    fb_w = fb_width;
    fb_h = fb_height;
    map_y0 = y0;
    map_y1 = y1;
    map_h = y1 - y0;
    pairs_w = fb_w / 2;
    if (canvas == NULL)
        canvas = malloc((size_t)pairs_w * map_h * sizeof(u32));
}

static void present(void)
{
    int y;

    for (y = 0; y < map_h; y++)
        memcpy(xfb_words + (size_t)(map_y0 + y) * pairs_w, canvas + (size_t)y * pairs_w,
               pairs_w * sizeof(u32));
}

static void fill(u32 colour)
{
    size_t i, n = (size_t)pairs_w * map_h;

    for (i = 0; i < n; i++)
        canvas[i] = colour;
}

/* Copy one tile to the canvas. sx (even) and sy are screen positions. */
static void blit(const struct tile *t, long sx, long sy)
{
    int row;

    for (row = 0; row < TILE; row++) {
        long y = sy + row;
        long x0 = sx, x1 = sx + TILE;
        const u32 *src;
        if (y < 0 || y >= map_h)
            continue;
        if (x0 < 0)
            x0 = 0;
        if (x1 > fb_w)
            x1 = fb_w;
        if (x0 >= x1)
            continue;
        src = t->yuv + row * (TILE / 2) + (x0 - sx) / 2;
        memcpy(canvas + y * pairs_w + x0 / 2, src, (size_t)(x1 - x0) / 2 * sizeof(u32));
    }
}

static void box(long cx, long cy, int half, u32 colour)
{
    long x, y;

    for (y = cy - half; y <= cy + half; y++) {
        if (y < 0 || y >= map_h)
            continue;
        for (x = (cx - half) & ~1L; x <= cx + half; x += 2) {
            if (x < 0 || x >= fb_w)
                continue;
            canvas[y * pairs_w + x / 2] = colour;
        }
    }
}

static void draw_nodes(const struct map_view *v, const struct map_node *nodes, int n)
{
    u32 red = solid(220, 20, 60), blue = solid(40, 70, 220), white = solid(255, 255, 255);
    u32 black = solid(0, 0, 0);
    long left, top;
    int i, pass;

    view_origin(v, &left, &top);
    /* Gateways last, so they stay visible on top. */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < n; i++) {
            long sx, sy;
            if (nodes[i].gateway != pass)
                continue;
            sx = (long)world_x(nodes[i].lon, v->zoom) - left;
            sy = (long)world_y(nodes[i].lat, v->zoom) - top;
            if (sx < -DOT || sx > fb_w + DOT || sy < -DOT || sy > map_h + DOT)
                continue;
            box(sx, sy, DOT / 2 + 1, white);
            box(sx, sy, DOT / 2, pass ? blue : red);
        }
    }
    /* Center cross */
    for (i = -10; i <= 10; i++) {
        box(fb_w / 2 + i, map_h / 2, 0, black);
        box(fb_w / 2, map_h / 2 + i, 0, black);
    }
}

void map_draw(const struct map_view *v, const struct map_node *nodes, int n)
{
    long left, top;
    int tx, ty, tx0, ty0, tx1, ty1, max = 1 << v->zoom;
    int pass;

    if (canvas == NULL)
        return;
    failed = 0;
    view_origin(v, &left, &top);
    tx0 = (int)floor((double)left / TILE);
    ty0 = (int)floor((double)top / TILE);
    tx1 = (int)floor((double)(left + fb_w - 1) / TILE);
    ty1 = (int)floor((double)(top + map_h - 1) / TILE);

    fill(solid(200, 200, 200));
    /* Pass 0 draws the cached tiles at once. Pass 1 loads the others. */
    for (pass = 0; pass < 2; pass++) {
        for (ty = ty0; ty <= ty1; ty++) {
            for (tx = tx0; tx <= tx1; tx++) {
                int wx = ((tx % max) + max) % max;
                struct tile *t;
                if (ty < 0 || ty >= max)
                    continue;
                t = find_tile(v->zoom, wx, ty);
                if (pass == 0) {
                    if (t != NULL && t->state == 1)
                        blit(t, (long)tx * TILE - left, (long)ty * TILE - top);
                    continue;
                }
                if (t != NULL)
                    continue;
                t = load_tile(v->zoom, wx, ty);
                if (t != NULL && t->state == 1)
                    blit(t, (long)tx * TILE - left, (long)ty * TILE - top);
                else
                    failed++;
                present();
            }
        }
        if (pass == 0)
            present();
    }
    draw_nodes(v, nodes, n);
    present();
}

int map_nearest(const struct map_view *v, const struct map_node *nodes, int n)
{
    double cx = world_x(v->lon, v->zoom), cy = world_y(v->lat, v->zoom);
    double best = 30.0 * 30.0;
    int i, found = -1;

    for (i = 0; i < n; i++) {
        double dx = world_x(nodes[i].lon, v->zoom) - cx;
        double dy = world_y(nodes[i].lat, v->zoom) - cy;
        double d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            found = i;
        }
    }
    return found;
}

void map_pan(struct map_view *v, int dx, int dy)
{
    double wx = world_x(v->lon, v->zoom) + dx;
    double wy = world_y(v->lat, v->zoom) + dy;
    double size = (double)(TILE << v->zoom);

    if (wy < 0)
        wy = 0;
    if (wy > size)
        wy = size;
    v->lon = lon_of(fmod(wx + size, size), v->zoom);
    v->lat = lat_of(wy, v->zoom);
}

int map_failed_tiles(void)
{
    return failed;
}

void fb_rect(int x, int y, int w, int h, int white)
{
    u32 c = white ? solid(255, 255, 255) : solid(0, 0, 0);
    int row, col;

    x &= ~1;
    w = (w + 1) & ~1;
    for (row = y; row < y + h; row++) {
        if (row < 0 || row >= fb_h)
            continue;
        for (col = x; col < x + w; col += 2) {
            if (col < 0 || col >= fb_w)
                continue;
            xfb_words[(size_t)row * pairs_w + col / 2] = c;
        }
    }
}
