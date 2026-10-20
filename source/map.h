/* Map screen: OpenStreetMap tiles and node dots, drawn into the XFB. */
#ifndef SONORAN_MAP_H
#define SONORAN_MAP_H

#include "api.h"

struct map_view {
    double lat, lon;   /* center */
    int zoom;
};

/* Area of the frame buffer that the map uses. The console text rows
 * above and below stay free. */
void map_setup(void *xfb, int fb_width, int fb_height, int y0, int y1);

/* Draw the tiles and the nodes. Tiles that are not in the cache are
 * loaded over HTTPS; this can take some seconds. */
void map_draw(const struct map_view *v, const struct map_node *nodes, int n);

/* Index of the node nearest to the center cross, or -1. */
int map_nearest(const struct map_view *v, const struct map_node *nodes, int n);

/* Move the center by a part of the screen. dx, dy in screen pixels. */
void map_pan(struct map_view *v, int dx, int dy);

/* Number of tiles that failed in the last map_draw. */
int map_failed_tiles(void);

#endif
