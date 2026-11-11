/*
 * SSIMF - ScreenShot IMage Format, version 2 (experimental), by imgformats.com
 * A lossless image format tuned for screenshots.
 *
 * For every pixel, in order:
 *   1. is this the start of a copy of earlier pixels (long-range match)?
 *   2. is it equal to a neighbour (left, up, up-right, up-left)?
 *   3. is it one of the 32 most recently used "new" colours?
 *   4. otherwise spell the colour out, predicted from neighbours.
 * Every decision is coded with an adaptive binary range coder whose
 * probabilities are chosen by the local context.
 *
 * Version 2 adds optional chunks for a colour profile (ICC) and metadata, and can cut the image into
 * horizontal stripes that are coded independently, so they can be encoded and decoded on several CPU
 * cores at once (opt-in: it makes files bigger, see default_rows). Pixels are still stored exactly (lossless). Version 1 files still decode.
 *
 * Version 2 layout (all numbers big-endian):
 *   "SSIMF" | version (u8 = 2) | width (u32) | height (u32) | flags (u8, bit0 = alpha)
 *   then chunks, each: type (4 ASCII bytes) | length (u32) | payload
 *     "ICCP"  ICC colour profile (optional)
 *     "META"  metadata as UTF-8 "key\0value\0" pairs (optional)
 *     "STRP"  rows per stripe (u32), then the coded size of each stripe (u32 each)   (required)
 *     "DATA"  the coded stripes, one after another                                    (required, last)
 *   Decoders skip chunk types they don't know.
 * Version 1 layout: "SSIMF" | version (u8 = 1) | width | height | flags | coded data (one stripe)
 *
 * Build with -DSSIMF_THREADS (and -lpthread on Linux/macOS) to use several cores.
 * MIT License, Copyright (c) 2026 imgformats.com. Experimental.
 */
#include "ssimf.h"
#ifdef SSIMF_FREESTANDING          /* WebAssembly build: these come from ssimf_wasm_shim.c */
void *malloc(size_t); void free(void *); void *calloc(size_t, size_t); int abs(int);
void *memset(void *, int, size_t); void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t); int memcmp(const void *, const void *, size_t);
#else
#include <stdlib.h>
#include <string.h>
#endif
#ifdef SSIMF_THREADS
#  ifdef _WIN32
#    include <windows.h>
#  else
#    include <pthread.h>
#    include <unistd.h>
#  endif
#endif

/* ---------- adaptive probability (count-based learning rate) ---------- */
typedef struct { uint16_t p, n; } Prob;           /* p = probability of a 0 bit, 16-bit scale */
#define LIMIT 40
static void pinit(Prob *p, size_t n) { for (size_t i = 0; i < n; i++) { p[i].p = 32768; p[i].n = 0; } }
static inline uint32_t pget(const Prob *p) { uint32_t v = p->p >> 4; return v < 1 ? 1 : v > 4095 ? 4095 : v; }
static inline void pupd(Prob *p, int bit) {
    int t = bit ? 0 : 65535;
    p->p = (uint16_t)(p->p + (t - (int)p->p) / (p->n + 2));
    if (p->n < LIMIT) p->n++;
}

