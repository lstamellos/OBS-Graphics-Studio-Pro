/*
 * title-renderer-gpu.cpp
 *
 * Small OBS graphics abstraction used as the first step away from a monolithic
 * CPU renderer.  The implementation currently draws the composed title texture
 * with OBS' default effect, and records a per-layer migration plan for future
 * shader/effect passes.
 */

#include "title-renderer-gpu.h"
#include "title-data.h"

#include <algorithm>
#include <utility>

namespace obsgs {
namespace {

static bool layer_has_animated_transform(const Layer &layer)
{
    return layer.pos_x.is_animated() ||
           layer.pos_y.is_animated() ||
           layer.scale_x.is_animated() ||
           layer.scale_y.is_animated() ||
           layer.rotation.is_animated() ||
           layer.opacity.is_animated() ||
           layer.box_width.is_animated() ||
           layer.box_height.is_animated() ||
           layer.origin_x_prop.is_animated() ||
           layer.origin_y_prop.is_animated();
}

static bool layer_uses_effect_properties(const Layer &layer)
{
    return layer.shadow_enabled || layer.shadow_enabled_prop.is_animated() ||
           layer.shadow_opacity_prop.is_animated() ||
           layer.shadow_distance_prop.is_animated() ||
           layer.shadow_angle_prop.is_animated() ||
           layer.shadow_blur_prop.is_animated() ||
           layer.shadow_spread_prop.is_animated() ||
           layer.outline_enabled ||
           layer.fill_type == 1 ||
           layer.background_enabled || layer.background_enabled_prop.is_animated();
}

static void append_transform_notes(const Layer &layer, GpuLayerPlan &plan)
{
    if (plan.uses_animated_transform)
        plan.migration_notes.emplace_back("Animated transform/opacity can be sent as per-layer uniforms instead of repainting pixels.");

    if (layer_uses_effect_properties(layer))
        plan.migration_notes.emplace_back("Blur, shadow, gradient, outline, and background parameters are shader/post-process candidates.");
}

static GpuLayerPlan build_layer_plan(const Layer &layer)
{
    GpuLayerPlan plan;
    plan.layer_id = layer.id;
    plan.layer_name = layer.name;
    plan.layer_type = layer_type_name(layer.type);
    plan.uses_animated_transform = layer_has_animated_transform(layer);
    switch (layer.type) {
    case LayerType::SolidRect:
    case LayerType::Shape:
        plan.can_render_geometry_on_gpu = true;
        plan.requires_cpu_raster = false;
        plan.stages.push_back(GpuPipelineStage::SolidGeometryShader);
        plan.stages.push_back(GpuPipelineStage::PostProcessShader);
        plan.migration_notes.emplace_back("Solid/shape layer can be drawn as GPU vertices with color/gradient uniforms.");
        break;
    case LayerType::Image:
        plan.can_render_geometry_on_gpu = !layer.image_path.empty();
        plan.requires_cpu_raster = layer.image_path.empty();
        plan.stages.push_back(GpuPipelineStage::TextureShader);
        plan.stages.push_back(GpuPipelineStage::PostProcessShader);
        if (plan.requires_cpu_raster)
            plan.stages.push_back(GpuPipelineStage::CpuRasterUpload);
        plan.migration_notes.emplace_back("Image layer can become a cached gs_texture sampled by a textured quad.");
        break;
    case LayerType::Text:
    case LayerType::Clock:
    case LayerType::Ticker:
        plan.can_render_geometry_on_gpu = true;
        plan.requires_cpu_raster = true;
        plan.stages.push_back(GpuPipelineStage::TextAtlasShader);
        plan.stages.push_back(GpuPipelineStage::PostProcessShader);
        plan.stages.push_back(GpuPipelineStage::CpuRasterUpload);
        plan.migration_notes.emplace_back("Text shaping/rasterization remains CPU-safe initially; glyph atlas and transform/blend passes can move to GPU incrementally.");
        break;
    default:
        plan.stages.push_back(GpuPipelineStage::CpuRasterUpload);
        plan.migration_notes.emplace_back("Unknown layer type remains on the compatibility CPU path.");
        break;
    }

    append_transform_notes(layer, plan);
    return plan;
}

} // namespace

GpuTextureFrame::~GpuTextureFrame()
{
    reset();
}

bool GpuTextureFrame::ensure_size(uint32_t width, uint32_t height)
{
    if (texture_ && width_ == width && height_ == height)
        return true;

    obs_enter_graphics();
    if (texture_)
        gs_texture_destroy(texture_);
    texture_ = gs_texture_create(width, height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
    obs_leave_graphics();

    if (!texture_) {
        width_ = 0;
        height_ = 0;
        return false;
    }

    width_ = width;
    height_ = height;
    return true;
}

bool GpuTextureFrame::upload_bgra(const uint8_t *pixels, uint32_t linesize)
{
    if (!texture_ || !pixels)
        return false;

    obs_enter_graphics();
    gs_texture_set_image(texture_, pixels, linesize, false);
    obs_leave_graphics();
    return true;
}

void GpuTextureFrame::reset()
{
    if (!texture_) {
        width_ = 0;
        height_ = 0;
        return;
    }

    obs_enter_graphics();
    gs_texture_destroy(texture_);
    obs_leave_graphics();
    texture_ = nullptr;
    width_ = 0;
    height_ = 0;
}

void GpuTextureFrame::render_default() const
{
    ObsGpuRenderPipeline pipeline;
    pipeline.render_texture(*this);
}

GpuTitlePlan ObsGpuRenderPipeline::build_migration_plan(const Title &title) const
{
    GpuTitlePlan plan;
    plan.width = static_cast<uint32_t>(std::max(1, title.width));
    plan.height = static_cast<uint32_t>(std::max(1, title.height));

    plan.bottlenecks.emplace_back("Full-frame Cairo/Pango composition writes CPU pixels before each OBS texture upload.");
    plan.bottlenecks.emplace_back("Image layers are decoded/rasterized in the render path instead of using persistent gs_texture caches.");
    plan.bottlenecks.emplace_back("Text, shadow blur, gradients, and outlines perform CPU raster work that scales with canvas size and layer count.");
    plan.bottlenecks.emplace_back("Animated transforms currently dirty the entire title frame rather than updating GPU matrices/uniforms.");

    for (const auto &layer : title.layers) {
        if (!layer)
            continue;
        GpuLayerPlan layer_plan = build_layer_plan(*layer);
        plan.has_gpu_migratable_layers = plan.has_gpu_migratable_layers || layer_plan.can_render_geometry_on_gpu;
        plan.requires_cpu_raster_pass = plan.requires_cpu_raster_pass || layer_plan.requires_cpu_raster;
        plan.layers.push_back(std::move(layer_plan));
    }

    plan.incremental_steps.emplace_back("Keep the Cairo renderer as a compatibility fallback while routing texture allocation, upload, and OBS draw through GpuTextureFrame/ObsGpuRenderPipeline.");
    plan.incremental_steps.emplace_back("Next migrate SolidRect/Shape layers to shader quads with transform, opacity, fill, gradient, and blend uniforms.");
    plan.incremental_steps.emplace_back("Then cache Image layers as gs_texture objects and composite them with the same textured-quad path.");
    plan.incremental_steps.emplace_back("Finally add text glyph atlas and post-process passes for blur/shadow/particles/3D transforms without changing OBS source registration.");
    return plan;
}

bool ObsGpuRenderPipeline::render_texture(const GpuTextureFrame &frame) const
{
    gs_texture_t *texture = frame.texture();
    if (!texture)
        return false;

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!effect)
        return false;

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image)
        return false;

    gs_effect_set_texture(image, texture);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, 0, 0);
    return true;
}

std::string layer_type_name(LayerType type)
{
    switch (type) {
    case LayerType::Text: return "Text";
    case LayerType::SolidRect: return "SolidRect";
    case LayerType::Image: return "Image";
    case LayerType::Shape: return "Shape";
    case LayerType::Clock: return "Clock";
    case LayerType::Ticker: return "Ticker";
    default: return "Unknown";
    }
}

} // namespace obsgs
