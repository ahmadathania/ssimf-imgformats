/*
 * SSIMF Viewer for Windows.
 * Opens .ssimf files (and PNG/JPG/BMP), shows them fitted to the window,
 * saves as PNG or SSIMF, and can register itself so double-clicking a .ssimf
 * file opens it.
 *
 *   ssimfview.exe                    open the viewer (offers to register on first run)
 *   ssimfview.exe file.ssimf           open a file
 *   ssimfview.exe --register         make .ssimf files open with this program
 *   ssimfview.exe --unregister       undo that
 *   ssimfview.exe --topng in out     convert without a window (for testing)
 *
 * MIT License, Copyright (c) 2026 imgformats.com.
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "ssimf.h"
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define APPNAME L"SSIMF Viewer"
#define PROGID L"SSIMF.Image"

static HWND g_wnd;
static uint8_t *g_rgba;          /* decoded pixels, RGBA */
static uint8_t *g_bgra;          /* display pixels, BGRA composited on a checkerboard */
static int g_w, g_h;
static size_t g_fileSize;
static int g_isSsf;
static wchar_t g_path[MAX_PATH * 2];
static wchar_t g_msg[512] = L"Drop a .ssimf file here, or press Ctrl+O to open one.";

/* ---------- file helpers ---------- */
static uint8_t *read_all(const wchar_t *path, size_t *len) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc(n);
    if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); *len = (size_t)n; return b;
}
static int write_all(const wchar_t *path, const void *data, size_t len) {
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f); return ok;
}
static void to_utf8(const wchar_t *w, char *out, int cap) { WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL); }

/* Loads .ssimf or a common image. Returns RGBA (malloc) or NULL with a message in err. */
static uint8_t *load_image(const wchar_t *path, int *w, int *h, size_t *fsize, int *isSsf, wchar_t *err, int errcap) {
    size_t len; uint8_t *data = read_all(path, &len);
    if (!data) { swprintf(err, errcap, L"Could not read the file."); return NULL; }
    *fsize = len;
    uint8_t *rgba = NULL;
    if ((len >= 5 && !memcmp(data, "SSIMF", 5)) || (len >= 4 && !memcmp(data, "SSF1", 4))) {  /* SSF1 = earlier test version */
        *isSsf = 1;
        if (ssimf_info(data, len, w, h) || (double)*w * *h > 400e6) { swprintf(err, errcap, L"This SSIMF file has an invalid header."); free(data); return NULL; }
        rgba = malloc((size_t)*w * *h * 4);
        if (!rgba) { swprintf(err, errcap, L"Not enough memory to open this image."); free(data); return NULL; }
        if (ssimf_decode(data, len, rgba) < 0) { swprintf(err, errcap, L"This SSIMF file is damaged and could not be decoded."); free(rgba); free(data); return NULL; }
    } else {
        *isSsf = 0;
        int ch; uint8_t *p = stbi_load_from_memory(data, (int)len, w, h, &ch, 4);
        if (!p) { swprintf(err, errcap, L"This is not an SSIMF file or a supported image (PNG, JPG, BMP, GIF, TGA)."); free(data); return NULL; }
        rgba = malloc((size_t)*w * *h * 4); memcpy(rgba, p, (size_t)*w * *h * 4); stbi_image_free(p);
    }
    free(data);
    return rgba;
}

static void build_display(void) {
    free(g_bgra);
    g_bgra = malloc((size_t)g_w * g_h * 4);
    if (!g_bgra) return;
    for (int y = 0; y < g_h; y++) for (int x = 0; x < g_w; x++) {
        size_t i = ((size_t)y * g_w + x) * 4;
        int a = g_rgba[i + 3], bg = (((x >> 3) ^ (y >> 3)) & 1) ? 204 : 255;
        for (int c = 0; c < 3; c++) g_bgra[i + 2 - c] = (uint8_t)((g_rgba[i + c] * a + bg * (255 - a) + 127) / 255);
        g_bgra[i + 3] = 255;
    }
}

static void update_title(void) {
    wchar_t t[MAX_PATH * 2 + 128];
    if (g_rgba) {
        const wchar_t *name = wcsrchr(g_path, L'\\'); name = name ? name + 1 : g_path;
        swprintf(t, sizeof t / sizeof *t, L"%ls  \x2014  %d \x00D7 %d  \x2014  %ls %.1f KB  \x2014  " APPNAME,
                 name, g_w, g_h, g_isSsf ? L"SSIMF" : L"image", g_fileSize / 1024.0);
    } else wcscpy(t, APPNAME);
    SetWindowTextW(g_wnd, t);
}