/* ---------- range coder (LZMA style) ---------- */
typedef struct {
    uint64_t low; uint32_t range, code; uint8_t cache; uint64_t csize;
    uint8_t *buf; size_t pos, len, cap; int overflow; uint32_t past;   /* past = bytes read beyond the end */
} RC;
static inline void put(RC *r, uint8_t b) { if (r->pos < r->cap) r->buf[r->pos] = b; else r->overflow = 1; r->pos++; }
static inline void shift_low(RC *r) {
    if ((uint32_t)r->low < 0xFF000000u || (r->low >> 32)) {
        uint8_t t = r->cache;
        do { put(r, (uint8_t)(t + (uint8_t)(r->low >> 32))); t = 0xFF; } while (--r->csize);
        r->cache = (uint8_t)(r->low >> 24);
    }
    r->csize++;
    r->low = (r->low & 0x00FFFFFFu) << 8;
}
static inline int bit(const int enc, RC *r, Prob *p, int b) {
    uint32_t bound = (r->range >> 12) * pget(p);
    if (enc) {
        if (!b) r->range = bound; else { r->low += bound; r->range -= bound; }
        while (r->range < (1u << 24)) { r->range <<= 8; shift_low(r); }
    } else {
        if (r->code < bound) { r->range = bound; b = 0; } else { r->code -= bound; r->range -= bound; b = 1; }
        while (r->range < (1u << 24)) { r->range <<= 8; r->code = (r->code << 8) | (r->pos < r->len ? r->buf[r->pos++] : (r->past++, 0)); }
    }
    pupd(p, b);
    return b;
}
static inline int tree(const int enc, RC *r, Prob *t, int nbits, int v) {
    int node = 1;
    for (int i = nbits - 1; i >= 0; i--) node = (node << 1) | bit(enc, r, &t[node], enc ? (v >> i) & 1 : 0);
    return node - (1 << nbits);
}
static inline int ilog(unsigned d) { return d ? 32 - __builtin_clz(d) : 0; }
static inline uint32_t gam(const int enc, RC *r, Prob *t, Prob (*bits)[32], uint32_t v) {  /* v >= 1 */
    int nb = tree(enc, r, t, 5, enc ? ilog(v) : 0);
    uint32_t out = 1;
    for (int i = nb - 2; i >= 0; i--) {
        int j = nb - 2 - i;
        out = (out << 1) | bit(enc, r, &bits[nb][j < 31 ? j : 31], enc ? (v >> i) & 1 : 0);
    }
    return out;
}
static inline int med(int l, int u, int ul) {
    int mx = l > u ? l : u, mn = l < u ? l : u;
    return ul >= mx ? mn : ul <= mn ? mx : l + u - ul;
}
static inline int zz(int v) { int8_t s = (int8_t)v; return s >= 0 ? 2 * s : -2 * s - 1; }
static inline int unzz(int z) { return (z & 1) ? -((z + 1) >> 1) : (z >> 1); }

/* ---------- model ---------- */
#define MT 32          /* recent colour list size */
#define MTBITS 5
#define MINLEN 4       /* shortest match */
#define MINHARD 3      /* match must cover >= 3 pixels that are not simple neighbour copies */
#define CHAIN 16       /* encoder search effort */
#define MAXSCAN 4096
#define HB 18
typedef struct {
    Prob cand[4][1024];
    Prob mhit[64], mtree[8][1 << MTBITS];
    Prob kt[350][16], mb[4][10][3][2];
    Prob mt[2][64], rep[64], rtree[4], dtree[32], dbits[32][32];
    Prob ltree[2][32], lbits[2][32][32], dxtree[2][32], dxbits[2][32][32], dsign;
} Model;

static inline uint32_t h4(const uint32_t *p) {
    uint32_t h = p[0] * 0x9E3779B1u; h ^= p[1] * 0x85EBCA77u; h ^= p[2] * 0xC2B2AE3Du; h ^= p[3] * 0x27D4EB2Fu;
    return h >> (32 - HB);
}

