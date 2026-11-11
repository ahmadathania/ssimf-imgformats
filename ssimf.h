/* SSIMF - ScreenShot IMage Format v2 (experimental). MIT License, Copyright (c) 2026 imgformats.com. */
#ifndef SSIMF_H
#define SSIMF_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Optional extras for ssimf_encode_ex. Any field can be left 0 / NULL. */
typedef struct {
    const uint8_t *icc;  size_t icc_len;   /* ICC colour profile to store */
    const uint8_t *meta; size_t meta_len;  /* metadata: UTF-8 "key\0value\0" pairs, e.g. "Software\0MyApp\0" */
    int stripe_rows;                       /* 0 = one stripe (smallest file, standard). N = stripes of N rows:
                                              decodes faster on several cores, but the file gets bigger */
    int threads;                           /* 0 = all cores (when built with SSIMF_THREADS), 1 = one core */
} ssimf_options;

/* Worst-case encoded size for a w x h image. Use the _ex form when storing an ICC profile or metadata
   (extra = icc_len + meta_len). */
size_t ssimf_max_size(int w, int h);
size_t ssimf_max_size_ex(int w, int h, size_t extra);

/* rgba: w*h*4 bytes. Writes a version 2 file into out (capacity cap). Returns the encoded size or -1. */
long ssimf_encode(const uint8_t *rgba, int w, int h, uint8_t *out, size_t cap);
long ssimf_encode_ex(const uint8_t *rgba, int w, int h, const ssimf_options *opt, uint8_t *out, size_t cap);

/* Reads width/height from an SSIMF file (any version) and checks its structure. Returns 0 on success.
   Images over 16,777,216 pixels per side or 268 million pixels in total are refused. */
int ssimf_info(const uint8_t *in, size_t len, int *w, int *h);

/* Finds an optional chunk ("ICCP" or "META") in a version 2 file. Returns 0 and points data at it, or -1. */
int ssimf_chunk(const uint8_t *in, size_t len, const char type[4], const uint8_t **data, size_t *data_len);

/* Decodes into rgba (w*h*4 bytes). Returns bytes written or -1 for a broken file.
   ssimf_decode uses all cores when built with SSIMF_THREADS; the _threads form lets you choose. */
long ssimf_decode(const uint8_t *in, size_t len, uint8_t *rgba);
long ssimf_decode_threads(const uint8_t *in, size_t len, uint8_t *rgba, int threads);

#ifdef __cplusplus
}
#endif
#endif
