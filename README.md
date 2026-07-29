# wasmcart-scriptc

**Statically compiled TypeScript as a wasmcart cartridge.**

A tracer proving the pipeline closes: TypeScript → [scriptc](https://github.com/vercel-labs/scriptc)
→ LLVM IR → wasm → a `.wasc` cart that runs on any wasmcart host. No
interpreter in the binary, no JIT, no QuickJS. The cart is 21KB.

This is a **feasibility spike, not a framework yet**. It draws a moving
gradient and a stepping box; there is no Canvas 2D, no WebGL, no audio. What
it establishes is that the toolchain works end to end, which was the open
question.

## Status

| | |
| --- | --- |
| scriptc IR → wasm | works |
| scriptc C runtime under Emscripten | 50 of 54 files |
| TS-owned game loop in a cart | works, via wasmcart's yield protocol |
| Pixels driven by compiled TS | verified to the pixel, with a control |
| Canvas 2D | **works**: gradients, paths, curves, alpha, transforms |
| Text (FreeType/HarfBuzz/ICU) | **works**, including `measureText` shaping |
| WebGL / audio | not started |

```sh
./build.sh     # -> build/cart.wasm
./verify.sh    # renders, advances, and lands where the TS says
```

Needs `emcc` on PATH, a `scriptc` checkout beside this one, and `zig` (for
scriptc's own native step, which produces the IR).

## How it works

The cart is two pieces, and the split is what makes the result meaningful:

- **`src/tracer.ts`** decides every pixel. Compiled by scriptc.
- **`src/cart_main.c`** is the wasmcart ABI shell and nothing else: it owns
  the framebuffer, exports `wc_get_info`/`wc_init`/`wc_render`, and hands the
  buffer to the TypeScript.

If C drew the picture, a passing run would prove only that emcc works.

### The loop is inverted, and the TS owns it

scriptc compiles a **program**, not a library. A module-level
`export function` that nothing calls is dead code and gets stripped, and FFI
format 1 has no callback class, so C cannot pass a function pointer back into
TypeScript either. There is no way to have the host call a TS render function.

So the TypeScript keeps its own `for(;;)` loop and *pulls* frames:

```ts
for (;;) {
  const frame = sg_wait_frame();   // suspends until the host wants a frame
  if (frame === 0) break;
  // ... draw ...
}
```

`sg_wait_frame` calls wasmcart's `wc_frame_yield`, and the host (which
post-processes the wasm with binaryen's asyncify pass) unwinds the whole stack
out of `wc_render`, then rewinds back to that exact point next frame. This is
a protocol wasmcart already provides for ported engines that own their main
loop; stratagus uses the same one.

That is arguably the better outcome anyway: a game author writes a normal main
loop, and the compiled TypeScript drives itself.

## Four traps, recorded because each cost real time

**1. Emscripten, not WASI.** Under `wasm32-wasi` scriptc's C runtime fails
with ~45 errors (`kill`, `pipe`, `nanosleep`, `SIGTERM`) because it assumes
POSIX. Emscripten shims those, and 50 of 54 runtime files then compile with a
single 6-line `arc4random_buf` shim over `getentropy`. The four that still
fail are `http`, `tls`, `regex` (wants quickjs's libregexp) and the dynamic
inspector: none is reachable from a cart.

**2. `-sASYNCIFY` deadlocks.** Emscripten's own asyncify resumes via the JS
event loop, but wasmcart calls `wc_render` synchronously and waits for the
return, so the cart hangs on frame 1. The binaryen pass scoped to
`asyncify-imports@env.wc_frame_yield` is the mechanism this host implements.

**3. Missing `wc_yield_buffer` traps as "unreachable".** The host asks the
cart for its unwind-stack descriptor. Without that export the host leaves its
pointer at 0, asyncify writes the suspended stack over address 0, and the
cart traps in a way that looks like a compiler bug.

**4. A rewind does NOT replay `wc_render`.** It restores the saved stack and
lands directly back at the yield call, so anything `wc_render` does above the
yield runs exactly once, on the first frame. Counting frames there left the
counter pinned at 1 while the host cheerfully reported 10 frames rendered, and
the screen froze on frame 1 with no error at all. The counter has to be bumped
on the *resume* side, in `sg_wait_frame`.

That last one is why `verify.sh` checks the box **moves** rather than that it
merely drew: a frozen cart and a working one produce identical screenshots.

## Verification

`verify.sh` runs three checks and each catches something the others miss: that
the cart renders, that the TS frame counter advances, and that the box lands
at exactly `(frame * 2) % (width - 64)`, the formula in the TypeScript.

It comes with a control that must fail. Changing the step from 2 to 5 in
`tracer.ts` and rebuilding moves the box to x=195 instead of 78, and
`verify.sh` reports the failure. Without that, "the pixels match" cannot be
distinguished from a harness reading a stale file.

## Canvas 2D

Working. `./build-canvas.sh` produces a 2.4MB `.wasc` that renders a gradient
background, twelve alpha-blended circles, a rotated square and a stroked
bezier, all through scriptc-game's real `web/canvas/context.js`.

Colours are byte-exact against what the TypeScript asked for: `#0b1020` reads
back `(11,16,33)`, `#1d3b6e` reads `(29,59,110)`, `#ffd166` reads
`(255,209,102)`. There is a control for that: changing the square to
`#22cc44` in the TS moves the sampled pixel to exactly `(34,204,68)`.

What ports unchanged: `shim/sg_skia_gen.cpp` and `shim/sg_skia_extra.cpp`
compile under emcc with no edits against wasmcart-skia's `libskiac.a`. The
only new C is `src/cart_core.cpp` (~130 lines), replacing the SDL window half
of `shim/sg_core.cpp` with a Skia surface copied into the cart framebuffer.

### Text

Works, and it is the part that proves the most: `fillText` pulls in FreeType,
HarfBuzz and ICU, all of which had to survive the wasm build.

Fonts come from the host's asset API, not a path. scriptc-game registers by
filename (`sg_font_register` -> skiac's `register_from_path`), which a cart
cannot do: there is no filesystem, and the WASM Skia is deliberately patched
not to scan `/fonts/`. `cart_core.cpp` adds `sg_cart_font_from_asset`, which
pulls the bytes through `wc_load_asset` and hands them to
`skiac_font_collection_register`. Same collection underneath, so
`sg_text_set_font` and `sg_text_draw` work unchanged.

`measureText` is checked too, because a stack that rasterises but cannot
shape would pass a glyphs-drawn test. The tracer underlines the title to the
measured width: the underline spans x 24-233 and the glyph ink 25-231, so
measurement tracks the rendering within antialiasing tolerance.

Two controls, because "no text" and "no font" look identical on screen:
asking for a nonexistent font asset drops the title row from 41 white pixels
to 0 AND paints the red failure bar, and a zero measured width paints it too.

The failure mode this guards against is quiet: with an empty font collection
Skia has no typeface to shape with and text draws nothing at all, with no
error anywhere.

### Getting there took four bugs, three of them mine

**1. scriptc assumed a 64-bit target.** Every `size_t` was emitted as `i64`,
and so were the runtime struct layouts and the byte offsets derived from
them. Fixed on the fork in `f3c0e59` and `55e0ca0`; see
[SCRIPTC-32BIT.md](SCRIPTC-32BIT.md).

**2. A missing object file, mistaken for a layout bug.** `scr_regex.c` was
excluded from the build as "needs libregexp, cart-irrelevant". Both halves
were wrong: it compiles fine with quickjs-ng on the include path, and
`scr_str_to_lower`/`to_upper` live in it, so any cart calling
`.toLowerCase()` got an undefined symbol. `-sERROR_ON_UNDEFINED_SYMBOLS=0`
turned that into a trapping stub rather than a link error, which sent me
hunting through struct layouts for what was a one-line build omission.

**3. The same omission again, one layer down.** After adding `scr_regex.o`,
the bisect harness still failed: it linked `scr_*.o` by glob, which does not
match `libregexp.o` or `libunicode.o`. Same silent-stub failure mode, and
this time it presented as a *wrong answer* (`"A".toLowerCase().length === 0`)
rather than a crash.

**4. Red and blue swapped.** The surface is RGBA-premultiplied, so Skia's
bytes are R,G,B,A; wasmcart reads 0xAARRGGBB little-endian, whose bytes are
B,G,R,A. `cart_core.cpp` claimed the two orders matched and memcpy'd. The
geometry was perfect and every colour was wrong, which reads as a gradient or
shader bug rather than a byte-order one.

The common thread in 2 and 3 is `-sERROR_ON_UNDEFINED_SYMBOLS=0`. It is
needed (the cart genuinely does not provide every symbol the runtime
references), but it converts "you forgot to link something" into a runtime
trap or, worse, a silently wrong value.

## Where this goes next

The interesting finding from the survey that preceded this: **`wasmcart-skia`
already ships a WASM Skia whose `skiac_*` C surface is byte-for-byte identical
to the native one scriptc-game's Canvas 2D binds to** (206 symbols vs 207,
empty diff). And `wasmcart-jsgame` builds webaudio-node to wasm from the same
upstream. So the two dependencies that looked like blockers for porting
scriptc-game's `web/` tier are already solved.

The order that makes sense from here:

1. Text, which needs the font stack verified end to end.
2. WebGL2, mapping `web/webgl/context.ts` onto the cart's `gl` imports the
   way `wasmcart-jsgame/src/webgl_shim.c` does.
3. threeTS-lite on top, which should need no changes if 2 is faithful.

Worth stating plainly: `wasmcart-jsgame` already runs real three.js as a cart
through QuickJS. This lane's argument is not "3D in a cart is possible": it
is AOT compilation instead of interpretation, and a 21KB baseline instead of a
5MB one.

## License

MIT.
