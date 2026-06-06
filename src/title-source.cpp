/*
 * title-source.cpp
 *
 * OBS source: renders a Title to an OBS texture via Cairo.
 *
 * Cairo renders to a CPU RGBA buffer; we upload it to a gs_texture
 * each frame (or only on change for static titles).
 *
 * Build dependency: cairo, pango, pangocairo
 */

#include "title-source.h"
#include "title-data.h"
#include "plugin-main.h"
#include "title-localization.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSvgRenderer>
#include <QString>
#include <QStringList>
#include <QLocale>
#include <QPointF>
#include <QPainter>
#include <QBrush>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QTextLayout>
#include <QTextOption>
#include <QDateTime>
#include <QTransform>
#include <QColor>
#include <QLinearGradient>
#include <QRadialGradient>

#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <mutex>
#include <limits>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr uint32_t kMaxSourceDimension = 16384;

static uint32_t clamped_source_dimension(int value)
{
    return static_cast<uint32_t>(std::clamp(value, 1, static_cast<int>(kMaxSourceDimension)));
}

static bool image_path_is_svg(const QString &path)
{
    return path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

static QImage load_layer_image(const QString &path, const QSize &fallback_size = QSize())
{
    if (image_path_is_svg(path)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QImage();

        QSize size = fallback_size.isValid() && !fallback_size.isEmpty()
            ? fallback_size
            : renderer.defaultSize();
        if (!size.isValid() || size.isEmpty())
            size = renderer.viewBox().size();
        if (!size.isValid() || size.isEmpty())
            size = QSize(256, 256);

        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter);
        return image;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    return reader.read();
}
}

/* ══════════════════════════════════════════════════════════════════
 *  Source private data
 * ══════════════════════════════════════════════════════════════════ */
struct TitleSourceData {
    obs_source_t *source  = nullptr;

    /* Settings */
    std::string title_id;
    bool        loop         = true;
    float       speed        = 1.0f;
    bool        auto_advance = false;  /* future: playlist mode */

    enum class CuePhase { FreeRun, IntroLoop, OutroThenIntro, OutroOnly };

    /* Playback state */
    double      playhead     = 0.0;    /* seconds */
    bool        playing      = true;
    bool        playback_reverse = false;
    uint64_t    seen_cue_revision = 0;
    CuePhase    cue_phase    = CuePhase::FreeRun;
    int         active_cue_row = -1;
    std::chrono::steady_clock::time_point last_tick;
    std::chrono::steady_clock::time_point last_clock_refresh;
    bool        first_tick   = true;
    bool        waiting_for_cue = true;

    /* GPU texture */
    std::mutex    texture_mutex;
    gs_texture_t *texture    = nullptr;
    uint32_t      tex_w      = 0;
    uint32_t      tex_h      = 0;

    /* CPU render buffer */
    std::vector<uint8_t> pixel_buf;   /* BGRA row-major */

    /* Dirty flag – avoid re-uploading unchanged frames */
    bool dirty = true;
    uint64_t seen_store_revision = 0;
};


static bool layer_has_animation(const Layer &layer)
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
           layer.origin_y_prop.is_animated() ||
           layer.shadow_enabled_prop.is_animated() ||
           layer.shadow_opacity_prop.is_animated() ||
           layer.shadow_distance_prop.is_animated() ||
           layer.shadow_angle_prop.is_animated() ||
           layer.shadow_blur_prop.is_animated() ||
           layer.shadow_spread_prop.is_animated() ||
           layer.shadow_color_a.is_animated() ||
           layer.shadow_color_r.is_animated() ||
           layer.shadow_color_g.is_animated() ||
           layer.shadow_color_b.is_animated() ||
           layer.background_enabled_prop.is_animated() ||
           layer.background_opacity_prop.is_animated() ||
           layer.background_padding_x_prop.is_animated() ||
           layer.background_padding_y_prop.is_animated() ||
           layer.background_corner_radius_prop.is_animated() ||
           layer.background_color_a.is_animated() ||
           layer.background_color_r.is_animated() ||
           layer.background_color_g.is_animated() ||
           layer.background_color_b.is_animated() ||
           layer.text_color_a.is_animated() ||
           layer.text_color_r.is_animated() ||
           layer.text_color_g.is_animated() ||
           layer.text_color_b.is_animated() ||
           layer.fill_color_a.is_animated() ||
           layer.fill_color_r.is_animated() ||
           layer.fill_color_g.is_animated() ||
           layer.fill_color_b.is_animated();
}

static bool include_property_bounds(const Layer &layer, const AnimatedProperty &prop,
                                    double &first_time, double &last_time)
{
    if (prop.keyframes.empty()) return false;
    first_time = std::min(first_time, layer.in_time + prop.keyframes.front().time);
    last_time = std::max(last_time, layer.in_time + prop.keyframes.back().time);
    return true;
}

static bool layer_animation_keyframe_bounds(const Layer &layer, double &first_time, double &last_time)
{
    bool has_bounds = false;
    has_bounds |= include_property_bounds(layer, layer.pos_x, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.pos_y, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale_x, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale_y, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.rotation, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.opacity, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.box_width, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.box_height, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_distance_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_angle_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_blur_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_spread_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_padding_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_padding_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_corner_radius_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_b, first_time, last_time);
    return has_bounds;
}

static bool title_has_clock_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Clock;
                       });
}

static bool title_has_ticker_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Ticker;
                       });
}

static bool title_has_animation(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer_has_animation(*layer);
                       });
}

static std::vector<std::shared_ptr<Layer>> order_exposed_text_layers(
    const std::vector<std::shared_ptr<Layer>> &exposed,
    const std::vector<std::string> &column_order)
{
    if (column_order.empty())
        return exposed;

    std::vector<std::shared_ptr<Layer>> ordered;
    ordered.reserve(exposed.size());
    for (const auto &layer_id : column_order) {
        auto it = std::find_if(exposed.begin(), exposed.end(),
                               [&](const std::shared_ptr<Layer> &layer) {
                                   return layer && layer->id == layer_id;
                               });
        if (it != exposed.end())
            ordered.push_back(*it);
    }
    for (const auto &layer : exposed) {
        if (!layer) continue;
        auto it = std::find_if(ordered.begin(), ordered.end(),
                               [&](const std::shared_ptr<Layer> &ordered_layer) {
                                   return ordered_layer && ordered_layer->id == layer->id;
                               });
        if (it == ordered.end())
            ordered.push_back(layer);
    }
    return ordered;
}

static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    if (!title) return exposed;
    for (const auto &layer : title->layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return order_exposed_text_layers(exposed, title->live_text_column_order);
}


static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const Title &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return order_exposed_text_layers(exposed, title.live_text_column_order);
}

static double cue_persistence_hold_time(const Title &title)
{
    if (title.playback_mode == 1)
        return std::clamp(title.loop_end, title.loop_start, title.duration);
    if (title.playback_mode == 2)
        return std::clamp(title.pause_time, 0.0, title.duration);
    return std::clamp(title.duration, 0.0, title.duration);
}

static void clear_cue_persistence_transition(const std::shared_ptr<Title> &title)
{
    if (!title || !title->cue_persistence_transition) return;
    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.clear();
    TitleDataStore::instance().touch_runtime_change();
}

static int exposed_text_layer_index(const std::vector<std::shared_ptr<Layer>> &exposed, const std::shared_ptr<Layer> &layer)
{
    for (int i = 0; i < (int)exposed.size(); ++i) {
        if (exposed[i] == layer)
            return i;
    }
    return -1;
}



static void apply_live_text_row(const std::shared_ptr<Title> &title, int row)
{
    if (!title || row < 0 || row >= (int)title->live_text_rows.size()) return;
    auto exposed = exposed_text_layers(title);
    for (int col = 0; col < (int)exposed.size() && col < (int)title->live_text_rows[row].size(); ++col)
        exposed[col]->text_content = title->live_text_rows[row][col];
}

