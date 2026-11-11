/*
 * SSIMF - ScreenShot IMage Format v2 (experimental), JavaScript encoder + decoder. By imgformats.com.
 * Produces byte-identical files to the C reference (ssimf.c). Reads version 1 and 2 files.
 * MIT License, Copyright (c) 2026 imgformats.com.
 *
 *   SSIMF.encode(rgba: Uint8Array|Uint8ClampedArray, width, height, options?) -> Uint8Array
 *       options: { icc: Uint8Array, meta: { key: value, ... }, stripeRows: number }
 *       (stripeRows: leave unset for the smallest, standard file)
 *   SSIMF.decode(bytes: Uint8Array) -> { width, height, rgba: Uint8ClampedArray, icc, meta }
 *   SSIMF.info(bytes) -> { width, height, alpha, version, stripes, icc, meta } or null
 *   SSIMF.loadWasm(urlOrBytes) -> Promise<boolean>
 *       Optional speed-up: loads ssimf.wasm (the C code built for WebAssembly). After that, encode and
 *       decode use it automatically and run about 1.6x faster. Files are identical either way, and if
 *       it can't load, everything keeps working in plain JavaScript.
 */
(function (root) {
  'use strict';
  const HDR = 15, MAX_SIDE = 16777216, MAX_PIXELS = 268435456, LIMIT = 40, MT = 32, MTBITS = 5, MINLEN = 4, MINHARD = 3, CHAIN = 16, MAXSCAN = 4096, HB = 18;
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
    const c = { P, N, enc, low: 0, range: 0xFFFFFFFF, code: 0, cache: 0, csize: 1, buf, pos: 0, len, past: 0 };
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
        c.code = ((c.code << 8) | (c.pos < c.len ? c.buf[c.pos++] : (c.past++, 0))) >>> 0;
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
      if (!enc && c.past > 64) throw new Error('corrupt SSIMF data');   // truncated or not real data
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


  const TE = typeof TextEncoder !== 'undefined' ? new TextEncoder() : null;
  const TD = typeof TextDecoder !== 'undefined' ? new TextDecoder() : null;
  const ascii = s => [s.charCodeAt(0), s.charCodeAt(1), s.charCodeAt(2), s.charCodeAt(3)];
  const isType = (b, p, s) => b[p] === s.charCodeAt(0) && b[p + 1] === s.charCodeAt(1) && b[p + 2] === s.charCodeAt(2) && b[p + 3] === s.charCodeAt(3);

  // metadata object <-> "key\0value\0" bytes
  function packMeta(meta) {
    if (!meta) return null;
    if (meta instanceof Uint8Array) return meta.length ? meta : null;
    const parts = [];
    Object.keys(meta).forEach(k => { parts.push(TE.encode(String(k)), [0], TE.encode(String(meta[k])), [0]); });
    let n = 0; parts.forEach(p => { n += p.length; });
    if (!n) return null;
    const out = new Uint8Array(n); let o = 0; parts.forEach(p => { out.set(p, o); o += p.length; });
    return out;
  }
  function unpackMeta(b) {
    const m = {}; let p = 0;
    while (p < b.length) {
      let k = p; while (k < b.length && b[k]) k++; if (k >= b.length) break;
      let v = k + 1; while (v < b.length && b[v]) v++;
      m[TD.decode(b.subarray(p, k))] = TD.decode(b.subarray(k + 1, v)); p = v + 1;
    }
    return m;
  }

  function encodeStripe(px, w, rows, alpha) {
    const c = makeCoder(true, new Uint8Array(Math.max(1024, w * rows)), 0);
    run(c, px, w, rows, alpha);
    for (let i = 0; i < 5; i++) shiftLow(c);
    return c.buf.subarray(0, c.pos);
  }

  /* ---------- optional WebAssembly speed-up ---------- */
  let wasmMod = null, W = null;
  function wasmReady() {
    if (!W && wasmMod) { try { W = new WebAssembly.Instance(wasmMod, {}).exports; } catch (e) { wasmMod = null; } }
    return W;
  }
  function wasmDone() { if (W && W.memory.buffer.byteLength > 1073741824) W = null; }   // start fresh after a huge image
  function loadWasm(src) {
    if (typeof WebAssembly === 'undefined') return Promise.resolve(false);
    const get = typeof src === 'string' ? fetch(src).then(r => { if (!r.ok) throw new Error('fetch'); return r.arrayBuffer(); }) : Promise.resolve(src);
    return get.then(b => WebAssembly.compile(b)).then(m => { wasmMod = m; W = null; return !!wasmReady(); }).catch(() => false);
  }
  function wasmEncode(rgba, w, h, icc, meta, rows) {
    const E = wasmReady(); if (!E) return null;
    const n = w * h * 4, extra = (icc ? icc.length : 0) + (meta ? meta.length : 0), cap = E.ssimf_max_size_ex(w, h, extra);
    if (!cap || cap > 2000000000) return null;
    const pin = E.ssimf_alloc(n), pi = icc ? E.ssimf_alloc(icc.length) : 0, pm = meta ? E.ssimf_alloc(meta.length) : 0;
    const po = E.ssimf_alloc(24), pout = E.ssimf_alloc(cap);
    let out = null;
    if (pin && po && pout && (!icc || pi) && (!meta || pm)) {
      const u8 = new Uint8Array(E.memory.buffer);
      u8.set(rgba, pin); if (icc) u8.set(icc, pi); if (meta) u8.set(meta, pm);
      const dv = new DataView(E.memory.buffer);   // ssimf_options on wasm32: 6 x 4 bytes
      dv.setUint32(po, pi, true); dv.setUint32(po + 4, icc ? icc.length : 0, true);
      dv.setUint32(po + 8, pm, true); dv.setUint32(po + 12, meta ? meta.length : 0, true);
      dv.setInt32(po + 16, rows === h ? 0 : rows, true); dv.setInt32(po + 20, 1, true);
      const r = E.ssimf_encode_ex(pin, w, h, po, pout, cap);
      if (r > 0) out = new Uint8Array(E.memory.buffer, pout, r).slice();
    }
    [pin, pi, pm, po, pout].forEach(p => { if (p) E.ssimf_free(p); });
    wasmDone();
    return out;
  }
  function wasmDecode(bytes, w, h) {
    const E = wasmReady(); if (!E) return null;
    const pin = E.ssimf_alloc(bytes.length), pout = E.ssimf_alloc(w * h * 4);
    let out = null, failed = false;
    if (pin && pout) {
      new Uint8Array(E.memory.buffer).set(bytes, pin);
      const r = E.ssimf_decode_threads(pin, bytes.length, pout, 1);
      if (r < 0) failed = true; else out = new Uint8ClampedArray(E.memory.buffer.slice(pout, pout + w * h * 4));
    }
    if (pin) E.ssimf_free(pin); if (pout) E.ssimf_free(pout);
    wasmDone();
    if (failed) throw new Error('corrupt SSIMF data');
    return out;
  }

  function encode(rgba, w, h, opt) {
    opt = opt || {};
    if (!(w > 0 && h > 0) || w > MAX_SIDE || h > MAX_SIDE || w * h > MAX_PIXELS) throw new Error('image too large for SSIMF');
    const n = w * h;
    let rows0 = opt.stripeRows > 0 ? Math.floor(opt.stripeRows) : h; if (rows0 > h) rows0 = h;
    const icc0 = opt.icc && opt.icc.length ? opt.icc : null, meta0 = packMeta(opt.meta);
    if (W || wasmMod) { const fast = wasmEncode(rgba, w, h, icc0, meta0, rows0); if (fast) return fast; }
    let alpha = 0;
    for (let i = 0; i < n; i++) if (rgba[4 * i + 3] !== 255) { alpha = 1; break; }
    const px = new Uint32Array(n);
    for (let i = 0; i < n; i++)
      px[i] = ((alpha ? rgba[4 * i + 3] << 24 : 0) | rgba[4 * i] << 16 | rgba[4 * i + 1] << 8 | rgba[4 * i + 2]) >>> 0;
    let rows = opt.stripeRows > 0 ? Math.floor(opt.stripeRows) : h;   // default: one stripe (smallest file)
    if (rows > h) rows = h;
    const ns = Math.ceil(h / rows), stripes = [];
    let total = 0;
    for (let s = 0; s < ns; s++) {
      const y0 = s * rows, rr = Math.min(rows, h - y0);
      const st = encodeStripe(px.subarray(y0 * w, (y0 + rr) * w), w, rr, alpha);
      stripes.push(st); total += st.length;
    }
    const icc = opt.icc && opt.icc.length ? opt.icc : null, meta = packMeta(opt.meta);
    const size = HDR + (icc ? 8 + icc.length : 0) + (meta ? 8 + meta.length : 0) + 12 + 4 * ns + 8 + total;
    const out = new Uint8Array(size), dv = new DataView(out.buffer);
    out.set([0x53, 0x53, 0x49, 0x4D, 0x46, 2], 0);  // "SSIMF" + version 2
    dv.setUint32(6, w); dv.setUint32(10, h); out[14] = alpha;
    let o = HDR;
    const chunk = (type, data) => { out.set(ascii(type), o); dv.setUint32(o + 4, data.length); out.set(data, o + 8); o += 8 + data.length; };
    if (icc) chunk('ICCP', icc);
    if (meta) chunk('META', meta);
    out.set(ascii('STRP'), o); dv.setUint32(o + 4, 4 + 4 * ns); dv.setUint32(o + 8, rows);
    for (let s = 0; s < ns; s++) dv.setUint32(o + 12 + 4 * s, stripes[s].length);
    o += 12 + 4 * ns;
    out.set(ascii('DATA'), o); dv.setUint32(o + 4, total); o += 8;
    for (let s = 0; s < ns; s++) { out.set(stripes[s], o); o += stripes[s].length; }
    return out;
  }

  // Reads and checks the header and chunks. Accepts version 1, version 2 and the early "SSF1" test version.
  function parse(bytes) {
    if (!bytes || bytes.length < 13) return null;
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), len = bytes.length;
    let w, h, alpha, version, hl;
    if (len >= HDR && isType(bytes, 0, 'SSIM') && bytes[4] === 0x46 && (bytes[5] === 1 || bytes[5] === 2)) {
      w = dv.getUint32(6); h = dv.getUint32(10); alpha = bytes[14] & 1; version = bytes[5]; hl = HDR;
    } else if (isType(bytes, 0, 'SSF1')) {
      w = dv.getUint32(4); h = dv.getUint32(8); alpha = bytes[12] & 1; version = 1; hl = 13;
    } else return null;
    if (!w || !h || w > MAX_SIDE || h > MAX_SIDE || w * h > MAX_PIXELS) return null;
    const p = { width: w, height: h, alpha, version, icc: null, meta: null };
    if (version === 1) { p.rows = h; p.sizes = [len - hl]; p.data = hl; p.dataLen = len - hl; return p; }
    let pos = hl, sizes = null;
    while (pos + 8 <= len) {
      const cl = dv.getUint32(pos + 4);
      if (cl > len - pos - 8) return null;
      if (isType(bytes, pos, 'STRP')) {
        if (cl < 8) return null;
        const rows = dv.getUint32(pos + 8);
        if (!rows) return null;
        const ns = Math.ceil(h / rows);
        if (cl !== 4 + 4 * ns) return null;
        p.rows = Math.min(rows, h); sizes = [];
        for (let s = 0; s < ns; s++) sizes.push(dv.getUint32(pos + 12 + 4 * s));
      } else if (isType(bytes, pos, 'ICCP')) p.icc = bytes.subarray(pos + 8, pos + 8 + cl);
      else if (isType(bytes, pos, 'META')) p.meta = bytes.subarray(pos + 8, pos + 8 + cl);
      else if (isType(bytes, pos, 'DATA')) {
        if (!sizes) return null;
        let sum = 0; sizes.forEach(v => { sum += v; });
        if (sum > cl) return null;
        p.sizes = sizes; p.data = pos + 8; p.dataLen = cl;
        return p;
      }
      pos += 8 + cl;
    }
    return null;
  }

  function info(bytes) {
    const p = parse(bytes);
    if (!p) return null;
    return { width: p.width, height: p.height, alpha: p.alpha, version: p.version, stripes: p.sizes.length,
             icc: p.icc, meta: p.meta && TD ? unpackMeta(p.meta) : null };
  }

  function decode(bytes) {
    const p = parse(bytes);
    if (!p) throw new Error('not an SSIMF file (or too large)');
    const w = p.width, h = p.height, alpha = p.alpha, n = w * h;
    const extras = { icc: p.icc, meta: p.meta && TD ? unpackMeta(p.meta) : null };
    if (W || wasmMod) { const fast = wasmDecode(bytes, w, h); if (fast) return { width: w, height: h, rgba: fast, icc: extras.icc, meta: extras.meta }; }
    const px = new Uint32Array(n);
    let off = p.data;
    for (let s = 0; s < p.sizes.length; s++) {
      const y0 = s * p.rows, rr = Math.min(p.rows, h - y0), sl = p.sizes[s];
      const c = makeCoder(false, bytes.subarray(off, off + sl), sl);
      for (let i = 0; i < 5; i++) c.code = ((c.code << 8) | (c.pos < c.len ? c.buf[c.pos++] : (c.past++, 0))) >>> 0;
      run(c, px.subarray(y0 * w, (y0 + rr) * w), w, rr, alpha);
      off += sl;
    }
    const rgba = new Uint8ClampedArray(n * 4);
    for (let i = 0; i < n; i++) {
      const v = px[i];
      rgba[4 * i] = (v >>> 16) & 255; rgba[4 * i + 1] = (v >>> 8) & 255; rgba[4 * i + 2] = v & 255;
      rgba[4 * i + 3] = alpha ? v >>> 24 : 255;
    }
    return { width: w, height: h, rgba, icc: p.icc, meta: p.meta && TD ? unpackMeta(p.meta) : null };
  }

  const SSIMF = { encode, decode, info, loadWasm, version: 2 };
  if (typeof module !== 'undefined' && module.exports) module.exports = SSIMF; else root.SSIMF = SSIMF;
})(typeof self !== 'undefined' ? self : this);
