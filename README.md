# SSIMF – ScreenShot IMage Format

**A lossless image format made for screenshots.** On 22 real full-screen screenshots it was
**34% smaller than fully optimized PNG** and **12% smaller than lossless WebP at maximum effort**.
It saved about 12 times faster than that WebP setting, and every file decoded back pixel-perfect.

**Try it online:** [imgformats.com/ssimf](https://imgformats.com/ssimf/). Convert screenshots to SSIMF, or open `.ssimf` files in your browser. Nothing is uploaded.

**Open .ssimf on Windows:** download **SSIMF Viewer** from the [Releases](../../releases) page, or from
[imgformats.com](https://imgformats.com/wp-content/uploads/2026/10/ssimf-viewer-windows.zip).

> Status: experimental (version 1). The format may still change, so don't use it as the only copy of anything important.

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

## Files

| File | What it is |
|---|---|
| `ssimf.c`, `ssimf.h` | Encoder and decoder in C, about 300 lines, no dependencies |
| `ssimf.js` | The same codec in JavaScript (browser or Node). Writes byte-identical files to the C version |
| `ssimf_cli.c` | Command-line converter (`ssimf encode` / `ssimf decode`) |
| `ssimfview.c`, `ssimfview.rc`, `ssimf.ico` | SSIMF Viewer for Windows |
| `demo.html` | Converter and viewer in one HTML file, which works offline |
| `stb_image.h`, `stb_image_write.h` | Public-domain PNG/JPG loaders by Sean Barrett, used by the tools |

## Use it

**JavaScript**

```html
<script src="ssimf.js"></script>
<script>
  const bytes = SSIMF.encode(imageData.data, width, height);   // Uint8Array, a .ssimf file
  const { width: w, height: h, rgba } = SSIMF.decode(bytes);   // back to pixels
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
```

**Command line**

```
ssimf encode screenshot.png screenshot.ssimf
ssimf decode screenshot.ssimf screenshot.png
```

## Build

macOS / Linux:

```
gcc -O2 -o ssimf ssimf_cli.c ssimf.c -lm
```

Windows (with [w64devkit](https://github.com/skeeto/w64devkit) or MinGW):

```
gcc -O2 -o ssimf.exe ssimf_cli.c ssimf.c
windres ssimfview.rc -O coff -o ssimfview_res.o
gcc -O2 -municode -mwindows -o ssimfview.exe ssimfview.c ssimf.c ssimfview_res.o -lcomdlg32 -lshell32 -lgdi32
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

## File layout (version 1)

```
offset  size  field
0       5     magic "SSIMF"
5       1     version (1)
6       4     width  (unsigned, big-endian)
10      4     height (unsigned, big-endian)
14      1     flags: bit 0 = has transparency
15      ...   range-coded pixel data
```

`ssimf.c` is the reference: a file is valid if `ssimf_decode` in `ssimf.c` decodes it. Decoders also accept files from the early test version (13-byte header starting `SSF1`).

## Limits

- Lossless only, so it is best for screenshots, UI and diagrams. Photos should use JPG, WebP or AVIF.
- Decoding is slower than PNG or WebP (about 0.1 s in C for a 3.4-megapixel screenshot).
- The decoder rejects obviously broken files, but it has not been hardened against malicious input yet.
- No browser or operating system supports `.ssimf` natively. Use `ssimf.js`, the tools here, or SSIMF Viewer.

## License

MIT © 2026 [imgformats.com](https://imgformats.com)