static void open_path(const wchar_t *path) {
    int w, h, isSsf; size_t fs; wchar_t err[256];
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    uint8_t *p = load_image(path, &w, &h, &fs, &isSsf, err, 256);
    SetCursor(old);
    if (!p) { MessageBoxW(g_wnd, err, APPNAME, MB_ICONWARNING); return; }
    free(g_rgba); g_rgba = p; g_w = w; g_h = h; g_fileSize = fs; g_isSsf = isSsf;
    wcsncpy(g_path, path, sizeof g_path / sizeof *g_path - 1);
    build_display(); update_title(); InvalidateRect(g_wnd, NULL, TRUE);
}

/* ---------- save ---------- */
static void stbw(void *ctx, void *data, int size) { fwrite(data, 1, size, (FILE *)ctx); }
static void save_as(void) {
    if (!g_rgba) return;
    wchar_t out[MAX_PATH * 2]; wcscpy(out, g_path);
    wchar_t *dot = wcsrchr(out, L'.'), *sl = wcsrchr(out, L'\\');
    if (dot && (!sl || dot > sl)) *dot = 0;
    OPENFILENAMEW of = {0};
    of.lStructSize = sizeof of; of.hwndOwner = g_wnd;
    of.lpstrFilter = L"PNG image (*.png)\0*.png\0SSIMF screenshot (*.ssimf)\0*.ssimf\0";
    of.nFilterIndex = g_isSsf ? 1 : 2;
    of.lpstrFile = out; of.nMaxFile = sizeof out / sizeof *out;
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    of.lpstrDefExt = g_isSsf ? L"png" : L"ssimf";
    if (!GetSaveFileNameW(&of)) return;
    const wchar_t *ext = wcsrchr(out, L'.');
    int asSsf = ext ? !_wcsicmp(ext, L".ssimf") : of.nFilterIndex == 2;
    int ok = 0;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (asSsf) {
        size_t cap = ssimf_max_size(g_w, g_h); uint8_t *buf = malloc(cap);
        long n = buf ? ssimf_encode(g_rgba, g_w, g_h, buf, cap) : -1;
        ok = n > 0 && write_all(out, buf, (size_t)n);
        free(buf);
    } else {
        FILE *f = _wfopen(out, L"wb");
        if (f) { ok = stbi_write_png_to_func(stbw, f, g_w, g_h, 4, g_rgba, g_w * 4); fclose(f); }
    }
    SetCursor(old);
    if (!ok) MessageBoxW(g_wnd, L"Could not save the file.", APPNAME, MB_ICONWARNING);
}

static void open_dialog(void) {
    wchar_t p[MAX_PATH * 2] = L"";
    OPENFILENAMEW of = {0};
    of.lStructSize = sizeof of; of.hwndOwner = g_wnd;
    of.lpstrFilter = L"SSIMF screenshots (*.ssimf)\0*.ssimf\0Images (*.ssimf;*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.ssimf;*.ssf;*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files\0*.*\0";
    of.lpstrFile = p; of.nMaxFile = sizeof p / sizeof *p; of.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&of)) open_path(p);
}

/* ---------- file association (current user only, no admin needed) ---------- */
static int set_key(const wchar_t *sub, const wchar_t *name, const wchar_t *val) {
    HKEY k; wchar_t full[512]; swprintf(full, 512, L"Software\\Classes\\%ls", sub);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, full, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k); return r == ERROR_SUCCESS;
}
/* Removes the association made by the earlier "SSF Viewer" test version, if it is still there. */
static void cleanup_legacy(void) {
    wchar_t have[64]; DWORD sz = sizeof have;
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.ssf", NULL, RRF_RT_REG_SZ, NULL, have, &sz) && !wcscmp(have, L"SSF.Image"))
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.ssf");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\SSF.Image");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\ssfview.exe");
}
static int do_register(void) {
    cleanup_legacy();
    wchar_t exe[MAX_PATH * 2], cmd[MAX_PATH * 2 + 16], icon[MAX_PATH * 2 + 8];
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"\"%ls\" \"%%1\"", exe);
    swprintf(icon, sizeof icon / sizeof *icon, L"\"%ls\",0", exe);
    int ok = set_key(L".ssimf", NULL, PROGID) && set_key(L".ssimf", L"Content Type", L"image/x-ssimf") && set_key(L".ssimf", L"PerceivedType", L"image")
          && set_key(L".ssimf\\OpenWithProgids", PROGID, L"")
          && set_key(PROGID, NULL, L"SSIMF screenshot") && set_key(PROGID L"\\DefaultIcon", NULL, icon)
          && set_key(PROGID L"\\shell\\open\\command", NULL, cmd)
          && set_key(L"Applications\\ssimfview.exe\\shell\\open\\command", NULL, cmd)
          && set_key(L"Applications\\ssimfview.exe\\SupportedTypes", L".ssimf", L"");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return ok;
}
static void do_unregister(void) {
    cleanup_legacy();
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\" PROGID);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\.ssimf");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\Applications\\ssimfview.exe");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}
static int is_registered_here(void) {
    wchar_t exe[MAX_PATH * 2], want[MAX_PATH * 2 + 16], have[MAX_PATH * 2 + 16]; DWORD sz = sizeof have;
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    swprintf(want, sizeof want / sizeof *want, L"\"%ls\" \"%%1\"", exe);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\" PROGID L"\\shell\\open\\command", NULL, RRF_RT_REG_SZ, NULL, have, &sz)) return 0;
    return !wcscmp(want, have);
}

