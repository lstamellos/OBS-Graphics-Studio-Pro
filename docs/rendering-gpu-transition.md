# Rendering, Composition, and GPU Transition Audit

## Current rendering architecture

OBS Graphics Studio Pro currently registers a single OBS input source and keeps source compatibility by rendering every title as one OBS texture. The active frame path is:

1. `TitleSource` evaluates playback, cue state, live text, ticker, clock, keyframes, and dirty state.
2. The renderer allocates a full-title BGRA CPU buffer sized to the title canvas.
3. Cairo/Pango/Qt paint each visible layer into that buffer from bottom to top.
4. The full buffer is uploaded to a dynamic `gs_texture_t`.
5. OBS draws that texture with the default OBS base effect.

This is safe and OBS-compatible, but most composition work happens before the GPU receives the frame.

## CPU-bound bottlenecks found

| Area | Current path | Bottleneck | GPU migration target |
|---|---|---|---|
| Full-frame composition | Cairo surface over a title-sized BGRA buffer | Animated layers dirty and repaint the whole title frame | Per-layer GPU render graph with transform/opacity uniforms |
| Texture upload | One dynamic `gs_texture_set_image` upload for the full canvas | Upload bandwidth scales with canvas size even when one layer moves | Upload only CPU-raster assets; composite on GPU |
| Solid/shape layers | Cairo paths, gradients, outlines, and shadow passes | Geometry, fill, gradient, and blend work is CPU rasterized | Shader quads/meshes with color, gradient, rounded-corner, outline, and shadow uniforms |
| Image layers | Qt image/SVG decode and Cairo source-surface paint | Image rasterization can happen in the render path; transforms force full-frame repaint | Persistent `gs_texture_t` cache plus textured-quad shader path |
| Text, clock, ticker | Pango/Qt shaping and `QPainterPath` raster work | Glyph raster, shadows, outlines, and ticker motion are CPU-bound | CPU shaping retained initially, then glyph atlas + GPU transform/blend/shadow passes |
| Effects | Shadow blur is approximated with repeated CPU draw passes | Blur cost grows with radius/layer count | Separable blur/post-process shader chain |
| 2D/3D transforms | Cairo translate/rotate/scale before raster output | Transform animation invalidates pixels | Matrix uniforms, perspective projection, and OBS effect passes |

## First refactor completed

The codebase now has an OBS-compatible GPU transition layer:

- `GpuTextureFrame` owns the dynamic OBS texture, size changes, BGRA uploads, and destruction.
- `ObsGpuRenderPipeline` owns final OBS drawing through the default OBS effect.
- `GpuTitlePlan` / `GpuLayerPlan` produce a per-title audit that classifies layers by GPU migration readiness and records bottlenecks/next steps.
- `TitleSource` still uses the Cairo compatibility renderer for pixel generation, but no longer owns raw `gs_texture_t` lifecycle or final effect drawing directly.

This keeps the existing OBS source ID, settings, frame timing, and output behavior intact while creating a stable seam for incremental shader integration.

## Layer migration readiness

### Highest priority: SolidRect and Shape

These are the best first GPU-rendered layers because they do not require text shaping or asset decoding. They can be represented as generated vertices with:

- Position, scale, rotation, origin, and opacity uniforms.
- Solid-color or gradient fill uniforms.
- Rounded-corner distance field or mesh subdivision.
- Optional outline and drop-shadow passes.
- Standard alpha blending through OBS graphics state.

### Next priority: Image

Image layers should move to a texture cache keyed by path, size, SVG raster target, and file revision. Once cached, each layer can use the same textured-quad pipeline as the title output:

- Decode/rasterize only when the asset changes.
- Apply transforms, opacity, background, shadow, and blending on GPU.
- Avoid full-frame CPU repaint for motion-only image animations.

### Incremental text path: Text, Clock, Ticker

Text should remain compatibility-first because shaping, wrapping, language support, outline behavior, and ticker layout are already encoded in the CPU path. The safe staged approach is:

1. Keep CPU text shaping/rasterization into small layer-local textures.
2. Composite those layer textures on GPU with transform and opacity uniforms.
3. Add glyph atlas caching for repeated glyphs.
4. Move shadows/blur/outline to post-process shaders.
5. Add ticker offset and 3D transform uniforms so motion does not dirty text pixels.

## Proposed GPU pipeline architecture

```text
TitleSource (OBS source callbacks)
  └─ GpuRenderGraph
      ├─ AssetCache (images, SVG raster targets, glyph atlases)
      ├─ LayerPass[]
      │   ├─ SolidGeometryPass
      │   ├─ TextureLayerPass
      │   ├─ TextAtlasPass
      │   └─ Particle/3D/EffectPass
      ├─ PostProcessPass[] (blur, shadows, glow, masks)
      └─ CompositePass (OBS-compatible final texture)
```

The graph should continue to expose a final OBS texture and keep source registration unchanged. That preserves scene/source compatibility while allowing individual layer types to leave the CPU renderer one at a time.

## Non-breaking integration rules

1. Keep Cairo/Pango rendering as the fallback until each layer type has visual parity tests.
2. Preserve `obs_graphics_studio_pro_source` as the source ID.
3. Preserve title JSON fields; add GPU-specific fields only as optional extensions.
4. Do not require a graphics API outside OBS `gs_*` abstractions.
5. Prefer OBS effect files/shader strings that compile through libobs rather than platform-specific OpenGL/Direct3D/Metal calls.
6. Make each GPU pass skippable so older scenes and unsupported layer features continue to render through the CPU path.

## Suggested next implementation phases

1. Add an offscreen render target abstraction and a small OBS effect for textured quads.
2. Implement GPU SolidRect/Shape rendering behind a feature flag while leaving Cairo fallback available.
3. Add image texture cache and route Image layers through the textured-quad pass.
4. Add per-layer dirty tracking to avoid full-frame uploads when only transforms change.
5. Introduce post-process passes for blur/shadow/glow using ping-pong render targets.
6. Add optional 3D transform uniforms and perspective projection for 2.5D/3D compositing.
7. Add particle layers as GPU instanced quads with CPU-authored emitter parameters.
