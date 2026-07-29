/* gl_cart.c - wasmcart ABI shell for a GL cart.
 *
 * Simpler than canvas_cart.c because there is nothing to present: a GL cart
 * draws straight into the host's context, so wc_render only has to let the
 * TypeScript run to its next yield. No framebuffer, no read_pixels, no
 * channel swizzle.
 *
 * `gpu_api = 1` announces WebGL2. The host also detects a GL cart from the
 * wasm import section (anything importing the "gl" module), so this field
 * confirms rather than gates; see wasmcart/SPEC.md.
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

/* The audio ring lives in cart_audio.cpp; wc_get_info publishes it. */
extern float    g_audio_ring[];
extern uint32_t g_audio_write;
extern void     sg_cart_audio_pump(uint32_t frames);
#define AUDIO_CAP 8192

static uint32_t frame_no     = 0;
static int      program_done = 0;

__attribute__((import_module("env"), import_name("wc_frame_yield")))
extern void wc_frame_yield(void);

extern int asyncify_get_state(void);   /* 0 normal, 1 unwinding, 2 rewinding */
extern int scriptc_main(int argc, char **argv);

/* ---- what the TypeScript calls -------------------------------------- */

uint32_t sg_gl_width(void)  { return CART_WIDTH; }
uint32_t sg_gl_height(void) { return CART_HEIGHT; }

/* Yield, then report the new frame. The counter is bumped AFTER the yield
 * returns, not in wc_render: a rewind lands directly back at this call
 * rather than replaying wc_render's body, so anything counted there runs
 * once and the frame number sticks at 1 forever. */
uint32_t sg_gl_frame(void) {
    /* Fill the ring BEFORE yielding: the host drains it right after
     * wc_render returns, so audio rendered after the yield would arrive a
     * frame late and the first frame would be silent.
     *
     * 800 frames is one 60Hz frame at 48kHz. Producing exactly one frame's
     * worth per frame keeps the ring from either starving or lapping the
     * host's read cursor. */
    sg_cart_audio_pump(800);
    wc_frame_yield();
    frame_no++;
    return frame_no;
}

/* ---- wasmcart ABI ---------------------------------------------------- */

__attribute__((export_name("wc_get_info")))
wc_info_t *wc_get_info(void) {
    info.version         = WC_ABI_VERSION;
    info.width           = CART_WIDTH;
    info.height          = CART_HEIGHT;
    info.fb_ptr          = 0;        /* GL cart: nothing to read back */
    info.audio_ptr       = (uint32_t)(uintptr_t)g_audio_ring;
    info.audio_cap       = AUDIO_CAP;
    info.audio_write_ptr = (uint32_t)(uintptr_t)&g_audio_write;
    info.input_ptr       = (uint32_t)(uintptr_t)pads;
    info.save_ptr        = 0;
    info.save_size       = 0;
    info.time_ptr        = (uint32_t)(uintptr_t)&time_info;
    info.host_info_ptr   = (uint32_t)(uintptr_t)&host_info;
    /* F32: the graph renders float, so handing the host int16 would mean a
     * conversion pass for nothing. */
    info.flags           = WC_FLAG_AUDIO_F32;
    info.audio_sample_rate = 48000;
    info.pointer_ptr     = 0;
    info.keys_ptr        = 0;
    info.gpu_api         = 1;        /* WebGL2 */
    return &info;
}

__attribute__((export_name("wc_init")))
void wc_init(void) { }

__attribute__((export_name("wc_render")))
void wc_render(void) {
    if (program_done) return;

    /* Unconditional entry: the asyncify rewind replays this call to reach
     * the suspended yield inside the TS loop. A first-frame guard would
     * leave the rewind nothing to replay and freeze on frame 1. */
    static char *argv0 = "cart";
    static char *argv[2] = { NULL, NULL };
    argv[0] = argv0;
    scriptc_main(1, argv);

    if (asyncify_get_state() == 0) program_done = 1;
}

/* The asyncify unwind stack. Without this export the host leaves its
 * descriptor at 0 and the suspended stack is written over address 0. 64KB:
 * the renderer's call path is deep: 64KB was not enough and a too-small
 * unwind buffer does not error, it silently fails to resume. */
static struct { uint32_t cur, end; uint8_t stack[262144]; } yield_buf;

__attribute__((export_name("wc_yield_buffer")))
uint32_t wc_yield_buffer(void) {
    yield_buf.cur = (uint32_t)(uintptr_t)yield_buf.stack;
    yield_buf.end = (uint32_t)(uintptr_t)(yield_buf.stack + sizeof(yield_buf.stack));
    return (uint32_t)(uintptr_t)&yield_buf;
}

__attribute__((export_name("sc_frame_count")))
uint32_t sc_frame_count(void) { return frame_no; }
