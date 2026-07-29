# scriptc: the LLVM backend assumes a 64-bit target

**For monteslu/scriptc (`game-integration`). Not filed upstream.**

Version tested: scriptc 0.0.17, `game-integration` at `fdaca6f`.

> **Status.** Part 1 (size_t widths) is FIXED, in `f3c0e59` on the fork.
> Part 2 (struct layouts and byte offsets), found while testing that fix,
> is NOT. The cart still traps; see "What is still broken" at the bottom.
> The original version of this document described only part 1 and was
> therefore incomplete about what a 32-bit target needs.

## The claim

The LLVM backend emits `i64` for every value that in C is `size_t` or
pointer-width. On a 64-bit target that is correct and invisible. On wasm32 it
is wrong: `size_t` is 32-bit there, so every one of those calls passes a
64-bit value into a function expecting 32.

`wasm-ld` reports each as a **warning** and links anyway. The result runs
until the first such call and then dies with `memory access out of bounds`.

This is the only thing blocking a scriptc-compiled Canvas 2D cart. Everything
else in that port works: see [README.md](README.md).

## Reproducing

Any program that allocates. The three declarations below come straight out of
`--emit-ir` for a program whose only interesting act is drawing through
scriptc-game's canvas tier:

```llvm
declare ptr @calloc(i64, i64)
declare ptr @scr_arr_new(i32, i64)
declare i32 @sg_text_set_font(ptr, i64, double, i32, i32)
```

Compile that IR with `emcc -c` and link, and wasm-ld says:

```
wasm-ld: warning: function signature mismatch: calloc
>>> defined as (i64, i64) -> i32 in build/obj-canvas/tracer.o
>>> defined as (i32, i32) -> i32 in .../libdlmalloc.a(dlmalloc.o)
```

The corresponding C really is `size_t`:

```c
ScrArr *scr_arr_new(ScrElemKind elem, size_t initial_cap);   /* scr_array.c:86 */
```

**`SCRIPTC_TARGET` does not affect this.** Tried `x86-linux-gnu` and
`wasm32-wasi`; the emitted widths are identical to `x86_64-linux-gnu`. That
is consistent with the backend having no notion of a target at all: there is
no `pointerWidth`, no `wordSize`, and the emitted `.ll` carries neither a
`target triple` nor a `target datalayout` line.

A useful negative control: a scriptc program that makes **zero** allocator
calls and passes no strings runs correctly as a wasm cart today. `grep -c
'call.*@calloc\|call.*@scr_arr_new'` on its IR returns 0. The moment a
program allocates or passes a string, it breaks.

## Where it is

Three groups, in `packages/compiler/src/backend/llvm/`.

**1. FFI `string` / `bytes` params: `emitter.ts:5026` and `:5039`.**

```ts
nativeParamTypes.push("ptr", "i64");
nativeArgs.push(`ptr ${data}`, `i64 ${len}`);
```

One TS argument becomes the C pair `(const uint8_t*, size_t)`. The length is
hardcoded `i64` in both the `declare` and the call.

Note this needs a real truncation, not a relabel: the length is loaded from
the `%ScrStr` / bytes header, which is genuinely a 64-bit field. On a 32-bit
target the loaded `i64` has to be `trunc`ed to `i32` before the call.

**2. Allocator declarations, several sites.**

```
classes.ts:482   declare ptr @calloc(i64, i64)
shapes.ts:776    declare ptr @calloc(i64, i64)
shapes.ts:516    declare ptr @scr_arr_new(i32, i64)
emitter.ts:1882  declare ptr @malloc(i64)
emitter.ts:2004  declare ptr @malloc(i64)
                 declare i64 @strlen(ptr)
                 declare ptr @scr_arr_new_ref(ptr, ptr, ptr, i64)
```

**3. The wider `scr_*` runtime surface.** 30 distinct `declare` lines
mentioning `i64` name `scr_*` functions. Each needs checking against its C
signature: an `i64` that corresponds to a real `int64_t` or a JS number is
fine and must stay, and only the `size_t`/pointer-width ones are wrong. This
is the part that needs judgment rather than a sweep.

For scale: 591 `i64` literals appear across `backend/llvm/`. The **vast
majority are correct**: JS numbers are doubles, array indices and string
offsets are internal, and none of that changes. Only values crossing into C
where the C says `size_t` are affected. Do not mass-replace.

## Part 1: size_t widths (FIXED in f3c0e59)

## Suggested shape of a fix

1. Give the backend a target notion. Minimally a `ptrBits: 32 | 64` on the
   emitter, derived from `SCRIPTC_TARGET`, defaulting to 64 so nothing
   changes for current targets.
