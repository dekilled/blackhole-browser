/* Image decoders written from scratch: PNG, JPEG (baseline + progressive), GIF (first frame). */
#ifndef BH_IMG_H
#define BH_IMG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "layout.h" /* Image */

#define IMG_MAX_DIM 16384
#define IMG_MAX_PIXELS (12u * 1024 * 1024)

/* Sniffs the format and decodes. On success out->px is malloc'd (ARGB, straight alpha). */
bool image_decode(const uint8_t *data, size_t len, Image *out);
void image_free(Image *img);

bool png_decode(const uint8_t *data, size_t len, Image *out);
bool jpeg_decode(const uint8_t *data, size_t len, Image *out);
bool gif_decode(const uint8_t *data, size_t len, Image *out);

#endif
