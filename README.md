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
| WebGL2 + threeTS-lite | **works**: lit, depth-tested 3D in a 456KB cart |
| Web Audio | **works**: graph, mixing, params, into the cart ring |

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

## WebGL2 and threeTS-lite

Working. `./build-gl.sh` produces a **456KB** `.wasc` (no Skia linked,
against the canvas cart's 6.8MB) that renders a lit, rotating, depth-tested
cube through scriptc-game's real `WebGLRenderer`, `Scene` and `Mesh`. The 3D
tier is unmodified.

Colours are checked, not eyeballed: with a control that changes only the
material colour in the TypeScript, the sampled cube face moves from
`(168,95,55)` to `(45,178,110)`.

### The bridge

scriptc lowers `host/gl-ffi.ts` to plain external symbols, which emcc emits
as imports from module `env`; wasmcart supplies GL under the module name
`gl`. `src/gl_bridge.c`, generated from `wasmcart.h`, defines each `glXxx`
locally and forwards to a same-named `import_module("gl")` import.

One detail worth keeping: the C++ GL shim must NOT force-include
`wasmcart.h`. It includes `GLES3/gl3.h` itself, whose declarations have C++
linkage and collide. The bridge alone does the redirect.

### Two bugs, both silent

**The math FFI was stubbed.** `sg_sqrt`, `sg_tan`, `sg_cos` live in
scriptc-game's `shim/sg_skia_extra.cpp` next to the Skia helpers. A GL cart
links no Skia, so excluding that file dropped the math with it, and
`-sERROR_ON_UNDEFINED_SYMBOLS=0` turned every one into a host stub.
`Math.tan(x)`, `Math.sqrt(x)` and `Math.cos(x)` all returned the SAME
constant regardless of argument, so every camera matrix was garbage.
`src/gl_support.c` supplies them.

That was the third instance of that failure mode (`scr_regex.c`, then
`libregexp.o`, then the math shim). The flag is necessary, but it converts
"you forgot to link something" into a wrong value rather than a link error.
**Start any new cart by diffing its undefined `env` imports against what it
means to provide.**

**`instanceof` compared garbage.** `ScrVt.pre`/`post` are `size_t`, and
`instanceof` is a preorder-interval test against them. The backend loaded
them as `i64` regardless of target, so on wasm32 `post` landed in the high
half of `pre`. It does not crash: it answers FALSE. `Scene.add()` dispatches
on `instanceof` to pick a typed registry, so every mesh fell through to the
plain-Object3D branch, was parented into `children`, registered in no draw
list, and the renderer cleared correctly and drew nothing. Fixed in
`d494e92` on the fork.

## Audio

Working. The same cart that draws the cube also plays a two-oscillator
chord through scriptc-game's real `web/audio/context.js`, in 586KB total.

Verified by spectrum, not by "sound came out". A Goertzel filter over the
drained samples finds power 29.0 at 220Hz and 29.1 at 330Hz, against 0.01
at 275Hz and 440Hz -- a ~3000x separation, and exactly the chord the
TypeScript asked for. Two voices rather than one because a single
oscillator cannot tell mixing from pass-through.

With a control: changing only the two frequencies in the TS moves the peaks
to 440/550 and drops the originals to 0.01.

### What changed, and what did not

The graph engine is webaudio-node's, unchanged -- the same
`createAudioGraph` / `createNode` / `processGraph` C ABI the native shim
drives, from the wasm archive `wasmcart-jsgame` already builds.
`web/audio/context.ts` is untouched.

`src/cart_audio.cpp` replaces `shim/sg_audio.cpp`, and is SMALLER than it
for two reasons:

- **No SDL device or audio thread.** A cart renders inline from
  `wc_render` into the ring buffer the host drains after each frame.
- **No command ring.** The native shim marshals every graph mutation
  through a lock-free queue because SDL calls its callback on another
  thread. In a cart the game and the renderer are the same thread, so calls
  apply directly, and the whole `CMD_*` apply() switch disappears.

Two details that would fail quietly if wrong. The ring cursor counts
FRAMES, not samples or bytes -- publishing the wrong unit plays back at
half or double speed, which sounds like resampling rather than indexing.
And the pump runs BEFORE the yield, because the host drains immediately
after `wc_render` returns; filling afterwards makes every frame's audio
arrive one frame late and the first frame silent.

Offline rendering (`sg_audio_render_offline`) writes a file and is reported
as unavailable rather than stubbed silently.

## A real game: dodge

`./build-game.sh` builds `examples/dodge` -- the reference game, with
sprites, mp3 music, SFX, text and gamepad input -- as a 6.9MB cart. **It
plays.** The score advances, coins spawn and spin, the ship responds to pad
input, and the HUD, grid and DejaVu text all render at 800x600.

Not fully: **music and SFX are silent**, and that is a known cause rather
than an open question. See below.

The game source is unmodified. What the cart supplies is the host side:

- **`cart_assets.ts`** -- an `AssetStore` (scriptc-game `a9045e8`) over the
  host asset API, so `fetch("music.mp3")` and `img.src = "player.png"`
  resolve out of the `.wasc`. Reading is size-then-fill; FFI format 1 has
  no bytes-RETURN class.
- **`cart_input.cpp`** -- `sg_input` over `wc_pad_t`.
- **`sg_font_register`** redirected to the asset store, guarded upstream by
  `SG_NO_FONT_PATH` (scriptc-game `2634257`) so the native shim is
  untouched.
- **The rest of `sg_core`** -- clock, delay, display_hz, fullscreen and
  PNG-save stubs.

### Four bugs, and a pattern worth stating

**1. The hang was an infinite loop, not a deadlock.** `input.pump()` drains
events with `while (kind !== EV_NONE)`. `EV_NONE` is 0, and wasmcart's host
auto-stubs unknown `env` imports with **`() => -1`** (CartHost.js:705), so
an unlinked `sg_poll_event` returned -1 forever. The cart spun inside frame
1, which from outside looks exactly like a hang during load.

That is the **fourth** time a missing object file has surfaced as a wrong
value rather than a link error, and the worst of the four: the others gave
garbage numbers, this one gave an infinite loop.

**2. `await` cannot cross an asyncify yield.** `run()` is async and ends
each frame with `await`; a scriptc async task suspends via `swapcontext`,
which does not exist under wasm and is host-stubbed. `runFrameSync`
(scriptc-game `a9045e8`) is the same body without it.

**3. A rewind does not replay `wc_render`.** Setting the clock and pad
state there ran once, on frame 1, so `performance.now()` never advanced,
every dt was 0, and the game drew one frame and sat perfectly still. Both
now refresh from `sg_present`'s resume side.

**4. The framebuffer must match the game's resolution.** Rows are copied
verbatim, so an 800-wide game in a 640-wide buffer wraps every row: it
rendered as four squeezed copies side by side rather than scaling or
cropping.

### Why audio is silent

`dodge` starts music with `fetch(...).then(...).then(decodeAudioData)`.
Those continuations are **microtasks**, and scriptc's microtask queue is
part of the same `scr_async.c` fiber machinery that needs `swapcontext`.
`__drainTasks()` runs queued host tasks synchronously, but a `.then` chain
still needs a turn to end.

The audio tier itself works -- the standalone audio cart renders a verified
chord -- so this is the promise plumbing, not the graph. Fixing it means
either a synchronous microtask drain in the runtime or `swapcontext` for
wasm.

## Where this goes next

The interesting finding from the survey that preceded this: **`wasmcart-skia`
already ships a WASM Skia whose `skiac_*` C surface is byte-for-byte identical
to the native one scriptc-game's Canvas 2D binds to** (206 symbols vs 207,
empty diff). And `wasmcart-jsgame` builds webaudio-node to wasm from the same
upstream. So the two dependencies that looked like blockers for porting
scriptc-game's `web/` tier are already solved.

The order that makes sense from here:

1. Microtasks without swapcontext, which is what silences dodge's music
   and would block any promise-based asset load.
2. Keyboard and pointer input: the pad path works, the rest is unwired.

Worth stating plainly: `wasmcart-jsgame` already runs real three.js as a cart
through QuickJS. This lane's argument is not "3D in a cart is possible": it
is AOT compilation instead of interpretation, and a 21KB baseline instead of a
5MB one.

## License

MIT.