static __attribute__((always_inline)) inline int run(const int enc, uint32_t *px, int w, int h, int alpha, RC *r) {
    Model *m = malloc(sizeof(Model));
    uint8_t *tok = calloc((size_t)2 * w, 1), *eb = calloc((size_t)2 * w, 1);
    size_t n = (size_t)w * h;
    int32_t *head = NULL, *prev = NULL;
    if (!m || !tok || !eb) { free(m); free(tok); free(eb); return -1; }
    pinit((Prob *)m, sizeof(Model) / sizeof(Prob));
    if (enc) {
        head = malloc(sizeof(int32_t) << HB); prev = malloc(sizeof(int32_t) * n);
        if (!head || !prev) { free(m); free(tok); free(eb); free(head); free(prev); return -1; }
        memset(head, 0xFF, sizeof(int32_t) << HB);
    }
    uint32_t mtf[MT]; int mtn = 0;
    uint32_t reps[4] = {0, 0, 0, 0};
    size_t ins = 0, mleft = 0, md = 0;
    int justm = 0;
    for (int y = 0; y < h; y++) {
        if (!enc && r->past > 64) { free(m); free(tok); free(eb); return -2; }   /* truncated or not real data */
        uint8_t *tc = tok + (y & 1) * w, *tp = tok + ((y + 1) & 1) * w;
        uint8_t *ec = eb + (y & 1) * w, *ep = eb + ((y + 1) & 1) * w;
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            if (mleft) {                       /* inside a match: copy */
                if (!enc) px[i] = px[i - md];
                tc[x] = 6; ec[x] = 0; mleft--; justm = 1;
                continue;
            }
            uint32_t L, U, UL, UR;
            if (y == 0) { L = x ? px[i - 1] : 0; U = UL = UR = L; }
            else {
                U = px[i - w]; L = x ? px[i - 1] : U;
                UL = x ? px[i - w - 1] : U; UR = x < w - 1 ? px[i - w + 1] : U;
            }
            int lt = x ? tc[x - 1] : 5, ut = y ? tp[x] : 5;
            int eL = x ? ec[x - 1] : 0, eU = y ? ep[x] : 0;

            /* 1. long-range match */
            size_t blen = 0, bd = 0;
            if (enc) {
                while (ins + 4 <= n && ins < i) { uint32_t hh = h4(px + ins); prev[ins] = head[hh]; head[hh] = (int32_t)ins; ins++; }
                int flat = x && i + 3 < n && px[i] == px[i - 1] && px[i + 1] == px[i - 1] && px[i + 2] == px[i - 1] && px[i + 3] == px[i - 1];
                if (i + 4 <= n && !flat) {
                    size_t maxl = n - i; if (maxl > MAXSCAN) maxl = MAXSCAN;
                    int best = 0; int32_t cj = head[h4(px + i)];
                    for (int k = -4; k < CHAIN; k++) {
                        int32_t j;
                        if (k < 0) { uint32_t rd = reps[k + 4]; j = rd && rd <= i ? (int32_t)(i - rd) : -1; }
                        else { if (cj < 0) break; j = cj; cj = prev[cj]; }
                        if (j < 0) continue;
                        size_t d = i - j; if (d <= 1 || d == (size_t)w) continue;
                        size_t l = 0; int hard = 0, qx = x;
                        while (l < maxl && px[j + l] == px[i + l]) {
                            size_t q = i + l; uint32_t c = px[q];
                            if (!((qx && c == px[q - 1]) || (q >= (size_t)w && c == px[q - w]))) hard++;
                            l++; if (++qx == w) qx = 0;
                        }
                        int isr = reps[0] == d || reps[1] == d || reps[2] == d || reps[3] == d;
                        int sc = hard + isr;
                        if (l >= MINLEN && sc > best) { best = sc; blen = l; bd = d; }
                    }
                    if (best < MINHARD) blen = 0;
                }
            }
            int mctx = lt * 8 + ut;
            if (bit(enc, r, &m->mt[justm][mctx], blen > 0)) {
                int ri = -1;
                if (enc) for (int z = 0; z < 4; z++) if (reps[z] == bd) { ri = z; break; }
                int isrep = bit(enc, r, &m->rep[mctx], ri >= 0);
                if (isrep) {
                    ri = tree(enc, r, m->rtree, 2, ri);
                    bd = reps[ri];
                    for (int z = ri; z > 0; z--) reps[z] = reps[z - 1];
                } else {
                    long dy = enc ? ((long)bd + w / 2) / w : 0, dx = enc ? (long)bd - dy * w : 0;
                    dy = (long)gam(enc, r, m->dtree, m->dbits, (uint32_t)(dy + 1)) - 1;
                    if (dy == 0) dx = gam(enc, r, m->dxtree[0], m->dxbits[0], (uint32_t)dx);
                    else {
                        int sg = bit(enc, r, &m->dsign, dx < 0);
                        long ax = (long)gam(enc, r, m->dxtree[1], m->dxbits[1], (uint32_t)((dx < 0 ? -dx : dx) + 1)) - 1;
                        dx = sg ? -ax : ax;
                    }
                    bd = (size_t)(dy * w + dx);
                    for (int z = 3; z > 0; z--) reps[z] = reps[z - 1];
                }
                reps[0] = (uint32_t)bd;
                blen = gam(enc, r, m->ltree[isrep], m->lbits[isrep], (uint32_t)(blen - MINLEN + 1)) + MINLEN - 1;
                if (bd == 0 || bd > i) { free(m); free(tok); free(eb); free(head); free(prev); return -2; } /* corrupt */
                md = bd; mleft = blen;
                if (!enc) px[i] = px[i - md];
                tc[x] = 6; ec[x] = 0; mleft--; justm = 1;
                continue;
            }
            justm = 0;

            /* 2. neighbour candidates */
            int ctx = ((L == U) | (U == UR) << 1 | (L == UL) << 2 | (U == UL) << 3 | lt << 4 | ut << 7) & 1023;
            uint32_t c = enc ? px[i] : 0, cands[4] = {L, U, UR, UL};
            int t = -1, eNew = 0;
            for (int k = 0; k < 4; k++) {
                int dup = 0;
                for (int j = 0; j < k; j++) if (cands[j] == cands[k]) dup = 1;
                if (dup) continue;
                if (bit(enc, r, &m->cand[k][ctx], enc ? c == cands[k] : 0)) { t = k; c = cands[k]; break; }
            }
            if (t < 0) {
                /* 3. recent colours */
                int pos = -1;
                if (enc) for (int k = 0; k < mtn; k++) if (mtf[k] == c) { pos = k; break; }
                if (bit(enc, r, &m->mhit[lt * 8 + ut], pos >= 0)) {
                    pos = tree(enc, r, m->mtree[lt], MTBITS, pos);
                    if (pos >= mtn) { free(m); free(tok); free(eb); free(head); free(prev); return -2; }
                    c = mtf[pos]; t = 7;
                    memmove(mtf + 1, mtf, pos * 4); mtf[0] = c;
                } else {
                    /* 4. literal: G, then R and B predicted in the (X - G) domain, then alpha */
                    static const int sh[4] = {8, 16, 0, 24};
                    int cur[4] = {0, 0, 0, 255}, kprev = 0;
                    for (int q = 0; q < (alpha ? 4 : 3); q++) {
                        int s = sh[q];
                        int l_ = (L >> s) & 255, u_ = (U >> s) & 255, ul_ = (UL >> s) & 255, ur_ = (UR >> s) & 255;
                        int act = abs(l_ - ul_) + abs(u_ - ul_) + abs(ur_ - u_);
                        int a_ = ilog(act); if (a_ > 9) a_ = 9;
                        int p, ectx;
                        if (q == 0 || q == 3) p = med(l_, u_, ul_);
                        else p = cur[0] + med(l_ - (int)((L >> 8) & 255), u_ - (int)((U >> 8) & 255), ul_ - (int)((UL >> 8) & 255));
                        p = p < 0 ? 0 : p > 255 ? 255 : p;
                        if (q == 0) { int e = eL + eU; ectx = a_ * 16 + (e > 15 ? 15 : e); }
                        else if (q < 3) ectx = 160 + (q - 1) * 90 + (kprev > 8 ? 8 : kprev) * 10 + a_;
                        else ectx = 340 + a_;
                        int e = enc ? zz((int)((c >> s) & 255) - p) : 0;
                        int k = tree(enc, r, m->kt[ectx], 4, enc ? ilog(e) : 0);
                        if (k > 8) { free(m); free(tok); free(eb); free(head); free(prev); return -2; }
                        if (k >= 2) {
                            int mm = 0;
                            for (int bi = k - 2; bi >= 0; bi--) {
                                int j = k - 2 - bi;
                                mm = (mm << 1) | bit(enc, r, &m->mb[q][k][j < 2 ? j : 2][j == 0 ? 0 : (mm & 1)], enc ? (e >> bi) & 1 : 0);
                            }
                            e = (1 << (k - 1)) | mm;
                        } else e = k;
                        int res = unzz(e);
                        cur[q] = (p + res) & 255;
                        if (q < 3) kprev = q == 0 ? k : (kprev > k ? kprev : k);
                    }
                    c = (alpha ? (uint32_t)cur[3] << 24 : 0) | (uint32_t)cur[1] << 16 | (uint32_t)cur[0] << 8 | (uint32_t)cur[2];
                    t = 5; eNew = kprev;
                    int k = mtn < MT ? mtn : MT - 1;
                    memmove(mtf + 1, mtf, k * 4); mtf[0] = c; if (mtn < MT) mtn++;
                }
            }
            if (!enc) px[i] = c;
            tc[x] = (uint8_t)t; ec[x] = (uint8_t)eNew;
        }
    }
    free(m); free(tok); free(eb); free(head); free(prev);
    return 0;
}

