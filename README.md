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
| Canvas 2D | **blocked upstream**, see below |
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

## Canvas 2D: built, and blocked on one compiler detail

`build-canvas.sh` builds it and it links (7MB with Skia). Everything on the
scriptc-game side ports cleanly:

- `shim/sg_skia_gen.cpp` and `shim/sg_skia_extra.cpp` compile under emcc
  **unmodified** against wasmcart-skia's `libskiac.a`.
- `src/cart_core.cpp` replaces only `shim/sg_core.cpp` (the SDL window,
  renderer and streaming texture) with a Skia surface copied into the cart
  framebuffer. That is ~130 lines.
- `src/canvas_tracer.ts` imports scriptc-game's real `web/canvas/context.js`.
  Not a reimplementation: the same file a native build uses.

**It traps at runtime with "memory access out of bounds", and the cause is in
scriptc, not here.** Its LLVM IR hardcodes 64-bit `size_t`:

```
declare ptr @calloc(i64, i64)
declare ptr @scr_arr_new(i32, i64)
declare i32 @sg_text_set_font(ptr, i64, double, i32, i32)
```

wasm32 wants `i32` for all of those. wasm-ld reports each as a "function
signature mismatch" warning and links anyway; the calls then read out of
bounds. Setting `SCRIPTC_TARGET` to a 32-bit triple (`x86-linux-gnu`,
`wasm32-wasi`) does not change the widths, so this is not a flag away.

The framebuffer tracer is unaffected because it makes **zero** allocator
calls and passes no spans: `grep -c 'call.*@calloc\|call.*@scr_arr_new'` on
its IR is 0. Anything that allocates or takes a string crosses the boundary.

The fix belongs in scriptc: lower `size_t`, the FFI `string`/`bytes` length,
and the allocator signatures to pointer width rather than always 64-bit.
Written up with exact sites and a suggested shape in
[SCRIPTC-32BIT.md](SCRIPTC-32BIT.md), for the monteslu fork. Not filed
upstream.

Two changes landed in scriptc-game while getting here, both green on the
native suite and worth having regardless: FFI span lengths are `uint32_t`
instead of `size_t` (identical natively, correct on wasm32), and
`SG_EXTRA_SHIM` lets `gen-ffi` parse C signatures from a consumer's own shim
sources.

## Where this goes next

The interesting finding from the survey that preceded this: **`wasmcart-skia`
already ships a WASM Skia whose `skiac_*` C surface is byte-for-byte identical
to the native one scriptc-game's Canvas 2D binds to** (206 symbols vs 207,
empty diff). And `wasmcart-jsgame` builds webaudio-node to wasm from the same
upstream. So the two dependencies that looked like blockers for porting
scriptc-game's `web/` tier are already solved.

The order that makes sense from here:

1. The `size_t` width fix in scriptc. Canvas 2D is otherwise done and
   waiting on it, and every later tier hits the same wall the moment it
   allocates.
2. WebGL2, mapping `web/webgl/context.ts` onto the cart's `gl` imports the
   way `wasmcart-jsgame/src/webgl_shim.c` does.
3. threeTS-lite on top, which should need no changes if 2 is faithful.

Worth stating plainly: `wasmcart-jsgame` already runs real three.js as a cart
through QuickJS. This lane's argument is not "3D in a cart is possible": it
is AOT compilation instead of interpretation, and a 21KB baseline instead of a
5MB one.

## License

MIT.