/* ══════════════════════════════════════════════════════════════════
 *  Helper: ARGB uint32 → r,g,b,a doubles (0..1)
 * ══════════════════════════════════════════════════════════════════ */
static void unpack_color(uint32_t c,
                          double &r, double &g, double &b, double &a)
{
    a = ((c >> 24) & 0xFF) / 255.0;
    r = ((c >> 16) & 0xFF) / 255.0;
    g = ((c >>  8) & 0xFF) / 255.0;
    b = ((c >>  0) & 0xFF) / 255.0;
}


static QFont font_for_layer(const Layer &layer);
static QString display_text_for_style(const Layer &layer);
static QString overflow_layout_text(const QString &text, const Layer &layer);

static bool is_text_box_auto_size_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock;
}

static double natural_text_width(const Layer &layer)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    QFontMetricsF metrics(font_for_layer(layer));
    QString text = display_text_for_style(layer);
    if (layer.text_overflow_mode == 2)
        text = overflow_layout_text(text, layer);

    double width = 1.0;
    for (const QString &line : text.split('\n'))
        width = std::max(width, static_cast<double>(metrics.horizontalAdvance(line)));
    return std::ceil(width);
}

static double natural_text_height(const Layer &layer, double width)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    QFont font = font_for_layer(layer);
    QFontMetricsF metrics(font);
    QString text = display_text_for_style(layer);
    if (layer.text_overflow_mode == 2)
        text = overflow_layout_text(text, layer);

    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? QTextOption::WrapAtWordBoundaryOrAnywhere
                           : QTextOption::NoWrap);

    double total_height = 0.0;
    const double leading = std::clamp((double)layer.text_leading, -200.0, 500.0);
    bool first_line = true;
    for (const QString &paragraph : text.split('\n')) {
        if (paragraph.isEmpty()) {
            if (!first_line) total_height += leading;
            total_height += metrics.lineSpacing();
            first_line = false;
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(layer.text_overflow_mode == 0 ? std::max(1.0, width) : 1000000.0);
            if (!first_line) total_height += leading;
            total_height += line.height();
            first_line = false;
            if (layer.text_overflow_mode != 0) break;
        }
        layout.endLayout();
    }
    return std::ceil(std::max(1.0, total_height));
}

static double eval_box_width(const Layer &layer, double t)
{
    double width = layer.box_width.is_animated()
        ? layer.box_width.evaluate(t)
        : static_cast<double>(layer.rect_width);
    if (layer.text_box_width_to_text && is_text_box_auto_size_layer(layer))
        width = std::min(natural_text_width(layer), std::max(1.0, (double)layer.max_text_box_width));
    return std::max(0.0, width);
}

static double eval_box_height(const Layer &layer, double t)
{
    double height = layer.box_height.is_animated()
        ? layer.box_height.evaluate(t)
        : static_cast<double>(layer.rect_height);
    if (layer.text_box_height_to_text && is_text_box_auto_size_layer(layer)) {
        const double width = eval_box_width(layer, t);
        height = std::min(natural_text_height(layer, width), std::max(1.0, (double)layer.max_text_box_height));
    }
    return std::max(0.0, height);
}

static int shadow_pass_count(double blur)
{
    const int passes = static_cast<int>(std::ceil(blur / 3.0));
    return passes < 1 ? 1 : passes;
}

static double eval_origin_x(const Layer &layer, double t)
{
    return std::clamp(layer.origin_x_prop.is_animated()
                          ? layer.origin_x_prop.evaluate(t)
                          : (double)layer.origin_x,
                      0.0, 1.0);
}

static double eval_origin_y(const Layer &layer, double t)
{
    return std::clamp(layer.origin_y_prop.is_animated()
                          ? layer.origin_y_prop.evaluate(t)
                          : (double)layer.origin_y,
                      0.0, 1.0);
}

static int eval_channel(const AnimatedProperty &prop, double fallback, double t)
{
    return (int)std::clamp(std::round(prop.is_animated() ? prop.evaluate(t) : fallback),
                           0.0, 255.0);
}

static uint32_t eval_text_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.text_color_a, (layer.text_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.text_color_r, (layer.text_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.text_color_g, (layer.text_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.text_color_b, layer.text_color & 0xFF, t);
}

static uint32_t eval_fill_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.fill_color_a, (layer.fill_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.fill_color_r, (layer.fill_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.fill_color_g, (layer.fill_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.fill_color_b, layer.fill_color & 0xFF, t);
}

static QColor gradient_color_with_opacity(uint32_t argb, double gradient_opacity, double stop_opacity)
{
    QColor color((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
    color.setAlphaF(std::clamp((double)color.alphaF() * gradient_opacity * stop_opacity, 0.0, 1.0));
    return color;
}

static QBrush gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    const double opacity = std::clamp((double)layer.gradient_opacity * layer_opacity, 0.0, 1.0);
    const double cx = box.left() + std::clamp((double)layer.gradient_center_x, 0.0, 1.0) * box.width();
    const double cy = box.top() + std::clamp((double)layer.gradient_center_y, 0.0, 1.0) * box.height();
    const double scale = std::clamp((double)layer.gradient_scale, 0.01, 10.0);
    const double start_pos = std::clamp((double)layer.gradient_start_pos, 0.0, 1.0);
    const double end_pos = std::clamp((double)layer.gradient_end_pos, 0.0, 1.0);
    if (layer.gradient_type == 1) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * scale;
        QRadialGradient gradient(QPointF(cx, cy), std::max(1.0, radius),
                                 QPointF(box.left() + std::clamp((double)layer.gradient_focal_x, 0.0, 1.0) * box.width(),
                                         box.top() + std::clamp((double)layer.gradient_focal_y, 0.0, 1.0) * box.height()));
        gradient.setColorAt(start_pos, gradient_color_with_opacity(layer.gradient_start_color, opacity, layer.gradient_start_opacity));
        gradient.setColorAt(end_pos, gradient_color_with_opacity(layer.gradient_end_color, opacity, layer.gradient_end_opacity));
        return QBrush(gradient);
    }
    const double length = std::hypot(box.width(), box.height()) * 0.5 * scale;
    const double angle = layer.gradient_angle * kPi / 180.0;
    const double dx = std::cos(angle) * length;
    const double dy = std::sin(angle) * length;
    QLinearGradient gradient(QPointF(cx - dx, cy - dy), QPointF(cx + dx, cy + dy));
    gradient.setColorAt(start_pos, gradient_color_with_opacity(layer.gradient_start_color, opacity, layer.gradient_start_opacity));
    gradient.setColorAt(end_pos, gradient_color_with_opacity(layer.gradient_end_color, opacity, layer.gradient_end_opacity));
    return QBrush(gradient);
}

static cairo_pattern_t *create_fill_gradient_pattern(const Layer &layer, double x, double y, double w, double h, double layer_alpha)
{
    const double opacity = std::clamp((double)layer.gradient_opacity * layer_alpha, 0.0, 1.0);
    const double cx = x + std::clamp((double)layer.gradient_center_x, 0.0, 1.0) * w;
    const double cy = y + std::clamp((double)layer.gradient_center_y, 0.0, 1.0) * h;
    const double scale = std::clamp((double)layer.gradient_scale, 0.01, 10.0);
    const double start_pos = std::clamp((double)layer.gradient_start_pos, 0.0, 1.0);
    const double end_pos = std::clamp((double)layer.gradient_end_pos, 0.0, 1.0);
    cairo_pattern_t *pattern = nullptr;
    if (layer.gradient_type == 1) {
        const double radius = std::max(w, h) * 0.5 * scale;
        const double fx = x + std::clamp((double)layer.gradient_focal_x, 0.0, 1.0) * w;
        const double fy = y + std::clamp((double)layer.gradient_focal_y, 0.0, 1.0) * h;
        pattern = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, std::max(1.0, radius));
    } else {
        const double length = std::hypot(w, h) * 0.5 * scale;
        const double angle = layer.gradient_angle * kPi / 180.0;
        const double dx = std::cos(angle) * length;
        const double dy = std::sin(angle) * length;
        pattern = cairo_pattern_create_linear(cx - dx, cy - dy, cx + dx, cy + dy);
    }
    auto add_stop = [&](double pos, uint32_t argb, double stop_opacity) {
        QColor color = gradient_color_with_opacity(argb, opacity, stop_opacity);
        cairo_pattern_add_color_stop_rgba(pattern, pos, color.redF(), color.greenF(), color.blueF(), color.alphaF());
    };
    add_stop(start_pos, layer.gradient_start_color, layer.gradient_start_opacity);
    add_stop(end_pos, layer.gradient_end_color, layer.gradient_end_opacity);
    cairo_pattern_set_extend(pattern, CAIRO_EXTEND_PAD);
    return pattern;
}

static bool eval_outline_enabled(const Layer &layer, double)
{
    return layer.outline_enabled;
}

static uint32_t eval_outline_color(const Layer &layer, double)
{
    return layer.stroke_color;
}

static double eval_outline_width(const Layer &layer, double)
{
    return eval_outline_enabled(layer, 0.0) ? std::max(0.0f, layer.stroke_width) : 0.0;
}

static double eval_outline_opacity(const Layer &layer, double)
{
    return std::clamp((double)layer.outline_opacity, 0.0, 1.0);
}

static bool eval_outline_on_front(const Layer &layer, double)
{
    return layer.outline_on_front;
}

static bool eval_outline_antialias(const Layer &layer, double)
{
    return layer.outline_antialias;
}

static cairo_antialias_t outline_cairo_antialias(const Layer &layer)
{
    return layer.outline_antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE;
}

static cairo_line_join_t outline_cairo_join_style(const Layer &layer)
{
    switch (layer.outline_join_style) {
    case 0: return CAIRO_LINE_JOIN_MITER;
    case 2: return CAIRO_LINE_JOIN_BEVEL;
    case 1:
    default: return CAIRO_LINE_JOIN_ROUND;
    }
}

static Qt::PenJoinStyle outline_pen_join_style(const Layer &layer)
{
    switch (layer.outline_join_style) {
    case 0: return Qt::MiterJoin;
    case 2: return Qt::BevelJoin;
    case 1:
    default: return Qt::RoundJoin;
    }
}

static bool eval_shadow_enabled(const Layer &layer, double t)
{
    return layer.shadow_enabled_prop.is_animated()
        ? layer.shadow_enabled_prop.evaluate(t) >= 0.5
        : layer.shadow_enabled;
}

static double eval_shadow_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.shadow_opacity_prop.is_animated() ? layer.shadow_opacity_prop.evaluate(t) : (double)layer.shadow_opacity, 0.0, 1.0);
}