static void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }


#define HDR 15                         /* "SSIMF" + version + width + height + flags */
#define MAX_SIDE (1 << 24)             /* largest width or height a decoder accepts */
#define MAX_PIXELS ((uint64_t)1 << 28) /* largest image (268 million pixels) a decoder accepts */

/* Default: the whole image is one stripe. Stripes can't reuse pixels from the stripes above them,
   and that reuse is where SSIMF saves most of its space on text-heavy screenshots (tests: email +76%,
   code +63%, spreadsheet +32% with 8 stripes). So stripes are opt-in, for when decode speed matters
   more than size. Every encoder uses this same default, so all encoders write identical files. */
static int default_rows(int w, int h) { (void)w; return h; }

/* ---------- one stripe ---------- */
static long enc_stripe(uint32_t *px, int w, int rows, int alpha, uint8_t *buf, size_t cap) {
    RC r = {0}; r.range = 0xFFFFFFFFu; r.csize = 1; r.buf = buf; r.cap = cap;
    int rc = run(1, px, w, rows, alpha, &r);
    for (int i = 0; i < 5; i++) shift_low(&r);
    if (rc || r.overflow) return -1;
    return (long)r.pos;
}
static long dec_stripe(uint32_t *px, int w, int rows, int alpha, const uint8_t *in, size_t len) {
    RC r = {0}; r.range = 0xFFFFFFFFu; r.buf = (uint8_t *)in; r.len = len;
    for (int i = 0; i < 5; i++) r.code = (r.code << 8) | (r.pos < r.len ? r.buf[r.pos++] : (r.past++, 0));
    return run(0, px, w, rows, alpha, &r) ? -1 : 0;
}

