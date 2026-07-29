#!/usr/bin/env bash
# Build the GL cart: scriptc-game's web/canvas tier, relinked for wasm.
#
# The claim: three/ and web/webgl/ are ALREADY portable. The renderer talks
# to a WebGL2RenderingContext-shaped class, which talks to gl* FFI
# declarations; in a cart those resolve to the host's `gl` import module
# instead of libGLESv2, and nothing in the 3D tier knows the difference.
#
# No Skia at all here, unlike build-gl.sh: a GL cart draws into the
# host's context and needs no surface, no readback and no font stack.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SG="${SCRIPTC_GAME:-$HERE/../scriptc-game}"
SCRIPTC_REPO="${SCRIPTC_REPO:-$HERE/../scriptc}"
SCRIPTC_BIN="${SCRIPTC_BIN:-$SCRIPTC_REPO/packages/cli/dist/main.js}"
RT="$SCRIPTC_REPO/packages/runtime"
OUT="$HERE/build"
OBJ="$OUT/obj-gl"

command -v emcc >/dev/null 2>&1 || { echo "build-gl.sh: emcc not on PATH" >&2; exit 1; }
[ -d "$SG/three" ] || { echo "build-gl.sh: scriptc-game not at $SG" >&2; exit 1; }

mkdir -p "$OBJ"


# ---- 1. the FFI manifest ---------------------------------------------
#
# gen-ffi walks the TS import graph and needs the C signatures for every
# symbol it finds. Ours live outside scriptc-game, hence SG_EXTRA_SHIM.
echo "==> ffi manifest"
( cd "$SG" && SG_EXTRA_SHIM="$HERE/src/gl_cart.c" \
  node codegen/gen-ffi.js linux-x86_64 "$HERE/src/gl_tracer.ts" >/dev/null )

# Strip the native link inputs. scriptc runs a real link to produce the IR,
# and a cart has no SDL2/GL/fontconfig to link against; the wasm link in step
# 6 supplies the archives itself. Without this scriptc stops at "unable to
# find dynamic system library 'SDL2'" and never writes the .ll.
python3 - "$SG/ffi/core.ffi.json" "$OUT/cart.ffi.json" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
d["libraries"] = []
d["system_libraries"] = []
json.dump(d, open(sys.argv[2], "w"), indent=2)
PYEOF

# ---- 2. TypeScript -> LLVM IR ----------------------------------------
#
# SCRIPTC_TARGET is a 32-BIT triple, and that is the whole point: it makes
# the backend emit i32 for size_t, matching wasm32. x86-linux-gnu rather than
# wasm32-wasi because scriptc's own C runtime does not compile under WASI
# (~45 POSIX errors) and the IR is never written; x86 gets far enough.
#
# The native link at the end fails by design (our shim symbols are wasm
# objects); the .ll is written first and is what we want.
echo "==> scriptc: gl_tracer.ts -> IR"
SCRIPTC_CC=zigcc SCRIPTC_TARGET=x86-linux-gnu \
  node "$SCRIPTC_BIN" build "$HERE/src/gl_tracer.ts" \
    --ffi "$OUT/cart.ffi.json" -o "$OUT/gl_native" >/dev/null 2>&1 || true
[ -f "$OUT/gl_tracer.ll" ] || { echo "build-gl.sh: no IR produced" >&2; exit 1; }

echo "==> emcc: IR -> wasm object"
sed 's/^define i32 @main(/define i32 @scriptc_main(/' \
    "$OUT/gl_tracer.ll" > "$OUT/gl_cart.ll"
emcc -O2 -c "$OUT/gl_cart.ll" -o "$OBJ/tracer.o" 2>&1 \
  | grep -v "overriding the module target" || true

# ---- 3. scriptc runtime ----------------------------------------------
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
# scr_regex.c pulls lre_* out of quickjs-ng's libregexp/libunicode.
QJS="$RT/vendor/quickjs-ng"
for q in libregexp libunicode; do
  emcc -O2 -c "$QJS/$q.c" -I"$QJS" -o "$OBJ/$q.o" 2>/dev/null
done