static double eval_shadow_distance(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_distance_prop.is_animated() ? layer.shadow_distance_prop.evaluate(t) : (double)layer.shadow_distance);
}

static double eval_shadow_angle(const Layer &layer, double t)
{
    return layer.shadow_angle_prop.is_animated() ? layer.shadow_angle_prop.evaluate(t) : (double)layer.shadow_angle;
}

static double eval_shadow_blur(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_blur_prop.is_animated() ? layer.shadow_blur_prop.evaluate(t) : (double)layer.shadow_blur);
}

static double eval_shadow_spread(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_spread_prop.is_animated() ? layer.shadow_spread_prop.evaluate(t) : (double)layer.shadow_spread);
}

static uint32_t eval_shadow_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.shadow_color_a, (layer.shadow_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.shadow_color_r, (layer.shadow_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.shadow_color_g, (layer.shadow_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.shadow_color_b, layer.shadow_color & 0xFF, t);
}

static QPointF shadow_offset(const Layer &layer, double t)
{
    double radians = eval_shadow_angle(layer, t) * kPi / 180.0;
    double distance = eval_shadow_distance(layer, t);
    return QPointF(std::cos(radians) * distance,
                   std::sin(radians) * distance);
}

/* ══════════════════════════════════════════════════════════════════
 *  Cairo rendering
 * ══════════════════════════════════════════════════════════════════ */


static QLocale locale_for_text_transform(const QString &text)
{
    QLocale locale;
    for (const QChar ch : text) {
        uint u = ch.unicode();
        if (u >= 0x0370 && u <= 0x03FF)
            return QLocale(QLocale::Greek, QLocale::Greece);
        if (QStringLiteral("ıİşŞğĞçÇ").contains(ch))
            return QLocale(QLocale::Turkish, QLocale::Turkey);
        if (ch == QChar(0x00DF))
            return QLocale(QLocale::German, QLocale::Germany);
    }
    return locale;
}


static QString php_date_format(const QString &format, const QDateTime &date_time)
{
    QString out;
    const QDate date = date_time.date();
    const QTime time = date_time.time();
    for (int i = 0; i < format.size(); ++i) {
        const QChar token = format.at(i);
        if (token == QLatin1Char('\\') && i + 1 < format.size()) {
            out.append(format.at(++i));
            continue;
        }
        switch (token.unicode()) {
        case 'd': out += QString("%1").arg(date.day(), 2, 10, QChar('0')); break;
        case 'D': out += date_time.toString("ddd"); break;
        case 'j': out += QString::number(date.day()); break;
        case 'l': out += date_time.toString("dddd"); break;
        case 'F': out += date_time.toString("MMMM"); break;
        case 'm': out += QString("%1").arg(date.month(), 2, 10, QChar('0')); break;
        case 'M': out += date_time.toString("MMM"); break;
        case 'n': out += QString::number(date.month()); break;
        case 'Y': out += QString::number(date.year()); break;
        case 'y': out += QString("%1").arg(date.year() % 100, 2, 10, QChar('0')); break;
        case 'a': out += (time.hour() < 12 ? "am" : "pm"); break;
        case 'A': out += (time.hour() < 12 ? "AM" : "PM"); break;
        case 'g': { int h = time.hour() % 12; out += QString::number(h == 0 ? 12 : h); break; }
        case 'G': out += QString::number(time.hour()); break;
        case 'h': { int h = time.hour() % 12; out += QString("%1").arg(h == 0 ? 12 : h, 2, 10, QChar('0')); break; }
        case 'H': out += QString("%1").arg(time.hour(), 2, 10, QChar('0')); break;
        case 'i': out += QString("%1").arg(time.minute(), 2, 10, QChar('0')); break;
        case 's': out += QString("%1").arg(time.second(), 2, 10, QChar('0')); break;
        case 'U': out += QString::number(date_time.toSecsSinceEpoch()); break;
        default: out.append(token); break;
        }
    }
    return out;
}

static QString clock_text_for_layer(const Layer &layer)
{
    QString format = QString::fromStdString(layer.clock_format);
    if (format.isEmpty()) format = QStringLiteral("H:i:s");
    return php_date_format(format, QDateTime::currentDateTime());
}

static QString display_text_for_style(const Layer &layer)
{
    QString text = layer.type == LayerType::Clock
        ? clock_text_for_layer(layer)
        : QString::fromStdString(layer.text_content);
    if (layer.text_style == 1)
        return locale_for_text_transform(text).toUpper(text);
    return text;
}

static void apply_text_style_to_font(QFont &font, const Layer &layer)
{
    if (layer.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (layer.text_style == 3 || layer.text_style == 4)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * 0.65)));
}

