/* The cart's asset store: host assets instead of node:fs.
 *
 * host/resources.ts owns URL resolution (the web root, the external-URL
 * rule, the warning) and delegates the actual bytes to an AssetStore. This
 * is that store for a cart, backed by the host's wc_asset_size /
 * wc_load_asset through cart_core.cpp.
 *
 * The game does not change. `fetch("data/level.json")` and
 * `img.src = "player.png"` still resolve the same way; only where the bytes
 * come from is different.
 */
import { AssetStore, setAssetStore } from "../../scriptc-game/host/resources.js";

/* Implemented in cart_core.cpp. FFI format 1 has no bytes-RETURN class, so
 * reading is two calls: ask the size, then hand in a buffer to fill. */
declare function sg_cart_asset_size(path: string): number;
declare function sg_cart_asset_read(path: string, dest: Buffer): number;

class CartAssets extends AssetStore {
  override size(path: string): number {
    return sg_cart_asset_size(path);
  }

  override read(path: string): Buffer | null {
    const n = sg_cart_asset_size(path);
    if (n <= 0) return null;
    const buf = Buffer.alloc(n);
    /* A short read is a corrupt asset, not an empty one: returning the
     * partial buffer would hand a truncated PNG to the decoder and produce
     * a confusing "unsupported image" rather than "missing file". */
    if (sg_cart_asset_read(path, buf) !== n) return null;
    return buf;
  }
}

/** Installs the store. Must run before any asset read. */
export function installCartAssets(): void {
  setAssetStore(new CartAssets());
}