/* ---------- stripes on several cores ---------- */
typedef struct {
    int enc, w, h, rows, alpha, ns;
    uint32_t *px;
    uint8_t *base; const size_t *off; const size_t *len;   /* where each stripe's bytes are (encode: room for it) */
    long *res;
    volatile long next;
} Work;
static long take(Work *k) {
#if defined(SSIMF_THREADS) && defined(_WIN32)
    return InterlockedExchangeAdd((LONG volatile *)&k->next, 1);
#elif defined(SSIMF_THREADS)
    return __atomic_fetch_add(&k->next, 1, __ATOMIC_RELAXED);
#else
    return k->next++;
#endif
}
static void work_loop(Work *k) {
    for (;;) {
        long s = take(k); if (s >= k->ns) break;
        int y0 = (int)s * k->rows, rr = k->h - y0 < k->rows ? k->h - y0 : k->rows;
        uint32_t *p = k->px + (size_t)y0 * k->w;
        k->res[s] = k->enc ? enc_stripe(p, k->w, rr, k->alpha, k->base + k->off[s], k->len[s])
                           : dec_stripe(p, k->w, rr, k->alpha, k->base + k->off[s], k->len[s]);
    }
}
#ifdef SSIMF_THREADS
#  ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID a) { work_loop((Work *)a); return 0; }
static int cores(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#  else
static void *thread_main(void *a) { work_loop((Work *)a); return NULL; }
static int cores(void) { long c = sysconf(_SC_NPROCESSORS_ONLN); return c > 0 ? (int)c : 1; }
#  endif
#endif
static void run_all(Work *k, int threads) {
#ifdef SSIMF_THREADS
    if (threads <= 0) threads = cores();
    if (threads > 16) threads = 16;
    if (threads > k->ns) threads = k->ns;
    if (threads > 1) {
#  ifdef _WIN32
        HANDLE t[16]; int made = 0;
        for (int i = 0; i < threads - 1; i++) { t[made] = CreateThread(NULL, 0, thread_main, k, 0, NULL); if (t[made]) made++; }
        work_loop(k);
        if (made) WaitForMultipleObjects((DWORD)made, t, TRUE, INFINITE);
        for (int i = 0; i < made; i++) CloseHandle(t[i]);
#  else
        pthread_t t[16]; int made = 0;
        for (int i = 0; i < threads - 1; i++) if (!pthread_create(&t[made], NULL, thread_main, k)) made++;
        work_loop(k);
        for (int i = 0; i < made; i++) pthread_join(t[i], NULL);
#  endif
        return;
    }
#endif
    (void)threads;
    work_loop(k);
}

/* ---------- encoder ---------- */
static size_t stripe_cap(int w, int rows) { return (size_t)w * rows * 8 + 64; }

size_t ssimf_max_size(int w, int h) { return ssimf_max_size_ex(w, h, 0); }
size_t ssimf_max_size_ex(int w, int h, size_t extra) {
    if (w <= 0 || h <= 0) return 0;
    return (size_t)w * h * 8 + (size_t)h * 68 + 4096 + extra;
}

static uint8_t *put_chunk(uint8_t *o, const char *type, const uint8_t *data, size_t len) {
    memcpy(o, type, 4); wr32(o + 4, (uint32_t)len);
    if (len) memcpy(o + 8, data, len);
    return o + 8 + len;
}

long ssimf_encode_ex(const uint8_t *rgba, int w, int h, const ssimf_options *opt, uint8_t *out, size_t cap) {
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * h > MAX_PIXELS) return -1;
    const uint8_t *icc = opt ? opt->icc : NULL, *meta = opt ? opt->meta : NULL;
    size_t icc_len = icc ? opt->icc_len : 0, meta_len = meta ? opt->meta_len : 0;
    if (icc_len > 0xFFFFFFF0u || meta_len > 0xFFFFFFF0u) return -1;
    int rows = opt && opt->stripe_rows > 0 ? opt->stripe_rows : default_rows(w, h);
    if (rows > h) rows = h;
    int ns = (h + rows - 1) / rows;
    size_t n = (size_t)w * h;
    int alpha = 0;
    for (size_t i = 0; i < n; i++) if (rgba[4 * i + 3] != 255) { alpha = 1; break; }

    size_t head = HDR + (icc_len ? 8 + icc_len : 0) + (meta_len ? 8 + meta_len : 0) + 8 + 4 + 4 * (size_t)ns + 8;
    size_t *off = malloc(sizeof(size_t) * ns), *len = malloc(sizeof(size_t) * ns), room = head;
    long *res = malloc(sizeof(long) * ns);
    uint32_t *px = malloc(n * 4);
    if (!off || !len || !res || !px) { free(off); free(len); free(res); free(px); return -1; }
    for (int s = 0; s < ns; s++) {
        int rr = h - s * rows < rows ? h - s * rows : rows;
        off[s] = room; len[s] = stripe_cap(w, rr); room += len[s];
    }
    if (room > cap) { free(off); free(len); free(res); free(px); return -1; }
    for (size_t i = 0; i < n; i++)
        px[i] = (alpha ? (uint32_t)rgba[4 * i + 3] << 24 : 0) | (uint32_t)rgba[4 * i] << 16 | (uint32_t)rgba[4 * i + 1] << 8 | rgba[4 * i + 2];

    Work k = {0}; k.enc = 1; k.w = w; k.h = h; k.rows = rows; k.alpha = alpha; k.ns = ns;
    k.px = px; k.base = out; k.off = off; k.len = len; k.res = res;
    run_all(&k, opt ? opt->threads : 0);
    free(px);

    long result = -1;
    for (int s = 0; s < ns; s++) if (res[s] < 0) goto done;
    memcpy(out, "SSIMF", 5); out[5] = 2;
    wr32(out + 6, (uint32_t)w); wr32(out + 10, (uint32_t)h); out[14] = (uint8_t)alpha;
    uint8_t *o = out + HDR;
    if (icc_len) o = put_chunk(o, "ICCP", icc, icc_len);
    if (meta_len) o = put_chunk(o, "META", meta, meta_len);
    memcpy(o, "STRP", 4); wr32(o + 4, 4 + 4 * (uint32_t)ns); wr32(o + 8, (uint32_t)rows);
    uint64_t total = 0;
    for (int s = 0; s < ns; s++) { wr32(o + 12 + 4 * s, (uint32_t)res[s]); total += (uint64_t)res[s]; }
    o += 12 + 4 * (size_t)ns;
    if (total > 0xFFFFFFFFu) goto done;
    memcpy(o, "DATA", 4); wr32(o + 4, (uint32_t)total); o += 8;
    for (int s = 0; s < ns; s++) { memmove(o, out + off[s], (size_t)res[s]); o += res[s]; }   /* stripes only move left */
    result = (long)(o - out);
done:
    free(off); free(len); free(res);
    return result;
}

