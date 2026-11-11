/* SSIMF command-line tool. MIT License, Copyright (c) 2026 imgformats.com.
 *   ssimf encode input.png output.ssimf [--meta Key=Value ...] [--stripes ROWS]
 *   ssimf decode input.ssimf output.png
 *   ssimf info   input.ssimf
 * encode reads PNG, JPG, BMP or TGA. A PNG's colour profile (iCCP) is kept, and written back on decode.
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
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *b = malloc(n > 0 ? n : 1);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); *len = (size_t)n; return b;
}
static int write_file(const char *path, const uint8_t *d, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    size_t w = fwrite(d, 1, n, f); fclose(f); return w == n ? 0 : -1;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* The ICC profile inside a PNG (iCCP chunk), uncompressed. NULL if there is none. */
static uint8_t *png_icc(const uint8_t *f, size_t n, size_t *icc_len) {
    if (n < 8 || memcmp(f, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    size_t p = 8;
    while (p + 12 <= n) {
        uint32_t cl = be32(f + p);
        if (cl > n - p - 12) break;
        const uint8_t *t = f + p + 4, *d = f + p + 8;
        if (!memcmp(t, "iCCP", 4)) {
            size_t k = 0; while (k < cl && d[k]) k++;          /* profile name, then 0, then method byte */
            if (k + 2 > cl) return NULL;
            int out = 0;
            char *icc = stbi_zlib_decode_malloc_guesssize_headerflag((const char *)d + k + 2, (int)(cl - k - 2), 4096, &out, 1);
            if (!icc || out <= 0) { free(icc); return NULL; }
            *icc_len = (size_t)out; return (uint8_t *)icc;
        }
        if (!memcmp(t, "IDAT", 4)) break;
        p += 12 + cl;
    }
    return NULL;
}

/* Puts an iCCP chunk right after IHDR in a PNG held in memory. */
static uint8_t *png_add_icc(const uint8_t *png, int n, const uint8_t *icc, size_t icc_len, int *out_n) {
    int zl = 0;
    unsigned char *z = stbi_zlib_compress((unsigned char *)icc, (int)icc_len, &zl, 8);
    if (!z || n < 33) { free(z); return NULL; }
    static const char name[] = "ICC Profile";
    size_t dl = sizeof name + 1 + (size_t)zl;            /* name + 0, method 0, data */
    uint8_t *o = malloc((size_t)n + 12 + dl);
    if (!o) { free(z); return NULL; }
    memcpy(o, png, 33);                                  /* signature + IHDR */
    uint8_t *c = o + 33;
    wbe32(c, (uint32_t)dl); memcpy(c + 4, "iCCP", 4);
    memcpy(c + 8, name, sizeof name); c[8 + sizeof name] = 0; memcpy(c + 9 + sizeof name, z, zl);
    wbe32(c + 8 + dl, stbiw__crc32(c + 4, (int)dl + 4));
    memcpy(c + 12 + dl, png + 33, (size_t)n - 33);
    free(z); *out_n = n + 12 + (int)dl; return o;
}

static void usage(const char *a) {
    fprintf(stderr, "usage:\n  %s encode input.png output.ssimf [--meta Key=Value ...] [--stripes ROWS]\n"
                    "  %s decode input.ssimf output.png\n  %s info input.ssimf\n", a, a, a);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }
    clock_t t0 = clock();
    if (!strcmp(argv[1], "encode") && argc >= 4) {
        ssimf_options o = {0};
        char meta[8192]; size_t ml = 0;
        for (int a = 4; a < argc; a++) {
            if (!strcmp(argv[a], "--stripes") && a + 1 < argc) o.stripe_rows = atoi(argv[++a]);
            else if (!strcmp(argv[a], "--meta") && a + 1 < argc) {
                const char *kv = argv[++a], *eq = strchr(kv, '=');
                size_t kl = eq ? (size_t)(eq - kv) : 0, vl = eq ? strlen(eq + 1) : 0;
                if (!eq || !kl || ml + kl + vl + 2 > sizeof meta) { fprintf(stderr, "bad --meta %s (use Key=Value)\n", kv); return 1; }
                memcpy(meta + ml, kv, kl); meta[ml + kl] = 0; memcpy(meta + ml + kl + 1, eq + 1, vl); meta[ml + kl + 1 + vl] = 0;
                ml += kl + vl + 2;
            } else { usage(argv[0]); return 1; }
        }
        size_t inlen; uint8_t *file = read_file(argv[2], &inlen);
        if (!file) { fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        int w, h, ch;
        uint8_t *rgba = stbi_load_from_memory(file, (int)inlen, &w, &h, &ch, 4);
        if (!rgba) { fprintf(stderr, "cannot read image %s\n", argv[2]); return 1; }
        size_t icc_len = 0; uint8_t *icc = png_icc(file, inlen, &icc_len);
        o.icc = icc; o.icc_len = icc_len;
        if (ml) { o.meta = (const uint8_t *)meta; o.meta_len = ml; }
        size_t cap = ssimf_max_size_ex(w, h, icc_len + ml); uint8_t *out = malloc(cap);
        long n = out ? ssimf_encode_ex(rgba, w, h, &o, out, cap) : -1;
        if (n < 0) { fprintf(stderr, "encode failed\n"); return 1; }
        if (write_file(argv[3], out, (size_t)n)) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        printf("%dx%d  %zu -> %ld bytes (%.1f%%)%s  %.2fs\n", w, h, inlen, n, 100.0 * n / inlen,
               icc ? "  colour profile kept" : "", (double)(clock() - t0) / CLOCKS_PER_SEC);
    } else if (!strcmp(argv[1], "decode") && argc == 4) {
        size_t len; uint8_t *in = read_file(argv[2], &len);
        int w, h;
        if (!in || ssimf_info(in, len, &w, &h)) { fprintf(stderr, "not an SSIMF file (or too large): %s\n", argv[2]); return 1; }
        uint8_t *rgba = malloc((size_t)w * h * 4);
        if (!rgba) { fprintf(stderr, "not enough memory\n"); return 1; }
        if (ssimf_decode(in, len, rgba) < 0) { fprintf(stderr, "decode failed (corrupt file?)\n"); return 1; }
        int pn = 0; unsigned char *png = stbi_write_png_to_mem(rgba, w * 4, w, h, 4, &pn);
        if (!png) { fprintf(stderr, "cannot make PNG\n"); return 1; }
        const uint8_t *icc; size_t icc_len;
        if (!ssimf_chunk(in, len, "ICCP", &icc, &icc_len) && icc_len) {
            int n2 = 0; uint8_t *p2 = png_add_icc(png, pn, icc, icc_len, &n2);
            if (p2) { STBIW_FREE(png); png = p2; pn = n2; }
        }
        if (write_file(argv[3], png, (size_t)pn)) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
        printf("%dx%d decoded in %.2fs\n", w, h, (double)(clock() - t0) / CLOCKS_PER_SEC);
    } else if (!strcmp(argv[1], "info") && argc == 3) {
        size_t len; uint8_t *in = read_file(argv[2], &len);
        int w, h;
        if (!in || ssimf_info(in, len, &w, &h)) { fprintf(stderr, "not an SSIMF file (or too large): %s\n", argv[2]); return 1; }
        printf("SSIMF version %d, %d x %d, %s\n", in[0] == 'S' && in[1] == 'S' && in[2] == 'I' ? in[5] : 0, w, h,
               (in[2] == 'I' ? in[14] : in[12]) & 1 ? "with transparency" : "no transparency");
        const uint8_t *d; size_t dl;
        if (!ssimf_chunk(in, len, "STRP", &d, &dl) && dl >= 4) printf("stripes: %zu (%u rows each)\n", (dl - 4) / 4, be32(d));
        if (!ssimf_chunk(in, len, "ICCP", &d, &dl)) printf("colour profile: %zu bytes\n", dl);
        if (!ssimf_chunk(in, len, "META", &d, &dl)) {
            size_t p = 0;
            while (p < dl) {
                const char *k = (const char *)d + p; size_t kl = strnlen(k, dl - p); if (p + kl + 1 >= dl) break;
                const char *v = k + kl + 1; size_t vl = strnlen(v, dl - p - kl - 1);
                printf("%.*s: %.*s\n", (int)kl, k, (int)vl, v); p += kl + vl + 2;
            }
        }
    } else { usage(argv[0]); return 1; }
    return 0;
}