static QFont font_for_layer(const Layer &layer)
{
    const QString family = QString::fromStdString(layer.font_family);
    const QString style = QString::fromStdString(layer.font_style);
    QFontDatabase fdb;
    QFont font = !style.isEmpty()
        ? fdb.font(family, style, layer.font_size)
        : QFont(family);
    font.setFamily(family);
    font.setPixelSize(layer.font_size);
    if (!style.isEmpty())
        font.setStyleName(style);
    font.setBold(layer.font_bold);
    font.setItalic(layer.font_italic);
    font.setUnderline(layer.text_underline);
    font.setStrikeOut(layer.text_strikethrough);
    font.setKerning(layer.kerning_mode != 2 && layer.font_kerning);
    const float effective_tracking = layer.char_tracking + (layer.kerning_mode == 2 ? layer.manual_kerning : 0.0f);
    font.setLetterSpacing(QFont::AbsoluteSpacing, effective_tracking);
    font.setStretch(std::clamp((int)std::round(layer.char_scale_x * 100.0f), 1, 4000));
    apply_text_style_to_font(font, layer);
    return font;
}

static QPainterPath apply_vertical_character_scale(const QPainterPath &path, const QRectF &rect,
                                                   Qt::Alignment alignment, const Layer &layer)
{
    double scale_y = std::clamp((double)layer.char_scale_y, 0.1, 5.0);
    if (std::abs(scale_y - 1.0) < 0.0001)
        return path;

    QRectF bounds = path.boundingRect();
    double anchor_y = bounds.top();
    if (alignment & Qt::AlignVCenter)
        anchor_y = bounds.center().y();
    else if (alignment & Qt::AlignBottom)
        anchor_y = bounds.bottom();
    else if (!bounds.isEmpty())
        anchor_y = rect.top();

    QTransform xf;
    xf.translate(0.0, anchor_y);
    xf.scale(1.0, scale_y);
    xf.translate(0.0, -anchor_y);
    return xf.map(path);
}

static QRectF text_rect_for_style(const QRectF &rect, const Layer &layer)
{
    if (layer.text_style == 3)
        return rect.adjusted(0.0, 0.0, 0.0, -rect.height() * 0.28);
    if (layer.text_style == 4)
        return rect.adjusted(0.0, rect.height() * 0.28, 0.0, 0.0);
    return rect;
}

static QString overflow_layout_text(const QString &text, const Layer &layer)
{
    if (layer.text_overflow_mode == 2) {
        QString single = text;
        single.replace('\r', ' ');
        single.replace('\n', ' ');
        return single;
    }
    return text;
}

static double horizontal_fit_scale(const QFont &font, const QRectF &rect,
                                   const QString &text, const Layer &layer)
{
    if (layer.text_overflow_mode != 2) return 1.0;
    QFontMetricsF metrics(font);
    const double text_width = static_cast<double>(metrics.horizontalAdvance(overflow_layout_text(text, layer)));
    double natural_width = std::max(1.0, text_width);
    if (natural_width <= rect.width()) return 1.0;
    return std::clamp(rect.width() / natural_width,
                      std::clamp((double)layer.text_fit_min_scale, 0.05, 1.0),
                      1.0);
}

static QPainterPath text_overflow_path(const QFont &font, const QRectF &rect,
                                       Qt::Alignment alignment, const QString &text,
                                       const Layer &layer, double *fit_scale = nullptr)
{
    QPainterPath path;
    QFontMetricsF metrics(font);
    if (layer.text_overflow_mode == 2) {
        QString single = overflow_layout_text(text, layer);
        QRectF bounds = metrics.boundingRect(single);
        double scale = horizontal_fit_scale(font, rect, text, layer);
        if (fit_scale) *fit_scale = scale;
        double visual_width = bounds.width() * scale;
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - visual_width) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - visual_width;
        double y = rect.top() - bounds.top();
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - bounds.height() - bounds.top();
        path.addText(QPointF(0, y), font, single);
        QTransform xf;
        xf.translate(x, 0.0);
        xf.scale(scale, 1.0);
        return xf.map(path);
    }
    if (fit_scale) *fit_scale = 1.0;

    struct Line { QString text; double width = 0.0; double ascent = 0.0; double height = 0.0; };
    std::vector<Line> lines;
    const QStringList paragraphs = text.split('\n');
    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? QTextOption::WrapAtWordBoundaryOrAnywhere
                           : QTextOption::NoWrap);
    for (const QString &paragraph : paragraphs) {
        if (paragraph.isEmpty()) {
            lines.push_back({QString(), 0.0, metrics.ascent(), metrics.lineSpacing()});
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(layer.text_overflow_mode == 0 ? rect.width() : 1000000.0);
            int start = line.textStart();
            int len = line.textLength();
            lines.push_back({paragraph.mid(start, len), line.naturalTextWidth(), line.ascent(), line.height()});
            if (layer.text_overflow_mode != 0) break;
        }
        layout.endLayout();
    }
    double total_height = 0.0;
    const double leading = std::clamp((double)layer.text_leading, -200.0, 500.0);
    for (size_t i = 0; i < lines.size(); ++i) {
        total_height += lines[i].height;
        if (i + 1 < lines.size())
            total_height += leading;
    }
    double y = rect.top();
    if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - total_height) / 2.0;
    else if (alignment & Qt::AlignBottom) y = rect.bottom() - total_height;
    for (const auto &line : lines) {
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line.width) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line.width;
        path.addText(QPointF(x, y + line.ascent), font, line.text);
        y += line.height + leading;
    }
    return path;
}


static double ticker_time_seconds()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

static QStringList ticker_lines(const QString &text)
{
    QString normalized = text;
    normalized.replace('\r', '\n');
    QStringList raw_lines = normalized.split('\n');
    QStringList lines;
    for (const QString &line : raw_lines) {
        if (!line.trimmed().isEmpty())
            lines << line;
    }
    if (lines.isEmpty()) lines << QString();
    return lines;
}

static QPainterPath ticker_text_path(const QFont &font, const QRectF &rect,
                                     Qt::Alignment alignment, const QString &text,
                                     const Layer &layer)
{
    QPainterPath path;
    QFontMetricsF metrics(font);
    const double speed = std::max(1.0, layer.ticker_speed);
    const double now = ticker_time_seconds();

    if (layer.ticker_style == 0) {
        QString single = text;
        single.replace('\r', ' ');
        single.replace('\n', QStringLiteral("     •     "));
        QRectF bounds = metrics.boundingRect(single);
        const double text_w = std::max(1.0, bounds.width());
        const double travel = rect.width() + text_w;
        const double progress = std::fmod(now * speed, travel);
        const double x = layer.ticker_direction == 0
            ? rect.left() - text_w + progress
            : rect.right() - progress;
        double y = rect.top() - bounds.top();
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - bounds.height() - bounds.top();
        path.addText(QPointF(x, y), font, single);
        return path;
    }

    const QStringList lines = ticker_lines(text);
    const int line_count = std::max(1, static_cast<int>(lines.size()));
    const double line_h = std::max(1.0, metrics.lineSpacing() + std::clamp((double)layer.text_leading, -200.0, 500.0));
    if (layer.ticker_style == 1) {
        const double hold = std::max(0.1, layer.ticker_line_hold);
        int idx = (int)std::floor(now / hold) % line_count;
        if (layer.ticker_direction == 0) idx = line_count - 1 - idx;
        QString line = lines.at(idx);
        double line_w = metrics.horizontalAdvance(line);
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line_w) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line_w;
        QRectF bounds = metrics.boundingRect(line);
        double y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        path.addText(QPointF(x, y), font, line);
        return path;
    }

    const double content_h = line_count * line_h;
    const double travel = rect.height() + content_h;
    const double progress = std::fmod(now * speed, travel);
    double y = layer.ticker_direction == 0
        ? rect.top() - content_h + progress
        : rect.bottom() - progress;
    for (const QString &line : lines) {
        double line_w = metrics.horizontalAdvance(line);
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line_w) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line_w;
        path.addText(QPointF(x, y + metrics.ascent()), font, line);
        y += line_h;
    }
    return path;
}

static QColor color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
}

