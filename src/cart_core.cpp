/* cart_core.cpp - scriptc-game's sg_core, with the window removed.
 *
 * scriptc-game's Canvas 2D tier is portable already: web/canvas/ talks to a
 * generated skiac shim (sg_skia_gen.cpp + sg_skia_extra.cpp), and BOTH of
 * those compile under emcc unchanged against wasmcart-skia's libskiac.a,
 * whose C surface is byte-identical to the native one.
 *
 * What is NOT portable is sg_core.cpp: it owns an SDL window, an SDL
 * renderer and a streaming texture, and presents by blitting through them.
 * This file provides the same handful of symbols that tier actually needs,
 * backed by a Skia surface whose pixels are copied into the cart's
 * framebuffer once a frame.
 *
 * Only the canvas-facing subset is here. Audio, input, decoding and the GL
 * tier are not: a cart gets those from the host, and pulling them in would
 * mean dragging SDL into a wasm module that has no use for it.
 */
#include <stdint.h>
#include <string.h>

extern "C" {
#include "sg_tables.h"
}
#include "skia_c.hpp"

#define SG_OK            0
#define SG_ESKIA        -3
#define SG_ERANGE       -4

/* The Skia surface every 2D draw lands on, and its size. */
static skiac_surface* g_surface = nullptr;
static uint32_t g_width = 0, g_height = 0;

/* Staging for read_pixels. Skia writes here, then the frame is copied into
 * the cart framebuffer the host reads. */
static uint8_t* g_pixels = nullptr;
static size_t   g_pixels_size = 0;

/* ---- error mailbox --------------------------------------------------
 *
 * host/mailbox.ts reads failures as a string through these. Kept because
 * the canvas tier calls into it on any error path, and a stub returning
 * nothing would turn a real Skia failure into a silent blank frame. */
static char   g_mail[256];
static size_t g_mail_len = 0;

static void mail_set(const char* s) {
    if (!s) { g_mail_len = 0; return; }
    size_t n = strlen(s);
    if (n >= sizeof(g_mail)) n = sizeof(g_mail) - 1;
    memcpy(g_mail, s, n);
    g_mail[n] = 0;
    g_mail_len = n;
}

extern "C" uint32_t sg_str_len(int32_t unused) { return (uint32_t)g_mail_len; }
extern "C" uint32_t sg_str_byte(uint32_t i) {
    return i < g_mail_len ? (uint32_t)(uint8_t)g_mail[i] : 0u;
}

/* ---- init / teardown ------------------------------------------------ */

/* Signature matches sg_init so host/runtime.ts boots unchanged. The flags
 * are accepted and ignored: resizable, vsync and headless are all window
 * concerns, and there is no window. */
extern "C" int32_t sg_init(uint32_t w, uint32_t h, uint32_t flags) {
    (void)flags;
    if (w == 0 || h == 0 || w > 4096 || h > 4096) {
        mail_set("surface dimensions out of range");
        return SG_ERANGE;
    }

    g_surface = skiac_surface_create_rgba_premultiplied((int)w, (int)h, 0);
    if (!g_surface) { mail_set("skia surface creation failed"); return SG_ESKIA; }

    g_pixels_size = (size_t)w * h * 4;
    g_pixels = (uint8_t*)malloc(g_pixels_size);
    if (!g_pixels) { mail_set("pixel buffer allocation failed"); return SG_ESKIA; }

    g_width = w;
    g_height = h;
    return SG_OK;
}

extern "C" void sg_quit(int32_t unused) {
    if (g_pixels) { free(g_pixels); g_pixels = nullptr; }
    g_surface = nullptr;      /* owned by Skia; the cart exits with the host */
}

/* ---- what web/canvas asks for --------------------------------------- */

extern "C" uint32_t sg_screen_canvas(int32_t unused) {
    if (!g_surface) return 0;
    skiac_canvas* c = skiac_surface_get_canvas(g_surface);
    return sg_table_alloc(SG_T_CANVAS, c);
}

extern "C" uint32_t sg_screen_width(int32_t unused)  { return g_width; }
extern "C" uint32_t sg_screen_height(int32_t unused) { return g_height; }

extern "C" int32_t sg_canvas_release(uint32_t hc) {
    return sg_table_take(SG_T_CANVAS, hc) ? SG_OK : -5 /* SG_EBADHANDLE */;
}

/* sg_surface_create is NOT here: shim/sg_skia_extra.cpp already exports it,
 * and defining it again is a duplicate-symbol link error. Only the symbols
 * unique to sg_core.cpp belong in this file. */

/* ---- present --------------------------------------------------------
 *
 * The native build blits through an SDL texture; here the finished frame is
 * copied straight into the cart's framebuffer, which the host reads after
 * wc_render returns.
 *
 * A per-pixel R/B SWAP, not a straight copy.
 *
 * The surface is created RGBA-premultiplied, so Skia's bytes are R,G,B,A in
 * memory; wasmcart reads the framebuffer as 0xAARRGGBB little-endian, whose
 * bytes are B,G,R,A. An earlier version of this file asserted the two orders
 * matched and memcpy'd, which rendered a dark blue scene brown and an amber
 * square cyan -- correct geometry, every colour wrong, which reads as a
 * shader or gradient bug rather than a byte-order one. */
extern "C" int32_t sg_cart_present(uint32_t* dest, uint32_t dest_px) {
    if (!g_surface || !g_pixels) { mail_set("present before init"); return SG_ESKIA; }

    skiac_surface_data data;
    data.ptr = g_pixels;
    data.size = g_pixels_size;
    skiac_surface_read_pixels(g_surface, &data);
    if (!data.ptr) { mail_set("surface read_pixels returned no data"); return SG_ESKIA; }

    size_t n = g_pixels_size / 4;
    if (n > dest_px) n = dest_px;
    const uint32_t* src = (const uint32_t*)data.ptr;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = src[i];                      /* 0xAABBGGRR in memory */
        dest[i] = (p & 0xFF00FF00u)               /* alpha and green stay */
                | ((p & 0x00FF0000u) >> 16)       /* B down to the low byte */
                | ((p & 0x000000FFu) << 16);      /* R up to the high byte */
    }
    return SG_OK;
}