rt_objs=("$OBJ/libregexp.o" "$OBJ/libunicode.o")
for f in "$RT"/src/*.c; do
  b="$(basename "$f" .c)"
  case "$b" in scr_http|scr_tls|scr_inspect_island) continue;; esac
  emcc -O2 -c "$f" -I"$RT/include" -I"$RT/src" -I"$QJS" -include "$OBJ/a4r_shim.h" -o "$OBJ/$b.o" 2>/dev/null
  rt_objs+=("$OBJ/$b.o")
done

# ---- 4. scriptc-game's skia shim, UNMODIFIED -------------------------
#
# The whole point: these are scriptc-game's files, compiled for wasm with no
# edits, against a different libskiac.a.
# No Skia here. A GL cart draws straight into the host's context, so the
# canvas tier and its 15 Skia archives are not linked at all -- which is why
# this cart is ~200KB against the canvas cart's 6.8MB.
emcc -O2 -c "$SG/shim/sg_tables.c" -I"$SG/shim" -o "$OBJ/sg_tables.o"

# scriptc-game's GL shim, UNMODIFIED, with wasmcart.h force-included.
#
# That include is what redirects every raw gl* call to the host: wasmcart.h
# declares them with __attribute__((import_module("gl"))), so the shim's
# glClear becomes an import from the cart's `gl` module instead of resolving
# to Emscripten's own GL (which would target a canvas that does not exist
# here). The shim source itself needs no edits.
# NOT force-including wasmcart.h here: sg_gl_*.cpp include GLES3/gl3.h,
# whose declarations have C++ linkage and collide with the header's. The
# shim keeps calling plain glXxx; gl_bridge.c is what makes those resolve to
# the host's gl module.
GLCXX="-O2 -std=c++20 -fno-exceptions -fno-rtti -I$SG/shim -I$HERE/src"
# The bridge: defines each glXxx locally and forwards to the same name
# imported from the host's "gl" module. Without it the TS side's gl* calls
# become "env" imports the host does not supply.
emcc -O2 -DWC_USE_GL -c "$HERE/src/gl_bridge.c" -I"$HERE/src" -o "$OBJ/gl_bridge.o"

em++ $GLCXX -c "$SG/shim/sg_gl_gen.cpp"   -o "$OBJ/sg_gl_gen.o"
em++ $GLCXX -c "$SG/shim/sg_gl_extra.cpp" -o "$OBJ/sg_gl_extra.o"

# ---- 5. cart shell ----------------------------------------------------
echo "==> emcc: cart shell"
emcc -O2 -c "$HERE/src/gl_cart.c" -I"$HERE/src" -o "$OBJ/gl_cart.o"

# The math FFI and error mailbox. They live in scriptc-game's
# sg_skia_extra.cpp, which a GL cart does not link, so without this every
# Math.sqrt/tan/cos became a host-stubbed import returning a constant and
# every camera matrix was silently garbage.
emcc -O2 -c "$HERE/src/gl_support.c" -I"$HERE/src" -o "$OBJ/gl_support.o"

# Audio: scriptc-game's sg_audio surface over the cart ring buffer, on
# webaudio-node's graph engine. The engine archive is the one
# wasmcart-jsgame already builds for wasm -- same upstream, same C ABI as
# the native shim drives.
WEBAUDIO="${WEBAUDIO_A:-$HERE/../wasmcart-jsgame/obj/libwebaudio.a}"
if [ -f "$WEBAUDIO" ]; then
  em++ -O2 -std=c++20 -fno-exceptions -fno-rtti -c "$HERE/src/cart_audio.cpp" \
    -o "$OBJ/cart_audio.o"
  AUDIO_OBJS=("$OBJ/cart_audio.o" "$WEBAUDIO")
else
  echo "    (no libwebaudio.a at $WEBAUDIO -- building without audio)"
  AUDIO_OBJS=()
fi

# ---- 6. link ----------------------------------------------------------
#
# Skia's archives must come in dependency order, and the whole set is needed:
# a partial link produced an out-of-bounds write on the first Canvas call
# rather than an undefined symbol (see wasmcart-jsgame/REBUILD_STATUS.md).
echo "==> emcc: link gl.wasm"
emcc -O2 "$OBJ/gl_cart.o" "$OBJ/tracer.o" "$OBJ/sg_tables.o" \
  "$OBJ/sg_gl_gen.o" "$OBJ/sg_gl_extra.o" "$OBJ/gl_bridge.o" "$OBJ/gl_support.o" \
  ${AUDIO_OBJS[@]+"${AUDIO_OBJS[@]}"} \
  "${rt_objs[@]}" \
  -o "$OUT/gl-pre.wasm" \
  -sSTANDALONE_WASM -sERROR_ON_UNDEFINED_SYMBOLS=0 \
  -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=67108864 -sSTACK_SIZE=4194304 \
  --no-entry \
  -sEXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render","_wc_yield_buffer","_sc_frame_count"]'

echo "==> wasm-opt: asyncify around wc_frame_yield"
WASM_OPT="${WASM_OPT:-$(dirname "$(command -v emcc)")/../bin/wasm-opt}"
[ -x "$WASM_OPT" ] || WASM_OPT=wasm-opt
# --enable-threads is about VALIDATION, not concurrency. Current Skia emits
# atomic ops (7146 of them in libskia.a; a May 2026 build had none), and
# wasm-opt refuses to validate a module containing them unless the feature is
# enabled -- "Atomic operations require threads [--enable-threads]". The cart
# is still single-threaded: nothing here spawns a worker, and the atomics are
# uncontended. Without this the asyncify pass fails outright and no cart is
# produced.
"$WASM_OPT" --asyncify \
  --pass-arg=asyncify-imports@env.wc_frame_yield \
  --enable-nontrapping-float-to-int --enable-bulk-memory --enable-sign-ext \
  --enable-mutable-globals --enable-simd --enable-threads \
  -O2 "$OUT/gl-pre.wasm" -o "$OUT/gl.wasm"

# Pack with assets: the font is loaded through the host asset API rather
# than embedded, since a cart has no filesystem for a path-based register.
node "$HERE/../wasmcart/bin/wasmcart.js" pack \
  --wasm "$OUT/gl.wasm" \
  --output "$HERE/gl.wasc" >/dev/null

ls -la "$OUT/gl.wasm" "$HERE/gl.wasc"
echo "built $HERE/gl.wasc"
