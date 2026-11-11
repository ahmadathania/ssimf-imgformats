# SSIMF – ScreenShot IMage Format

**A lossless image format made for screenshots.** On 22 real full-screen screenshots it was
**34% smaller than fully optimized PNG** and **12% smaller than lossless WebP at maximum effort**.
It saved about 12 times faster than that WebP setting, and every file decoded back pixel-perfect.

**Try it online:** [imgformats.com/ssimf](https://imgformats.com/ssimf/). Convert screenshots to SSIMF, or open `.ssimf` files in your browser. Nothing is uploaded.

**Open .ssimf on Windows:** download **SSIMF Viewer** from the [Releases](../../releases) page, or from
[imgformats.com](https://imgformats.com/wp-content/uploads/2026/11/ssimf-viewer-windows-v2.zip).

> Status: experimental (version 2). The format may still change, so don't use it as the only copy of anything important.
> Version 2 files need the updated decoder; version 1 files still open everywhere.

## Results

22 full-screen Windows screenshots (2256 × 1504): Gmail, Shopify admin, Razorpay, Google Sheets,
Figma, Google Drive and websites. Every format is lossless, and every file was checked pixel by pixel.

| Format | Total size | vs optimized PNG | Avg. save time | Avg. open time |
|---|---:|---:|---:|---:|
| **SSIMF** | **6.24 MB** | **−34%** | **0.61 s** | 86 ms |
| WebP lossless, max effort | 7.06 MB | −25% | 7.52 s | 14 ms |
| WebP lossless, default | 8.51 MB | −10% | 1.24 s | 18 ms |
| PNG, fully optimized (oxipng) | 9.44 MB | same | 5.88 s | 10 ms |
| JPEG XL lossless (effort 7) | 9.49 MB | +1% | 1.91 s | 408 ms |
| PNG, as Windows saves it | 13.93 MB | +48% | – | 8 ms |
| AVIF lossless | 15.38 MB | +63% | 4.63 s | 111 ms |
| QOI | 17.62 MB | +87% | 0.01 s | 10 ms |

Times were measured on a 2-core cloud machine using the C version. SSIMF's settings were tuned on these same
screenshots, so independent test sets are very welcome.

## What's new in version 2

- **Colour profile (ICC).** A PNG's colour profile is kept, so wide-colour (Display P3) screenshots show their true colours. Converting back to PNG puts it back.
- **Metadata.** Optional key/value text, such as the app or the date.
- **Safer decoding.** The decoder was fuzz-tested (about 535,000 broken and random files under AddressSanitizer and UBSan, with no crashes). It now refuses images larger than 268 million pixels and stops quickly on truncated or fake files.
- **Faster in the browser.** `ssimf.js` can load `ssimf.wasm` (the C code built for WebAssembly) and then opens files about 1.6x faster. Files are identical either way.
- **Optional stripes.** The image can be split into stripes that encode and decode on several CPU cores at once. This is off by default, because stripes can't reuse text from the stripes above them, and files got much bigger in tests (email +76%, code +63%, spreadsheet +32% with 8 stripes). Use it only when speed matters more than size.
- **Pixels are unchanged.** Version 2 stores pixels exactly like version 1. Default files are 24 bytes larger (the new chunk headers), plus any colour profile or metadata.

## Files

| File | What it is |
|---|---|
| `ssimf.c`, `ssimf.h` | Encoder and decoder in C, about 300 lines, no dependencies |
| `ssimf.js` | The same codec in JavaScript (browser or Node). Writes byte-identical files to the C version |
| `ssimf.wasm`, `ssimf_wasm_shim.c` | Optional WebAssembly build of `ssimf.c` that `ssimf.js` can load for speed, and the tiny runtime it is built with |
| `ssimf_cli.c` | Command-line converter (`ssimf encode` / `ssimf decode`) |
| `ssimfview.c`, `ssimfview.rc`, `ssimf.ico` | SSIMF Viewer for Windows |
| `demo.html` | Converter and viewer in one HTML file, which works offline |
| `stb_image.h`, `stb_image_write.h` | Public-domain PNG/JPG loaders by Sean Barrett, used by the tools |

## Use it

**JavaScript**

```html
<script src="ssimf.js"></script>
<script>
  await SSIMF.loadWasm('ssimf.wasm');                          // optional: about 1.6x faster

  const bytes = SSIMF.encode(imageData.data, width, height);   // Uint8Array, a .ssimf file
  const { width: w, height: h, rgba } = SSIMF.decode(bytes);   // back to pixels

  // optional extras
  const file = SSIMF.encode(rgba, w, h, { icc: iccBytes, meta: { Software: 'MyApp' } });
  const { icc, meta } = SSIMF.decode(file);
</script>
```

**C**

```c
#include "ssimf.h"
size_t cap = ssimf_max_size(w, h);
uint8_t *out = malloc(cap);
long n = ssimf_encode(rgba, w, h, out, cap);      /* rgba = w*h*4 bytes */

int w2, h2; ssimf_info(data, len, &w2, &h2);
uint8_t *pixels = malloc((size_t)w2 * h2 * 4);
ssimf_decode(data, len, pixels);

/* optional extras */
ssimf_options o = {0};
o.icc = icc; o.icc_len = icc_len;
o.meta = (const uint8_t *)"Software\0MyApp\0"; o.meta_len = 15;
long n2 = ssimf_encode_ex(rgba, w, h, &o, out, ssimf_max_size_ex(w, h, icc_len + 15));

const uint8_t *p; size_t plen;
if (!ssimf_chunk(data, len, "ICCP", &p, &plen)) { /* p, plen = the colour profile */ }
```

**Command line**

```
ssimf encode screenshot.png screenshot.ssimf [--meta Key=Value ...] [--stripes ROWS]
ssimf decode screenshot.ssimf screenshot.png
ssimf info   screenshot.ssimf
```

## Build

macOS / Linux:

```
gcc -O2 -DSSIMF_THREADS -o ssimf ssimf_cli.c ssimf.c -lm -lpthread
```

`-DSSIMF_THREADS` is optional. It lets striped files use several cores; without it everything runs on one core.

Windows (with [w64devkit](https://github.com/skeeto/w64devkit) or MinGW):

```
gcc -O2 -DSSIMF_THREADS -o ssimf.exe ssimf_cli.c ssimf.c
windres ssimfview.rc -O coff -o ssimfview_res.o
gcc -O2 -DSSIMF_THREADS -municode -mwindows -o ssimfview.exe ssimfview.c ssimf.c ssimfview_res.o -lcomdlg32 -lshell32 -lgdi32
```

WebAssembly (needs clang and wasm-ld):

```
clang --target=wasm32 -O3 -nostdlib -ffreestanding -mbulk-memory -DSSIMF_FREESTANDING -Wl,--no-entry -Wl,-z,stack-size=1048576 \
  -Wl,--export=ssimf_decode_threads -Wl,--export=ssimf_info -Wl,--export=ssimf_encode_ex -Wl,--export=ssimf_max_size_ex \
  -Wl,--export=ssimf_alloc -Wl,--export=ssimf_free -Wl,--strip-all -o ssimf.wasm ssimf.c ssimf_wasm_shim.c
```

## How it works

SSIMF stands for **S**creen**S**hot **IM**age **F**ormat.

For every pixel, in order:

1. **Copy from earlier?** A long-range match copies a run of pixels from anywhere earlier in the
   image, such as repeated words, icons or table rows. The last 4 distances are remembered and cheap to reuse.
2. **Same as a neighbour?** Left, up, up-right or up-left.
3. **A recent colour?** The 32 most recently written new colours.
4. **Spell it out.** Green is predicted from the neighbours (MED predictor). Red and blue are
   predicted as "green plus the neighbours' colour difference", which suits anti-aliased text.

Every decision and number is written with an adaptive binary range coder (LZMA-style). Its
probabilities are picked by context: what the neighbouring pixels were, how busy the area is,
and how large the previous prediction error was.

## File layout (version 2)

All numbers are unsigned and big-endian.

```
offset  size  field
0       5     magic "SSIMF"
5       1     version (2)
6       4     width
10      4     height
14      1     flags: bit 0 = has transparency
15      ...   chunks, each: type (4 ASCII bytes), length (4 bytes), payload
```

| Chunk | Required | Payload |
|---|---|---|
| `ICCP` | no | ICC colour profile |
| `META` | no | UTF-8 text as `key\0value\0` pairs |
| `STRP` | yes | rows per stripe (4 bytes), then the coded size of each stripe (4 bytes each). The number of stripes is height ÷ rows, rounded up |
| `DATA` | yes, last | the coded stripes, one after another |

Decoders skip chunk types they don't know. Each stripe is coded on its own, exactly like a whole version 1 image of that height. The standard encoder writes one stripe, so every encoder (C, JavaScript, WebAssembly) produces identical files.

**Version 1** (still decoded): `"SSIMF"`, version `1`, width, height, flags, then the coded data of one stripe.

`ssimf.c` is the reference: a file is valid if `ssimf_decode` in `ssimf.c` decodes it. Decoders also accept files from the early test version (13-byte header starting `SSF1`).

## Limits

- Lossless only, so it is best for screenshots, UI and diagrams. Photos should use JPG, WebP or AVIF.
- Decoding is slower than PNG or WebP (about 0.1 s in C for a 3.4-megapixel screenshot). Stripes can speed this up on several cores, but they make files bigger.
- There is no checksum, so a file damaged in the middle may still open, with wrong pixels in part of the image.
- The decoder has been fuzz-tested and refuses oversized images, but it hasn't had an outside security review.
- No browser or operating system supports `.ssimf` natively. Use `ssimf.js`, the tools here, or SSIMF Viewer.

## License

MIT © 2026 [imgformats.com](https://imgformats.com)
