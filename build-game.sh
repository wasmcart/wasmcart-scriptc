#!/usr/bin/env bash
# Build a real scriptc-game example as a cart: scriptc-game's web/canvas tier, relinked for wasm.
#
# The claim this script makes concrete: web/canvas/ is ALREADY portable. It
# talks to a generated skiac shim, and both halves of that shim compile under
# emcc unchanged against wasmcart-skia's libskiac.a, whose C surface is
# byte-identical to the native one (206 symbols vs 207, empty diff).
#
# What is NOT reused is shim/sg_core.cpp: it owns an SDL window, renderer and
# streaming texture. src/cart_core.cpp provides the same symbols over a Skia
# surface copied into the cart framebuffer.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SG="${SCRIPTC_GAME:-$HERE/../scriptc-game}"
SKIA="${WASMCART_SKIA:-$HERE/../wasmcart-skia/out}"
SKIA_HDRS="${SKIA_HEADERS:-$HERE/../napi-canvas/skia}"
SCRIPTC_REPO="${SCRIPTC_REPO:-$HERE/../scriptc}"
SCRIPTC_BIN="${SCRIPTC_BIN:-$SCRIPTC_REPO/packages/cli/dist/main.js}"
RT="$SCRIPTC_REPO/packages/runtime"
OUT="$HERE/build"
OBJ="$OUT/obj-game"

command -v emcc >/dev/null 2>&1 || { echo "build-canvas.sh: emcc not on PATH" >&2; exit 1; }
[ -f "$SKIA/libskiac.a" ] || { echo "build-canvas.sh: no libskiac.a in $SKIA" >&2; exit 1; }
[ -d "$SG/web/canvas" ]   || { echo "build-canvas.sh: scriptc-game not at $SG" >&2; exit 1; }

mkdir -p "$OBJ"

CXXF="-O2 -std=c++20 -fno-exceptions -fno-rtti -DSK_RELEASE -DSG_NO_FONT_PATH -I$SG/shim -I$SKIA/include -I$SKIA_HDRS"

# ---- 1. the FFI manifest ---------------------------------------------
#
# gen-ffi walks the TS import graph and needs the C signatures for every
# symbol it finds. Ours live outside scriptc-game, hence SG_EXTRA_SHIM.
echo "==> ffi manifest"
( cd "$SG" && SG_EXTRA_SHIM="$HERE/src/game_cart.c:$HERE/src/cart_core.cpp:$HERE/src/cart_audio.cpp:$HERE/src/cart_input.cpp:$HERE/src/gl_support.c" \
  node codegen/gen-ffi.js linux-x86_64 "$HERE/src/game_entry.ts" >/dev/null )

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
echo "==> scriptc: game_entry.ts -> IR"
SCRIPTC_CC=zigcc SCRIPTC_TARGET=x86-linux-gnu \
  node "$SCRIPTC_BIN" build "$HERE/src/game_entry.ts" \
    --ffi "$OUT/cart.ffi.json" -o "$OUT/game_native" >/dev/null 2>&1 || true
[ -f "$OUT/game_entry.ll" ] || { echo "build-canvas.sh: no IR produced" >&2; exit 1; }

echo "==> emcc: IR -> wasm object"
sed 's/^define i32 @main(/define i32 @scriptc_main(/' \
    "$OUT/game_entry.ll" > "$OUT/game_cart.ll"
emcc -O2 -c "$OUT/game_cart.ll" -o "$OBJ/tracer.o" 2>&1 \
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
echo "==> emcc: scriptc-game skia shim (unmodified)"
em++ $CXXF -c "$SG/shim/sg_skia_gen.cpp"   -o "$OBJ/sg_skia_gen.o"
em++ $CXXF -c "$SG/shim/sg_skia_extra.cpp" -o "$OBJ/sg_skia_extra.o"
emcc -O2 -c "$SG/shim/sg_tables.c" -I"$SG/shim" -o "$OBJ/sg_tables.o"

# Audio: the cart shim over webaudio-node's wasm engine, plus scriptc-game's
# own decoder shim (mp3/wav/ogg/flac) which is pure C and ports unchanged.
WEBAUDIO="${WEBAUDIO_A:-$HERE/../wasmcart-jsgame/obj/libwebaudio.a}"
em++ -O2 -std=c++20 -fno-exceptions -fno-rtti -c "$HERE/src/cart_audio.cpp" -o "$OBJ/cart_audio.o"

