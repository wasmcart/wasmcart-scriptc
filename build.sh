#!/usr/bin/env bash
# Build the scriptc tracer cart: TypeScript -> LLVM IR -> wasm -> .wasc
#
# The pipeline, and why each step is what it is:
#
#   1. scriptc compiles src/tracer.ts to LLVM IR. The IR carries NO target
#      triple, which is what makes step 2 possible at all.
#   2. emcc lowers that IR to a wasm object. Emscripten rather than
#      wasm32-wasi: scriptc's C runtime wants POSIX (signals, subprocess,
#      pipes) and Emscripten shims those, while WASI does not -- under
#      wasm32-wasi the same runtime fails with ~45 errors.
#   3. scriptc's runtime C compiles alongside it, minus four files that need
#      network/regex deps a cart cannot use anyway.
#   4. emcc links the lot with cart_main.c into a wasm exporting the wasmcart
#      ABI, then wasmcart packs it into a .wasc.
#
# Needs: emcc on PATH (or EMSDK_ENV pointing at emsdk_env.sh), a scriptc
# checkout, and zig for scriptc's own native step.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPTC_REPO="${SCRIPTC_REPO:-$HERE/../scriptc}"
SCRIPTC_BIN="${SCRIPTC_BIN:-$SCRIPTC_REPO/packages/cli/dist/main.js}"
RT="$SCRIPTC_REPO/packages/runtime"
OUT="$HERE/build"
OBJ="$OUT/obj"

command -v emcc >/dev/null 2>&1 || {
  echo "build.sh: emcc not on PATH (source your emsdk_env.sh)" >&2; exit 1; }
[ -f "$SCRIPTC_BIN" ] || {
  echo "build.sh: scriptc CLI not found at $SCRIPTC_BIN" >&2; exit 1; }

mkdir -p "$OBJ"

# ---- 1. TypeScript -> LLVM IR ----------------------------------------
#
# The native link at the end of this FAILS by design: the FFI symbols live in
# cart_main.c, which is a wasm object, so there is nothing for the host linker
# to resolve them against. The .ll is written before that point and is all we
# want, so the failure is tolerated rather than treated as an error.
echo "==> scriptc: tracer.ts -> IR"
SCRIPTC_CC=zigcc SCRIPTC_TARGET=x86_64-linux-gnu \
  node "$SCRIPTC_BIN" build "$HERE/src/tracer.ts" \
    --ffi "$HERE/src/tracer.ffi.json" -o "$OUT/tracer_native" >/dev/null 2>&1 || true
[ -f "$OUT/tracer.ll" ] || { echo "build.sh: scriptc produced no IR" >&2; exit 1; }

# ---- 2. IR -> wasm object --------------------------------------------
#
# scriptc names the program entry `main`; Emscripten wants that name for its
# own. Renaming in the IR is the least invasive fix: cart_main.c calls
# scriptc_main() explicitly, so the program runs when the cart says so rather
# than at module load.
echo "==> emcc: IR -> wasm object"
sed 's/^define i32 @main(/define i32 @scriptc_main(/' "$OUT/tracer.ll" > "$OUT/tracer_cart.ll"
emcc -O2 -c "$OUT/tracer_cart.ll" -o "$OBJ/tracer.o" 2>&1 | grep -v "overriding the module target" || true

# ---- 3. scriptc runtime ----------------------------------------------
#
# arc4random_buf is BSD/glibc-only and Emscripten's musl lacks it; getentropy
# IS present and is what it is built on anyway. Four runtime files are skipped:
# http and tls need a network stack, regex needs quickjs's libregexp, and
# inspect_island belongs to the dynamic engine. None is reachable from a cart.
echo "==> emcc: scriptc runtime"
cat > "$OBJ/a4r_shim.h" <<'EOF'
#ifndef SC_A4R_SHIM_H
#define SC_A4R_SHIM_H
#include <unistd.h>
#include <stddef.h>
static inline void arc4random_buf(void *buf, size_t n) {
  unsigned char *p = (unsigned char *)buf;
  while (n > 0) { size_t c = n > 256 ? 256 : n; getentropy(p, c); p += c; n -= c; }
}
#endif
EOF
rt_objs=()
for f in "$RT"/src/*.c; do
  b="$(basename "$f" .c)"
  case "$b" in scr_http|scr_regex|scr_tls|scr_inspect_island) continue;; esac
  emcc -O2 -c "$f" -I"$RT/include" -I"$RT/src" -include "$OBJ/a4r_shim.h" \
       -o "$OBJ/$b.o" 2>/dev/null
  rt_objs+=("$OBJ/$b.o")
done
echo "    ${#rt_objs[@]} runtime objects"

# ---- 4. cart shell + link --------------------------------------------
#
# Linked WITHOUT -sASYNCIFY. The asyncify transform is applied afterwards by
# binaryen, scoped to the single import the host drives (env.wc_frame_yield).
# Emscripten's own -sASYNCIFY was the first attempt and deadlocks: its
# emscripten_sleep waits on the JS event loop, while wasmcart calls wc_render
# synchronously and waits for the return.
echo "==> emcc: link cart.wasm"
emcc -O2 -c "$HERE/src/cart_main.c" -I"$HERE/src" -o "$OBJ/cart_main.o"

emcc -O2 "$OBJ/cart_main.o" "$OBJ/tracer.o" "${rt_objs[@]}" \
  -o "$OUT/cart-pre.wasm" \
  -sSTANDALONE_WASM \
  -sERROR_ON_UNDEFINED_SYMBOLS=0 \
  -sALLOW_MEMORY_GROWTH \
  --no-entry \
  -sEXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render","_wc_yield_buffer","_sc_frame_count"]'

# ---- 5. asyncify the yield point -------------------------------------
#
# Only env.wc_frame_yield is made suspendable, so the transform stays cheap:
# asyncify instruments every function that can reach a suspending import, and
# naming the one import keeps that set to the actual call path.
echo "==> wasm-opt: asyncify around wc_frame_yield"
WASM_OPT="${WASM_OPT:-$(dirname "$(command -v emcc)")/../bin/wasm-opt}"
[ -x "$WASM_OPT" ] || WASM_OPT=wasm-opt
# The --enable-* flags match what emcc already emitted: wasm-opt validates
# against a bare MVP baseline otherwise and rejects its own toolchain's
# output ("memory.copy requires bulk memory operations").
"$WASM_OPT" --asyncify \
  --pass-arg=asyncify-imports@env.wc_frame_yield \
  --enable-nontrapping-float-to-int --enable-bulk-memory --enable-sign-ext \
  --enable-mutable-globals --enable-simd \
  -O2 "$OUT/cart-pre.wasm" -o "$OUT/cart.wasm"

ls -la "$OUT/cart.wasm"
echo "built $OUT/cart.wasm"
