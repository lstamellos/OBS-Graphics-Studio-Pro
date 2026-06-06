/*
 * title-renderer-gpu.h
 *
 * OBS-compatible GPU rendering transition layer for Graphics Studio titles.
 *
 * This file intentionally keeps the first integration non-breaking: the
 * existing Cairo/Pango renderer can still produce a BGRA frame, while texture
 * lifetime, upload, final OBS draw, and migration analysis now live behind a
 * GPU pipeline abstraction that can grow into shader/effect passes.
 */

#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>

#include <cstdint>
#include <string>
#include <vector>

struct Layer;
struct Title;

enum class LayerType;

namespace obsgs {

enum class GpuPipelineStage {
    CpuRasterUpload,
    SolidGeometryShader,
    TextureShader,
    TextAtlasShader,
    PostProcessShader,
};

struct GpuLayerPlan {
    std::string layer_id;
    std::string layer_name;
    std::string layer_type;
    bool can_render_geometry_on_gpu = false;
    bool requires_cpu_raster = true;
    bool uses_animated_transform = false;
    bool uses_gpu_blending = true;
    std::vector<GpuPipelineStage> stages;
    std::vector<std::string> migration_notes;
};

struct GpuTitlePlan {
    uint32_t width = 0;
    uint32_t height = 0;
    bool requires_cpu_raster_pass = false;
    bool has_gpu_migratable_layers = false;
    std::vector<GpuLayerPlan> layers;
    std::vector<std::string> bottlenecks;
    std::vector<std::string> incremental_steps;
};

class GpuTextureFrame {
public:
    GpuTextureFrame() = default;
    ~GpuTextureFrame();

    GpuTextureFrame(const GpuTextureFrame &) = delete;
    GpuTextureFrame &operator=(const GpuTextureFrame &) = delete;

    bool ensure_size(uint32_t width, uint32_t height);
    bool upload_bgra(const uint8_t *pixels, uint32_t linesize);
    void reset();
    void render_default() const;

    gs_texture_t *texture() const { return texture_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    gs_texture_t *texture_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

class ObsGpuRenderPipeline {
public:
    GpuTitlePlan build_migration_plan(const Title &title) const;
    bool render_texture(const GpuTextureFrame &frame) const;
};

std::string layer_type_name(LayerType type);

} // namespace obsgs
