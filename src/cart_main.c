/* cart_main.c - the wasmcart ABI shell for a scriptc-compiled cart.
 *
 * This file is deliberately DUMB. It owns the framebuffer and implements the
 * three ABI exports, and that is all. Every pixel is decided by
 * src/tracer.ts, compiled to wasm by scriptc; this side only hands it the
 * buffer and lets it run.
 *
 * The split matters for what the tracer proves. If C drew the picture, a
 * passing run would prove only that emcc works. Keeping the drawing in TS
 * means a correct frame is evidence that scriptc-compiled TypeScript ran
 * inside the cart, which is the open question.
 *
 * WHY THE CART PULLS FRAMES.
 *
 * scriptc compiles a PROGRAM, not a library: an exported TS function that
 * nothing calls is stripped as dead code, and FFI format 1 has no callback
 * class, so C cannot hand a function pointer back into TS either. There is no
 * way to have the host call a TS render function directly.
 *
 * So the TS keeps its own main loop and PULLS frames: it calls
 * sg_wait_frame(), which suspends the whole wasm stack through the host's
 * wc_frame_yield protocol and returns to the host. When the host next calls
 * wc_render, the stack is rewound and sg_wait_frame returns the new frame. The game loop lives
 * in TypeScript, which is both what a game author would write and a stronger
 * result than a callback: the compiled TS drives itself.
 *
 * A 2D framebuffer cart (gpu_api = 0), not GL. GL is the harder path and is
 * not the question here: this establishes that scriptc -> wasm -> wasmcart
 * closes at all.
 */
#include <stdint.h>
#include <string.h>
#include "wasmcart.h"

/* The host's loop-inversion hook.
 *
 * wasmcart has a protocol built for exactly this case: a cart that owns its
 * main loop calls wc_frame_yield() once per frame, and the host (which
 * post-processes the wasm with binaryen's asyncify pass) unwinds the whole
 * stack out of wc_render, then rewinds back to this point on the next frame.
 *
 * Emscripten's own -sASYNCIFY does NOT work here and was the first attempt:
 * emscripten_sleep needs the host's JS event loop to resume it, but wasmcart
 * calls wc_render synchronously and waits for it to return, so the cart
 * deadlocked on frame 1. The two models are incompatible; wc_frame_yield is
 * the one this host actually implements. */
__attribute__((import_module("env"), import_name("wc_frame_yield")))
extern void wc_frame_yield(void);

/* Injected by binaryen's asyncify pass. 0 = normal, 1 = unwinding,
 * 2 = rewinding. */
extern int asyncify_get_state(void);

#define CART_WIDTH   640
#define CART_HEIGHT  360

static wc_info_t      info;
static wc_host_info_t host_info;
static wc_time_t      time_info;
static wc_pad_t       pads[4];

/* The framebuffer the host reads. 0xAARRGGBB little-endian. */
static uint32_t framebuffer[CART_WIDTH * CART_HEIGHT];

static uint32_t frame_no    = 0;
static uint32_t entry_state = 99;   /* incremented per wc_render */
static int      program_done = 0;  /* TS main loop returned */

/* ---- what the TypeScript side calls ---------------------------------
 *
 * The FFI surface named in tracer.ffi.json. Bounds-checked because a bug
 * writing past the framebuffer would otherwise corrupt whatever the linker
 * placed next and present as a baffling failure somewhere unrelated. */

uint32_t sg_fb_width(void)  { return CART_WIDTH; }
uint32_t sg_fb_height(void) { return CART_HEIGHT; }

void sg_fb_set(uint32_t index, uint32_t argb) {
    if (index < (uint32_t)(CART_WIDTH * CART_HEIGHT)) framebuffer[index] = argb;
}

/* Suspend until the host asks for the next frame.
 *
 * Frames are paced entirely by the host calling wc_render; this never spins
 * and never sleeps. */
uint32_t sg_wait_frame(void) {
    /* Suspends here; resumes on the line after wc_frame_yield when the host
     * next calls wc_render.
     *
     * The counter is bumped HERE, on resume, not in wc_render. A rewind does
     * not replay wc_render's body -- it restores the saved stack and lands
     * directly back at this call -- so anything wc_render does above the
     * yield runs exactly once, on the very first frame. Counting there left
     * the frame number pinned at 1 forever while the host happily reported
     * 10 frames rendered. */
    wc_frame_yield();
    frame_no++;
    return frame_no;
}

