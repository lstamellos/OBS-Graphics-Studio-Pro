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

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <QImage>
#include <QString>
#include <QPointF>
#include <QPainter>
#include <QFont>
#include <QColor>

#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
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

    enum class CuePhase { FreeRun, IntroLoop, OutroThenIntro };

    /* Playback state */
    double      playhead     = 0.0;    /* seconds */
    bool        playing      = true;
    bool        playback_reverse = false;
    uint64_t    seen_cue_revision = 0;
    CuePhase    cue_phase    = CuePhase::FreeRun;
    std::chrono::steady_clock::time_point last_tick;
    bool        first_tick   = true;

    /* GPU texture */
    gs_texture_t *texture    = nullptr;
    uint32_t      tex_w      = 0;
    uint32_t      tex_h      = 0;

    /* CPU render buffer */
    std::vector<uint8_t> pixel_buf;   /* BGRA row-major */

    /* Dirty flag – avoid re-uploading unchanged frames */
    bool dirty = true;
    uint64_t seen_store_revision = 0;
};


static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    if (!title) return exposed;
    for (const auto &layer : title->layers) {
        if (layer->type == LayerType::Text && layer->expose_text)
            exposed.push_back(layer);
    }
    return exposed;
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


static double eval_box_width(const Layer &layer, double t)
{
    return std::max(1.0, layer.box_width.is_animated()
                         ? layer.box_width.evaluate(t)
                         : (double)layer.rect_width);
}

static double eval_box_height(const Layer &layer, double t)
{
    return std::max(1.0, layer.box_height.is_animated()
                         ? layer.box_height.evaluate(t)
                         : (double)layer.rect_height);
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
static QColor color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
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
    double box_w = std::max(1.0, eval_box_width(layer, t));
    double box_h = std::max(1.0, eval_box_height(layer, t));

    QPointF off = shadow_offset(layer, t);
    double blur = eval_shadow_blur(layer, t);
    double spread = eval_shadow_spread(layer, t);
    int pad = eval_shadow_enabled(layer, t)
        ? (int)std::ceil(std::max(std::abs(off.x()), std::abs(off.y())) + blur + spread + 4.0)
        : 0;
    int img_w = std::max(1, (int)std::ceil(box_w) + pad * 2);
    int img_h = std::max(1, (int)std::ceil(box_h) + pad * 2);
    QImage text_image(img_w, img_h, QImage::Format_ARGB32_Premultiplied);
    text_image.fill(Qt::transparent);

    QPainter painter(&text_image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font(QString::fromStdString(layer.font_family));
    font.setPixelSize(layer.font_size);
    font.setBold(layer.font_bold);
    font.setItalic(layer.font_italic);
    font.setKerning(true);
    painter.setFont(font);

    QRectF text_rect(pad, pad, box_w, box_h);
    Qt::Alignment align = Qt::AlignVCenter | Qt::AlignHCenter;
    if (layer.align_h == 0) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignLeft;
    if (layer.align_h == 2) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignRight;
    if (layer.align_v == 0) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignTop;
    if (layer.align_v == 2) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignBottom;

    if (eval_shadow_enabled(layer, t)) {
        QColor shadow = color_from_argb(eval_shadow_color(layer, t));
        shadow.setAlphaF(std::clamp((double)shadow.alphaF() * eval_shadow_opacity(layer, t), 0.0, 1.0));
        int passes = std::max(1, (int)std::ceil(blur / 3.0));
        for (int pass = passes; pass >= 1; --pass) {
            QColor pass_color = shadow;
            pass_color.setAlphaF(shadow.alphaF() / passes);
            painter.setPen(pass_color);
            double radius = blur * pass / passes;
            for (double dx : {-spread - radius, 0.0, spread + radius})
                for (double dy : {-spread - radius, 0.0, spread + radius})
                    painter.drawText(text_rect.translated(off + QPointF(dx, dy)), align, QString::fromStdString(layer.text_content));
        }
    }

    QColor fill = color_from_argb(eval_text_color(layer, t));
    fill.setAlphaF(std::clamp((double)fill.alphaF(), 0.0, 1.0));
    painter.setPen(fill);
    painter.drawText(text_rect, align, QString::fromStdString(layer.text_content));
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
        int passes = std::max(1, (int)std::ceil(blur / 3.0));
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
    cairo_set_source_rgba(cr, fr, fg, fb, fa * alpha);
    cairo_fill(cr);
    cairo_restore(cr);
}


static void render_layer_image(cairo_t *cr, const Layer &layer, double t)
{
    if (layer.image_path.empty()) return;

    QImage image(QString::fromStdString(layer.image_path));
    if (image.isNull()) return;

    QImage argb = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    double px = layer.pos_x.evaluate(t);
    double py = layer.pos_y.evaluate(t);
    double sx = layer.scale_x.evaluate(t);
    double sy = layer.scale_y.evaluate(t);
    double rot = layer.rotation.evaluate(t) * kPi / 180.0;
    double alpha = layer.opacity.evaluate(t);
    double w = eval_box_width(layer, t);
    double h = eval_box_height(layer, t);

    cairo_surface_t *img_surface = cairo_image_surface_create_for_data(
        argb.bits(), CAIRO_FORMAT_ARGB32,
        argb.width(), argb.height(), argb.bytesPerLine());

    cairo_save(cr);
    cairo_translate(cr, px, py);
    cairo_rotate(cr, rot);
    cairo_scale(cr, sx * (w / argb.width()), sy * (h / argb.height()));
    cairo_set_source_surface(cr, img_surface,
                             -eval_origin_x(layer, t) * argb.width(),
                             -eval_origin_y(layer, t) * argb.height());
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);

    cairo_surface_destroy(img_surface);
}

