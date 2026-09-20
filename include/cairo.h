#ifndef _SWAY_CAIRO_H
#define _SWAY_CAIRO_H

#include <stdint.h>
#include <cairo/cairo.h>
#include <wayland-client.h>

void cairo_set_source_u32(cairo_t *cairo, uint32_t color);
cairo_subpixel_order_t to_cairo_subpixel_order(enum wl_output_subpixel subpixel);

#endif