long ssimf_encode(const uint8_t *rgba, int w, int h, uint8_t *out, size_t cap) {
    return ssimf_encode_ex(rgba, w, h, NULL, out, cap);
}

/* ---------- decoder ---------- */
typedef struct {
    int w, h, alpha, version;
    const uint8_t *data; size_t data_len;      /* coded stripes */
    int rows, ns; const uint8_t *table;        /* STRP: stripe sizes (v2) */
} Parsed;

/* Reads and checks the header and chunks. Also accepts version 1 and the early "SSF1" test version. */
static int parse(const uint8_t *in, size_t len, Parsed *p) {
    memset(p, 0, sizeof *p);
    if (!in) return -1;
    uint32_t w, h; size_t hl;
    if (len >= HDR && !memcmp(in, "SSIMF", 5) && (in[5] == 1 || in[5] == 2)) {
        w = rd32(in + 6); h = rd32(in + 10); p->alpha = in[14] & 1; p->version = in[5]; hl = HDR;
    } else if (len >= 13 && !memcmp(in, "SSF1", 4)) {
        w = rd32(in + 4); h = rd32(in + 8); p->alpha = in[12] & 1; p->version = 1; hl = 13;
    } else return -1;
    if (!w || !h || w > MAX_SIDE || h > MAX_SIDE || (uint64_t)w * h > MAX_PIXELS) return -1;
    p->w = (int)w; p->h = (int)h;
    if (p->version == 1) { p->data = in + hl; p->data_len = len - hl; p->rows = p->h; p->ns = 1; return 0; }
    size_t pos = hl; int have_strp = 0;
    while (pos + 8 <= len) {
        const uint8_t *c = in + pos; uint32_t cl = rd32(c + 4);
        if ((uint64_t)cl > len - pos - 8) return -1;
        if (!memcmp(c, "STRP", 4)) {
            if (cl < 8) return -1;
            uint32_t rows = rd32(c + 8);
            if (!rows) return -1;
            uint64_t ns = ((uint64_t)h + rows - 1) / rows;
            if ((uint64_t)cl != 4 + 4 * ns) return -1;
            p->rows = rows > h ? (int)h : (int)rows; p->ns = (int)ns; p->table = c + 12; have_strp = 1;
        } else if (!memcmp(c, "DATA", 4)) {
            if (!have_strp) return -1;
            p->data = c + 8; p->data_len = cl;
            uint64_t sum = 0;
            for (int s = 0; s < p->ns; s++) sum += rd32(p->table + 4 * s);
            return sum <= cl ? 0 : -1;
        }
        pos += 8 + (size_t)cl;
    }
    return -1;
}

