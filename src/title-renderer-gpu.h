/*
 * title-renderer-gpu.h
 *
 * OBS-native GPU renderer for Graphics Studio titles.
 *
 * The public API intentionally stays inside libobs' gs_* abstraction so the
 * plugin remains compatible with OBS' Direct3D/OpenGL/Metal backends.  CPU
 * 2-D raster backends such as Cairo/Pango are not part of this renderer; layer
 * drawing, transforms, blending, and effects are expressed as GPU passes.
 */

#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>
#include <QImage>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct Layer;
struct Title;

enum class LayerType;

namespace obsgs {

enum class GpuPipelineStage {
    SolidGeometryShader,
    TextureShader,
    TextAtlasShader,
    EffectShader,
    CompositePass,
    Future3DPass,
};

struct GpuLayerPlan {
    std::string layer_id;
    std::string layer_name;
    std::string layer_type;
    bool gpu_composited = true;
    bool needs_texture_asset = false;
    bool needs_text_atlas = false;
    bool uses_animated_transform = false;
    bool uses_effect_pass = false;
    std::vector<GpuPipelineStage> stages;
    std::vector<std::string> migration_notes;
};

struct GpuTitlePlan {
    uint32_t width = 0;
    uint32_t height = 0;
    bool has_gpu_geometry_layers = false;
    bool has_gpu_texture_layers = false;
    bool has_gpu_text_layers = false;
    std::vector<GpuLayerPlan> layers;
    std::vector<std::string> eliminated_cpu_paths;
    std::vector<std::string> incremental_steps;
};

struct LayerAsset {
    QImage image;
    double origin_x = 0.5;
    double origin_y = 0.5;
    double width = 0.0;
    double height = 0.0;
};

class GpuTextureFrame {
public:
    GpuTextureFrame() = default;
    ~GpuTextureFrame();

    GpuTextureFrame(const GpuTextureFrame &) = delete;
    GpuTextureFrame &operator=(const GpuTextureFrame &) = delete;

    bool ensure_dynamic_bgra(uint32_t width, uint32_t height);
    bool upload_bgra_asset(const uint8_t *pixels, uint32_t linesize);
    void reset();

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
    bool render_title(const Title &title, double time_seconds);
    void reset();

private:
    GpuTextureFrame *texture_for_image_layer(const Layer &layer, double opacity);
    GpuTextureFrame *texture_for_raster_layer(const Layer &layer, const LayerAsset &asset, double opacity);
    std::unordered_map<std::string, std::unique_ptr<GpuTextureFrame>> image_textures_;
    std::unordered_map<std::string, std::unique_ptr<GpuTextureFrame>> raster_textures_;
};

QImage render_title_to_qimage(const Title &title, double time_seconds);
std::string layer_type_name(LayerType type);

} // namespace obsgs
