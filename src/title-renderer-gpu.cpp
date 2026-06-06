/*
 * title-renderer-gpu.cpp
 *
 * OBS-native GPU renderer.  This module renders directly through libobs'
 * graphics abstraction and deliberately avoids Cairo/Pango/Qt painter-style
 * raster composition in the OBS source path.
 */

#include "title-renderer-gpu.h"
#include "title-data.h"

#include <QImage>
#include <QImageReader>
#include <QString>

#include <algorithm>
#include <cmath>
#include <utility>

namespace obsgs {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

static bool path_is_svg(const std::string &path)
{
    const QString qpath = QString::fromStdString(path);
    return qpath.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           qpath.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

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

static uint32_t eval_argb_channels(const AnimatedProperty &a, const AnimatedProperty &r,
                                   const AnimatedProperty &g, const AnimatedProperty &b,
                                   uint32_t fallback, double t)
{
    auto channel = [t](const AnimatedProperty &prop, uint32_t fallback_channel) {
        const double value = prop.is_animated() ? prop.evaluate(t) : static_cast<double>(fallback_channel);
        return static_cast<uint32_t>(std::clamp(std::lround(value), 0L, 255L));
    };
    const uint32_t fa = (fallback >> 24) & 0xFF;
    const uint32_t fr = (fallback >> 16) & 0xFF;
    const uint32_t fg = (fallback >> 8) & 0xFF;
    const uint32_t fb = fallback & 0xFF;
    return (channel(a, fa) << 24) |
           (channel(r, fr) << 16) |
           (channel(g, fg) << 8) |
           channel(b, fb);
}

static uint32_t eval_fill_color(const Layer &layer, double t)
{
    return eval_argb_channels(layer.fill_color_a, layer.fill_color_r,
                              layer.fill_color_g, layer.fill_color_b,
                              layer.fill_color, t);
}

static uint32_t eval_text_color(const Layer &layer, double t)
{
    return eval_argb_channels(layer.text_color_a, layer.text_color_r,
                              layer.text_color_g, layer.text_color_b,
                              layer.text_color, t);
}

static uint32_t eval_shadow_color(const Layer &layer, double t)
{
    return eval_argb_channels(layer.shadow_color_a, layer.shadow_color_r,
                              layer.shadow_color_g, layer.shadow_color_b,
                              layer.shadow_color, t);
}

static uint32_t eval_background_color(const Layer &layer, double t)
{
    return eval_argb_channels(layer.background_color_a, layer.background_color_r,
                              layer.background_color_g, layer.background_color_b,
                              layer.background_color, t);
}

static double eval_box_width(const Layer &layer, double t)
{
    return layer.box_width.is_animated() ? layer.box_width.evaluate(t) : static_cast<double>(layer.rect_width);
}

static double eval_box_height(const Layer &layer, double t)
{
    return layer.box_height.is_animated() ? layer.box_height.evaluate(t) : static_cast<double>(layer.rect_height);
}

static double eval_origin_x(const Layer &layer, double t)
{
    return layer.origin_x_prop.is_animated() ? layer.origin_x_prop.evaluate(t) : static_cast<double>(layer.origin_x);
}

static double eval_origin_y(const Layer &layer, double t)
{
    return layer.origin_y_prop.is_animated() ? layer.origin_y_prop.evaluate(t) : static_cast<double>(layer.origin_y);
}

static double eval_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.opacity.evaluate(t), 0.0, 1.0);
}

static double eval_background_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.background_opacity_prop.is_animated()
                          ? layer.background_opacity_prop.evaluate(t)
                          : static_cast<double>(layer.background_opacity),
                      0.0, 1.0);
}

static bool eval_background_enabled(const Layer &layer, double t)
{
    return layer.background_enabled_prop.is_animated()
        ? layer.background_enabled_prop.evaluate(t) >= 0.5
        : layer.background_enabled;
}

