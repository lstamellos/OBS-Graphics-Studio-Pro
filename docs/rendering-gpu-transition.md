# Rendering, Composition, and GPU Transition Audit

## Current rendering architecture

OBS Graphics Studio Pro registers a single OBS input source and keeps source compatibility while rendering through libobs GPU passes. The previous CPU full-frame renderer has been removed from the live source path; the active path is now:

1. `TitleSource` evaluates playback, cue state, live text, ticker, clock, keyframes, and dirty state.
2. `ObsGpuRenderPipeline` converts each visible layer into a GPU pass plan.
3. Backgrounds, solid/shape layers, image textures, transforms, alpha blending, and shadow placeholders are submitted through OBS `gs_*` effects.
4. Bitmap image assets are cached as GPU textures and composited as textured quads.
5. Text layers reserve a GPU text-atlas pass and deliberately do not fall back to CPU rasterization.

This preserves OBS source compatibility while eliminating the previous CPU full-canvas composition/upload loop from live rendering.

## CPU-bound bottlenecks eliminated or isolated

| Area | Previous bottleneck | New GPU path | Remaining work |
|---|---|---|---|
| Full-frame composition | Animated layers repainted the entire CPU canvas | Layer-by-layer GPU submission with transform/opacity state | Add render-target graph for grouped effects |
| Texture upload | One full-canvas upload per dirty frame | Persistent GPU textures for assets; no composed CPU canvas upload | Add invalidation by file revision and atlas page |
| Solid/shape layers | CPU paths, fills, gradients, outlines, and shadow passes | OBS solid-effect geometry pass with GPU transforms and alpha | Add rounded-corner/gradient/outline shader techniques |
| Image layers | Asset paint into CPU canvas before upload | Cached GPU texture + textured-quad composition | Add GPU-native SVG/vector tessellation |
| Text, clock, ticker | CPU glyph/path rasterization | Reserved GPU text-atlas pass; CPU fallback disabled | Implement glyph/vector atlas shader |
| Effects | CPU repeated draw passes for blur/shadow | EffectShader stage in migration plan; shadow offset placeholder on GPU | Add ping-pong render targets and separable blur |
| 2D/3D transforms | CPU raster transform invalidated pixels | OBS matrix state per layer | Add perspective projection and 3D camera uniforms |

## First refactor completed

The codebase now has an OBS-compatible GPU transition layer:

- `ObsGpuRenderPipeline` owns live source drawing through OBS `gs_*` effects.
- `GpuTextureFrame` owns GPU-side asset textures for image layers.
- `GpuTitlePlan` / `GpuLayerPlan` produce a per-title audit that classifies layers by GPU pass readiness and records next steps.
- `TitleSource` no longer invokes a CPU full-frame renderer for live OBS output.

This keeps the existing OBS source ID, settings, frame timing, and source dimensions intact while removing the legacy CPU 2-D composition path from the live renderer.

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

- Decode bitmap assets only when the file changes, then upload/cache them as GPU textures.
- Apply transforms, opacity, background, shadow, and blending on GPU.
- Avoid full-frame CPU repaint for motion-only image animations.

### Incremental text path: Text, Clock, Ticker

Text is intentionally GPU-first in the live source path. Until the atlas shader lands, text layers are represented in the migration plan but are not routed through a CPU raster fallback. The safe staged approach is:

1. Add GPU glyph/vector atlas pages for repeated glyphs.
2. Composite atlas quads on GPU with transform and opacity uniforms.
3. Move shadows/blur/outline to post-process shaders.
4. Add ticker offset and 3D transform uniforms so motion does not dirty text geometry.
5. Add language/script shaping inputs without reintroducing CPU pixel rasterization.

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

1. Do not reintroduce CPU 2-D raster fallbacks in the live OBS source path.
2. Preserve `obs_graphics_studio_pro_source` as the source ID.
3. Preserve title JSON fields; add GPU-specific fields only as optional extensions.
4. Do not require a graphics API outside OBS `gs_*` abstractions.
5. Prefer OBS effect files/shader strings that compile through libobs rather than platform-specific OpenGL/Direct3D/Metal calls.
6. Make each GPU pass skippable so older scenes remain loadable while unsupported features are represented as no-op GPU stages rather than CPU raster fallbacks.

## Suggested next implementation phases

1. Add an offscreen render target abstraction and a small OBS effect for textured quads.
2. Extend the SolidRect/Shape GPU pass with rounded corners, gradients, outlines, and blur parity.
3. Expand image texture cache invalidation and route all bitmap Image layers through the textured-quad pass.
4. Add per-layer dirty tracking to avoid full-frame uploads when only transforms change.
5. Introduce post-process passes for blur/shadow/glow using ping-pong render targets.
6. Add optional 3D transform uniforms and perspective projection for 2.5D/3D compositing.
7. Add particle layers as GPU instanced quads with CPU-authored emitter parameters.
