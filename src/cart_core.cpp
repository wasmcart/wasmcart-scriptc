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
#include <stdlib.h>
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

/* Exported, not static: the GL shim and sg_audio_decode.cpp call
 * sg_mail_set on their error paths, and an undefined one is host-stubbed
 * to -1 -- so a real decode failure would report nothing at all. */
extern "C" void sg_mail_set(const char* s);

static void mail_set(const char* s) { sg_mail_set(s); }

extern "C" void sg_mail_set(const char* s) {
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

/* ---- assets ---------------------------------------------------------
 *
 * host/resources.ts reads game files with node:fs, which a cart has none of.
 * These expose the host's asset API instead, in the only shape FFI format 1
 * allows: it has no bytes-RETURN class, so a caller asks for the size,
 * allocates, and passes the buffer in to be filled.
 *
 * Paths are the same relative URLs a game already writes ("player.png"),
 * resolved by the host against the .wasc's asset directory. */
__attribute__((import_module("env"), import_name("wc_asset_size")))
extern "C" int wc_asset_size(const char* path, unsigned int path_len);
__attribute__((import_module("env"), import_name("wc_load_asset")))
extern "C" int wc_load_asset(const char* path, unsigned int path_len,
                             void* dest, unsigned int max_size);

/** Byte length of an asset, or -1 when it does not exist. */
extern "C" int32_t sg_cart_asset_size(const uint8_t* path, uint32_t path_len) {
    char p[512];
    if (path_len >= sizeof(p)) return -1;
    memcpy(p, path, path_len);
    p[path_len] = 0;
    return wc_asset_size(p, path_len);
}

/** Fills `dest` with the asset; returns the byte count, or -1 on failure. */
extern "C" int32_t sg_cart_asset_read(const uint8_t* path, uint32_t path_len,
                                      const uint8_t* dest, uint32_t dest_len) {
    char p[512];
    if (path_len >= sizeof(p)) return -1;
    memcpy(p, path, path_len);
    p[path_len] = 0;
    return wc_load_asset(p, path_len, (void*)dest, dest_len);
}

/* ---- timing and window stubs ----------------------------------------
 *
 * The rest of shim/sg_core.cpp's surface. host/runtime.ts and web/globals.ts
 * call these on every frame, so a missing definition becomes a host-stubbed
 * import returning a constant -- which for a CLOCK means performance.now()
 * never advances and every dt is zero. That is a frozen game with no error,
 * the same silent-stub failure mode as the math FFI.
 *
 * The clock is the host's: it writes time_ms into the wc_time_t the cart
 * publishes, once per frame, before wc_render. sg_cart_set_clock hands that
 * to us so sg_ticks can report it. */
static double g_now_ms;

extern "C" void sg_cart_set_clock(double ms) { g_now_ms = ms; }

extern "C" double sg_ticks(int32_t unused) { (void)unused; return g_now_ms; }

/* No sleeping in a cart: the host paces frames, and blocking here would
 * stall wc_render rather than yield. */
extern "C" void sg_delay(uint32_t ms) { (void)ms; }

/* 0 means "unknown", which host/runtime.ts already handles by falling back
 * to a 60Hz budget. Claiming a rate we cannot verify would mislabel every
 * hitch. */
extern "C" uint32_t sg_display_hz(int32_t unused) { (void)unused; return 0; }

/* Fullscreen belongs to the host's window, not the cart. */
extern "C" int32_t sg_set_fullscreen(int32_t on) { (void)on; return SG_ESKIA; }
extern "C" int32_t sg_is_fullscreen(int32_t unused) { (void)unused; return 0; }

/* No filesystem to write a PNG to. Reported as failure rather than
 * pretending to succeed. */
extern "C" int32_t sg_surface_save_png(uint32_t hs, const uint8_t* path, uint32_t len) {
    (void)hs; (void)path; (void)len;
    mail_set("screenshots are unavailable in a cart");
    return SG_ESKIA;
}

/* Handle-table diagnostics; the real counters live in sg_tables.c. */
extern "C" uint32_t sg_debug_live(uint32_t kind) { (void)kind; return 0; }
extern "C" uint32_t sg_debug_high_water(uint32_t kind) { (void)kind; return 0; }

/* ---- fonts ----------------------------------------------------------
 *
 * scriptc-game registers fonts by PATH (sg_font_register -> skiac's
 * register_from_path), which a cart cannot do: there is no filesystem, and
 * the WASM Skia is deliberately patched not to scan /fonts/ (a directory
 * nothing creates, which used to be an out-of-bounds write on the first
 * Canvas call -- see wasmcart-jsgame/REBUILD_STATUS.md).
 *
 * So the cart registers from BYTES instead, pulled through the host's asset
 * API. Same skiac font collection underneath, so sg_text_set_font and
 * sg_text_draw work unchanged once a family is in it.
 *
 * Without any registered font Skia has no typeface to shape with, and text
 * silently draws nothing: no error, no glyphs, just an unchanged
 * framebuffer. That is why sg_cart_font_from_asset reports failures through
 * the mailbox rather than returning void. */
extern "C" skiac_font_collection* sg_fonts(void);   /* shim/sg_skia_extra.cpp */

__attribute__((import_module("env"), import_name("wc_asset_size")))
extern "C" int wc_asset_size(const char* path, unsigned int path_len);
__attribute__((import_module("env"), import_name("wc_load_asset")))
extern "C" int wc_load_asset(const char* path, unsigned int path_len,
                             void* dest, unsigned int max_size);

/* sg_font_register: the path-based entry point web/globals.ts's
 * FontFace.load() calls.
 *
 * The native shim hands the path to Skia, which opens a file. A cart has no
 * files, so this resolves the SAME path through the host asset API instead.
 * That keeps `new FontFace("X", "url(DejaVuSans.ttf)")` working unchanged --
 * without it the registration fails, the font collection stays empty, and
 * every fillText draws nothing at all with only a one-line warning. */
extern "C" int32_t sg_cart_font_from_asset(const uint8_t* path, uint32_t path_len,
                                           const uint8_t* alias, uint32_t alias_len);

extern "C" int32_t sg_font_register(const uint8_t* path, uint32_t path_len) {
    return sg_cart_font_from_asset(path, path_len, nullptr, 0);
}

extern "C" int32_t sg_cart_font_from_asset(const uint8_t* path, uint32_t path_len,
                                           const uint8_t* alias, uint32_t alias_len) {
    skiac_font_collection* fc = sg_fonts();
    if (!fc) { mail_set("font collection unavailable"); return SG_ESKIA; }

    char p[512];
    if (path_len >= sizeof(p)) { mail_set("font asset path too long"); return SG_ERANGE; }
    memcpy(p, path, path_len);
    p[path_len] = 0;

    int n = wc_asset_size(p, path_len);
    if (n <= 0) { mail_set("font asset not found"); return SG_ESKIA; }

    uint8_t* buf = (uint8_t*)malloc((size_t)n);
    if (!buf) { mail_set("font buffer allocation failed"); return SG_ESKIA; }
    if (wc_load_asset(p, path_len, buf, (unsigned int)n) != n) {
        free(buf);
        mail_set("font asset read short");
        return SG_ESKIA;
    }

    char a[128];
    const char* name = nullptr;
    if (alias_len > 0 && alias_len < sizeof(a)) {
        memcpy(a, alias, alias_len);
        a[alias_len] = 0;
        name = a;
    }

    /* Skia COPIES the bytes into its own typeface, so the buffer is ours to
     * free immediately; keeping it would leak 742KB per registration. */
    uint32_t id = skiac_font_collection_register(fc, buf, (size_t)n, name);
    free(buf);
    if (id == 0) { mail_set("font registration failed (unparseable font data)"); return SG_ESKIA; }
    return SG_OK;
}

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
/* sg_present IS the frame boundary for a cart.
 *
 * host/runtime.ts already calls ffi.present() exactly once per frame, at
 * the right point: after the game has drawn and after any screenshot. So
 * rather than restructuring run()'s `for(;;)` into something a cart can
 * step -- which would fork the host runtime -- the cart yields from HERE.
 * Copy the finished Skia frame into the cart framebuffer, suspend, and
 * return when the host asks for the next frame.
 *
 * run() then loops forever exactly as it does natively, and runtime.ts
 * needs no cart-specific code at all. */
__attribute__((import_module("env"), import_name("wc_frame_yield")))
extern "C" void wc_frame_yield(void);

/* Set by the ABI shell so this file can reach the framebuffer it owns. */
static uint32_t* g_fb;
static uint32_t  g_fb_px;

extern "C" void sg_cart_set_fb(uint32_t* fb, uint32_t px) { g_fb = fb; g_fb_px = px; }

extern "C" int32_t sg_cart_present(uint32_t* dest, uint32_t dest_px);

/* Refreshed by the ABI shell so the RESUME side can update per-frame state
 * that wc_render's body would otherwise set only once.
 *
 * This matters because an asyncify rewind does not replay wc_render: it
 * restores the saved stack and lands directly back at the wc_frame_yield
 * below. Anything wc_render does above scriptc_main therefore runs exactly
 * once, on the first frame. Setting the clock there left performance.now()
 * frozen at its frame-1 value, so every dt was 0 and the game rendered one
 * frame and then sat perfectly still -- with no error at all. */
extern "C" void sg_cart_refresh_frame(void);

/* scriptc's event loop: drains process.nextTick, then microtasks to
 * exhaustion, then timers. Exported by the runtime (scr_async.c). */
extern "C" void scr_loop_run(void);

/* Diagnostic counters, exported so a harness can read them WITHOUT going
 * through the game's own render path.
 *
 * Kept because an in-game probe reported sg_present as never called, which
 * sent this investigation down a wrong path for a while: the probe was
 * painting into a framebuffer the game then overwrote. Reading a counter
 * through an export cannot be fooled that way. */
uint32_t sc_present_calls = 0;
uint32_t sc_loop_calls = 0;
__attribute__((export_name("sc_present_calls")))
extern "C" uint32_t sc_get_present_calls(void) { return sc_present_calls; }
__attribute__((export_name("sc_loop_calls")))
extern "C" uint32_t sc_get_loop_calls(void) { return sc_loop_calls; }

/* Is anything ON the microtask queue when the turn ends? If this stays 0
 * the continuations were never queued; if it climbs, they are queued but
 * not running. */
extern "C" bool scr_loop_has_ready(void);

uint32_t sc_ready_seen = 0;
__attribute__((export_name("sc_ready_seen")))
extern "C" uint32_t sc_get_ready_seen(void) { return sc_ready_seen; }

extern "C" int32_t sg_present(int32_t unused) {
    (void)unused;
    sc_present_calls++;
    if (g_fb) sg_cart_present(g_fb, g_fb_px);
    /* END THE TURN before suspending.
     *
     * A native build ends each frame with `await`, which lets promise
     * continuations run; the cart's loop is synchronous (asyncify cannot
     * rewind across a scriptc await), so nothing would ever drain the
     * microtask queue and a fetch().then() chain would never advance. That
     * is exactly what silenced a game's music: the fetch resolved and the
     * handler that decodes it simply never ran, with no error.
     *
     * scr_loop_run does what the await did: nextTick, then microtasks to
     * exhaustion, then due timers. Called here rather than at the top of
     * the frame so continuations land before the frame that observes
     * them, matching the ordering a browser gives. */
    sc_loop_calls++;
    /* Check for queued microtasks BOTH before and after the host task
     * drain, since a resolved fetch queues its continuation from there. */
    if (scr_loop_has_ready()) sc_ready_seen++;
    scr_loop_run();

    wc_frame_yield();
    /* Resumed: the host has advanced its clock and written new pad state. */
    sg_cart_refresh_frame();
    return SG_OK;
}

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