/* ---------- window ---------- */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(32, 36, 40)); FillRect(dc, &rc, bg); DeleteObject(bg);
        if (g_bgra) {
            int cw = rc.right, ch = rc.bottom;
            double s = (double)cw / g_w; if ((double)ch / g_h < s) s = (double)ch / g_h; if (s > 1) s = 1;
            int dw = (int)(g_w * s + .5), dh = (int)(g_h * s + .5), dx = (cw - dw) / 2, dy = (ch - dh) / 2;
            BITMAPINFO bi = {0};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_w; bi.bmiHeader.biHeight = -g_h;
            bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, s < 1 ? HALFTONE : COLORONCOLOR); SetBrushOrgEx(dc, 0, 0, NULL);
            StretchDIBits(dc, dx, dy, dw, dh, 0, 0, g_w, g_h, g_bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 208, 214));
            HFONT f = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            HGDIOBJ o = SelectObject(dc, f);
            DrawTextW(dc, g_msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, o); DeleteObject(f);
        }
        EndPaint(h, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_DROPFILES: {
        wchar_t p[MAX_PATH * 2];
        if (DragQueryFileW((HDROP)wp, 0, p, MAX_PATH * 2)) open_path(p);
        DragFinish((HDROP)wp); return 0;
    }
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) < 0 && wp == 'O') open_dialog();
        else if (GetKeyState(VK_CONTROL) < 0 && wp == 'S') save_as();
        else if (wp == VK_ESCAPE) PostMessage(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_RBUTTONUP: {
        HMENU mn = CreatePopupMenu();
        AppendMenuW(mn, MF_STRING, 1, L"Open\x2026\tCtrl+O");
        AppendMenuW(mn, MF_STRING | (g_rgba ? 0 : MF_GRAYED), 2, L"Save as PNG or SSIMF\x2026\tCtrl+S");
        AppendMenuW(mn, MF_SEPARATOR, 0, NULL);
        AppendMenuW(mn, MF_STRING, 3, L"Open .ssimf files with SSIMF Viewer");
        POINT pt; GetCursorPos(&pt);
        int c = TrackPopupMenu(mn, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
        DestroyMenu(mn);
        if (c == 1) open_dialog(); else if (c == 2) save_as();
        else if (c == 3) MessageBoxW(h, do_register() ? L"Done. Double-click any .ssimf file to open it here.\n\nIf Windows asks which app to use, pick SSIMF Viewer and tick \"Always\"." : L"Could not update the file settings.", APPNAME, MB_ICONINFORMATION);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && !wcscmp(argv[1], L"--register")) return do_register() ? 0 : 1;
    if (argc >= 2 && !wcscmp(argv[1], L"--unregister")) { do_unregister(); return 0; }
    if (argc >= 4 && !wcscmp(argv[1], L"--topng")) {
        int w, h, s; size_t fs; wchar_t err[256];
        uint8_t *p = load_image(argv[2], &w, &h, &fs, &s, err, 256);
        if (!p) return 2;
        FILE *f = _wfopen(argv[3], L"wb"); if (!f) return 3;
        int ok = stbi_write_png_to_func(stbw, f, w, h, 4, p, w * 4); fclose(f);
        return ok ? 0 : 4;
    }

    SetProcessDPIAware();
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof wc; wc.lpfnWndProc = wndproc; wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"SSIMFViewer";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1)); wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"SSIMFViewer", APPNAME, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1200, 820, NULL, NULL, inst, NULL);
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);

    if (argc >= 2) open_path(argv[1]);
    else if (!is_registered_here()) {
        if (MessageBoxW(g_wnd, L"Do you want .ssimf files to open in SSIMF Viewer when you double-click them?\n\n"
                               L"Tip: first move this folder somewhere permanent (for example Documents\\SSIMF), "
                               L"because Windows will remember where SSIMF Viewer is.", APPNAME, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            MessageBoxW(g_wnd, do_register() ? L"Done. Double-click any .ssimf file to open it here.\n\nIf Windows asks which app to use, pick SSIMF Viewer and tick \"Always\"."
                                             : L"Could not update the file settings.", APPNAME, MB_ICONINFORMATION);
        }
    }
    LocalFree(argv);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
