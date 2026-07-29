/* A real scriptc-game example, as a cart.
 *
 * This mirrors the generated entry a native build uses
 * (examples/<game>/.sg-build/entry.ts) with two differences, both about
 * where things come from rather than what runs:
 *
 *   1. The asset store is installed FIRST, so `fetch("music.mp3")` and
 *      `img.src = "player.png"` read from the .wasc instead of node:fs.
 *   2. There is no window to open, so boot() builds the Skia surface and
 *      the document over the cart framebuffer.
 *
 * The loop is runtime.ts's own, driven a frame at a time through
 * runFrameSync rather than through run(). run() is `async` and ends each
 * iteration with `await`, and a scriptc async task suspends via
 * swapcontext -- which does not exist under wasm, is stubbed to a no-op,
 * and leaves asyncify unable to rewind (it traps as "table index is out of
 * bounds"). runFrameSync is the same body without the await.
 *
 * ffi.present() inside it is the frame boundary: cart_core.cpp copies the
 * Skia frame out and yields there.
 *
 * The game module itself is imported unchanged.
 */
import { boot, startSync, runFrameSync, HostOptions, FrameState }
  from "../../scriptc-game/host/runtime.js";
import { installCartAssets } from "./cart_assets.js";
import { __setInlineTasks } from "../../scriptc-game/web/globals.js";

/* Before anything reads an asset. ES imports hoist, so the game module's
 * top level has already run by the time this statement executes -- which is
 * fine, because a game's asset loading belongs in its `load` handler, the
 * same rule a browser page follows. */
installCartAssets();

/* A cart cannot park a fiber (no ucontext, and emscripten's fiber swap is a
 * JS-glue import a standalone module does not get). Deferring a resolution
 * to a later turn would therefore mean the continuation NEVER runs, because
 * the `.then` attaches before the promise settles and then has nothing to
 * suspend on.
 *
 * Inline mode settles synchronously instead. The API is unchanged -- game
 * code still writes fetch(...).then(...) -- and the backing store here IS
 * synchronous (assets come out of the .wasc), so nothing is being faked. */
__setInlineTasks(true);

const opts = new HostOptions();
opts.width = 800;
opts.height = 600;
/* The web root is the .wasc's asset root; the host resolves relative paths
 * against it, so "" rather than a directory name. */
opts.gameDir = "";

const brc = boot(opts);
if (brc !== 0) process.exit(brc);

import "../../scriptc-game/examples/dodge/main.js";

const st = new FrameState();
startSync(opts, st);
/* present() suspends inside runFrameSync, so this loop is the game's whole
 * lifetime: one iteration per host frame. */
for (;;) {
  if (!runFrameSync(opts, st)) break;
}
