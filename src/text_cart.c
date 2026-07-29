/* text_cart.c - wasmcart ABI shell for the text-input cart.
 *
 * canvas_cart.c with two additions, both about input rather than drawing:
 * pads are handed to the input adapter each frame, and wc_on_text is
 * re-exported from cart_input.cpp so the host can deliver typed characters.
 *
 * See cart_main.c for the four asyncify traps this shape exists to avoid;
 * the important one is repeated at sg_present_frame below because it is the
 * easiest to reintroduce.
 */
#include <stdint.h>
#include <string.h>
#include "wasmcart.h"

#define CART_WIDTH   640
#define CART_HEIGHT  360

static wc_info_t      info;
static wc_host_info_t host_info;
static wc_time_t      time_info;
static wc_pad_t       pads[4];

static uint32_t framebuffer[CART_WIDTH * CART_HEIGHT];
static uint32_t frame_no     = 0;
static int      program_done = 0;

/* The host's loop-inversion hook: the cart owns its main loop and suspends
 * here once a frame. See cart_main.c for why Emscripten's own -sASYNCIFY
 * cannot be used instead. */
__attribute__((import_module("env"), import_name("wc_frame_yield")))
extern void wc_frame_yield(void);

extern int asyncify_get_state(void);   /* 0 normal, 1 unwinding, 2 rewinding */

/* From cart_core.cpp: copy the finished Skia frame into `dest`. */
extern int32_t sg_cart_present(uint32_t *dest, uint32_t dest_px);
/* scriptc-game's own init, minus the window (cart_core.cpp). */
extern int32_t sg_init(uint32_t w, uint32_t h, uint32_t flags);

/* From cart_input.cpp: hand it this frame's pad snapshot. Text needs no
 * equivalent -- the host calls wc_on_text directly, and cart_input.cpp
 * exports that itself. */
extern void sg_cart_set_pads(const wc_pad_t *pads);

/* scriptc's program entry. (argc, argv) is its emitted signature; declaring
 * it void produces a wasm-ld signature mismatch, which traps at runtime
 * rather than failing the link. */
extern int scriptc_main(int argc, char **argv);

/* ---- what the TypeScript calls once a frame -------------------------
 *
 * Present the frame we just drew, then suspend until the host asks for the
 * next one. Returns the new frame number, or 0 when the host is done.
 *
 * The counter is bumped HERE, after the yield returns, and NOT in
 * wc_render. A rewind does not replay wc_render's body: it restores the
 * saved stack and lands directly back at this call, so anything above the
 * yield runs exactly once, on the first frame. Counting there pinned the
 * frame number at 1 forever while the host reported frames rendered
 * normally, and the picture simply froze with no error anywhere. */
uint32_t sg_present_frame(void) {
    sg_cart_present(framebuffer, CART_WIDTH * CART_HEIGHT);
    wc_frame_yield();
    frame_no++;
    /* AFTER the yield, for the same reason the counter is bumped here: the
     * rewind lands at this line, so anything above it runs once. Refreshing
     * pads above the yield would freeze input on frame 1's snapshot. */
    sg_cart_set_pads(pads);
    return frame_no;
}

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
    /* Build the Skia surface the canvas tier draws on. Flags are ignored by
     * cart_core: resizable/vsync/headless are window concerns. */
    sg_init(CART_WIDTH, CART_HEIGHT, 0);
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
    if (program_done) return;

    /* UNCONDITIONAL entry, every frame: the asyncify rewind replays this
     * call to reach the suspended yield. A first-frame guard here would
     * leave the rewind nothing to replay and freeze the cart on frame 1. */
    static char *argv0 = "cart";
    static char *argv[2] = { NULL, NULL };
    argv[0] = argv0;
    scriptc_main(1, argv);

    /* Returning with an unwind in flight is the normal yield path. Only a
     * return with no unwind pending means the TS loop finished. */
    if (asyncify_get_state() == 0) program_done = 1;
}

/* The asyncify unwind stack. Without this export the host leaves its
 * descriptor pointer at 0, asyncify writes the suspended stack over address
 * 0, and the cart traps as "unreachable" on the first yield. 64KB rather
 * than the tracer's 16: the canvas call path is far deeper. */
static struct { uint32_t cur, end; uint8_t stack[65536]; } yield_buf;

__attribute__((export_name("wc_yield_buffer")))
uint32_t wc_yield_buffer(void) {
    yield_buf.cur = (uint32_t)(uintptr_t)yield_buf.stack;
    yield_buf.end = (uint32_t)(uintptr_t)(yield_buf.stack + sizeof(yield_buf.stack));
    return (uint32_t)(uintptr_t)&yield_buf;
}

__attribute__((export_name("sc_frame_count")))
uint32_t sc_frame_count(void) { return frame_no; }
