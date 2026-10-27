/* SSIMF - ScreenShot IMage Format v1 (experimental). MIT License, Copyright (c) 2026 imgformats.com. */
#ifndef SSIMF_H
#define SSIMF_H
#include <stddef.h>
#include <stdint.h>
/* Worst-case encoded size for a w x h image. */
size_t ssimf_max_size(int w, int h);
/* rgba: w*h*4 bytes. Writes into out (capacity cap). Returns encoded size or -1. */
long ssimf_encode(const uint8_t *rgba, int w, int h, uint8_t *out, size_t cap);
/* Reads width/height from an SSIMF header. Returns 0 on success. */
int ssimf_info(const uint8_t *in, size_t len, int *w, int *h);
/* Decodes into rgba (w*h*4 bytes). Returns bytes written or -1. */
long ssimf_decode(const uint8_t *in, size_t len, uint8_t *rgba);
#endif
