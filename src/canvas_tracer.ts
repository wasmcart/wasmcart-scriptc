/* Canvas 2D in a wasmcart cart, drawn with scriptc-game's real web/ tier.
 *
 * The imports below are the POINT. `Context2D` here is the same file a
 * native scriptc-game builds against, unmodified: it talks to a generated
 * skiac shim which links against wasmcart-skia's libskiac.a instead of the
 * native one. Nothing in web/canvas/ knows the difference.
 *
 * So this is not a reimplementation of Canvas for carts. It is the existing
 * implementation, relinked.
 *
 * What it draws exercises the parts of Skia a game actually leans on, so
 * that a passing frame means more than "fillRect works": paths and curves,
 * a gradient, transforms, alpha compositing, line joins, and text (which
 * pulls in the whole font stack: FreeType, HarfBuzz and ICU, all of which
 * have to have survived the wasm build).
 */
import { Context2D } from "../../scriptc-game/web/canvas/context.js";
import { Math as M } from "../../scriptc-game/web/globals.js";
import * as ffi from "../../scriptc-game/host/ffi.js";

/* Copy the finished Skia frame into the cart framebuffer, then suspend
 * until the host wants the next one. Implemented in cart_main.c. */
declare function sg_present_frame(): number;

const W = ffi.screenWidth();
const H = ffi.screenHeight();
const ctx = new Context2D(ffi.screenCanvas(), 0);

for (;;) {
  const frame = sg_present_frame();
  if (frame === 0) break;

  const t = frame * 0.05;

  /* Background: a vertical gradient, so a broken shader shows up as flat
   * colour rather than as nothing at all. */
  const bg = ctx.createLinearGradient(0, 0, 0, H);
  bg.addColorStop(0, "#0b1020");
  bg.addColorStop(1, "#1d3b6e");
  /* setFillGradient, not `fillStyle = gradient`: the dialect has no
   * overloads, so fillStyle takes a CSS string only. Same operation, one
   * name per type. */
  ctx.setFillGradient(bg);
  ctx.fillRect(0, 0, W, H);

  /* An orbiting ring of translucent circles: tests arcs, alpha blending and
   * per-frame transform state. */
  for (let i = 0; i < 12; i++) {
    const a = t + (i * M.PI * 2) / 12;
    const cx = W / 2 + M.cos(a) * 120;
    const cy = H / 2 + M.sin(a) * 80;
    ctx.beginPath();
    ctx.arc(cx, cy, 26, 0, M.PI * 2, false);
    ctx.fillStyle = i % 2 === 0 ? "rgba(120, 220, 255, 0.55)"
                                : "rgba(255, 160, 90, 0.55)";
    ctx.fill("nonzero");
  }

  /* A stroked bezier: curves, line width and joins. */
  ctx.beginPath();
  ctx.moveTo(40, H - 60);
  ctx.bezierCurveTo(W * 0.3, H - 60 - M.sin(t) * 90,
                    W * 0.7, H - 60 + M.cos(t) * 90,
                    W - 40, H - 60);
  ctx.strokeStyle = "#7cf7c4";
  ctx.lineWidth = 6;
  ctx.lineCap = "round";
  ctx.stroke();

  /* A rotated square about its own centre: save/restore and transforms. */
  ctx.save();
  ctx.translate(W / 2, H / 2);
  ctx.rotate(t);
  ctx.fillStyle = "#ffd166";
  ctx.fillRect(-34, -34, 68, 68);
  ctx.restore();

  /* Text last, so it sits on top. This is the check that matters most: it
   * proves the font stack (FreeType + HarfBuzz + ICU) survived the wasm
   * build, which is the part most likely to be quietly missing. */
  ctx.fillStyle = "#ffffff";
  ctx.font = "24px sans-serif";
  ctx.fillText("scriptc canvas 2d", 24, 40);
}