2. Add a helper (`sizeTy()` returning `"i64"` or `"i32"`) and use it at the
   sites above instead of the literal.
3. At the FFI span sites, `trunc i64 %len to i32` when `ptrBits === 32`.
4. Emit a `target datalayout` / `target triple` line when the target is
   known. That alone would have turned this from a runtime out-of-bounds
   into an immediate, obvious error, because LLVM would have complained rather than
   silently producing a mismatched module.

Point 4 is arguably the highest-value part independent of wasm: a backend
that emits untargeted IR will produce this class of bug again for any future
non-64-bit target.

## What was already fixed downstream

Two changes landed in scriptc-game while diagnosing this, both green on its
full native suite. Neither fixes the above; they were prerequisites.

- FFI span lengths in the C shim declared `uint32_t` instead of `size_t`.
  Identical on every native target (where `size_t` is 64-bit anyway) and
  correct for wasm32. **This does not help on its own**: the width is chosen
  by the compiler's lowering, not the C declaration, which is what made the
  root cause clear.
- `SG_EXTRA_SHIM`, so `gen-ffi` can parse C signatures from a consumer's own
  shim sources rather than only scriptc-game's.

## Why it matters here

The Canvas 2D cart is otherwise complete. `sg_skia_gen.cpp` and
`sg_skia_extra.cpp` compile under emcc unmodified against wasmcart-skia's
`libskiac.a`; the only new code is ~130 lines replacing the SDL window half
of `sg_core.cpp`; and the TypeScript imports scriptc-game's real
`web/canvas/context.js`. It links at 7MB and traps on the first allocation.

Every later tier hits the same wall the moment it allocates, so this is the
gating item for the whole wasmcart lane rather than a Canvas-specific
problem.


## What is still broken (part 2, found while testing part 1)

Fixing every `size_t` width was necessary and not sufficient. With all
signature mismatches gone, the Canvas cart still traps, now inside
`scr_map_set_str_f64` during module init:

```
scr_map_set_str_f64
sc_f__x25_init_2
sc_f__x25_init_13
sc_f__x25_init_27
scriptc_main
wc_render
```

That function's own signature has no `size_t`. The fault is one level down:
**the runtime STRUCT layouts are hardcoded 64-bit too.**

```ts
// backend/llvm/emitter.ts:1286
`%ScrStr = type { i64, i64, i64 }`,
`%ScrVt = type { i64, i64, ptr }`,
`%ScrUnion = type { i64, i32, ptr, ptr, ptr, i64 }`,
`%ScrClosure = type { i64, ptr, i64, ptr }`,
`%ScrArr = type { i64, i64, i64, i32, ptr, ptr, ptr, ptr }`,
`%ScrError = type { i64, ptr, ptr, ptr, ptr }`,
```

The C is:

```c
typedef struct ScrStr {
  size_t rc;
  size_t len;
  size_t cap;
  char data[];
} ScrStr;
```

On wasm32 that is `{i32, i32, i32}` with `data` at offset 12, but the
backend emits `{i64, i64, i64}` and reads `data` at 24. Every string, array,
closure and union field access is at the wrong offset. `scr_map_hash_str`
reads `k->data` and `k->len`, gets garbage, and walks off the heap.

The hardcoded byte offsets that accompany those layouts have the same
problem. Counting just one idiom in `backend/llvm/`:

```
9 x  getelementptr inbounds i8, ptr %x, i64 16
8 x  ... i64 8
8 x  ... i64 24
4 x  ... i64 32
```

Every one of those constants assumes 8-byte `size_t` fields.

### What part 2 needs

Not a sweep. The layouts and their offsets have to be derived from one
place rather than written twice, so they cannot drift:

1. Emit the struct types from a table keyed on `ptrBits`, so `%ScrStr` is
   `{i32, i32, i32}` on a 32-bit target.
2. Replace the literal byte offsets with computed ones from that same table.
   The `getelementptr inbounds %ScrStr, ptr %s, i64 0, i32 1` form is
   already layout-driven and needs nothing; it is the raw
   `getelementptr i8 ... i64 24` byte arithmetic that must be generated.
3. Audit the remaining struct-shaped `i64`s (`ScrArr`, `ScrClosure`,
   `ScrUnion`, `ScrError`, `ScrRegex`, `ScrBytes`, `ScrMap`) against their C
   definitions the same way part 1 audited the function signatures: by
   parsing `packages/runtime/src`, not by matching names.

Point 4 of the original fix suggestion (emit a `target datalayout` /
`target triple`) is worth more than ever here. With a datalayout in the IR,
LLVM itself would flag a struct whose layout disagrees with the target
instead of silently producing wrong offsets, and part 2 would have been
caught at compile time rather than as a heap walk at frame 1.