static bool eval_background_enabled(const Layer &layer, double t)
{
    return layer.background_enabled_prop.is_animated()
        ? layer.background_enabled_prop.evaluate(t) >= 0.5
        : layer.background_enabled;
}

static double eval_background_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.background_opacity_prop.is_animated()
                          ? layer.background_opacity_prop.evaluate(t)
                          : (double)layer.background_opacity,
                      0.0, 1.0);
}

static double eval_background_padding_x(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_padding_x_prop.is_animated()
                             ? layer.background_padding_x_prop.evaluate(t)
                             : (double)layer.background_padding_x);
}

static double eval_background_padding_y(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_padding_y_prop.is_animated()
                             ? layer.background_padding_y_prop.evaluate(t)
                             : (double)layer.background_padding_y);
}

static double eval_background_corner_radius(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_corner_radius_prop.is_animated()
                             ? layer.background_corner_radius_prop.evaluate(t)
                             : (double)layer.background_corner_radius);
}

static uint32_t eval_background_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.background_color_a, (layer.background_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.background_color_r, (layer.background_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.background_color_g, (layer.background_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.background_color_b, layer.background_color & 0xFF, t);
}

static QColor evaluated_background_color(const Layer &layer, double t)
{
    QColor color = color_from_argb(eval_background_color(layer, t));
    color.setAlphaF(std::clamp((double)color.alphaF() * eval_background_opacity(layer, t), 0.0, 1.0));
    return color;
}

static void render_layer_text(cairo_t *cr, const Layer &layer, double t,
                               int canvas_w, int canvas_h)
{
    (void)canvas_w;
    (void)canvas_h;

    double px = layer.pos_x.evaluate(t);
    double py = layer.pos_y.evaluate(t);
    double sx = layer.scale_x.evaluate(t);
    double sy = layer.scale_y.evaluate(t);
    double rot = layer.rotation.evaluate(t) * kPi / 180.0;
    double alpha = layer.opacity.evaluate(t);
    double box_w = eval_box_width(layer, t);
    double box_h = eval_box_height(layer, t);
    if (box_w <= 0.0 || box_h <= 0.0) return;

    QPointF off = shadow_offset(layer, t);
    double blur = eval_shadow_blur(layer, t);
    double spread = eval_shadow_spread(layer, t);
    int pad = eval_shadow_enabled(layer, t)
        ? (int)std::ceil(std::max(std::abs(off.x()), std::abs(off.y())) + blur + spread + 4.0)
        : 0;
    if (eval_background_enabled(layer, t))
        pad += (int)std::ceil(std::max(eval_background_padding_x(layer, t), eval_background_padding_y(layer, t)));
    int img_w = std::max(1, (int)std::ceil(box_w) + pad * 2);
    int img_h = std::max(1, (int)std::ceil(box_h) + pad * 2);
    QImage text_image(img_w, img_h, QImage::Format_ARGB32_Premultiplied);
    text_image.fill(Qt::transparent);

    QPainter painter(&text_image);
    const bool previous_shape_aa = painter.testRenderHint(QPainter::Antialiasing);
    const bool previous_text_aa = painter.testRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = font_for_layer(layer);
    painter.setFont(font);

    QRectF base_rect(pad, pad, box_w, box_h);
    if (eval_background_enabled(layer, t)) {
        const double bg_pad_x = eval_background_padding_x(layer, t);
        const double bg_pad_y = eval_background_padding_y(layer, t);
        const double bg_corner = eval_background_corner_radius(layer, t);
        QRectF bg_rect = base_rect.adjusted(-bg_pad_x, -bg_pad_y, bg_pad_x, bg_pad_y);
        QColor bg = evaluated_background_color(layer, t);
        if (bg.alpha() > 0 || layer.fill_type == 1) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(layer.fill_type == 1 ? gradient_fill_brush(layer, bg_rect, eval_background_opacity(layer, t)) : QBrush(bg));
            painter.drawRoundedRect(bg_rect, bg_corner, bg_corner);
        }
    }

    QRectF text_rect = text_rect_for_style(base_rect, layer);
    QString text = display_text_for_style(layer);
    painter.save();
    painter.setClipRect(text_rect);
    Qt::Alignment align = Qt::AlignVCenter | Qt::AlignHCenter;
    if (layer.align_h == 0) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignLeft;
    if (layer.align_h == 2) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignRight;
    if (layer.align_v == 0) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignTop;
    if (layer.align_v == 2) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignBottom;
    QPainterPath text_path = layer.type == LayerType::Ticker
        ? ticker_text_path(font, text_rect, align, text, layer)
        : text_overflow_path(font, text_rect, align, text, layer);
    text_path = apply_vertical_character_scale(text_path, text_rect, align, layer);
    if (std::abs(layer.baseline_shift) > 0.0001)
        text_path.translate(0.0, -layer.baseline_shift);

    if (eval_shadow_enabled(layer, t)) {
        QColor shadow = color_from_argb(eval_shadow_color(layer, t));
        shadow.setAlphaF(std::clamp((double)shadow.alphaF() * eval_shadow_opacity(layer, t), 0.0, 1.0));
        int passes = shadow_pass_count(blur);
        for (int pass = passes; pass >= 1; --pass) {
            QColor pass_color = shadow;
            pass_color.setAlphaF(shadow.alphaF() / passes);
            painter.setPen(Qt::NoPen);
            painter.setBrush(pass_color);
            double radius = blur * pass / passes;
            for (double dx : {-spread - radius, 0.0, spread + radius})
                for (double dy : {-spread - radius, 0.0, spread + radius})
                    painter.drawPath(text_path.translated(off + QPointF(dx, dy)));
        }
    }

    double outline_width = eval_outline_width(layer, t);
    QColor outline = color_from_argb(eval_outline_color(layer, t));
    outline.setAlphaF(std::clamp((double)outline.alphaF() * eval_outline_opacity(layer, t), 0.0, 1.0));
    QColor fill = color_from_argb(eval_text_color(layer, t));
    fill.setAlphaF(std::clamp((double)fill.alphaF(), 0.0, 1.0));
    auto draw_text_fill = [&]() {
        painter.setPen(Qt::NoPen);
        painter.setBrush(layer.fill_type == 1 ? gradient_fill_brush(layer, text_rect) : QBrush(fill));
        painter.drawPath(text_path);
    };
    auto draw_text_outline = [&]() {
        if (outline_width <= 0.0 || outline.alpha() <= 0) return;
        bool previous_aa = painter.testRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::Antialiasing, eval_outline_antialias(layer, t));
        painter.setPen(QPen(outline, outline_width, Qt::SolidLine, Qt::RoundCap, outline_pen_join_style(layer)));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(text_path);
        painter.setRenderHint(QPainter::Antialiasing, previous_aa);
    };
    if (!eval_outline_on_front(layer, t)) draw_text_outline();
    draw_text_fill();
    if (eval_outline_on_front(layer, t)) draw_text_outline();
    painter.setRenderHint(QPainter::TextAntialiasing, previous_text_aa);
    painter.setRenderHint(QPainter::Antialiasing, previous_shape_aa);
    painter.restore();
    painter.end();

    cairo_surface_t *text_surface = cairo_image_surface_create_for_data(
        text_image.bits(), CAIRO_FORMAT_ARGB32,
        text_image.width(), text_image.height(), text_image.bytesPerLine());

    cairo_save(cr);
    cairo_translate(cr, px, py);
    cairo_rotate(cr, rot);
    cairo_scale(cr, sx, sy);
    cairo_set_source_surface(cr, text_surface, -eval_origin_x(layer, t) * box_w - pad, -eval_origin_y(layer, t) * box_h - pad);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);

    cairo_surface_destroy(text_surface);
}

