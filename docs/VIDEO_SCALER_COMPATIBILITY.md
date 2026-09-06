# Stable embedded video scaling: avoid uninitialized mpv LUT padding

## Application policy

Omatrack explicitly sets libmpv's `scale`, `cscale`, and `dscale` to **bilinear**.
These use builtin texture sampling and bypass the padded spatial scaler lookup
textures described below. This is a deliberate playback-quality/performance
tradeoff versus higher-order Lanczos filtering: slightly softer scaling in return
for stable video on the affected dependency builds. Hardware decoding remains
`auto-safe`; no global Mesa, GLSL or graphics-driver override is applied.

**Image inference is unaffected by the display filter.** The image reader decodes
the original full-resolution frame separately and uses its validated crop/resize
algorithm. Bilinear display scaling does not resize the model's input, smooth its
predictions or change source timestamps/native telemetry.

After a fixed libmpv dependency is available and validated, higher-order display
filters can be reconsidered. Do not silently restore them on an affected build.

## Root cause and causal proof

During integration, Qt6.11.2/libmpv0.41.0 on Linux aarch64 sometimes displayed a
black video with only a tiny coloured strip/pixel. The failure also occurred with
native telemetry and the image reader disabled. Pausing was where screenshots
noticed it, but internal FBO probes showed the corruption **during playback**.

The investigation established:

1. The actually displayed Xvfb window and QQuickWindow readback were both black,
   even after completed frame callbacks and with the overlay hidden. Longer waits
   did not fix it.
2. FBO size, viewport, scissor, culling, rasterizer discard and GL error state were
   valid. Same-instance mpv `screenshot-sw=yes` / `screenshot-raw video rgba` returned
   good decoded pixels while the rendered FBO was black. Decoder/GUI inference
   failures were therefore excluded for the reproduced case.
3. A failing GL trace and passing control uploaded **byte-identical video luma**.
   Quad vertices and inspected uniforms were correct.
4. The inherited default Lanczos chroma scaler had six meaningful weights in an
   eight-float row stride, uploaded as a2×256 RGBA16F lookup texture. The remaining
   two floats in each row were uninitialized heap contents, including NaNs.
   The first chroma pass produced NaNs on every odd row; later passes propagated
   those invalid values into the final video.
5. A copy of the failing trace changed **only those two unused floats per row to
   zero**. All meaningful weights, source planes, calls, shaders, uniforms,
   vertices and ordering were unchanged. Original first chroma pass:2,073,600
   nonfinite float components. Corrected pass:zero. Original final FBO:one lit
   pixel out of901,120. Corrected final FBO:393,706 lit pixels and the full image.

This is a one-data-change replay proof, not a successful retry or a timing guess.
GL_LINEAR sampling can propagate a NaN from nominally unused padding even at
texel centres (zero interpolation weight does not make a NaN harmless).

The exact upstream allocation in mpv v0.41.0 is
[`video/out/gpu/video.c:1852`](https://github.com/mpv-player/mpv/blob/v0.41.0/video/out/gpu/video.c#L1852):
`talloc_array(... lut_size * stride)`. `mp_compute_lut` populates only the filter's
meaningful taps; padding remains uninitialized. The minimal upstream correction
is zero-initialization, preserved locally in
[mpv-zero-scaler-padding.patch](patches/mpv-zero-scaler-padding.patch).
No upstream commit, PR, dependency publication or system library modification was
performed for this work.

## Why all three bilinear choices

In v0.41.0 `reinit_scaler` returns before LUT allocation for builtin scalers, and
`pass_sample` emits direct texture sampling for bilinear. Explicitly selecting all
three spatial filters avoids fixing chroma while leaving another padded spatial
LUT on resize/downscale. Temporal interpolation is not enabled; its default
oversample mode also has no such kernel LUT. Dither/optional ICC tables are
separate paths, not the reproduced six/eight-weight allocation.

The separate default-state reset around `mpv_render_context_create` preserves
mpv's documented GL API contract, but it did **not** fix this bug by itself.
No speculative viewport change, skipped redraw, forced repaint, longer tolerance,
advanced-control mode or hardware-decoding workaround is used as the fix.

## Application regression evidence

With the bilinear policy and normal hardware-decoding selection:

- **Three native-telemetry runs** passed real displayed-video checks at150/500/1000ms
  after pause; native telemetry retained precedence and no inferred values appeared.
- **Three supported-HUD runs** passed initial/paused displayed-video checks, live
  extraction, seek burst invalidation, disable/reenable and source-switch tests.
- Blank and extraction-disabled modes passed; blank footage remained unknown and
  normal footage stayed visible with extraction disabled.

These were isolated authenticated Xvfb+Mesa llvmpipe **OpenGL** runs, not Qt's
geometry-less offscreen backend. They are not a target-laptop or high-refresh
performance claim. Strict visual assertions remain in the acceptance harness.

Private evidence lives under ignored `build-video-dev/gl-investigation/`:
`proof-summary.json`, `padding-zeroed-receipt.json`, `zero-lut-padding.py`,
`evidence.html`, source/control/padding-zeroed traces and intermediate captures.
Raw GL framebuffer images have the expected inverted GL orientation; application
screenshots under `build-video-dev/integration/screenshots/` are correctly oriented.
Original media and failed traces are preserved. No footage was published.
