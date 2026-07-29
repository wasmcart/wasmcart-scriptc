/* Text input in a cart, through scriptc-game's real Input class.
 *
 * wasmcart 0.16.0 added a text ABI, and it is the first input path where the
 * host and scriptc-game disagree about SHAPE. Pads are a snapshot on both
 * sides, so bridging them is a matter of reading the right struct field.
 * Text is a CALLBACK on the host side (wc_on_text) and a POLLED EVENT QUEUE
 * on the game side (pollEvent -> EV_TEXT -> textEvents()). cart_input.cpp is
 * the adapter, and this cart is what proves the adapter works.
 *
 * The game-facing code here is the unmodified web tier: Input.textInput(true)
 * to ask for text, Input.pump() to drain, Input.textEvents() to read. A
 * browser build of this same source would be handed characters by the DOM;
 * here they come from the host. Neither is visible from this file, which is
 * the whole claim.
 *
 * What it draws is a text field: a caret, the accumulated string, and a
 * per-frame readout of what arrived. Backspace is deliberately handled as a
 * KEY rather than a character, matching the ABI note in wasmcart.h -- editing
 * keys stay on the keyboard path, only characters come through wc_on_text.
 */
import { Context2D } from "../../scriptc-game/web/canvas/context.js";
import { Math as M } from "../../scriptc-game/web/globals.js";
import { Input } from "../../scriptc-game/web/input/input.js";
import * as ffi from "../../scriptc-game/host/ffi.js";

declare function sg_present_frame(): number;
declare function sg_cart_font_from_asset(path: string, alias: string): number;

/* How many wc_on_text calls the shell has seen. Read straight from
 * cart_input.cpp rather than inferred from the string, so a frame that
 * received text but failed to surface it as an event is distinguishable
 * from a frame that received nothing. Those two look identical from the
 * game's side, and only one of them is a bug in the adapter. */
declare function sg_cart_text_calls(): number;

const fontRc = sg_cart_font_from_asset("DejaVuSans.ttf", "sans-serif");

const W = ffi.screenWidth();
const H = ffi.screenHeight();
const ctx = new Context2D(ffi.screenCanvas(), 0);

const input = new Input();

/* Text is OFF until asked for. On a phone this is what raises the on-screen
 * keyboard, and while it is active the host stops routing keys to gameplay
 * bindings -- so this call is not a formality, it changes host behaviour. */
input.textInput(true);

let typed = "";
let lastChunk = "";
let chunkFrame = 0;

for (;;) {
  const frame = sg_present_frame();
  if (frame === 0) break;

  input.pump();

  /* textEvents() is per-frame and cleared by the next pump, so it is read
   * here and accumulated. Each entry is one wc_on_text delivery, which may
   * be several characters: an IME commits a whole word at once. */
  const events = input.textEvents();
  for (let i = 0; i < events.length; i++) {
    typed += events[i];
    lastChunk = events[i];
    chunkFrame = frame;
  }

  ctx.fillStyle = "#10131c";
  ctx.fillRect(0, 0, W, H);

  ctx.fillStyle = "#8fa3c8";
  ctx.font = "16px sans-serif";
  ctx.fillText("type something", 24, 34);

  /* The field. Drawn as a box plus its contents so an empty string still
   * shows a caret, rather than an empty frame that could equally mean the
   * cart stopped drawing. */
  ctx.fillStyle = "#1c2231";
  ctx.fillRect(24, 52, W - 48, 44);

  ctx.fillStyle = "#ffffff";
  ctx.font = "22px sans-serif";
  ctx.fillText(typed, 36, 82);

  /* Caret at the end of the measured text: blinking, because a static bar
   * cannot be told apart from a frozen frame. */
  const tw = ctx.measureText(typed).width;
  if (M.floor(frame / 20) % 2 === 0) {
    ctx.fillStyle = "#7cf7c4";
    ctx.fillRect(36 + tw + 2, 60, 2, 28);
  }

  /* Diagnostics. `calls` is the host-side count and `len` the game-side
   * total: they move together when the adapter is correct, and diverge
   * exactly when it is not. */
  const calls = sg_cart_text_calls();
  ctx.fillStyle = "#8fa3c8";
  ctx.font = "14px sans-serif";
  ctx.fillText("wc_on_text calls: " + calls.toString(), 24, H - 44);
  ctx.fillText("chars accumulated: " + typed.length.toString(), 24, H - 26);
  ctx.fillText("last chunk @f" + chunkFrame.toString() + ": \"" + lastChunk + "\"",
               24, H - 8);

  if (fontRc !== 0) {
    ctx.fillStyle = "#ff0000";
    ctx.fillRect(0, 0, W, 8);
  }
}
