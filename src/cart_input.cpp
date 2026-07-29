/* cart_input.cpp - scriptc-game's sg_input, over the cart's pad state.
 *
 * WHY THIS EXISTS, and why its absence hung the game rather than breaking
 * input: web/input/input.ts drains events with
 *
 *     let kind = ffi.pollEvent();
 *     while (kind !== EV_NONE) { handle(kind); kind = ffi.pollEvent(); }
 *
 * EV_NONE is 0. wasmcart's host auto-stubs unknown `env` imports with
 * `() => -1` (CartHost.js:705), so an unlinked sg_poll_event returns -1
 * forever and that loop never terminates. The cart did not crash or drop
 * input: it spun inside frame 1, which from the outside looks exactly like
 * a hang during load.
 *
 * That is the FOURTH time a missing object file has surfaced as a wrong
 * value rather than a link error (scr_regex.c, libregexp.o, the math shim,
 * now this). The -1 default is the worst of the four, because unlike a
 * garbage number it produces an infinite loop.
 *
 * shim/sg_input.cpp cannot be reused: it is 74 references deep into SDL.
 * This provides the same surface over wc_pad_t, which the host writes
 * before each wc_render.
 */
#include <stdint.h>
#include <string.h>
#include "wasmcart.h"

#define SG_OK        0
#define SG_EV_NONE   0

/* Written by the ABI shell each frame, from the pads the host filled. */
static wc_pad_t g_pads[4];

extern "C" void sg_cart_set_pads(const wc_pad_t* pads) {
    memcpy(g_pads, pads, sizeof(g_pads));
}

/* ---- events ---------------------------------------------------------
 *
 * wasmcart delivers pad state as a SNAPSHOT, not a queue: the host writes
 * the current buttons and axes and the cart samples them. There is nothing
 * to drain there, and gamepads are read separately through the sg_pad_*
 * accessors below (pollGamepads(), which is state-based on every platform).
 *
 * Text is the exception, and the reason this function is not simply a
 * `return SG_EV_NONE`. Typed characters are delivered once, by callback, and
 * are gone if the frame that received them does not report them -- so they
 * DO queue. Everything else still returns NONE, which is what ends
 * input.pump()'s loop. */
/* ---- text input (wasmcart 0.16.0+) ----------------------------------
 *
 * The host composes text for us: layout, shift, dead keys and IME commits
 * are already applied, so a cart never reimplements keyboard layouts. It
 * arrives by CALLBACK (wc_on_text), while scriptc-game reads it by POLLING
 * an event queue, so this bridges the two.
 *
 * Text is off until asked for, and that is load-bearing on the host side:
 * it raises the on-screen keyboard on mobile and suppresses gameplay key
 * bindings while active. So sg_text_input really does begin/end rather
 * than just setting a flag. */
/* wc_text_input_begin/end come from wasmcart.h, already carrying their
 * import attributes. Redeclaring them here is not merely redundant: the
 * header's are C++-linkage, so an extern "C" copy is a hard error. */

/* One frame's worth of typed text. An IME can commit a whole word in one
 * call, so this is a buffer rather than a single codepoint. */
static char     g_text[256];
static uint32_t g_text_len;
/* Pending EV_TEXT events for sg_poll_event to hand back. */
static uint32_t g_text_pending;
/* Monotonic, never decremented: a diagnostic, not a queue depth. */
static uint32_t g_text_calls;

__attribute__((export_name("wc_on_text")))
extern "C" void wc_on_text(const char* utf8, uint32_t len) {
    /* The pointer is valid only for this call, so copy now. Truncate rather
     * than overflow: a 256-byte frame of typing is already implausible. */
    if (len > sizeof(g_text)) len = sizeof(g_text);
    memcpy(g_text, utf8, len);
    g_text_len = len;
    g_text_pending++;
    g_text_calls++;
}

extern "C" int32_t sg_text_input(uint32_t enable) {
    if (enable) wc_text_input_begin(); else wc_text_input_end();
    return SG_OK;
}

/* Host-side delivery count, for a cart that wants to distinguish "no text
 * arrived" from "text arrived and the adapter lost it". Those are the same
 * picture from the game's side. */
extern "C" uint32_t sg_cart_text_calls(void) { return g_text_calls; }

extern "C" uint32_t sg_text_len(int32_t unused)  { (void)unused; return g_text_len; }
extern "C" uint32_t sg_text_byte(uint32_t i) {
    return i < g_text_len ? (uint32_t)(uint8_t)g_text[i] : 0u;
}

#define SG_EV_TEXT 8

extern "C" uint32_t sg_poll_event(int32_t unused) {
    (void)unused;
    /* Text is the one thing that genuinely queues: pad state is a snapshot
     * the accessors read directly, but typed characters would be lost if
     * the frame that received them did not report them. */
    if (g_text_pending > 0) { g_text_pending--; return SG_EV_TEXT; }
    return SG_EV_NONE;
}

extern "C" int32_t sg_evt_i32(uint32_t field) { (void)field; return 0; }

extern "C" int32_t sg_input_init(int32_t unused) { (void)unused; return SG_OK; }
extern "C" void    sg_input_quit(int32_t unused) { (void)unused; }

