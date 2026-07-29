/* The tracer's game logic, in TypeScript, compiled by scriptc.
 *
 * THIS IS THE POINT OF THE WHOLE EXPERIMENT. The C shell beside this file is
 * only the wasmcart ABI (wc_get_info / wc_init / wc_render); every pixel it
 * shows is decided here, in statically-compiled TypeScript. If the cart
 * renders what this file says it should, then scriptc-compiled TS is driving
 * a wasmcart cart, which is the thing being proved.
 *
 * WHY THE FRAME LOOP IS DRIVEN FROM C, NOT EXPORTED FROM TS.
 *
 * scriptc compiles a PROGRAM, not a library: a module-level `export function`
 * that nothing calls is dead code and gets stripped, so the IR came out with
 * neither the function nor the FFI symbols it used. There is no
 * "export this for my C to call" in the model.
 *
 * So the direction is inverted. The TS top level is the whole program, and it
 * pulls frames from the host by calling sg_wait_frame(), which blocks in C
 * until the host asks for one. The loop lives here, in TypeScript, which is
 * both what a game author would write and a stronger result than a callback:
 * the compiled TS is in control of its own main loop.
 *
 * What it draws: a gradient that MOVES with the frame counter, plus a solid
 * box that steps across the screen. Both are deliberate:
 *
 *   - A static image cannot distinguish "the cart rendered" from "the cart
 *     ran once and froze". Motion proves the loop is really turning.
 *   - The box has exact, predictable coordinates, so the harness can assert a
 *     specific colour at a specific pixel rather than eyeballing it.
 *
 * The framebuffer is 0xAARRGGBB little-endian, wasmcart's fb format.
 */

/* Implemented in cart_main.c, bound through src/tracer.ffi.json. */
declare function sg_fb_width(): number;
declare function sg_fb_height(): number;
declare function sg_fb_set(index: number, argb: number): void;
/* Blocks until the host calls wc_render, then returns the frame number.
 * Returns 0 when the host is done, which ends the program. */
declare function sg_wait_frame(): number;

const w = sg_fb_width();
const h = sg_fb_height();

const BOX = 64;
const boxY = ((h - BOX) / 2) | 0;

for (;;) {
  const frame = sg_wait_frame();
  if (frame === 0) break;

  /* A moving gradient. Integer math only: it exercises the compiled
   * arithmetic path and keeps expected values exactly predictable. */
  const shift = frame * 3;
  for (let y = 0; y < h; y++) {
    const row = y * w;
    const g = ((y * 255) / h) | 0;
    for (let x = 0; x < w; x++) {
      const r = ((x * 255) / w) | 0;
      const b = (x + shift) & 255;
      sg_fb_set(row + x, 0xff000000 | (r << 16) | (g << 8) | b);
    }
  }

  /* A white box stepping right 2px per frame. Its position is a known
   * function of the frame count, so the harness can check one exact pixel
   * and learn both THAT the TS ran and HOW MANY times. */
  const boxX = (frame * 2) % (w - BOX);
  for (let y = boxY; y < boxY + BOX; y++) {
    const row = y * w;
    for (let x = boxX; x < boxX + BOX; x++) {
      sg_fb_set(row + x, 0xffffffff);
    }
  }
}
