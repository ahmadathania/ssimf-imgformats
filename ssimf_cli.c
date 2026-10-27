/* SSIMF command-line tool. MIT License, Copyright (c) 2026 imgformats.com.
 *   ssimf encode input.png output.ssimf     (input can be PNG, JPG, BMP, TGA)
 *   ssimf decode input.ssimf output.png
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ssimf.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n > 0 ? n : 1);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); *len = (size_t)n; return b;
}
int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "encode") && strcmp(argv[1], "decode"))) {
        fprintf(stderr, "usage:\n  %s encode input.png output.ssimf\n  %s decode input.ssimf output.png\n", argv[0], argv[0]);
        return 1;
    }
    clock_t t0 = clock();
    if (!strcmp(argv[1], "encode")) {
        int w, h, ch;
        uint8_t *rgba = stbi_load(argv[2], &w, &h, &ch, 4);
        if (!rgba) { fprintf(stderr, "cannot read image %s\n", argv[2]); return 1; }
        size_t cap = ssimf_max_size(w, h); uint8_t *out = malloc(cap);
        long n = ssimf_encode(rgba, w, h, out, cap);
        if (n < 0) { fprintf(stderr, "encode failed\n"); return 1; }
        FILE *f = fopen(argv[3], "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        fwrite(out, 1, n, f); fclose(f);
        size_t inlen; uint8_t *in = read_file(argv[2], &inlen); free(in);
        printf("%dx%d  %zu -> %ld bytes (%.1f%%)  %.2fs\n", w, h, inlen, n, 100.0 * n / inlen, (double)(clock() - t0) / CLOCKS_PER_SEC);
    } else {
        size_t len; uint8_t *in = read_file(argv[2], &len);
        int w, h;
        if (!in || ssimf_info(in, len, &w, &h)) { fprintf(stderr, "not an SSIMF file: %s\n", argv[2]); return 1; }
        uint8_t *rgba = malloc((size_t)w * h * 4);
        if (ssimf_decode(in, len, rgba) < 0) { fprintf(stderr, "decode failed (corrupt file?)\n"); return 1; }
        if (!stbi_write_png(argv[3], w, h, 4, rgba, w * 4)) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        printf("%dx%d decoded in %.2fs\n", w, h, (double)(clock() - t0) / CLOCKS_PER_SEC);
    }
    return 0;
}
