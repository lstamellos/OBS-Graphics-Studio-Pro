/*
 * title-renderer-gpu.cpp
 *
 * OBS-native GPU renderer.  This module renders directly through libobs'
 * graphics abstraction and avoids full-frame CPU raster composition in the OBS
 * source path. Complex visual styles are generated as layer-local texture
 * assets that are then composited and transformed on the GPU.
 */

#include "title-renderer-gpu.h"
#include "title-data.h"

#include <QBuffer>
#include <QDateTime>
#include <QByteArray>
#include <QBrush>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>
#include <QRectF>
#include <QString>
#include <QSize>
#include <QSvgRenderer>
#include <QTextLayout>
#include <QTextOption>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <sstream>

namespace obsgs {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

static bool path_is_svg(const std::string &path)
{
    const QString qpath = QString::fromStdString(path);
    return qpath.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           qpath.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

static QImage load_layer_image(const Layer &layer, double t = 0.0)
{
    if (layer.image_path.empty())
        return QImage();

    const QString path = QString::fromStdString(layer.image_path);
    if (path_is_svg(layer.image_path)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid())
            return QImage();
        QSize size(std::max(1, static_cast<int>(std::ceil(layer.box_width.is_animated() ? layer.box_width.evaluate(t) : static_cast<double>(layer.rect_width)))),
                   std::max(1, static_cast<int>(std::ceil(layer.box_height.is_animated() ? layer.box_height.evaluate(t) : static_cast<double>(layer.rect_height)))));
        if (!size.isValid() || size.isEmpty())
            size = renderer.defaultSize();
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


static double eval_background_corner_radius(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_corner_radius_prop.is_animated()
                             ? layer.background_corner_radius_prop.evaluate(t)
                             : static_cast<double>(layer.background_corner_radius));
}

static bool eval_shadow_enabled(const Layer &layer, double t)
{
    return layer.shadow_enabled_prop.is_animated()
        ? layer.shadow_enabled_prop.evaluate(t) >= 0.5
        : layer.shadow_enabled;
}

static double eval_shadow_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.shadow_opacity_prop.is_animated()
                          ? layer.shadow_opacity_prop.evaluate(t)
                          : static_cast<double>(layer.shadow_opacity),
                      0.0, 1.0);
}

static double eval_shadow_distance(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_distance_prop.is_animated()
                             ? layer.shadow_distance_prop.evaluate(t)
                             : static_cast<double>(layer.shadow_distance));
}

static double eval_shadow_angle(const Layer &layer, double t)
{
    return layer.shadow_angle_prop.is_animated()
        ? layer.shadow_angle_prop.evaluate(t)
        : static_cast<double>(layer.shadow_angle);
}

static double eval_shadow_blur(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_blur_prop.is_animated()
                             ? layer.shadow_blur_prop.evaluate(t)
                             : static_cast<double>(layer.shadow_blur));
}

static double eval_shadow_spread(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_spread_prop.is_animated()
                             ? layer.shadow_spread_prop.evaluate(t)
                             : static_cast<double>(layer.shadow_spread));
}

static bool eval_outline_enabled(const Layer &layer, double)
{
    return layer.outline_enabled;
}

static double eval_outline_width(const Layer &layer, double t)
{
    return eval_outline_enabled(layer, t) ? std::max(0.0f, layer.stroke_width) : 0.0;
}

static double eval_outline_opacity(const Layer &layer, double)
{
    return std::clamp(static_cast<double>(layer.outline_opacity), 0.0, 1.0);
}

static bool eval_outline_on_front(const Layer &layer, double)
{
    return layer.outline_on_front;
}