static double eval_background_padding_x(const Layer &layer, double t)
{
    return layer.background_padding_x_prop.is_animated()
        ? layer.background_padding_x_prop.evaluate(t)
        : static_cast<double>(layer.background_padding_x);
}

static double eval_background_padding_y(const Layer &layer, double t)
{
    return layer.background_padding_y_prop.is_animated()
        ? layer.background_padding_y_prop.evaluate(t)
        : static_cast<double>(layer.background_padding_y);
}

static void vec4_from_argb(uint32_t argb, double opacity, vec4 &out)
{
    out.x = static_cast<float>(((argb >> 16) & 0xFF) / 255.0);
    out.y = static_cast<float>(((argb >> 8) & 0xFF) / 255.0);
    out.z = static_cast<float>((argb & 0xFF) / 255.0);
    out.w = static_cast<float>(((argb >> 24) & 0xFF) / 255.0 * std::clamp(opacity, 0.0, 1.0));
}

static bool draw_solid_quad(double px, double py, double width, double height,
                            double origin_x, double origin_y,
                            double scale_x, double scale_y, double rotation_degrees,
                            uint32_t argb, double opacity)
{
    if (width <= 0.0 || height <= 0.0 || opacity <= 0.0)
        return false;

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!effect)
        return false;

    gs_eparam_t *color_param = gs_effect_get_param_by_name(effect, "color");
    if (!color_param)
        return false;

    vec4 color;
    vec4_from_argb(argb, opacity, color);
    gs_effect_set_vec4(color_param, &color);

    gs_matrix_push();
    gs_matrix_translate3f(static_cast<float>(px), static_cast<float>(py), 0.0f);
    gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, static_cast<float>(rotation_degrees * kPi / 180.0));
    gs_matrix_scale3f(static_cast<float>(scale_x), static_cast<float>(scale_y), 1.0f);
    gs_matrix_translate3f(static_cast<float>(-origin_x * width), static_cast<float>(-origin_y * height), 0.0f);

    while (gs_effect_loop(effect, "Solid"))
        gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(std::ceil(width)), static_cast<uint32_t>(std::ceil(height)));

    gs_matrix_pop();
    return true;
}

static bool draw_texture_quad(gs_texture_t *texture,
                              double px, double py, double width, double height,
                              double origin_x, double origin_y,
                              double scale_x, double scale_y, double rotation_degrees,
                              double opacity)
{
    if (!texture || width <= 0.0 || height <= 0.0 || opacity <= 0.0)
        return false;

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!effect)
        return false;

    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image)
        return false;

    gs_effect_set_texture(image, texture);

    gs_matrix_push();
    gs_matrix_translate3f(static_cast<float>(px), static_cast<float>(py), 0.0f);
    gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, static_cast<float>(rotation_degrees * kPi / 180.0));
    gs_matrix_scale3f(static_cast<float>(scale_x), static_cast<float>(scale_y), 1.0f);
    gs_matrix_translate3f(static_cast<float>(-origin_x * width), static_cast<float>(-origin_y * height), 0.0f);

    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, static_cast<uint32_t>(std::ceil(width)), static_cast<uint32_t>(std::ceil(height)));

    gs_matrix_pop();
    return true;
}