/* Composite a full title frame into pixel_buf */
static void render_title_frame(TitleSourceData *data,
                                const Title &title, double t)
{
    uint32_t w = (uint32_t)title.width;
    uint32_t h = (uint32_t)title.height;

    /* (Re)allocate buffer & texture if size changed */
    if (data->tex_w != w || data->tex_h != h) {
        obs_enter_graphics();
        if (data->texture) gs_texture_destroy(data->texture);
        data->texture = gs_texture_create(w, h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
        obs_leave_graphics();

        data->tex_w = w;
        data->tex_h = h;
        data->pixel_buf.resize(w * h * 4, 0);
    }

    /* Cairo surface over our buffer */
    cairo_surface_t *surface =
        cairo_image_surface_create_for_data(
            data->pixel_buf.data(),
            CAIRO_FORMAT_ARGB32,  /* == BGRA on LE – matches GS_BGRA */
            (int)w, (int)h,
            (int)w * 4);

    cairo_t *cr = cairo_create(surface);

    /* Clear with background */
    double br, bg, bb, ba;
    unpack_color(title.bg_color, br, bg, bb, ba);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, br, bg, bb, ba);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Render layers bottom → top */
    for (auto &layer : title.layers) {
        if (!layer->visible) continue;
        if (t < layer->in_time || t > layer->out_time) continue;
        double lt = t - layer->in_time;  /* local layer time */

        switch (layer->type) {
        case LayerType::Text:
            render_layer_text(cr, *layer, lt, (int)w, (int)h);
            break;
        case LayerType::SolidRect:
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
    obs_enter_graphics();
    const uint8_t *ptr = data->pixel_buf.data();
    uint32_t linesize  = w * 4;
    gs_texture_set_image(data->texture, ptr, linesize, false);
    obs_leave_graphics();

    data->dirty = false;
}

/* ══════════════════════════════════════════════════════════════════
 *  OBS source callbacks
 * ══════════════════════════════════════════════════════════════════ */
static const char *source_get_name(void *)
{
    return "OBS Titler Pro";
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TitleSourceData();
    data->source    = source;
    data->title_id  = obs_data_get_string(settings, PROP_TITLE_ID);
    data->loop      = obs_data_get_bool(settings,   PROP_LOOP);
    data->speed     = (float)obs_data_get_double(settings, PROP_SPEED);
    data->last_tick = std::chrono::steady_clock::now();
    return data;
}

static void source_destroy(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    obs_enter_graphics();
    if (data->texture) gs_texture_destroy(data->texture);
    obs_leave_graphics();
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
    data->playing = true;
    data->dirty    = true;
}

static uint32_t source_get_width(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? (uint32_t)title->width : 1920;
}

static uint32_t source_get_height(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? (uint32_t)title->height : 1080;
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
        if (title->playback_mode == 1) {
            if (has_pending) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroThenIntro;
            } else {
                data->playhead = 0.0;
                data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
            }
        } else if (title->playback_mode == 2 && has_pending) {
            data->playhead = pause_time;
            data->cue_phase = TitleSourceData::CuePhase::OutroThenIntro;
        } else {
            if (has_pending) {
                apply_live_text_row(title, title->pending_cue_row);
                title->current_cue_row = title->pending_cue_row;
                title->pending_cue_row = -1;
                TitleDataStore::instance().touch_runtime_change();
            }
            data->playhead = 0.0;
            data->cue_phase = TitleSourceData::CuePhase::FreeRun;
        }
        data->seen_cue_revision = title->cue_revision;
        data->playback_reverse = false;
        data->playing = true;
        data->dirty = true;
    }

    if (data->playing) {
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
                    data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                    data->playback_reverse = true;
                } else if (data->playback_reverse && data->playhead <= loop_start) {
                    data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                    data->playback_reverse = false;
                }
            } else if (data->playhead >= loop_end) {
                data->playhead = loop_start + std::fmod(data->playhead - loop_start, loop_len);
            }
        } else if (data->cue_phase == TitleSourceData::CuePhase::OutroThenIntro &&
                   data->playhead >= title->duration) {
            double next_intro_time = std::max(0.0, data->playhead - title->duration);
            if (title->pending_cue_row >= 0 && title->pending_cue_row < (int)title->live_text_rows.size()) {
                apply_live_text_row(title, title->pending_cue_row);
                title->current_cue_row = title->pending_cue_row;
                title->pending_cue_row = -1;
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
                }
            } else if (data->playhead >= title->duration) {
                data->playhead = title->duration;
                data->playing  = false;
            }
        }
        data->dirty = true;
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
    if (!data->texture) return;

    gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(eff, "image");
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
        props, PROP_TITLE_ID, "Title",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(p, "(none)", "");
    for (auto &t : TitleDataStore::instance().titles())
        obs_property_list_add_string(p, t->name.c_str(), t->id.c_str());

    obs_properties_add_bool(props,   PROP_LOOP,  "Loop");
    obs_properties_add_float_slider(props, PROP_SPEED,
        "Playback Speed", 0.1, 4.0, 0.05);

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
    si.id             = "obs_titles_source";
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
    blog(LOG_INFO, "[OBS Titler Pro] Source type registered.");
}
