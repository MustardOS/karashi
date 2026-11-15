#ifndef KARASHI_PIXEL_H
#define KARASHI_PIXEL_H

#include <stdint.h>

typedef uint16_t karashi_pixel;

#define KARASHI_PIXEL(red, green, blue) \
	((karashi_pixel) ((((red) >> 3) << 11) | (((green) >> 2) << 5) | ((blue) >> 3)))

#endif