static void render_layer_rect(cairo_t *cr, const Layer &layer, double t)
{
    double px = layer.pos_x.evaluate(t);
    double py = layer.pos_y.evaluate(t);
    double sx = layer.scale_x.evaluate(t);
    double sy = layer.scale_y.evaluate(t);
    double rot = layer.rotation.evaluate(t) * kPi / 180.0;
    double alpha = layer.opacity.evaluate(t);

    double w = eval_box_width(layer, t);
    double h = eval_box_height(layer, t);
    if (w <= 0.0 || h <= 0.0) return;
    double r = std::min<double>(layer.corner_radius, std::min(w, h) / 2.0);
    double x = -eval_origin_x(layer, t) * w;
    double y = -eval_origin_y(layer, t) * h;
    QPointF off = shadow_offset(layer, t);
    double blur = eval_shadow_blur(layer, t);
    double spread = eval_shadow_spread(layer, t);

    double fr, fg, fb, fa;
    unpack_color(eval_fill_color(layer, t), fr, fg, fb, fa);

    cairo_save(cr);
    cairo_translate(cr, px, py);
    cairo_rotate(cr, rot);
    cairo_scale(cr, sx, sy);
    cairo_translate(cr, x, y);

    if (eval_shadow_enabled(layer, t)) {
        double sr, sg, sb, sa;
        unpack_color(eval_shadow_color(layer, t), sr, sg, sb, sa);
        int passes = shadow_pass_count(blur);
        for (int pass = passes; pass >= 1; --pass) {
            double radius = blur * pass / passes;
            double grow = spread + radius;
            double sx0 = -grow;
            double sy0 = -grow;
            double sw = w + grow * 2.0;
            double sh = h + grow * 2.0;
            double sradius = std::max(0.0, r + grow);
            cairo_save(cr);
            cairo_translate(cr, off.x(), off.y());
            if (sradius > 0.0) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, sx0 + sradius,      sy0 + sradius,      sradius, kPi,     3*kPi/2);
                cairo_arc(cr, sx0 + sw - sradius, sy0 + sradius,      sradius, 3*kPi/2, 2*kPi);
                cairo_arc(cr, sx0 + sw - sradius, sy0 + sh - sradius, sradius, 0,       kPi/2);
                cairo_arc(cr, sx0 + sradius,      sy0 + sh - sradius, sradius, kPi/2,   kPi);
                cairo_close_path(cr);
            } else {
                cairo_rectangle(cr, sx0, sy0, sw, sh);
            }
            cairo_set_source_rgba(cr, sr, sg, sb, sa * alpha * eval_shadow_opacity(layer, t) / passes);
            cairo_fill(cr);
            cairo_restore(cr);
        }
    }

    if (r > 0.0) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, r,     r,     r,  kPi,       3*kPi/2);
        cairo_arc(cr, w-r,   r,     r,  3*kPi/2,   2*kPi);
        cairo_arc(cr, w-r,   h-r,   r,  0,          kPi/2);
        cairo_arc(cr, r,     h-r,   r,  kPi/2,     kPi);
        cairo_close_path(cr);
    } else {
        cairo_rectangle(cr, 0, 0, w, h);
    }
    double outline_width = eval_outline_width(layer, t);
    uint32_t outline_color = eval_outline_color(layer, t);
    bool has_outline = outline_width > 0.0 && ((outline_color >> 24) & 0xFF) > 0;
    auto stroke_outline = [&]() {
        double sr, sg, sb, sa;
        unpack_color(outline_color, sr, sg, sb, sa);
        cairo_set_antialias(cr, outline_cairo_antialias(layer));
        cairo_set_line_width(cr, outline_width);
        cairo_set_line_join(cr, outline_cairo_join_style(layer));
        cairo_set_source_rgba(cr, sr, sg, sb, sa * alpha * eval_outline_opacity(layer, t));
        cairo_stroke_preserve(cr);
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    };
    if (has_outline && !eval_outline_on_front(layer, t))
        stroke_outline();
    cairo_pattern_t *gradient_pattern = nullptr;
    if (layer.fill_type == 1) {
        gradient_pattern = create_fill_gradient_pattern(layer, 0.0, 0.0, w, h, alpha);
        cairo_set_source(cr, gradient_pattern);
    } else {
        cairo_set_source_rgba(cr, fr, fg, fb, fa * alpha);
    }
    if (has_outline && eval_outline_on_front(layer, t)) {
        cairo_fill_preserve(cr);
        if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
        stroke_outline();
        cairo_new_path(cr);
    } else {
        cairo_fill(cr);
        if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
    }
    cairo_restore(cr);
}


static void render_layer_image(cairo_t *cr, const Layer &layer, double t)
{
    if (layer.image_path.empty()) return;

    double px = layer.pos_x.evaluate(t);
    double py = layer.pos_y.evaluate(t);
    double sx = layer.scale_x.evaluate(t);
    double sy = layer.scale_y.evaluate(t);
    double rot = layer.rotation.evaluate(t) * kPi / 180.0;
    double alpha = layer.opacity.evaluate(t);
    double w = eval_box_width(layer, t);
    double h = eval_box_height(layer, t);
    if (w <= 0.0 || h <= 0.0) return;

    QImage image = load_layer_image(QString::fromStdString(layer.image_path),
                                    QSize(std::max(1, (int)std::ceil(w)),
                                          std::max(1, (int)std::ceil(h))));
    if (image.isNull()) return;

    QImage argb = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (argb.width() <= 0 || argb.height() <= 0) return;


    cairo_surface_t *img_surface = cairo_image_surface_create_for_data(
        argb.bits(), CAIRO_FORMAT_ARGB32,
        argb.width(), argb.height(), argb.bytesPerLine());

    cairo_save(cr);
    cairo_translate(cr, px, py);
    cairo_rotate(cr, rot);
    cairo_scale(cr, sx, sy);
    const double origin_x = eval_origin_x(layer, t);
    const double origin_y = eval_origin_y(layer, t);
    if (eval_background_enabled(layer, t)) {
        const double bg_pad_x = eval_background_padding_x(layer, t);
        const double bg_pad_y = eval_background_padding_y(layer, t);
        QColor bg = evaluated_background_color(layer, t);
        double br, bgc, bb, ba;
        br = bg.redF(); bgc = bg.greenF(); bb = bg.blueF(); ba = bg.alphaF();
        if (ba > 0.0 || layer.fill_type == 1) {
            const double x = -origin_x * w - bg_pad_x;
            const double y = -origin_y * h - bg_pad_y;
            const double bw = w + bg_pad_x * 2.0;
            const double bh = h + bg_pad_y * 2.0;
            const double radius = std::min<double>(eval_background_corner_radius(layer, t), std::min(bw, bh) / 2.0);
            if (radius > 0.0) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, x + radius,      y + radius,      radius, kPi,     3*kPi/2);
                cairo_arc(cr, x + bw - radius, y + radius,      radius, 3*kPi/2, 2*kPi);
                cairo_arc(cr, x + bw - radius, y + bh - radius, radius, 0,       kPi/2);
                cairo_arc(cr, x + radius,      y + bh - radius, radius, kPi/2,   kPi);
                cairo_close_path(cr);
            } else {
                cairo_rectangle(cr, x, y, bw, bh);
            }
            cairo_pattern_t *gradient_pattern = nullptr;
            if (layer.fill_type == 1) {
                gradient_pattern = create_fill_gradient_pattern(layer, x, y, bw, bh, alpha * eval_background_opacity(layer, t));
                cairo_set_source(cr, gradient_pattern);
            } else {
                cairo_set_source_rgba(cr, br, bgc, bb, ba * alpha);
            }
            cairo_fill(cr);
            if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
        }
    }
    cairo_scale(cr, w / argb.width(), h / argb.height());
    cairo_set_source_surface(cr, img_surface,
                             -origin_x * argb.width(),
                             -origin_y * argb.height());
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);

    cairo_surface_destroy(img_surface);
}