/* ---- gamepads -------------------------------------------------------
 *
 * sg_pad_button takes an SDL_CONTROLLER_BUTTON index, because that is what
 * the native shim passes to SDL_GameControllerGetButton; web/input/gamepad.ts
 * reorders those into the W3C standard mapping. So the translation here is
 * wasmcart bit -> SDL index, NOT wasmcart bit -> standard index. Getting
 * that wrong swaps A/B and scatters the d-pad, which reads as a device
 * quirk rather than a mapping bug. */
enum {
    SDL_BTN_A = 0, SDL_BTN_B = 1, SDL_BTN_X = 2, SDL_BTN_Y = 3,
    SDL_BTN_BACK = 4, SDL_BTN_GUIDE = 5, SDL_BTN_START = 6,
    SDL_BTN_LEFTSTICK = 7, SDL_BTN_RIGHTSTICK = 8,
    SDL_BTN_LEFTSHOULDER = 9, SDL_BTN_RIGHTSHOULDER = 10,
    SDL_BTN_DPAD_UP = 11, SDL_BTN_DPAD_DOWN = 12,
    SDL_BTN_DPAD_LEFT = 13, SDL_BTN_DPAD_RIGHT = 14,
    SDL_BTN_MAX = 15,
};

static uint16_t mask_for(uint32_t sdl_button) {
    switch (sdl_button) {
    case SDL_BTN_A:              return WC_BTN_A;
    case SDL_BTN_B:              return WC_BTN_B;
    case SDL_BTN_X:              return WC_BTN_X;
    case SDL_BTN_Y:              return WC_BTN_Y;
    case SDL_BTN_LEFTSHOULDER:   return WC_BTN_L;
    case SDL_BTN_RIGHTSHOULDER:  return WC_BTN_R;
    case SDL_BTN_START:          return WC_BTN_START;
    case SDL_BTN_BACK:           return WC_BTN_SELECT;
    case SDL_BTN_DPAD_UP:        return WC_BTN_UP;
    case SDL_BTN_DPAD_DOWN:      return WC_BTN_DOWN;
    case SDL_BTN_DPAD_LEFT:      return WC_BTN_LEFT;
    case SDL_BTN_DPAD_RIGHT:     return WC_BTN_RIGHT;
    case SDL_BTN_LEFTSTICK:      return WC_BTN_L3;
    case SDL_BTN_RIGHTSTICK:     return WC_BTN_R3;
    default:                     return 0;   /* GUIDE: no wasmcart bit */
    }
}

extern "C" uint32_t sg_pad_connected(uint32_t slot) {
    return slot < 4 && g_pads[slot].connected ? 1u : 0u;
}

extern "C" uint32_t sg_pad_button(uint32_t slot, uint32_t button) {
    if (slot >= 4 || button >= SDL_BTN_MAX) return 0;
    uint16_t m = mask_for(button);
    return m && (g_pads[slot].buttons & m) ? 1u : 0u;
}

/* Axes are int16 on the wire and -1..1 to the game. Triggers are uint8 and
 * 0..1: SDL reports them as axes 4 and 5, which is where gamepad.ts looks
 * for L2/R2. */
extern "C" double sg_pad_axis(uint32_t slot, uint32_t axis) {
    if (slot >= 4) return 0.0;
    const wc_pad_t* p = &g_pads[slot];
    switch (axis) {
    case 0: return (double)p->left_x  / 32767.0;
    case 1: return (double)p->left_y  / 32767.0;
    case 2: return (double)p->right_x / 32767.0;
    case 3: return (double)p->right_y / 32767.0;
    case 4: return (double)p->left_trigger  / 255.0;
    case 5: return (double)p->right_trigger / 255.0;
    default: return 0.0;
    }
}

/* The name goes through the string mailbox, which this file does not own.
 * 0 = no name available; gamepad.ts falls back to a generic label. */
extern "C" uint32_t sg_pad_name(uint32_t slot) { (void)slot; return 0; }

/* No rumble in the cart ABI. Reported honestly so a game can hide the
 * setting rather than offering one that does nothing. */
extern "C" uint32_t sg_pad_has_rumble(uint32_t slot) { (void)slot; return 0; }

extern "C" int32_t sg_pad_rumble(uint32_t slot, double low, double high,
                                 uint32_t ms) {
    (void)slot; (void)low; (void)high; (void)ms;
    return SG_OK;   /* silently absent, never an error a game must handle */
}

/* SDL mapping strings and virtual pads have no meaning here: the host
 * already presents a single normalised layout. */
extern "C" int32_t sg_pad_add_mapping(const uint8_t* text, uint32_t len) {
    (void)text; (void)len; return SG_OK;
}
extern "C" int32_t sg_pad_attach_virtual(int32_t naxes, int32_t nbuttons) {
    (void)naxes; (void)nbuttons; return -1;
}
extern "C" int32_t sg_pad_detach_virtual(int32_t device_index) {
    (void)device_index; return -1;
}

/* State is refreshed by the shell before each frame, so an explicit update
 * is a no-op rather than a poll. */
extern "C" int32_t sg_pad_update(int32_t unused) { (void)unused; return SG_OK; }