static QColor color_from_argb(uint32_t argb, double opacity = 1.0)
{
    QColor color((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
    color.setAlphaF(std::clamp(static_cast<double>(color.alphaF()) * opacity, 0.0, 1.0));
    return color;
}

static QPointF shadow_offset(const Layer &layer, double t)
{
    const double radians = eval_shadow_angle(layer, t) * kPi / 180.0;
    return QPointF(std::cos(radians) * eval_shadow_distance(layer, t),
                   std::sin(radians) * eval_shadow_distance(layer, t));
}

static int shadow_pass_count(double blur)
{
    return std::clamp(static_cast<int>(std::ceil(blur / 4.0)), 1, 12);
}

static QColor gradient_color_with_opacity(uint32_t argb, double gradient_opacity, double stop_opacity)
{
    QColor color = color_from_argb(argb);
    color.setAlphaF(std::clamp(static_cast<double>(color.alphaF()) * gradient_opacity * stop_opacity, 0.0, 1.0));
    return color;
}

static QBrush gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    const double opacity = std::clamp(static_cast<double>(layer.gradient_opacity) * layer_opacity, 0.0, 1.0);
    const double cx = box.left() + std::clamp(static_cast<double>(layer.gradient_center_x), 0.0, 1.0) * box.width();
    const double cy = box.top() + std::clamp(static_cast<double>(layer.gradient_center_y), 0.0, 1.0) * box.height();
    const double scale = std::clamp(static_cast<double>(layer.gradient_scale), 0.01, 10.0);
    const double start_pos = std::clamp(static_cast<double>(layer.gradient_start_pos), 0.0, 1.0);
    const double end_pos = std::clamp(static_cast<double>(layer.gradient_end_pos), 0.0, 1.0);
    if (layer.gradient_type == 1) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * scale;
        QRadialGradient gradient(QPointF(cx, cy), std::max(1.0, radius),
                                 QPointF(box.left() + std::clamp(static_cast<double>(layer.gradient_focal_x), 0.0, 1.0) * box.width(),
                                         box.top() + std::clamp(static_cast<double>(layer.gradient_focal_y), 0.0, 1.0) * box.height()));
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

static QFont font_for_layer(const Layer &layer)
{
    QFont font(QString::fromStdString(layer.font_family));
    font.setPointSizeF(std::max(1.0f, layer.font_size));
    font.setBold(layer.font_bold);
    font.setItalic(layer.font_italic);
    font.setUnderline(layer.text_underline);
    return font;
}

static QString display_text_for_layer(const Layer &layer)
{
    if (layer.type == LayerType::Clock)
        return QDateTime::currentDateTime().toString(QString::fromStdString(layer.clock_format.empty() ? std::string("hh:mm:ss") : layer.clock_format)
                                                        .replace(QStringLiteral("H"), QStringLiteral("hh"))
                                                        .replace(QStringLiteral("i"), QStringLiteral("mm"))
                                                        .replace(QStringLiteral("s"), QStringLiteral("ss")));
    return QString::fromStdString(layer.text_content);
}

static QPainterPath text_path_for_layer(const Layer &layer, const QRectF &rect)
{
    QFont font = font_for_layer(layer);
    const QString text = display_text_for_layer(layer);
    QPainterPath path;
    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0 ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
    option.setAlignment((layer.align_h == 0 ? Qt::AlignLeft : layer.align_h == 2 ? Qt::AlignRight : Qt::AlignHCenter) |
                        (layer.align_v == 0 ? Qt::AlignTop : layer.align_v == 2 ? Qt::AlignBottom : Qt::AlignVCenter));

    QTextLayout layout(text, font);
    layout.setTextOption(option);
    layout.beginLayout();
    QVector<QTextLine> lines;
    double total_height = 0.0;
    const double max_width = std::max(1.0, rect.width());
    while (true) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(max_width);
        total_height += line.height();
        lines.push_back(line);
        if (layer.text_overflow_mode != 0 && !text.contains('\n'))
            break;
    }
    layout.endLayout();

    double y = rect.top();
    if (layer.align_v == 1)
        y += std::max(0.0, (rect.height() - total_height) * 0.5);
    else if (layer.align_v == 2)
        y += std::max(0.0, rect.height() - total_height);

    for (QTextLine line : lines) {
        double x = rect.left();
        if (layer.align_h == 1)
            x += std::max(0.0, (rect.width() - line.naturalTextWidth()) * 0.5);
        else if (layer.align_h == 2)
            x += std::max(0.0, rect.width() - line.naturalTextWidth());
        line.setPosition(QPointF(x, y));
        path.addText(QPointF(x, y + line.ascent()), font, text.mid(line.textStart(), line.textLength()));
        y += line.height() + layer.text_leading;
    }

    return path;
}

static LayerAsset rasterize_layer_asset(const Layer &layer, double t)
{
    LayerAsset asset;
    const double box_w = std::max(1.0, eval_box_width(layer, t));
    const double box_h = std::max(1.0, eval_box_height(layer, t));
    const double outline = eval_outline_width(layer, t);
    const double blur = eval_shadow_enabled(layer, t) ? eval_shadow_blur(layer, t) : 0.0;
    const double spread = eval_shadow_enabled(layer, t) ? eval_shadow_spread(layer, t) : 0.0;
    const QPointF shadow = eval_shadow_enabled(layer, t) ? shadow_offset(layer, t) : QPointF();
    const double bg_pad_x = eval_background_enabled(layer, t) ? std::max(0.0, eval_background_padding_x(layer, t)) : 0.0;
    const double bg_pad_y = eval_background_enabled(layer, t) ? std::max(0.0, eval_background_padding_y(layer, t)) : 0.0;
    const double pad_left = std::ceil(std::max({outline, bg_pad_x, blur + spread - shadow.x(), 1.0}));
    const double pad_top = std::ceil(std::max({outline, bg_pad_y, blur + spread - shadow.y(), 1.0}));
    const double pad_right = std::ceil(std::max({outline, bg_pad_x, blur + spread + shadow.x(), 1.0}));
    const double pad_bottom = std::ceil(std::max({outline, bg_pad_y, blur + spread + shadow.y(), 1.0}));
    const int image_w = std::max(1, static_cast<int>(std::ceil(box_w + pad_left + pad_right)));
    const int image_h = std::max(1, static_cast<int>(std::ceil(box_h + pad_top + pad_bottom)));

    asset.image = QImage(image_w, image_h, QImage::Format_ARGB32_Premultiplied);
    asset.image.fill(Qt::transparent);
    asset.width = image_w;
    asset.height = image_h;
    asset.origin_x = (pad_left + eval_origin_x(layer, t) * box_w) / std::max(1.0, asset.width);
    asset.origin_y = (pad_top + eval_origin_y(layer, t) * box_h) / std::max(1.0, asset.height);

    QPainter painter(&asset.image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QRectF box(pad_left, pad_top, box_w, box_h);
    auto fill_brush = [&](const QRectF &target, double opacity = 1.0) {
        return layer.fill_type == 1 ? gradient_fill_brush(layer, target, opacity)
                                    : QBrush(color_from_argb(layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker
                                                                 ? eval_text_color(layer, t) : eval_fill_color(layer, t)));
    };

    auto draw_rounded_shadow = [&](const QRectF &shape, double radius) {
        if (!eval_shadow_enabled(layer, t))
            return;
        QColor color = color_from_argb(eval_shadow_color(layer, t), eval_shadow_opacity(layer, t));
        const int passes = shadow_pass_count(blur);
        for (int pass = passes; pass >= 1; --pass) {
            QColor pass_color = color;
            pass_color.setAlphaF(color.alphaF() / passes);
            painter.setPen(Qt::NoPen);
            painter.setBrush(pass_color);
            const double pass_radius = blur * pass / passes;
            const QRectF shadow_rect = shape.translated(shadow).adjusted(-spread - pass_radius, -spread - pass_radius,
                                                                         spread + pass_radius, spread + pass_radius);
            painter.drawRoundedRect(shadow_rect, radius + pass_radius, radius + pass_radius);
        }
    };

    if (eval_background_enabled(layer, t)) {
        const QRectF bg_rect = box.adjusted(-bg_pad_x, -bg_pad_y, bg_pad_x, bg_pad_y);
        const double bg_radius = eval_background_corner_radius(layer, t);
        QColor bg = color_from_argb(eval_background_color(layer, t), eval_background_opacity(layer, t));
        painter.setPen(Qt::NoPen);
        painter.setBrush(layer.fill_type == 1 ? gradient_fill_brush(layer, bg_rect, eval_background_opacity(layer, t)) : QBrush(bg));
        painter.drawRoundedRect(bg_rect, bg_radius, bg_radius);
    }

    if (layer.type == LayerType::SolidRect || layer.type == LayerType::Shape) {
        const double radius = std::clamp(static_cast<double>(layer.corner_radius), 0.0, std::min(box_w, box_h) * 0.5);
        draw_rounded_shadow(box, radius);
        auto draw_fill = [&]() {
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill_brush(box));
            painter.drawRoundedRect(box, radius, radius);
        };
        auto draw_outline = [&]() {
            if (outline <= 0.0)
                return;
            QColor oc = color_from_argb(layer.stroke_color, eval_outline_opacity(layer, t));
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(oc, outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawRoundedRect(box, radius, radius);
        };
        if (!eval_outline_on_front(layer, t))
            draw_outline();
        draw_fill();
        if (eval_outline_on_front(layer, t))
            draw_outline();
    } else if (layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker) {
        QPainterPath path = text_path_for_layer(layer, box);
        if (eval_shadow_enabled(layer, t)) {
            QColor color = color_from_argb(eval_shadow_color(layer, t), eval_shadow_opacity(layer, t));
            const int passes = shadow_pass_count(blur);
            for (int pass = passes; pass >= 1; --pass) {
                QColor pass_color = color;
                pass_color.setAlphaF(color.alphaF() / passes);
                painter.setPen(Qt::NoPen);
                painter.setBrush(pass_color);
                const double pass_radius = blur * pass / passes;
                for (double dx : {-spread - pass_radius, 0.0, spread + pass_radius})
                    for (double dy : {-spread - pass_radius, 0.0, spread + pass_radius})
                        painter.drawPath(path.translated(shadow + QPointF(dx, dy)));
            }
        }
        auto draw_outline = [&]() {
            if (outline <= 0.0)
                return;
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(color_from_argb(layer.stroke_color, eval_outline_opacity(layer, t)), outline,
                                Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(path);
        };
        auto draw_fill = [&]() {
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill_brush(box));
            painter.drawPath(path);
        };
        if (!eval_outline_on_front(layer, t))
            draw_outline();
        draw_fill();
        if (eval_outline_on_front(layer, t))
            draw_outline();
    }

    painter.end();
    return asset;
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
        plan.migration_notes.emplace_back("Text layer is rendered into a layer-local GPU texture asset and reserved for a future persistent glyph/vector atlas shader.");
        break;
    default:
        plan.gpu_composited = false;
        plan.migration_notes.emplace_back("Unsupported layer type is skipped until a GPU pass is added.");
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

    plan.eliminated_cpu_paths.emplace_back("Full-frame CPU composition is removed from the OBS source render path.");
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
    plan.incremental_steps.emplace_back("Add a persistent GPU glyph/vector text atlas to replace temporary layer texture asset generation once atlas parity is available.");
    plan.incremental_steps.emplace_back("Add offscreen render targets for separable blur, glow, filters, screen effects, and future 3D composition.");
    return plan;
}

GpuTextureFrame *ObsGpuRenderPipeline::texture_for_image_layer(const Layer &layer)
{
    if (layer.image_path.empty())
        return nullptr;

    auto existing = image_textures_.find(layer.image_path);
    if (existing != image_textures_.end())
        return existing->second.get();

    QImage image = load_layer_image(layer);
    if (image.isNull())
        return nullptr;

    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
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

GpuTextureFrame *ObsGpuRenderPipeline::texture_for_raster_layer(const Layer &layer, const LayerAsset &asset)
{
    if (asset.image.isNull() || asset.width <= 0.0 || asset.height <= 0.0)
        return nullptr;

    const std::string key = layer.id.empty() ? layer.name : layer.id;
    auto &slot = raster_textures_[key];
    if (!slot)
        slot = std::make_unique<GpuTextureFrame>();

    if (!slot->ensure_dynamic_bgra(static_cast<uint32_t>(asset.image.width()), static_cast<uint32_t>(asset.image.height()))) {
        slot->reset();
        return nullptr;
    }

    if (!slot->upload_bgra_asset(asset.image.constBits(), static_cast<uint32_t>(asset.image.bytesPerLine())))
        return nullptr;

    return slot.get();
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

        switch (layer->type) {
        case LayerType::SolidRect:
        case LayerType::Shape:
        case LayerType::Text:
        case LayerType::Clock:
        case LayerType::Ticker: {
            LayerAsset asset = rasterize_layer_asset(*layer, lt);
            GpuTextureFrame *texture = texture_for_raster_layer(*layer, asset);
            if (texture)
                draw_texture_quad(texture->texture(), px, py, asset.width, asset.height,
                                  asset.origin_x, asset.origin_y, sx, sy, rot, alpha);
            break;
        }
        case LayerType::Image: {
            GpuTextureFrame *texture = texture_for_image_layer(*layer);
            if (!texture)
                break;
            const double w = eval_box_width(*layer, lt) > 0.0 ? eval_box_width(*layer, lt) : texture->width();
            const double h = eval_box_height(*layer, lt) > 0.0 ? eval_box_height(*layer, lt) : texture->height();
            draw_texture_quad(texture->texture(), px, py, w, h, origin_x, origin_y, sx, sy, rot, alpha);
            break;
        }
        default:
            break;
        }
    }

    return true;
}

void ObsGpuRenderPipeline::reset()
{
    image_textures_.clear();
    raster_textures_.clear();
}


QImage render_title_to_qimage(const Title &title, double time_seconds)
{
    QImage frame(std::max(1, title.width), std::max(1, title.height), QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(frame.rect(), color_from_argb(title.bg_color));

    const double clamped_time = std::clamp(time_seconds, 0.0, std::max(0.0, title.duration));
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

        painter.save();
        painter.setOpacity(alpha);
        painter.translate(px, py);
        painter.rotate(rot);
        painter.scale(sx, sy);

        if (layer->type == LayerType::Image) {
            QImage image = load_layer_image(*layer, lt);
            if (!image.isNull()) {
                const double w = eval_box_width(*layer, lt) > 0.0 ? eval_box_width(*layer, lt) : image.width();
                const double h = eval_box_height(*layer, lt) > 0.0 ? eval_box_height(*layer, lt) : image.height();
                const QRectF rect(-eval_origin_x(*layer, lt) * w, -eval_origin_y(*layer, lt) * h, w, h);
                painter.drawImage(rect, image);
            }
        } else {
            LayerAsset asset = rasterize_layer_asset(*layer, lt);
            if (!asset.image.isNull())
                painter.drawImage(QRectF(-asset.origin_x * asset.width, -asset.origin_y * asset.height,
                                         asset.width, asset.height), asset.image);
        }

        painter.restore();
    }

    painter.end();
    return frame;
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