/* Composite a full title frame into pixel_buf */
static void render_title_frame(TitleSourceData *data,
                                const Title &title, double t)
{
    uint32_t w = clamped_source_dimension(title.width);
    uint32_t h = clamped_source_dimension(title.height);

    /* (Re)allocate buffer & texture if size changed */
    if (data->tex_w != w || data->tex_h != h) {
        bool texture_created = false;
        {
            std::lock_guard<std::mutex> lock(data->texture_mutex);
            obs_enter_graphics();
            if (data->texture) gs_texture_destroy(data->texture);
            data->texture = gs_texture_create(w, h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
            texture_created = data->texture != nullptr;
            obs_leave_graphics();
        }

        if (!texture_created) {
            data->tex_w = 0;
            data->tex_h = 0;
            data->pixel_buf.clear();
            data->dirty = false;
            return;
        }

        data->tex_w = w;
        data->tex_h = h;
        data->pixel_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    }

    /* Cairo surface over our buffer */
    cairo_surface_t *surface =
        cairo_image_surface_create_for_data(
            data->pixel_buf.data(),
            CAIRO_FORMAT_ARGB32,  /* == BGRA on LE – matches GS_BGRA */
            (int)w, (int)h,
            (int)w * 4);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        data->dirty = false;
        return;
    }

    cairo_t *cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        data->dirty = false;
        return;
    }

    /* Clear with background */
    double br, bg, bb, ba;
    unpack_color(title.bg_color, br, bg, bb, ba);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, br, bg, bb, ba);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const bool background_persistence = title.cue_background_persistence &&
        title.cue_persistence_transition && title.current_cue_row >= 0 && !title.live_text_rows.empty();
    const double persistence_time = cue_persistence_hold_time(title);
    const auto exposed = background_persistence ? exposed_text_layers(title) : std::vector<std::shared_ptr<Layer>>();

    /* Render layers bottom → top */
    for (auto &layer : title.layers) {
        if (!layer || !layer->visible) continue;

        double layer_time = t;
        if (background_persistence) {
            const int exposed_index = exposed_text_layer_index(exposed, layer);
            const bool persistent_text = exposed_index >= 0 && title.cue_text_persistence &&
                exposed_index < (int)title.cue_persistent_text_columns.size() &&
                title.cue_persistent_text_columns[exposed_index];
            if (exposed_index < 0 || persistent_text)
                layer_time = persistence_time;
        }

        if (layer_time < layer->in_time || layer_time > layer->out_time) continue;
        double lt = layer_time - layer->in_time;  /* local layer time */

        switch (layer->type) {
        case LayerType::Text:
        case LayerType::Clock:
        case LayerType::Ticker:
            render_layer_text(cr, *layer, lt, (int)w, (int)h);
            break;
        case LayerType::SolidRect:
        case LayerType::Shape:
            render_layer_rect(cr, *layer, lt);
            break;
        case LayerType::Image:
            render_layer_image(cr, *layer, lt);
            break;
        default:
            break;
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);

    /* Upload to GPU */
    {
        std::lock_guard<std::mutex> lock(data->texture_mutex);
        if (data->texture) {
            obs_enter_graphics();
            const uint8_t *ptr = data->pixel_buf.data();
            uint32_t linesize  = w * 4;
            gs_texture_set_image(data->texture, ptr, linesize, false);
            obs_leave_graphics();
        }
    }

    data->dirty = false;
}

QImage render_title_to_image(const Title &title, double t)
{
    const int w = std::max(1, title.width);
    const int h = std::max(1, title.height);
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);

    cairo_surface_t *surface = cairo_image_surface_create_for_data(
        image.bits(), CAIRO_FORMAT_ARGB32, w, h, image.bytesPerLine());
    cairo_t *cr = cairo_create(surface);

    double br, bg, bb, ba;
    unpack_color(title.bg_color, br, bg, bb, ba);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, br, bg, bb, ba);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    const double clamped_time = std::clamp(t, 0.0, std::max(0.0, title.duration));
    for (auto &layer : title.layers) {
        if (!layer || !layer->visible) continue;
        if (clamped_time < layer->in_time || clamped_time > layer->out_time) continue;
        const double lt = clamped_time - layer->in_time;

        switch (layer->type) {
        case LayerType::Text:
        case LayerType::Clock:
        case LayerType::Ticker:
            render_layer_text(cr, *layer, lt, w, h);
            break;
        case LayerType::SolidRect:
        case LayerType::Shape:
            render_layer_rect(cr, *layer, lt);
            break;
        case LayerType::Image:
            render_layer_image(cr, *layer, lt);
            break;
        default:
            break;
        }
    }

    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    return image;
}

/* ══════════════════════════════════════════════════════════════════
 *  OBS source callbacks
 * ══════════════════════════════════════════════════════════════════ */
static const char *source_get_name(void *)
{
    return obsgs_tr_c("OBSTitles.SourceName");
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TitleSourceData();
    data->source    = source;
    data->title_id  = obs_data_get_string(settings, PROP_TITLE_ID);
    data->loop      = obs_data_get_bool(settings,   PROP_LOOP);
    data->speed     = (float)obs_data_get_double(settings, PROP_SPEED);
    data->last_tick = std::chrono::steady_clock::now();
    data->last_clock_refresh = data->last_tick;
    if (auto title = TitleDataStore::instance().get_title(data->title_id))
        data->seen_cue_revision = title->cue_revision;
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    data->dirty = true;
    return data;
}

static void source_destroy(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    {
        std::lock_guard<std::mutex> lock(data->texture_mutex);
        obs_enter_graphics();
        if (data->texture) gs_texture_destroy(data->texture);
        obs_leave_graphics();
        data->texture = nullptr;
        data->tex_w = 0;
        data->tex_h = 0;
    }
    delete data;
}

static void source_update(void *priv, obs_data_t *settings)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
    data->loop     = obs_data_get_bool(settings,   PROP_LOOP);
    data->speed    = (float)obs_data_get_double(settings, PROP_SPEED);
    data->playhead = 0.0;
    data->playback_reverse = false;
    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    if (auto title = TitleDataStore::instance().get_title(data->title_id))
        data->seen_cue_revision = title->cue_revision;
    else
        data->seen_cue_revision = 0;
    data->last_clock_refresh = std::chrono::steady_clock::now();
    data->dirty    = true;
}

static uint32_t source_get_width(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? clamped_source_dimension(title->width) : 1920;
}

static uint32_t source_get_height(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? clamped_source_dimension(title->height) : 1080;
}