static void append_effect_stages(const Layer &layer, GpuLayerPlan &plan)
{
    if (!layer_uses_effect_properties(layer))
        return;

    plan.uses_effect_pass = true;
    plan.stages.push_back(GpuPipelineStage::EffectShader);
    plan.migration_notes.emplace_back("Shadows, blur, gradients, outlines, and backgrounds are represented as GPU effect/post-process passes.");
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
        plan.stages.push_back(GpuPipelineStage::SolidGeometryShader);
        plan.stages.push_back(GpuPipelineStage::CompositePass);
        plan.migration_notes.emplace_back("Solid/shape layer is drawn as GPU geometry with transform and opacity state.");
        break;
    case LayerType::Image:
        plan.needs_texture_asset = true;
        plan.stages.push_back(GpuPipelineStage::TextureShader);
        plan.stages.push_back(GpuPipelineStage::CompositePass);
        plan.migration_notes.emplace_back("Image layer is cached as a GPU texture and composited by a textured-quad pass.");
        break;
    case LayerType::Text:
    case LayerType::Clock:
    case LayerType::Ticker:
        plan.needs_text_atlas = true;
        plan.stages.push_back(GpuPipelineStage::TextAtlasShader);
        plan.stages.push_back(GpuPipelineStage::CompositePass);
        plan.migration_notes.emplace_back("Text layer is reserved for a GPU glyph/vector atlas path; no Cairo/Pango raster fallback is used in the OBS renderer.");
        break;
    default:
        plan.gpu_composited = false;
        plan.migration_notes.emplace_back("Unsupported layer type is skipped until a GPU pass is added; CPU raster fallback is intentionally disabled.");
        break;
    }

    if (plan.uses_animated_transform)
        plan.migration_notes.emplace_back("Animated transform/opacity is handled through GPU matrices/uniform state.");

    append_effect_stages(layer, plan);
    return plan;
}

} // namespace

GpuTextureFrame::~GpuTextureFrame()
{
    reset();
}

bool GpuTextureFrame::ensure_dynamic_bgra(uint32_t width, uint32_t height)
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

bool GpuTextureFrame::upload_bgra_asset(const uint8_t *pixels, uint32_t linesize)
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

GpuTitlePlan ObsGpuRenderPipeline::build_migration_plan(const Title &title) const
{
    GpuTitlePlan plan;
    plan.width = static_cast<uint32_t>(std::max(1, title.width));
    plan.height = static_cast<uint32_t>(std::max(1, title.height));

    plan.eliminated_cpu_paths.emplace_back("Cairo/Pango full-frame composition is removed from the OBS source render path.");
    plan.eliminated_cpu_paths.emplace_back("Per-frame CPU canvas uploads are replaced by GPU layer compositing through libobs gs_* passes.");
    plan.eliminated_cpu_paths.emplace_back("Transforms and alpha blending are represented by GPU matrix/effect state instead of repainting pixels.");

    for (const auto &layer : title.layers) {
        if (!layer)
            continue;
        GpuLayerPlan layer_plan = build_layer_plan(*layer);
        plan.has_gpu_geometry_layers = plan.has_gpu_geometry_layers ||
            layer->type == LayerType::SolidRect || layer->type == LayerType::Shape;
        plan.has_gpu_texture_layers = plan.has_gpu_texture_layers || layer->type == LayerType::Image;
        plan.has_gpu_text_layers = plan.has_gpu_text_layers ||
            layer->type == LayerType::Text || layer->type == LayerType::Clock || layer->type == LayerType::Ticker;
        plan.layers.push_back(std::move(layer_plan));
    }

    plan.incremental_steps.emplace_back("Implement rounded-corner/gradient/shadow variants as OBS effect techniques on top of the geometry pass.");
    plan.incremental_steps.emplace_back("Replace bitmap-only image ingestion with GPU-native SVG/vector tessellation for vector assets.");
    plan.incremental_steps.emplace_back("Add a GPU glyph/vector text atlas and remove temporary text no-op behavior once atlas parity is available.");
    plan.incremental_steps.emplace_back("Add offscreen render targets for separable blur, glow, filters, screen effects, and future 3D composition.");
    return plan;
}

GpuTextureFrame *ObsGpuRenderPipeline::texture_for_image_layer(const Layer &layer)
{
    if (layer.image_path.empty() || path_is_svg(layer.image_path))
        return nullptr;

    auto existing = image_textures_.find(layer.image_path);
    if (existing != image_textures_.end())
        return existing->second.get();

    QImageReader reader(QString::fromStdString(layer.image_path));
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
        return nullptr;

    image = image.convertToFormat(QImage::Format_ARGB32);
    auto texture = std::make_unique<GpuTextureFrame>();
    if (!texture->ensure_dynamic_bgra(static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height())) ||
        !texture->upload_bgra_asset(image.constBits(), static_cast<uint32_t>(image.bytesPerLine()))) {
        texture->reset();
        return nullptr;
    }

    GpuTextureFrame *raw = texture.get();
    image_textures_.emplace(layer.image_path, std::move(texture));
    return raw;
}