int ssimf_info(const uint8_t *in, size_t len, int *w, int *h) {
    Parsed p;
    if (parse(in, len, &p)) return -1;
    *w = p.w; *h = p.h; return 0;
}

int ssimf_chunk(const uint8_t *in, size_t len, const char type[4], const uint8_t **data, size_t *data_len) {
    Parsed p;
    if (parse(in, len, &p) || p.version < 2) return -1;
    size_t pos = HDR;
    while (pos + 8 <= len) {
        const uint8_t *c = in + pos; uint32_t cl = rd32(c + 4);
        if (!memcmp(c, type, 4)) { *data = c + 8; *data_len = cl; return 0; }
        if (!memcmp(c, "DATA", 4)) break;
        pos += 8 + (size_t)cl;
    }
    return -1;
}

long ssimf_decode_threads(const uint8_t *in, size_t len, uint8_t *rgba, int threads) {
    Parsed p;
    if (parse(in, len, &p)) return -1;
    size_t n = (size_t)p.w * p.h;
    uint32_t *px = malloc(n * 4);
    size_t *off = malloc(sizeof(size_t) * p.ns), *ln = malloc(sizeof(size_t) * p.ns);
    long *res = malloc(sizeof(long) * p.ns);
    long result = -1;
    if (!px || !off || !ln || !res) goto done;
    if (p.version == 1) { off[0] = 0; ln[0] = p.data_len; }
    else {
        size_t o = 0;
        for (int s = 0; s < p.ns; s++) { off[s] = o; ln[s] = rd32(p.table + 4 * s); o += ln[s]; }
    }
    Work k = {0}; k.enc = 0; k.w = p.w; k.h = p.h; k.rows = p.rows; k.alpha = p.alpha; k.ns = p.ns;
    k.px = px; k.base = (uint8_t *)p.data; k.off = off; k.len = ln; k.res = res;
    run_all(&k, threads);
    for (int s = 0; s < p.ns; s++) if (res[s] < 0) goto done;
    for (size_t i = 0; i < n; i++) {
        rgba[4 * i] = px[i] >> 16; rgba[4 * i + 1] = px[i] >> 8; rgba[4 * i + 2] = px[i];
        rgba[4 * i + 3] = p.alpha ? px[i] >> 24 : 255;
    }
    result = (long)(n * 4);
done:
    free(px); free(off); free(ln); free(res);
    return result;
}

long ssimf_decode(const uint8_t *in, size_t len, uint8_t *rgba) { return ssimf_decode_threads(in, len, rgba, 0); }