static void source_video_tick(void *priv, float seconds)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (data->title_id.empty()) return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title) return;

    if (title->cue_revision != data->seen_cue_revision) {
        double loop_end = std::clamp(title->loop_end, title->loop_start, title->duration);
        double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
        bool has_pending = title->pending_cue_row >= 0 &&
                           title->pending_cue_row < (int)title->live_text_rows.size();
        bool has_current = title->current_cue_row >= 0 &&
                           title->current_cue_row < (int)title->live_text_rows.size();
        bool is_uncue = !has_pending && !has_current && data->active_cue_row >= 0;
        if (title->playback_mode == 1) {
            if (has_pending) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroThenIntro;
            } else if (is_uncue) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroOnly;
            } else {
                data->playhead = 0.0;
                data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
            }
        } else if (title->playback_mode == 2 && (has_pending || is_uncue)) {
            data->playhead = pause_time;
            data->cue_phase = has_pending
                ? TitleSourceData::CuePhase::OutroThenIntro
                : TitleSourceData::CuePhase::OutroOnly;
        } else {
            if (has_pending) {
                apply_live_text_row(title, title->pending_cue_row);
                title->current_cue_row = title->pending_cue_row;
                title->pending_cue_row = -1;
                has_current = true;
                TitleDataStore::instance().touch_runtime_change();
            }
            if (!is_uncue)
                data->playhead = 0.0;
            data->cue_phase = TitleSourceData::CuePhase::FreeRun;
        }
        if (has_current)
            data->active_cue_row = title->current_cue_row;
        data->seen_cue_revision = title->cue_revision;
        data->playback_reverse = false;
        data->waiting_for_cue = false;
        data->playing = true;
        data->dirty = true;
    }

    const bool has_clock_layer = title_has_clock_layer(title);
    const bool has_ticker_layer = title_has_ticker_layer(title);
    const bool has_timeline_animation = title_has_animation(title);
    const bool static_clock_title = has_clock_layer && !has_timeline_animation;

    if (data->playing && !static_clock_title) {
        double dt = (double)seconds * data->speed;
        double duration = std::max(0.001, title->duration);
        double loop_start = std::clamp(title->loop_start, 0.0, title->duration);
        double loop_end = std::clamp(title->loop_end, loop_start, title->duration);

        const bool ping_pong_loop = title->playback_mode == 1 && title->loop_type == 1 &&
                                    (data->cue_phase == TitleSourceData::CuePhase::FreeRun ||
                                     data->cue_phase == TitleSourceData::CuePhase::IntroLoop);
        if (ping_pong_loop) {
            data->playhead += data->playback_reverse ? -dt : dt;
        } else {
            data->playhead += dt;
        }

        if (data->cue_phase == TitleSourceData::CuePhase::IntroLoop && loop_end > loop_start) {
            double loop_len = std::max(0.001, loop_end - loop_start);
            if (title->loop_type == 1) {
                if (!data->playback_reverse && data->playhead >= loop_end) {
                    clear_cue_persistence_transition(title);
                    data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                    data->playback_reverse = true;
                } else if (data->playback_reverse && data->playhead <= loop_start) {
                    data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                    data->playback_reverse = false;
                }
            } else if (data->playhead >= loop_end) {
                clear_cue_persistence_transition(title);
                data->playhead = loop_start + std::fmod(data->playhead - loop_start, loop_len);
            }
        } else if ((data->cue_phase == TitleSourceData::CuePhase::OutroThenIntro ||
                    data->cue_phase == TitleSourceData::CuePhase::OutroOnly) &&
                   data->playhead >= title->duration) {
            double next_intro_time = std::max(0.0, data->playhead - title->duration);
            if (data->cue_phase == TitleSourceData::CuePhase::OutroOnly) {
                data->playhead = title->duration;
                data->playing = false;
                data->cue_phase = TitleSourceData::CuePhase::FreeRun;
                data->active_cue_row = -1;
                title->current_cue_row = -1;
                title->pending_cue_row = -1;
                title->cue_persistence_transition = false;
                title->cue_persistent_text_columns.clear();
                TitleDataStore::instance().touch_runtime_change();
            } else {
                if (title->pending_cue_row >= 0 && title->pending_cue_row < (int)title->live_text_rows.size()) {
                    apply_live_text_row(title, title->pending_cue_row);
                    title->current_cue_row = title->pending_cue_row;
                    title->pending_cue_row = -1;
                    data->active_cue_row = title->current_cue_row;
                    TitleDataStore::instance().touch_runtime_change();
                }
                if (title->playback_mode == 1) {
                    if (loop_end > loop_start && next_intro_time >= loop_end) {
                        next_intro_time = loop_start + std::fmod(next_intro_time - loop_start,
                                                                 std::max(0.001, loop_end - loop_start));
                    }
                    data->playhead = std::clamp(next_intro_time, 0.0, title->duration);
                    data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
                } else {
                    data->playhead = 0.0;
                    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
                }
            }
            data->playback_reverse = false;
        } else if (data->cue_phase == TitleSourceData::CuePhase::FreeRun) {
            if (title->playback_mode == 1) {
                double loop_len = std::max(0.001, loop_end - loop_start);
                if (loop_end <= loop_start + 0.0001) {
                    if (data->playhead >= title->duration)
                        data->playhead = std::fmod(data->playhead, duration);
                } else if (title->loop_type == 1) {
                    if (!data->playback_reverse && data->playhead >= loop_end) {
                        data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                        data->playback_reverse = true;
                    } else if (data->playback_reverse && data->playhead <= loop_start) {
                        data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                        data->playback_reverse = false;
                    }
                } else if (data->playhead >= loop_end) {
                    data->playhead = loop_start + std::fmod(data->playhead - loop_end, loop_len);
                }
            } else if (title->playback_mode == 2) {
                double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
                if (data->playhead >= pause_time) {
                    data->playhead = pause_time;
                    data->playing = false;
                    clear_cue_persistence_transition(title);
                }
            } else if (data->playhead >= title->duration) {
                data->playhead = title->duration;
                data->playing  = false;
                if (title->current_cue_row >= 0 || title->pending_cue_row >= 0 || data->active_cue_row >= 0) {
                    title->current_cue_row = -1;
                    title->pending_cue_row = -1;
                    title->cue_persistence_transition = false;
                    title->cue_persistent_text_columns.clear();
                    data->active_cue_row = -1;
                    TitleDataStore::instance().touch_runtime_change();
                }
            }
        }
        data->dirty = true;
    }


    if (has_ticker_layer)
        data->dirty = true;

    if (static_clock_title || (!data->playing && has_clock_layer)) {
        auto now = std::chrono::steady_clock::now();
        if (now - data->last_clock_refresh >= std::chrono::seconds(1)) {
            data->last_clock_refresh = now;
            data->dirty = true;
        }
    }

    uint64_t revision = TitleDataStore::instance().revision();
    if (revision != data->seen_store_revision) {
        data->seen_store_revision = revision;
        data->dirty = true;
    }

    if (data->dirty)
        render_title_frame(data, *title, data->playhead);
}

static void source_video_render(void *priv, gs_effect_t * /*effect*/)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    std::lock_guard<std::mutex> lock(data->texture_mutex);
    if (!data->texture) return;

    gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!eff) return;

    gs_eparam_t *image = gs_effect_get_param_by_name(eff, "image");
    if (!image) return;

    gs_effect_set_texture(image, data->texture);

    while (gs_effect_loop(eff, "Draw"))
        gs_draw_sprite(data->texture, 0, 0, 0);
}

/* ── Properties panel ─────────────────────────────────────────────── */
static obs_properties_t *source_get_properties(void * /*priv*/)
{
    obs_properties_t *props = obs_properties_create();

    /* Title selector */
    obs_property_t *p = obs_properties_add_list(
        props, PROP_TITLE_ID, obsgs_tr_c("OBSTitles.TitleID"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(p, obsgs_tr_c("OBSTitles.NoTitle"), "");
    for (auto &t : TitleDataStore::instance().titles())
        obs_property_list_add_string(p, t->name.c_str(), t->id.c_str());

    obs_properties_add_bool(props,   PROP_LOOP,  obsgs_tr_c("OBSTitles.Loop"));
    obs_properties_add_float_slider(props, PROP_SPEED,
        obsgs_tr_c("OBSTitles.Speed"), 0.1, 4.0, 0.05);

    return props;
}

static void source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, PROP_TITLE_ID, "");
    obs_data_set_default_bool(settings,   PROP_LOOP,     true);
    obs_data_set_default_double(settings, PROP_SPEED,    1.0);
}

/* ══════════════════════════════════════════════════════════════════
 *  Registration
 * ══════════════════════════════════════════════════════════════════ */
void title_source_register()
{
    static obs_source_info si = {};
    si.id             = "obs_graphics_studio_pro_source";
    si.type           = OBS_SOURCE_TYPE_INPUT;
    si.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    si.get_name       = source_get_name;
    si.create         = source_create;
    si.destroy        = source_destroy;
    si.update         = source_update;
    si.get_width      = source_get_width;
    si.get_height     = source_get_height;
    si.video_tick     = source_video_tick;
    si.video_render   = source_video_render;
    si.get_properties = source_get_properties;
    si.get_defaults   = source_get_defaults;

    obs_register_source(&si);
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Source type registered.");
}
