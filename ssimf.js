/*
 * SSIMF - ScreenShot IMage Format v1 (experimental), JavaScript encoder + decoder. By imgformats.com.
 * Produces byte-identical files to the C reference (ssimf.c).
 * MIT License, Copyright (c) 2026 imgformats.com.
 *
 *   SSIMF.encode(rgba: Uint8Array|Uint8ClampedArray, width, height) -> Uint8Array
 *   SSIMF.decode(bytes: Uint8Array) -> { width, height, rgba: Uint8ClampedArray }
 *   SSIMF.info(bytes) -> { width, height, alpha } or null
 */
(function (root) {
  'use strict';
  const HDR = 15, LIMIT = 40, MT = 32, MTBITS = 5, MINLEN = 4, MINHARD = 3, CHAIN = 16, MAXSCAN = 4096, HB = 18;
  const TWO32 = 4294967296;

  // Model layout: every probability is a (p, n) pair stored in two flat arrays.
  const L_CAND = 0, S_CAND = 4 * 1024;
  const L_MHIT = L_CAND + S_CAND, L_MTREE = L_MHIT + 64;
  const L_KT = L_MTREE + 8 * 32, L_MB = L_KT + 350 * 16;
  const L_MT = L_MB + 4 * 10 * 3 * 2, L_REP = L_MT + 128, L_RTREE = L_REP + 64;
  const L_DTREE = L_RTREE + 4, L_DBITS = L_DTREE + 32;
  const L_LTREE = L_DBITS + 1024, L_LBITS = L_LTREE + 64;
  const L_DXTREE = L_LBITS + 2048, L_DXBITS = L_DXTREE + 64, L_DSIGN = L_DXBITS + 2048;
  const NPROB = L_DSIGN + 1;

  const ilog = d => d ? 32 - Math.clz32(d) : 0;
  const med = (l, u, ul) => {
    const mx = l > u ? l : u, mn = l < u ? l : u;
    return ul >= mx ? mn : ul <= mn ? mx : l + u - ul;
  };
  const zz = v => { const s = ((v & 255) << 24) >> 24; return s >= 0 ? 2 * s : -2 * s - 1; };
  const unzz = z => (z & 1) ? -((z + 1) >> 1) : (z >> 1);

  function makeCoder(enc, buf, len) {
    const P = new Uint16Array(NPROB).fill(32768), N = new Uint8Array(NPROB);
    const c = { P, N, enc, low: 0, range: 0xFFFFFFFF, code: 0, cache: 0, csize: 1, buf, pos: 0, len };
    return c;
  }
  function grow(c) { const nb = new Uint8Array(c.buf.length * 2); nb.set(c.buf); c.buf = nb; }
  function shiftLow(c) {
    const carry = c.low >= TWO32 ? 1 : 0, low32 = carry ? c.low - TWO32 : c.low;
    if (low32 < 0xFF000000 || carry) {
      let t = c.cache;
      do {
        if (c.pos >= c.buf.length) grow(c);
        c.buf[c.pos++] = (t + carry) & 255; t = 0xFF;
      } while (--c.csize);
      c.cache = (low32 >>> 24) & 255;
    }
    c.csize++;
    c.low = (low32 & 0x00FFFFFF) * 256;
  }
  function bit(c, i, b) {
    const P = c.P, N = c.N;
    let pv = P[i] >> 4; if (pv < 1) pv = 1; else if (pv > 4095) pv = 4095;
    const bound = (c.range >>> 12) * pv;
    if (c.enc) {
      if (!b) c.range = bound; else { c.low += bound; c.range -= bound; }
      while (c.range < 16777216) { c.range = c.range * 256; shiftLow(c); }
    } else {
      if (c.code < bound) { c.range = bound; b = 0; } else { c.code -= bound; c.range -= bound; b = 1; }
      while (c.range < 16777216) {
        c.range = c.range * 256;
        c.code = ((c.code << 8) | (c.pos < c.len ? c.buf[c.pos++] : 0)) >>> 0;
      }
    }
    const n = N[i], t = b ? 0 : 65535, p = P[i];
    P[i] = p + Math.trunc((t - p) / (n + 2));
    if (n < LIMIT) N[i] = n + 1;
    return b;
  }
  function tree(c, base, nbits, v) {
    let node = 1;
    for (let i = nbits - 1; i >= 0; i--) node = (node << 1) | bit(c, base + node, c.enc ? (v >> i) & 1 : 0);
    return node - (1 << nbits);
  }
  function gam(c, tbase, bbase, v) {
    const nb = tree(c, tbase, 5, c.enc ? ilog(v) : 0);
    let out = 1;
    for (let i = nb - 2; i >= 0; i--) {
      const j = nb - 2 - i;
      out = (out * 2) + bit(c, bbase + nb * 32 + (j < 31 ? j : 31), c.enc ? Math.floor(v / Math.pow(2, i)) & 1 : 0);
    }
    return out;
  }
  function h4(px, i) {
    let h = Math.imul(px[i], 0x9E3779B1);
    h ^= Math.imul(px[i + 1], 0x85EBCA77);
    h ^= Math.imul(px[i + 2], 0xC2B2AE3D);
    h ^= Math.imul(px[i + 3], 0x27D4EB2F);
    return h >>> (32 - HB);
  }
  const SH = [8, 16, 0, 24];

  function run(c, px, w, h, alpha) {
    const enc = c.enc, n = w * h;
    const tok = new Uint8Array(2 * w), eb = new Uint8Array(2 * w);
    let head = null, prev = null;
    if (enc) { head = new Int32Array(1 << HB).fill(-1); prev = new Int32Array(n); }
    const mtf = new Uint32Array(MT); let mtn = 0;
    const reps = [0, 0, 0, 0];
    let ins = 0, mleft = 0, md = 0, justm = 0;
    const cands = new Uint32Array(4), cur = [0, 0, 0, 255];
    for (let y = 0; y < h; y++) {
      const tc = (y & 1) * w, tp = ((y + 1) & 1) * w;
      for (let x = 0; x < w; x++) {
        const i = y * w + x;
        if (mleft) {
          if (!enc) px[i] = px[i - md];
          tok[tc + x] = 6; eb[tc + x] = 0; mleft--; justm = 1;
          continue;
        }
        let L, U, UL, UR;
        if (y === 0) { L = x ? px[i - 1] : 0; U = UL = UR = L; }
        else {
          U = px[i - w]; L = x ? px[i - 1] : U;
          UL = x ? px[i - w - 1] : U; UR = x < w - 1 ? px[i - w + 1] : U;
        }
        const lt = x ? tok[tc + x - 1] : 5, ut = y ? tok[tp + x] : 5;
        const eL = x ? eb[tc + x - 1] : 0, eU = y ? eb[tp + x] : 0;

        // 1. long-range match
        let blen = 0, bd = 0;
        if (enc) {
          while (ins + 4 <= n && ins < i) { const hh = h4(px, ins); prev[ins] = head[hh]; head[hh] = ins; ins++; }
          const flat = x && i + 3 < n && px[i] === px[i - 1] && px[i + 1] === px[i - 1] && px[i + 2] === px[i - 1] && px[i + 3] === px[i - 1];
          if (i + 4 <= n && !flat) {
            let maxl = n - i; if (maxl > MAXSCAN) maxl = MAXSCAN;
            let best = 0, cj = head[h4(px, i)];
            for (let k = -4; k < CHAIN; k++) {
              let j;
              if (k < 0) { const rd = reps[k + 4]; j = rd && rd <= i ? i - rd : -1; }
              else { if (cj < 0) break; j = cj; cj = prev[cj]; }
              if (j < 0) continue;
              const d = i - j; if (d <= 1 || d === w) continue;
              let l = 0, hard = 0, qx = x;
              while (l < maxl && px[j + l] === px[i + l]) {
                const q = i + l, cc = px[q];
                if (!((qx && cc === px[q - 1]) || (q >= w && cc === px[q - w]))) hard++;
                l++; if (++qx === w) qx = 0;
              }
              const isr = (reps[0] === d || reps[1] === d || reps[2] === d || reps[3] === d) ? 1 : 0;
              const sc = hard + isr;
              if (l >= MINLEN && sc > best) { best = sc; blen = l; bd = d; }
            }
            if (best < MINHARD) blen = 0;
          }
        }
        const mctx = lt * 8 + ut;
        if (bit(c, L_MT + justm * 64 + mctx, blen > 0 ? 1 : 0)) {
          let ri = -1;
          if (enc) for (let z = 0; z < 4; z++) if (reps[z] === bd) { ri = z; break; }
          const isrep = bit(c, L_REP + mctx, ri >= 0 ? 1 : 0);
          if (isrep) {
            ri = tree(c, L_RTREE, 2, ri);
            bd = reps[ri];
            for (let z = ri; z > 0; z--) reps[z] = reps[z - 1];
          } else {
            let dy = enc ? Math.trunc((bd + (w >> 1)) / w) : 0, dx = enc ? bd - dy * w : 0;
            dy = gam(c, L_DTREE, L_DBITS, dy + 1) - 1;
            if (dy === 0) dx = gam(c, L_DXTREE, L_DXBITS, dx);
            else {
              const sg = bit(c, L_DSIGN, dx < 0 ? 1 : 0);
              const ax = gam(c, L_DXTREE + 32, L_DXBITS + 1024, (dx < 0 ? -dx : dx) + 1) - 1;
              dx = sg ? -ax : ax;
            }
            bd = dy * w + dx;
            for (let z = 3; z > 0; z--) reps[z] = reps[z - 1];
          }
          reps[0] = bd;
          blen = gam(c, L_LTREE + isrep * 32, L_LBITS + isrep * 1024, blen - MINLEN + 1) + MINLEN - 1;
          if (bd <= 0 || bd > i) throw new Error('corrupt SSIMF data');
          md = bd; mleft = blen;
          if (!enc) px[i] = px[i - md];
          tok[tc + x] = 6; eb[tc + x] = 0; mleft--; justm = 1;
          continue;
        }
        justm = 0;

        // 2. neighbour candidates
        const ctx = ((L === U ? 1 : 0) | (U === UR ? 2 : 0) | (L === UL ? 4 : 0) | (U === UL ? 8 : 0) | lt << 4 | ut << 7) & 1023;
        let col = enc ? px[i] : 0, t = -1, eNew = 0;
        cands[0] = L; cands[1] = U; cands[2] = UR; cands[3] = UL;
        for (let k = 0; k < 4; k++) {
          let dup = false;
          for (let j = 0; j < k; j++) if (cands[j] === cands[k]) dup = true;
          if (dup) continue;
          if (bit(c, L_CAND + k * 1024 + ctx, enc && col === cands[k] ? 1 : 0)) { t = k; col = cands[k]; break; }
        }
        if (t < 0) {
          // 3. recent colours
          let pos = -1;
          if (enc) for (let k = 0; k < mtn; k++) if (mtf[k] === col) { pos = k; break; }
          if (bit(c, L_MHIT + lt * 8 + ut, pos >= 0 ? 1 : 0)) {
            pos = tree(c, L_MTREE + lt * 32, MTBITS, pos);
            if (pos >= mtn) throw new Error('corrupt SSIMF data');
            col = mtf[pos]; t = 7;
            mtf.copyWithin(1, 0, pos); mtf[0] = col;
          } else {
            // 4. literal
            cur[0] = 0; cur[1] = 0; cur[2] = 0; cur[3] = 255;
            let kprev = 0;
            const nq = alpha ? 4 : 3;
            for (let q = 0; q < nq; q++) {
              const s = SH[q];
              const l_ = (L >>> s) & 255, u_ = (U >>> s) & 255, ul_ = (UL >>> s) & 255, ur_ = (UR >>> s) & 255;
              const act = Math.abs(l_ - ul_) + Math.abs(u_ - ul_) + Math.abs(ur_ - u_);
              let a_ = ilog(act); if (a_ > 9) a_ = 9;
              let p, ectx;
              if (q === 0 || q === 3) p = med(l_, u_, ul_);
              else p = cur[0] + med(l_ - ((L >>> 8) & 255), u_ - ((U >>> 8) & 255), ul_ - ((UL >>> 8) & 255));
              p = p < 0 ? 0 : p > 255 ? 255 : p;
              if (q === 0) { const e = eL + eU; ectx = a_ * 16 + (e > 15 ? 15 : e); }
              else if (q < 3) ectx = 160 + (q - 1) * 90 + (kprev > 8 ? 8 : kprev) * 10 + a_;
              else ectx = 340 + a_;
              let e = enc ? zz(((col >>> s) & 255) - p) : 0;
              const k = tree(c, L_KT + ectx * 16, 4, enc ? ilog(e) : 0);
              if (k > 8) throw new Error('corrupt SSIMF data');
              if (k >= 2) {
                let mm = 0;
                for (let bi = k - 2; bi >= 0; bi--) {
                  const j = k - 2 - bi;
                  const idx = L_MB + ((q * 10 + k) * 3 + (j < 2 ? j : 2)) * 2 + (j === 0 ? 0 : (mm & 1));
                  mm = (mm << 1) | bit(c, idx, enc ? (e >> bi) & 1 : 0);
                }
                e = (1 << (k - 1)) | mm;
              } else e = k;
              cur[q] = (p + unzz(e)) & 255;
              if (q < 3) kprev = q === 0 ? k : (kprev > k ? kprev : k);
            }
            col = ((alpha ? cur[3] << 24 : 0) | cur[1] << 16 | cur[0] << 8 | cur[2]) >>> 0;
            t = 5; eNew = kprev;
            const kk = mtn < MT ? mtn : MT - 1;
            mtf.copyWithin(1, 0, kk); mtf[0] = col; if (mtn < MT) mtn++;
          }
        }
        if (!enc) px[i] = col;
        tok[tc + x] = t; eb[tc + x] = eNew;
      }
    }
  }

  function encode(rgba, w, h) {
    const n = w * h;
    let alpha = 0;
    for (let i = 0; i < n; i++) if (rgba[4 * i + 3] !== 255) { alpha = 1; break; }
    const px = new Uint32Array(n);
    for (let i = 0; i < n; i++)
      px[i] = ((alpha ? rgba[4 * i + 3] << 24 : 0) | rgba[4 * i] << 16 | rgba[4 * i + 1] << 8 | rgba[4 * i + 2]) >>> 0;
    const c = makeCoder(true, new Uint8Array(Math.max(1024, n)), 0);
    run(c, px, w, h, alpha);
    for (let i = 0; i < 5; i++) shiftLow(c);
    const out = new Uint8Array(HDR + c.pos);
    out.set([0x53, 0x53, 0x49, 0x4D, 0x46, 1], 0); // "SSIMF" + version 1
    const dv = new DataView(out.buffer);
    dv.setUint32(6, w); dv.setUint32(10, h); out[14] = alpha;
    out.set(c.buf.subarray(0, c.pos), HDR);
    return out;
  }

  // Reads the header. Also accepts files from the earlier "SSF1" test version.
  function info(bytes) {
    if (!bytes || bytes.length < 13) return null;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let w, h, alpha, header;
    if (bytes.length >= HDR && bytes[0] === 0x53 && bytes[1] === 0x53 && bytes[2] === 0x49 && bytes[3] === 0x4D && bytes[4] === 0x46 && bytes[5] === 1) {
      w = dv.getUint32(6); h = dv.getUint32(10); alpha = bytes[14] & 1; header = HDR;
    } else if (bytes[0] === 0x53 && bytes[1] === 0x53 && bytes[2] === 0x46 && bytes[3] === 0x31) {
      w = dv.getUint32(4); h = dv.getUint32(8); alpha = bytes[12] & 1; header = 13;
    } else return null;
    if (!w || !h || w * h > 1e9) return null;
    return { width: w, height: h, alpha, header };
  }

  function decode(bytes) {
    const inf = info(bytes);
    if (!inf) throw new Error('not an SSIMF file');
    const { width: w, height: h, alpha } = inf, n = w * h;
    const px = new Uint32Array(n);
    const c = makeCoder(false, bytes.subarray(inf.header), bytes.length - inf.header);
    for (let i = 0; i < 5; i++) c.code = ((c.code << 8) | (c.pos < c.len ? c.buf[c.pos++] : 0)) >>> 0;
    run(c, px, w, h, alpha);
    const rgba = new Uint8ClampedArray(n * 4);
    for (let i = 0; i < n; i++) {
      const v = px[i];
      rgba[4 * i] = (v >>> 16) & 255; rgba[4 * i + 1] = (v >>> 8) & 255; rgba[4 * i + 2] = v & 255;
      rgba[4 * i + 3] = alpha ? v >>> 24 : 255;
    }
    return { width: w, height: h, rgba };
  }

  const SSIMF = { encode, decode, info, version: 1 };
  if (typeof module !== 'undefined' && module.exports) module.exports = SSIMF; else root.SSIMF = SSIMF;
})(typeof self !== 'undefined' ? self : this);