# Input. Without it sg_poll_event is an undefined import the host stubs to
# -1, and input.pump()'s "drain until EV_NONE" loop never terminates.
em++ -O2 -std=c++20 -fno-exceptions -fno-rtti -I"$HERE/src" \
  -c "$HERE/src/cart_input.cpp" -o "$OBJ/cart_input.o"
# dr_mp3/dr_wav/stb_vorbis live in webaudio-node's vendor dir, the same
# place the native build takes them from.
WA_SRC="${WEBAUDIO_SRC:-$HERE/../webaudio-node/src}"
em++ $CXXF -I"$WA_SRC/vendor" -c "$SG/shim/sg_audio_decode.cpp" -o "$OBJ/sg_audio_decode.o"

# ---- 5. cart shell ----------------------------------------------------
echo "==> emcc: cart shell"
em++ $CXXF -c "$HERE/src/cart_core.cpp" -o "$OBJ/cart_core.o"
emcc -O2 -c "$HERE/src/game_cart.c" -I"$HERE/src" -o "$OBJ/game_cart.o"

# ---- 6. link ----------------------------------------------------------
#
# Skia's archives must come in dependency order, and the whole set is needed:
# a partial link produced an out-of-bounds write on the first Canvas call
# rather than an undefined symbol (see wasmcart-jsgame/REBUILD_STATUS.md).
echo "==> emcc: link game.wasm"
emcc -O2 "$OBJ/game_cart.o" "$OBJ/cart_core.o" "$OBJ/tracer.o" \
  "$OBJ/sg_skia_gen.o" "$OBJ/sg_skia_extra.o" "$OBJ/sg_tables.o" \
  "$OBJ/cart_audio.o" "$OBJ/cart_input.o" "$OBJ/sg_audio_decode.o" "$WEBAUDIO" \
  "${rt_objs[@]}" \
  "$SKIA/libskiac.a" "$SKIA/libskia.a" "$SKIA/libskshaper.a" \
  "$SKIA/libskparagraph.a" "$SKIA/libskunicode_core.a" "$SKIA/libskunicode_icu.a" \
  "$SKIA/libharfbuzz.a" "$SKIA/libicu.a" "$SKIA/libfreetype2.a" \
  "$SKIA/libpng.a" "$SKIA/libjpeg.a" "$SKIA/libwebp.a" "$SKIA/libwuffs.a" \
  "$SKIA/libzlib.a" "$SKIA/libskcms.a" \
  -o "$OUT/game-pre.wasm" \
  -sSTANDALONE_WASM -sERROR_ON_UNDEFINED_SYMBOLS=0 \
  `# ASYNCIFY here, not as a separate binaryen pass: scr_async.c's fibers` \
  `# are emscripten_fiber_t, which IS Asyncify, so the two must be the same` \
  `# transform. ASYNCIFY_IMPORTS names the cart's frame yield so it stays` \
  `# suspendable alongside the fiber swaps.` \
  -sASYNCIFY -sASYNCIFY_IMPORTS='["env.wc_frame_yield"]' \
  -sASYNCIFY_STACK_SIZE=65536 \
  -sALLOW_MEMORY_GROWTH -sINITIAL_MEMORY=134217728 -sSTACK_SIZE=4194304 \
  --no-entry \
  -sEXPORTED_FUNCTIONS='["_wc_get_info","_wc_init","_wc_render","_wc_yield_buffer","_sc_frame_count"]'

# No separate wasm-opt --asyncify step: emcc already applied it above, and
# running the pass twice fails with "asyncify_get_state already exists".
cp "$OUT/game-pre.wasm" "$OUT/game.wasm"

# Pack with the game's own assets: the cart reads them through the host
# asset API, so this is what makes fetch()/Image src resolve.
node "$HERE/../wasmcart/bin/wasmcart.js" pack \
  --wasm "$OUT/game.wasm" --assets "$SG/examples/dodge/public" \
  --output "$HERE/game.wasc" >/dev/null

ls -la "$OUT/game.wasm" "$HERE/game.wasc"
echo "built $HERE/game.wasc"
