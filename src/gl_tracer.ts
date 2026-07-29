/* threeTS-lite in a wasmcart cart, over the host's GL.
 *
 * The imports are the point again: `WebGLRenderer`, `Scene`, `Mesh` and the
 * rest are scriptc-game's own files, unmodified. They talk to
 * web/webgl/context.ts, which talks to gl* FFI declarations, which resolve
 * to the cart's `gl` import module instead of libGLESv2. Nothing in the 3D
 * tier knows the difference.
 *
 * A spinning lit cube rather than a triangle: it exercises the parts that
 * actually break, which a single clear-and-draw would not. Depth testing,
 * an index buffer, a VAO, per-frame uniform uploads, and a normal-matrix
 * path that only shows up as wrong shading.
 */
import { Scene } from "../../scriptc-game/three/core/Scene.js";
import { PerspectiveCamera } from "../../scriptc-game/three/core/PerspectiveCamera.js";
import { Mesh } from "../../scriptc-game/three/objects/Mesh.js";
import { BoxGeometry } from "../../scriptc-game/three/geometries/BoxGeometry.js";
import { MeshLambertMaterial } from "../../scriptc-game/three/materials/Material.js";
import { AmbientLight, DirectionalLight } from "../../scriptc-game/three/lights/Light.js";
import { WebGLRenderer } from "../../scriptc-game/three/renderer/WebGLRenderer.js";
import { Vector3 } from "../../scriptc-game/three/math/Vector3.js";

import { WebGL2RenderingContext } from "../../scriptc-game/web/webgl/context.js";

/* Suspends until the host asks for the next frame; see canvas_cart.c. The
 * GL cart has no framebuffer to present, so this only yields. */
declare function sg_gl_frame(): number;
declare function sg_gl_width(): number;
declare function sg_gl_height(): number;

const W = sg_gl_width();
const H = sg_gl_height();

/* No canvas.getContextGL() here: that path asks the host shim to create a
 * context on an SDL window. In a cart the CONTEXT ALREADY EXISTS -- it is
 * the host's, reached through the gl import module -- so the wrapper is
 * constructed directly over it. */
const gl = new WebGL2RenderingContext(W, H);

const renderer = new WebGLRenderer(gl);
renderer.setSize(W, H);
renderer.setClearColor(0x0b1020);


const scene = new Scene();
scene.add(new AmbientLight(0x404a66, 1));
const sun = new DirectionalLight(0xffffff, 1);
sun.position.set(0.5, 1, 0.8);
scene.add(sun);

const cube = new Mesh(new BoxGeometry(1.4, 1.4, 1.4), new MeshLambertMaterial(0xff8844));
scene.add(cube);

const camera = new PerspectiveCamera(60, W / H, 0.1, 100);
camera.position.set(0, 0, 4);

camera.lookAt(new Vector3(0, 0, 0));

for (;;) {
  const frame = sg_gl_frame();
  if (frame === 0) break;

  /* Rotate on two axes so a frame is distinguishable from its neighbours
   * and a frozen loop cannot pass as a working one.
   *
   * Through the quaternion, not `rotation`: unlike three, Object3D.rotation
   * is a read-only VIEW here (the dialect has no property setters to sync
   * it back), so assigning to it would compile and do nothing. */
  const t = frame * 0.03;
  cube.quaternion.setFromEuler(t * 0.7, t, 0);

  /* PROBE: encode projection[0] and matrixWorldInverse[14] into the clear
   * colour. A correct 60-degree/16:9 perspective has e[0] ~ 0.974, and the
   * view matrix for a camera at z=4 looking at the origin has e[14] = -4. */
  const pe = camera.projectionMatrix.elements;
  const ve = camera.matrixWorldInverse.elements;
  /* Three elements at once: projection[0], projection[5], view[14]. */
  /* W, H/4, and camera.position.z*10 -- the three FFI-sourced numbers the
   * matrices are built from. */
  /* Matrix4 arithmetic in isolation: identity, then a known translation.
   * m.elements[14] after makeTranslation(0,0,-4) must be -4. */
  /* camera.matrixWorld[14] should be +4 (the camera sits at z=4) and its
   * inverse [14] should be -4. If matrixWorld is already 8, the fault is in
   * updateMatrix/compose, not in the inversion. */
  /* invert() in isolation: the inverse of translate(0,0,4) must be
   * translate(0,0,-4). If this reads 80 the fault is in invert itself. */
  /* Does copy().invert() leave the SOURCE untouched? If the chain returned
   * the source rather than the destination, b would be inverted too. */
  /* Recompute the view matrix by hand and compare with the camera's own.
   * Same inputs, same call: if these disagree the fault is in WHEN the
   * camera takes its inverse, not in the arithmetic. */
  /* Walk back one more step: camera.matrix (from compose) vs matrixWorld
   * (a copy of it). Both should read +4 at element 14. */
  renderer.render(scene, camera);
}