/* scriptc's program entry, renamed at link time so it does not collide with
 * Emscripten's own main. */
/* (argc, argv) because that is scriptc's emitted signature. Declaring it
 * void caused a wasm-ld signature mismatch, which in wasm is not cosmetic:
 * an indirect call through the wrong type traps at runtime as "null function
 * or function signature mismatch". */
extern int scriptc_main(int argc, char **argv);

/* ---- wasmcart ABI ---------------------------------------------------- */

__attribute__((export_name("wc_get_info")))
wc_info_t *wc_get_info(void) {
    info.version         = WC_ABI_VERSION;
    info.width           = CART_WIDTH;
    info.height          = CART_HEIGHT;
    info.fb_ptr          = (uint32_t)(uintptr_t)framebuffer;
    info.audio_ptr       = 0;
    info.audio_cap       = 0;
    info.audio_write_ptr = 0;
    info.input_ptr       = (uint32_t)(uintptr_t)pads;
    info.save_ptr        = 0;
    info.save_size       = 0;
    info.time_ptr        = (uint32_t)(uintptr_t)&time_info;
    info.host_info_ptr   = (uint32_t)(uintptr_t)&host_info;
    info.flags           = 0;
    info.audio_sample_rate = 0;
    info.pointer_ptr     = 0;
    info.keys_ptr        = 0;
    info.gpu_api         = 0;        /* 2D framebuffer */
    return &info;
}

__attribute__((export_name("wc_init")))
void wc_init(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
    if (program_done) return;


    /* UNCONDITIONAL entry, every frame. Not a first-frame guard.
     *
     * This is the part that is easy to get wrong: an asyncify REWIND replays
     * this call chain from the top of wc_render to fast-forward back to the
     * suspended yield point inside the TS loop. Guarding the call with a
     * `started` flag (the obvious-looking version) means the rewind has
     * nothing to replay, so the loop never resumes: the cart renders frame 1
     * correctly and then freezes with the host happily counting frames. That
     * failure is silent -- a still image, no error -- which is why the
     * harness checks that the box MOVES rather than merely that it drew. */
    static char *argv0 = "cart";
    static char *argv[2] = { NULL, NULL };
    argv[0] = argv0;
    scriptc_main(1, argv);

    /* Reaching here with an unwind IN FLIGHT is the normal yield path: the
     * call above returned only because asyncify is unwinding, and the host
     * will call asyncify_stop_unwind after we return. Only a return with NO
     * unwind pending means the TS loop genuinely finished.
     *
     * asyncify_get_state(): 0 = normal, 1 = unwinding, 2 = rewinding. */
    if (asyncify_get_state() == 0) program_done = 1;
}

/* The asyncify unwind stack.
 *
 * The host reads this through wc_yield_buffer() and hands the address to
 * asyncify_start_unwind/rewind. Without the export the host leaves its
 * descriptor pointer at 0, asyncify writes the suspended stack over address
 * 0, and the cart traps as "unreachable" on the first yield -- which is
 * exactly what happened before this was added, and it looks like a compiler
 * bug rather than a missing export.
 *
 * Layout is fixed by asyncify: {current, end} then the stack area. 16KB is
 * ample for the tracer's shallow loop; a deep call stack at the yield point
 * would need more. */
static struct { uint32_t cur, end; uint8_t stack[16384]; } yield_buf;

__attribute__((export_name("wc_yield_buffer")))
uint32_t wc_yield_buffer(void) {
    yield_buf.cur = (uint32_t)(uintptr_t)yield_buf.stack;
    yield_buf.end = (uint32_t)(uintptr_t)(yield_buf.stack + sizeof(yield_buf.stack));
    return (uint32_t)(uintptr_t)&yield_buf;
}

/* Not part of the ABI: a probe the harness calls to confirm the loop
 * advanced, rather than inferring it from pixels alone. */
__attribute__((export_name("sc_frame_count")))
uint32_t sc_frame_count(void) { return frame_no; }