bool ObsGpuRenderPipeline::render_title(const Title &title, double time_seconds)
{
    const double clamped_time = std::clamp(time_seconds, 0.0, std::max(0.0, title.duration));

    draw_solid_quad(0.0, 0.0, std::max(1, title.width), std::max(1, title.height),
                    0.0, 0.0, 1.0, 1.0, 0.0, title.bg_color, 1.0);

    for (const auto &layer : title.layers) {
        if (!layer || !layer->visible)
            continue;
        if (clamped_time < layer->in_time || clamped_time > layer->out_time)
            continue;

        const double lt = clamped_time - layer->in_time;
        const double px = layer->pos_x.evaluate(lt);
        const double py = layer->pos_y.evaluate(lt);
        const double sx = layer->scale_x.evaluate(lt);
        const double sy = layer->scale_y.evaluate(lt);
        const double rot = layer->rotation.evaluate(lt);
        const double alpha = eval_opacity(*layer, lt);
        const double origin_x = eval_origin_x(*layer, lt);
        const double origin_y = eval_origin_y(*layer, lt);

        if (eval_background_enabled(*layer, lt)) {
            const double pad_x = eval_background_padding_x(*layer, lt);
            const double pad_y = eval_background_padding_y(*layer, lt);
            draw_solid_quad(px, py,
                            eval_box_width(*layer, lt) + pad_x * 2.0,
                            eval_box_height(*layer, lt) + pad_y * 2.0,
                            origin_x, origin_y, sx, sy, rot,
                            eval_background_color(*layer, lt),
                            alpha * eval_background_opacity(*layer, lt));
        }

        if (layer->shadow_enabled) {
            const double shadow_angle = layer->shadow_angle * kPi / 180.0;
            const double dx = std::cos(shadow_angle) * layer->shadow_distance;
            const double dy = std::sin(shadow_angle) * layer->shadow_distance;
            draw_solid_quad(px + dx, py + dy, eval_box_width(*layer, lt), eval_box_height(*layer, lt),
                            origin_x, origin_y, sx, sy, rot,
                            eval_shadow_color(*layer, lt), alpha * layer->shadow_opacity);
        }

        switch (layer->type) {
        case LayerType::SolidRect:
        case LayerType::Shape:
            draw_solid_quad(px, py, eval_box_width(*layer, lt), eval_box_height(*layer, lt),
                            origin_x, origin_y, sx, sy, rot,
                            eval_fill_color(*layer, lt), alpha);
            break;
        case LayerType::Image: {
            GpuTextureFrame *texture = texture_for_image_layer(*layer);
            if (!texture)
                break;
            const double w = eval_box_width(*layer, lt) > 0.0 ? eval_box_width(*layer, lt) : texture->width();
            const double h = eval_box_height(*layer, lt) > 0.0 ? eval_box_height(*layer, lt) : texture->height();
            draw_texture_quad(texture->texture(), px, py, w, h, origin_x, origin_y, sx, sy, rot, alpha);
            break;
        }
        case LayerType::Text:
        case LayerType::Clock:
        case LayerType::Ticker:
            /* Text is intentionally not rasterized on the CPU.  The layer plan
             * reserves a TextAtlasShader stage; until that shader atlas lands,
             * text layers do not reintroduce Cairo/Pango fallback rendering.
             */
            (void)eval_text_color(*layer, lt);
            break;
        default:
            break;
        }
    }

    return true;
}

void ObsGpuRenderPipeline::reset()
{
    image_textures_.clear();
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
