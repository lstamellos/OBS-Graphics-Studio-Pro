/*
 * title-editor.cpp
 *
 * After Effects-style title editor.
 * Implemented with plain Qt widgets for maximum OBS compatibility.
 */

#include "title-editor.h"
#include "title-data.h"

#include <obs-module.h>

#include <QPainter>
#include <QImage>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QToolBar>
#include <QAction>
#include <QIcon>
#include <QStringList>
#include <QStyle>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QColorDialog>
#include <QFileDialog>
#include <QFontDatabase>
#include <QScrollArea>
#include <QFrame>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QKeySequence>
#include <QAbstractSpinBox>
#include <QAbstractItemModel>
#include <QTextEdit>
#include <QToolButton>
#include <QMenu>
#include <QContextMenuEvent>
#include <cmath>
#include <algorithm>
#include <vector>
#include <initializer_list>
#include <set>
#include <limits>

/* ────────────────────────────────────────────────────────────────── */
/*  Dark AE-style palette constants                                   */
/* ────────────────────────────────────────────────────────────────── */
static const QColor C_BG_DARK  { 0x1a1a1a };
static const QColor C_BG_MID   { 0x252525 };
static const QColor C_BG_LIGHT { 0x2e2e2e };
static const QColor C_ACCENT   { 0x0078d4 };
static const QColor C_TEXT     { 0xcccccc };
static const QColor C_RULER    { 0x1e1e1e };
static const QColor C_KF_DOT   { 0xf0a020 };
static const QColor C_PLAYHEAD { 0xff4444 };




static double obs_frame_rate()
{
    struct obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.fps_den > 0 && ovi.fps_num > 0)
        return (double)ovi.fps_num / (double)ovi.fps_den;
    return 30.0;
}

static double obs_frame_duration()
{
    return 1.0 / std::max(1.0, obs_frame_rate());
}

static double snap_to_obs_frame(double t)
{
    double fd = obs_frame_duration();
    return std::round(t / fd) * fd;
}

static QString format_timecode(double t)
{
    double fps_d = obs_frame_rate();
    int fps = std::max(1, (int)std::round(fps_d));
    int total_frames = std::max(0, (int)std::round(t * fps_d));
    int frames = total_frames % fps;
    int total_seconds = total_frames / fps;
    int seconds = total_seconds % 60;
    int minutes = (total_seconds / 60) % 60;
    int hours = total_seconds / 3600;
    return QString("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

static QColor layer_color(const Layer &layer, int row)
{
    if (layer.type == LayerType::Text)
        return QColor(0xb4, 0x5a, 0xa0);
    if (layer.type == LayerType::SolidRect)
        return QColor(0x4f, 0x8f, 0x58);
    if (layer.type == LayerType::Image)
        return QColor(0x7d, 0x8b, 0x7f);
    static const QColor palette[] = {
        QColor(0x65, 0x8a, 0xc8), QColor(0xb8, 0x8a, 0x48),
        QColor(0x8a, 0x70, 0xb8), QColor(0x4e, 0x8c, 0x9a)};
    return palette[row % 4];
}

static QString layer_type_short(LayerType type)
{
    switch (type) {
    case LayerType::Text: return "T";
    case LayerType::SolidRect: return "■";
    case LayerType::Image: return "▧";
    case LayerType::Shape: return "◆";
    }
    return "•";
}

static QIcon obs_icon(QWidget *widget, const QStringList &names, QStyle::StandardPixmap fallback)
{
    for (const QString &name : names) {
        QIcon icon = QIcon::fromTheme(name);
        if (!icon.isNull()) return icon;
    }
    return widget ? widget->style()->standardIcon(fallback) : QIcon();
}

static QColor color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
}

static uint32_t argb_from_color(const QColor &color)
{
    return ((uint32_t)color.alpha() << 24) |
           ((uint32_t)color.red() << 16) |
           ((uint32_t)color.green() << 8) |
           (uint32_t)color.blue();
}

static QPointF anchor_point_from_index(int index)
{
    static const QPointF anchors[] = {
        {0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0},
        {0.0, 0.5}, {0.5, 0.5}, {1.0, 0.5},
        {0.0, 1.0}, {0.5, 1.0}, {1.0, 1.0},
    };
    if (index < 0 || index >= 9) return anchors[4];
    return anchors[index];
}

static int anchor_index_from_layer(const Layer &layer)
{
    int x = layer.origin_x < 0.25f ? 0 : (layer.origin_x > 0.75f ? 2 : 1);
    int y = layer.origin_y < 0.25f ? 0 : (layer.origin_y > 0.75f ? 2 : 1);
    return y * 3 + x;
}

static QPointF rotated_scaled_delta(double dx, double dy, double rot_deg, double sx, double sy)
{
    double rot = rot_deg * 3.14159265358979323846 / 180.0;
    double x = dx * sx;
    double y = dy * sy;
    double c = std::cos(rot);
    double s = std::sin(rot);
    return QPointF(x * c - y * s, x * s + y * c);
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
    double radians = eval_shadow_angle(layer, t) * 3.14159265358979323846 / 180.0;
    double distance = eval_shadow_distance(layer, t);
    return QPointF(std::cos(radians) * distance, std::sin(radians) * distance);
}

static void set_channel_statics(Layer &layer, bool text, uint32_t argb)
{
    auto &a = text ? layer.text_color_a : layer.fill_color_a;
    auto &r = text ? layer.text_color_r : layer.fill_color_r;
    auto &g = text ? layer.text_color_g : layer.fill_color_g;
    auto &b = text ? layer.text_color_b : layer.fill_color_b;
    a.static_value = (argb >> 24) & 0xFF;
    r.static_value = (argb >> 16) & 0xFF;
    g.static_value = (argb >> 8) & 0xFF;
    b.static_value = argb & 0xFF;
}

static void add_or_replace_keyframe(AnimatedProperty &prop, double time, double value)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    prop.static_value = value;
    for (auto &kf : prop.keyframes) {
        if (std::abs(kf.time - time) <= kEpsilon) {
            kf.time = time;
            kf.value = value;
            return;
        }
    }
    Keyframe kf;
    kf.time = time;
    kf.value = value;
    kf.easing = EasingType::Linear;
    prop.keyframes.push_back(kf);
    std::sort(prop.keyframes.begin(), prop.keyframes.end(),
              [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
}

static void set_animated_value(AnimatedProperty &prop, double time, double value)
{
    if (prop.is_animated())
        add_or_replace_keyframe(prop, time, value);
    else
        prop.static_value = value;
}

static void set_color_channels_at(Layer &layer, bool text, double time, uint32_t argb)
{
    auto &a = text ? layer.text_color_a : layer.fill_color_a;
    auto &r = text ? layer.text_color_r : layer.fill_color_r;
    auto &g = text ? layer.text_color_g : layer.fill_color_g;
    auto &b = text ? layer.text_color_b : layer.fill_color_b;
    set_animated_value(a, time, (argb >> 24) & 0xFF);
    set_animated_value(r, time, (argb >> 16) & 0xFF);
    set_animated_value(g, time, (argb >> 8) & 0xFF);
    set_animated_value(b, time, argb & 0xFF);
}

static void set_shadow_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.shadow_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.shadow_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.shadow_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.shadow_color_b, time, argb & 0xFF);
}

static bool keyframe_at_time(const AnimatedProperty &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    for (const auto &kf : prop.keyframes)
        if (std::abs(kf.time - time) <= kEpsilon) return true;
    return false;
}

static void remove_keyframe_at(AnimatedProperty &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    prop.keyframes.erase(
        std::remove_if(prop.keyframes.begin(), prop.keyframes.end(),
                       [&](const Keyframe &kf) { return std::abs(kf.time - time) <= kEpsilon; }),
        prop.keyframes.end());
}

static void toggle_keyframe(AnimatedProperty &prop, double time, double value)
{
    if (keyframe_at_time(prop, time))
        remove_keyframe_at(prop, time);
    else
        add_or_replace_keyframe(prop, time, value);
}

static bool any_keyframe_at_time(std::initializer_list<const AnimatedProperty *> props, double time)
{
    for (const auto *prop : props)
        if (prop && keyframe_at_time(*prop, time)) return true;
    return false;
}

static void style_color_button(QPushButton *button, uint32_t argb)
{
    QColor c = color_from_argb(argb);
    button->setText(c.name(QColor::HexArgb));
    button->setStyleSheet(QString(
        "QPushButton{color:%1;background:%2;border:1px solid #555;"
        "border-radius:3px;padding:3px 8px;}")
        .arg(c.lightness() < 128 ? "#fff" : "#000")
        .arg(c.name(QColor::HexArgb)));
}


static QColor keyframe_color(EasingType easing)
{
    switch (easing) {
    case EasingType::Linear:
        return C_KF_DOT;
    case EasingType::Hold:
        return QColor(0xd8, 0x44, 0x44);
    case EasingType::EaseIn:
    case EasingType::EaseOut:
    case EasingType::EaseInOut:
        return QColor(0x43, 0xd1, 0x7a);
    case EasingType::Bezier:
        return QColor(0x55, 0xbc, 0xff);
    default:
        return C_KF_DOT;
    }
}

static std::vector<AnimatedProperty *> timeline_properties(Layer &layer)
{
    return {&layer.pos_x, &layer.pos_y,
            &layer.scale_x, &layer.scale_y,
            &layer.rotation, &layer.opacity,
            &layer.box_width, &layer.box_height,
            &layer.origin_x_prop, &layer.origin_y_prop,
            &layer.text_color_a, &layer.text_color_r,
            &layer.text_color_g, &layer.text_color_b,
            &layer.fill_color_a, &layer.fill_color_r,
            &layer.fill_color_g, &layer.fill_color_b,
            &layer.shadow_enabled_prop, &layer.shadow_opacity_prop,
            &layer.shadow_distance_prop, &layer.shadow_angle_prop,
            &layer.shadow_blur_prop, &layer.shadow_spread_prop,
            &layer.shadow_color_a, &layer.shadow_color_r,
            &layer.shadow_color_g, &layer.shadow_color_b};
}

static QString property_label(const std::string &name)
{
    if (name == "pos_x" || name == "pos_y") return "Position";
    if (name == "scale_x" || name == "scale_y") return "Scale";
    if (name == "box_width" || name == "box_height") return "Size";
    if (name == "origin_x" || name == "origin_y") return "Origin";
    if (name == "text_color_a" || name == "text_color_r" ||
        name == "text_color_g" || name == "text_color_b") return "Text Color";
    if (name == "fill_color_a" || name == "fill_color_r" ||
        name == "fill_color_g" || name == "fill_color_b") return "Fill Color";
    if (name == "shadow_color_a" || name == "shadow_color_r" ||
        name == "shadow_color_g" || name == "shadow_color_b") return "Shadow Color";
    if (name == "shadow_enabled") return "Shadow Enable";
    if (name == "shadow_opacity") return "Shadow Opacity";
    if (name == "shadow_distance") return "Shadow Distance";
    if (name == "shadow_angle") return "Shadow Angle";
    if (name == "shadow_blur") return "Shadow Blur";
    if (name == "shadow_spread") return "Shadow Spread";
    if (name == "rotation") return "Rotation";
    if (name == "opacity") return "Opacity";
    return QString::fromStdString(name);
}

static QString property_value_text(const AnimatedProperty &prop, const Layer &layer)
{
    double value = prop.static_value;
    if (prop.name == "pos_x")
        return QString("%1,%2").arg(layer.pos_x.static_value, 0, 'f', 1)
                                .arg(layer.pos_y.static_value, 0, 'f', 1);
    if (prop.name == "scale_x")
        return QString("%1,%2%").arg(layer.scale_x.static_value * 100.0, 0, 'f', 1)
                                 .arg(layer.scale_y.static_value * 100.0, 0, 'f', 1);
    if (prop.name == "box_width")
        return QString("%1 × %2").arg(layer.box_width.static_value, 0, 'f', 0)
                                  .arg(layer.box_height.static_value, 0, 'f', 0);
    if (prop.name == "origin_x")
        return QString("%1,%2").arg(layer.origin_x_prop.static_value, 0, 'f', 2)
                                .arg(layer.origin_y_prop.static_value, 0, 'f', 2);
    if (prop.name == "opacity" || prop.name == "shadow_opacity") value *= 100.0;
    if (prop.name == "shadow_enabled") return value >= 0.5 ? "On" : "Off";
    return QString::number(value, 'f', (prop.name == "opacity" || prop.name == "shadow_opacity") ? 1 : 2);
}

struct TimelineRow {
    std::shared_ptr<Layer> layer;
    AnimatedProperty *prop = nullptr;
    bool is_property = false;
};

static std::vector<TimelineRow> timeline_rows(const std::shared_ptr<Title> &title)
{
    std::vector<TimelineRow> rows;
    if (!title) return rows;
    for (auto it = title->layers.rbegin(); it != title->layers.rend(); ++it) {
        auto layer = *it;
        rows.push_back({layer, nullptr, false});
        if (!layer->properties_expanded) continue;
        std::set<std::string> seen;
        for (auto *prop : timeline_properties(*layer)) {
            if (!prop->is_animated()) continue;
            QString label = property_label(prop->name);
            std::string key = label.toStdString();
            if (seen.insert(key).second)
                rows.push_back({layer, prop, true});
        }
    }
    return rows;
}

/* ══════════════════════════════════════════════════════════════════
 *  TitleEditor
 * ══════════════════════════════════════════════════════════════════ */
TitleEditor::TitleEditor(QWidget *parent)
    : QDialog(parent, Qt::Window)
{
    setWindowTitle("OBS Titler Pro Editor");
    resize(1280, 760);
    setMinimumSize(900, 600);

    /* Dark background */
    QPalette pal = palette();
    pal.setColor(QPalette::Window,     C_BG_DARK);
    pal.setColor(QPalette::WindowText, C_TEXT);
    pal.setColor(QPalette::Base,       C_BG_MID);
    pal.setColor(QPalette::AlternateBase, C_BG_LIGHT);
    pal.setColor(QPalette::Text,       C_TEXT);
    pal.setColor(QPalette::Button,     C_BG_LIGHT);
    pal.setColor(QPalette::ButtonText, C_TEXT);
    pal.setColor(QPalette::Highlight,  C_ACCENT);
    setPalette(pal);
    setAutoFillBackground(true);

    build_ui();

    play_timer_ = new QTimer(this);
    play_timer_->setInterval(std::max(1, (int)std::round(obs_frame_duration() * 1000.0)));
    connect(play_timer_, &QTimer::timeout, this, &TitleEditor::tick);
}

void TitleEditor::build_ui()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    /* ── Toolbar ── */
    build_toolbar();
    root->addWidget(toolbar_);

    /* ── Title name label bar ── */
    title_lbl_ = new QLabel("—", this);
    title_lbl_->setAlignment(Qt::AlignCenter);
    QFont tf = title_lbl_->font();
    tf.setPointSize(tf.pointSize() + 1);
    tf.setBold(true);
    title_lbl_->setFont(tf);
    title_lbl_->setStyleSheet("background:#1e1e1e; color:#fff; padding:3px;");
    root->addWidget(title_lbl_);

    /* ── Upper split: Global Settings | Canvas | Properties ── */
    auto *upper_split = new QSplitter(Qt::Horizontal, this);

    auto *global_panel = new QWidget(upper_split);
    auto *global_layout = new QVBoxLayout(global_panel);
    global_layout->setContentsMargins(0, 0, 0, 0);
    global_layout->setSpacing(4);
    title_props_ = new TitlePropertiesPanel(global_panel);
    global_layout->addWidget(title_props_);
    global_layout->addStretch(1);
    global_panel->setFixedWidth(300);
    upper_split->addWidget(global_panel);

    canvas_ = new CanvasPreview(upper_split);
    canvas_->setMinimumSize(300, 200);
    upper_split->addWidget(canvas_);

    auto *side_panel = new QWidget(upper_split);
    auto *side_layout = new QVBoxLayout(side_panel);
    side_layout->setContentsMargins(0, 0, 0, 0);
    side_layout->setSpacing(4);
    props_ = new PropertiesPanel(side_panel);
    side_layout->addWidget(props_, 1);
    side_panel->setFixedWidth(300);
    upper_split->addWidget(side_panel);
    upper_split->setStretchFactor(0, 0);
    upper_split->setStretchFactor(1, 3);
    upper_split->setStretchFactor(2, 1);

    /* ── Lower split: LayerStack | Timeline ── */
    auto *lower_split = new QSplitter(Qt::Horizontal, this);

    auto *layers_panel = new QWidget(lower_split);
    auto *layers_layout = new QVBoxLayout(layers_panel);
    layers_layout->setContentsMargins(0, 0, 0, 0);
    layers_layout->setSpacing(0);

    auto *layer_transport = new QToolBar(layers_panel);
    layer_transport->setMovable(false);
    layer_transport->setIconSize(QSize(14, 14));
    layer_transport->setStyleSheet(
        "QToolBar{background:#141414;border-bottom:1px solid #333;spacing:1px;}"
        "QToolButton{color:#ccc;background:transparent;padding:3px 5px;border:none;}"
        "QToolButton:hover{background:#333;border-radius:2px;}");
    layer_transport->addAction(act_rew_);
    layer_transport->addAction(act_prev_kf_);
    layer_transport->addAction(act_play_);
    layer_transport->addAction(act_full_loop_);
    layer_transport->addAction("▶|", this, &TitleEditor::step_forward);
    layer_transport->addAction(act_next_kf_);
    layers_layout->addWidget(layer_transport);

    layers_ = new LayerStack(layers_panel);
    layers_->setMinimumHeight(140);
    layers_layout->addWidget(layers_, 1);
    layers_panel->setFixedWidth(360);
    lower_split->addWidget(layers_panel);

    timeline_ = new TimelineWidget(lower_split);
    timeline_->setMinimumHeight(140);
    lower_split->addWidget(timeline_);
    lower_split->setStretchFactor(0, 0);
    lower_split->setStretchFactor(1, 1);

    /* ── Outer vertical split ── */
    auto *vsplit = new QSplitter(Qt::Vertical, this);
    vsplit->addWidget(upper_split);
    vsplit->addWidget(lower_split);
    vsplit->setStretchFactor(0, 3);
    vsplit->setStretchFactor(1, 2);
    root->addWidget(vsplit, 1);

    /* ── Connect sub-widget signals ── */
    connect(layers_, &LayerStack::layer_selected,
            this, &TitleEditor::on_layer_selected);

    connect(layers_, &LayerStack::add_layer_requested,
            this, [this](LayerType type) {
                if (!title_) return;
                auto l = std::make_shared<Layer>();
                l->id   = TitleDataStore::make_uuid();
                l->name = (type == LayerType::Text) ? "Text" :
                          (type == LayerType::Image) ? "Image" : "Rectangle";
                l->type = type;
                l->text_content = (type == LayerType::Text) ? "New Text" : "";
                l->pos_x.static_value = title_->width  / 2.0;
                l->pos_y.static_value = title_->height / 2.0;
                l->rect_width = title_->width * 0.5f;
                l->rect_height = (type == LayerType::Image) ? title_->height * 0.4f : 160.0f;
                l->box_width.static_value = l->rect_width;
                l->box_height.static_value = l->rect_height;
                l->origin_x_prop.static_value = l->origin_x;
                l->origin_y_prop.static_value = l->origin_y;
                set_channel_statics(*l, true, l->text_color);
                set_channel_statics(*l, false, l->fill_color);
                if (type == LayerType::Image) {
                    l->lock_aspect_ratio = true;
                    QString path = QFileDialog::getOpenFileName(
                        this, "Choose Image", QString(),
                        "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)");
                    if (path.isEmpty()) return;
                    l->image_path = path.toStdString();
                    QImage img(path);
                    if (!img.isNull()) {
                        l->rect_width = (float)img.width();
                        l->rect_height = (float)img.height();
                        l->box_width.static_value = l->rect_width;
                        l->box_height.static_value = l->rect_height;
                    }
                }
                l->out_time = title_->duration;
                title_->add_layer(l);
                layers_->refresh();
                on_layer_selected(l->id);
                on_title_modified();
            });

    connect(layers_, &LayerStack::delete_layer_requested,
            this, [this](const std::string &lid) {
                if (!title_) return;
                title_->remove_layer(lid);
                if (sel_layer_id_ == lid) sel_layer_id_.clear();
                layers_->refresh();

                if (!title_->layers.empty())
                    on_layer_selected(title_->layers.back()->id);
                else
                    props_->set_layer(nullptr, playhead_);

                on_title_modified();
            });

    connect(layers_, &LayerStack::layer_visibility_changed,
            this, [this](const std::string &lid, bool visible) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->visible = visible;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_lock_changed,
            this, [this](const std::string &lid, bool locked) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->locked = locked;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_expand_changed,
            this, [this](const std::string &lid, bool expanded) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->properties_expanded = expanded;
                    layers_->refresh();
                    timeline_->set_title(title_);
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_parent_changed,
            this, [this](const std::string &lid, const std::string &parent_id) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->parent_id = parent_id;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_name_changed,
            this, [this](const std::string &lid, const std::string &name) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    if (layer->name == name) return;
                    layer->name = name.empty() ? "Layer" : name;
                    timeline_->set_title(title_);
                    on_title_modified();
                    QTimer::singleShot(0, layers_, [this]() {
                        if (layers_) layers_->refresh();
                    });
                }
            });

    connect(timeline_, &TimelineWidget::playhead_changed,
            this, &TitleEditor::on_playhead_changed);
    connect(timeline_, &TimelineWidget::keyframe_easing_changed,
            this, &TitleEditor::on_title_modified);

    connect(props_, &PropertiesPanel::property_changed,
            this, &TitleEditor::on_title_modified);
    connect(title_props_, &TitlePropertiesPanel::title_changed,
            this, [this]() {
                if (!title_) return;
                playhead_ = std::clamp(playhead_, 0.0, title_->duration);
                on_title_modified();
                timeline_->set_title(title_);
                on_playhead_changed(playhead_);
            });
    connect(layers_, &LayerStack::layer_order_changed,
            this, [this]() {
                layers_->refresh();
                canvas_->refresh_preview();
                timeline_->set_title(title_);
                on_title_modified();
            });

    connect(canvas_, &CanvasPreview::layer_clicked,
            this, &TitleEditor::on_layer_selected);
    connect(canvas_, &CanvasPreview::layer_geometry_changed,
            this, [this]() {
                on_title_modified();
                if (title_ && !sel_layer_id_.empty()) {
                    if (auto layer = title_->find_layer(sel_layer_id_))
                        props_->set_layer(layer, playhead_);
                }
            });
}

void TitleEditor::align_selected_to_canvas(int x_mode, int y_mode)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        double w = eval_box_width(*layer, lt);
        double h = eval_box_height(*layer, lt);
        double x = layer->origin_x * w;
        if (x_mode == 1) x = title_->width / 2.0;
        if (x_mode == 2) x = title_->width - (1.0 - layer->origin_x) * w;
        double y = layer->origin_y * h;
        if (y_mode == 1) y = title_->height / 2.0;
        if (y_mode == 2) y = title_->height - (1.0 - layer->origin_y) * h;
        set_animated_value(layer->pos_x, lt, x);
        set_animated_value(layer->pos_y, lt, y);
        last_layer = layer;
    }
    on_title_modified();
    if (props_ && last_layer) props_->set_layer(last_layer, playhead_);
}


void TitleEditor::align_selected_layers_horizontal()
{
    align_selected_layers(1, -1);
}

void TitleEditor::align_selected_layers_vertical()
{
    align_selected_layers(-1, 1);
}

void TitleEditor::align_selected_layers(int x_mode, int y_mode)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    struct Entry {
        std::shared_ptr<Layer> layer;
        double lt;
        double width;
        double height;
        double scale_x;
        double scale_y;
    };

    std::vector<Entry> entries;
    double min_left = std::numeric_limits<double>::infinity();
    double max_right = -std::numeric_limits<double>::infinity();
    double min_top = std::numeric_limits<double>::infinity();
    double max_bottom = -std::numeric_limits<double>::infinity();

    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        double width = eval_box_width(*layer, lt);
        double height = eval_box_height(*layer, lt);
        double sx = layer->scale_x.evaluate(lt);
        double sy = layer->scale_y.evaluate(lt);
        double left = layer->pos_x.evaluate(lt) - layer->origin_x * width * sx;
        double right = layer->pos_x.evaluate(lt) + (1.0 - layer->origin_x) * width * sx;
        double top = layer->pos_y.evaluate(lt) - layer->origin_y * height * sy;
        double bottom = layer->pos_y.evaluate(lt) + (1.0 - layer->origin_y) * height * sy;
        min_left = std::min(min_left, left);
        max_right = std::max(max_right, right);
        min_top = std::min(min_top, top);
        max_bottom = std::max(max_bottom, bottom);
        entries.push_back({layer, lt, width, height, sx, sy});
    }

    if (entries.empty()) return;
    if (alignment_target_ == 0 && entries.size() < 2) return;

    double target_left = alignment_target_ == 2 ? 0.0 : min_left;
    double target_hcenter = alignment_target_ == 2 ? title_->width / 2.0 : (min_left + max_right) / 2.0;
    double target_right = alignment_target_ == 2 ? title_->width : max_right;
    double target_top = alignment_target_ == 2 ? 0.0 : min_top;
    double target_vcenter = alignment_target_ == 2 ? title_->height / 2.0 : (min_top + max_bottom) / 2.0;
    double target_bottom = alignment_target_ == 2 ? title_->height : max_bottom;

    std::shared_ptr<Layer> last_layer;
    for (const auto &entry : entries) {
        if (x_mode >= 0) {
            double next_x = entry.layer->pos_x.evaluate(entry.lt);
            if (x_mode == 0) next_x = target_left + entry.layer->origin_x * entry.width * entry.scale_x;
            if (x_mode == 1) next_x = target_hcenter - (0.5 - entry.layer->origin_x) * entry.width * entry.scale_x;
            if (x_mode == 2) next_x = target_right - (1.0 - entry.layer->origin_x) * entry.width * entry.scale_x;
            set_animated_value(entry.layer->pos_x, entry.lt, next_x);
        }
        if (y_mode >= 0) {
            double next_y = entry.layer->pos_y.evaluate(entry.lt);
            if (y_mode == 0) next_y = target_top + entry.layer->origin_y * entry.height * entry.scale_y;
            if (y_mode == 1) next_y = target_vcenter - (0.5 - entry.layer->origin_y) * entry.height * entry.scale_y;
            if (y_mode == 2) next_y = target_bottom - (1.0 - entry.layer->origin_y) * entry.height * entry.scale_y;
            set_animated_value(entry.layer->pos_y, entry.lt, next_y);
        }
        last_layer = entry.layer;
    }
    on_title_modified();
    if (props_ && last_layer) props_->set_layer(last_layer, playhead_);
}

void TitleEditor::build_toolbar()
{
    toolbar_ = new QToolBar(this);
    toolbar_->setMovable(false);
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setStyleSheet(
        "QToolBar { background:#1a1a1a; border-bottom:1px solid #333; spacing:2px; }"
        "QToolButton { color:#ccc; background:transparent; padding:4px 8px; border:none; }"
        "QToolButton:hover { background:#333; border-radius:3px; }"
        "QToolButton:pressed { background:#0078d4; }");

    act_rew_ = new QAction(obs_icon(this, {"media-skip-backward", "go-first"}, QStyle::SP_MediaSkipBackward), "⏮", this);
    act_prev_kf_ = new QAction(obs_icon(this, {"go-previous", "media-seek-backward"}, QStyle::SP_MediaSeekBackward), "◆◀", this);
    act_play_ = new QAction(obs_icon(this, {"media-playback-start"}, QStyle::SP_MediaPlay), "▶", this);
    act_full_loop_ = new QAction(obs_icon(this, {"media-playlist-repeat", "view-refresh"}, QStyle::SP_BrowserReload), "↻", this);
    act_full_loop_->setToolTip("Loop preview from beginning to end of the title");
    act_next_kf_ = new QAction(obs_icon(this, {"go-next", "media-seek-forward"}, QStyle::SP_MediaSeekForward), "▶◆", this);

    connect(act_rew_, &QAction::triggered, this, &TitleEditor::rewind);
    connect(act_prev_kf_, &QAction::triggered, this, &TitleEditor::previous_keyframe);
    connect(act_play_, &QAction::triggered, this, &TitleEditor::play_pause);
    connect(act_full_loop_, &QAction::triggered, this, &TitleEditor::play_full_loop);
    connect(act_next_kf_, &QAction::triggered, this, &TitleEditor::next_keyframe);

    time_lbl_ = new QLabel("0.000 s", toolbar_);
    time_lbl_->setStyleSheet("color:#0af; font-family:monospace; min-width:70px;");
    toolbar_->addWidget(time_lbl_);

    toolbar_->addSeparator();

    /* Zoom controls */
    auto *zoom_lbl = new QLabel(" Zoom: ", toolbar_);
    zoom_lbl->setStyleSheet("color:#888;");
    toolbar_->addWidget(zoom_lbl);

    auto *zoom_in  = new QPushButton("+", toolbar_);
    auto *zoom_out = new QPushButton("−", toolbar_);
    zoom_in->setIcon(obs_icon(this, {"zoom-in"}, QStyle::SP_ArrowUp));
    zoom_out->setIcon(obs_icon(this, {"zoom-out"}, QStyle::SP_ArrowDown));
    zoom_in->setFixedWidth(22);
    zoom_out->setFixedWidth(22);
    zoom_in->setStyleSheet("color:#ccc; background:#2a2a2a; border:none; border-radius:2px;");
    zoom_out->setStyleSheet(zoom_in->styleSheet());
    toolbar_->addWidget(zoom_out);
    toolbar_->addWidget(zoom_in);

    toolbar_->addSeparator();
    auto *align_target = new QToolButton(toolbar_);
    align_target->setText("▣⌄");
    align_target->setToolTip("Alignment target");
    align_target->setPopupMode(QToolButton::InstantPopup);
    align_target->setStyleSheet("QToolButton{color:#ddd;background:#3a3a3a;border:1px solid #666;border-radius:2px;padding:3px 8px;} QToolButton::menu-indicator{image:none;}");
    auto *align_menu = new QMenu(align_target);
    QAction *target_selection = align_menu->addAction("Align to Selection");
    QAction *target_key = align_menu->addAction("Align to Key Object");
    target_key->setEnabled(false);
    QAction *target_artboard = align_menu->addAction("Align to Artboard");
    target_selection->setCheckable(true);
    target_artboard->setCheckable(true);
    target_artboard->setChecked(true);
    auto update_alignment_target = [this, align_target, target_selection, target_artboard](int target) {
        alignment_target_ = target;
        target_selection->setChecked(target == 0);
        target_artboard->setChecked(target == 2);
        align_target->setToolTip(target == 0 ? "Align to Selection" : "Align to Artboard");
    };
    connect(target_selection, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(0); });
    connect(target_artboard, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(2); });
    align_target->setMenu(align_menu);
    toolbar_->addWidget(align_target);

    auto add_align_action = [this](const QString &text, const QString &tip, int x_mode, int y_mode) {
        QAction *action = toolbar_->addAction(text);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, x_mode, y_mode]() {
            align_selected_layers(x_mode, y_mode);
        });
        return action;
    };
    add_align_action("|◧", "Align Left", 0, -1);
    add_align_action("↔", "Align Horizontal Center", 1, -1);
    add_align_action("◨|", "Align Right", 2, -1);
    add_align_action("▔", "Align Top", -1, 0);
    add_align_action("↕", "Align Vertical Center", -1, 1);
    add_align_action("▁", "Align Bottom", -1, 2);
    add_align_action("▦", "Align Center to Artboard", 1, 1);

    act_safe_guides_ = toolbar_->addAction("Safe");
    act_safe_guides_->setCheckable(true);
    act_safe_guides_->setToolTip("Show title/action safe guides in the editor preview only");
    connect(act_safe_guides_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_safe_guides_visible(visible);
    });

    toolbar_->addSeparator();
    act_undo_ = toolbar_->addAction("↶");
    act_undo_->setToolTip("Undo");
    act_undo_->setShortcut(QKeySequence::Undo);
    connect(act_undo_, &QAction::triggered, this, [this]() {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
    });
    act_redo_ = toolbar_->addAction("↷");
    act_redo_->setToolTip("Redo");
    act_redo_->setShortcut(QKeySequence::Redo);
    connect(act_redo_, &QAction::triggered, this, [this]() {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
    });
    addAction(act_undo_);
    addAction(act_redo_);
    update_undo_redo_actions();

    toolbar_->addSeparator();

    /* Save button */
    auto *btn_save = new QPushButton("Save", toolbar_);
    btn_save->setIcon(obs_icon(this, {"document-save"}, QStyle::SP_DialogSaveButton));
    btn_save->setStyleSheet(
        "QPushButton { color:#fff; background:#0078d4; border:none;"
        "  border-radius:3px; padding:4px 10px; }"
        "QPushButton:hover { background:#1088e4; }");
    connect(btn_save, &QPushButton::clicked, this, [this]() {
        TitleDataStore::instance().save();
        if (title_) emit title_saved(title_->id);
        setWindowTitle("OBS Titler Pro Editor  ·  saved");
    });
    toolbar_->addWidget(btn_save);
}

/* ── open_title ──────────────────────────────────────────────────── */
void TitleEditor::open_title(const std::string &tid)
{
    play_timer_->stop();
    playing_ = false;
    act_play_->setText("▶");
    act_play_->setIcon(obs_icon(this, {"media-playback-start"}, QStyle::SP_MediaPlay));
    playhead_ = 0.0;
    playback_reverse_ = false;
    full_loop_playback_ = false;

    title_ = TitleDataStore::instance().get_title(tid);
    if (!title_) return;

    update_title_bar();
    canvas_->set_title(title_);
    layers_->set_title(title_);
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);

    if (!title_->layers.empty())
        on_layer_selected(title_->layers.back()->id);
    else
        props_->set_layer(nullptr, playhead_);

    undo_stack_.clear();
    undo_index_ = -1;
    push_undo_snapshot();
    update_undo_redo_actions();

    on_playhead_changed(0.0);
}

std::shared_ptr<Title> TitleEditor::clone_title(const Title &title) const
{
    auto clone = std::make_shared<Title>(title);
    clone->layers.clear();
    clone->layers.reserve(title.layers.size());
    for (const auto &layer : title.layers) {
        if (layer) clone->layers.push_back(std::make_shared<Layer>(*layer));
    }
    return clone;
}

void TitleEditor::push_undo_snapshot()
{
    if (!title_ || restoring_undo_) return;
    if (undo_index_ + 1 < (int)undo_stack_.size())
        undo_stack_.erase(undo_stack_.begin() + undo_index_ + 1, undo_stack_.end());
    undo_stack_.push_back(clone_title(*title_));
    if (undo_stack_.size() > 30)
        undo_stack_.erase(undo_stack_.begin());
    undo_index_ = (int)undo_stack_.size() - 1;
    update_undo_redo_actions();
}

void TitleEditor::restore_undo_snapshot(int index)
{
    if (!title_ || index < 0 || index >= (int)undo_stack_.size()) return;
    restoring_undo_ = true;
    auto snapshot = undo_stack_[(size_t)index];
    title_->name = snapshot->name;
    title_->duration = snapshot->duration;
    title_->loop_start = snapshot->loop_start;
    title_->loop_end = snapshot->loop_end;
    title_->playback_mode = snapshot->playback_mode;
    title_->loop_type = snapshot->loop_type;
    title_->pause_time = snapshot->pause_time;
    title_->bg_color = snapshot->bg_color;
    title_->width = snapshot->width;
    title_->height = snapshot->height;
    title_->live_text_rows = snapshot->live_text_rows;
    title_->current_cue_row = snapshot->current_cue_row;
    title_->pending_cue_row = snapshot->pending_cue_row;
    title_->cue_revision = snapshot->cue_revision;
    title_->layers.clear();
    title_->layers.reserve(snapshot->layers.size());
    for (const auto &layer : snapshot->layers) {
        if (layer) title_->layers.push_back(std::make_shared<Layer>(*layer));
    }
    undo_index_ = index;
    if (!sel_layer_id_.empty() && !title_->find_layer(sel_layer_id_))
        sel_layer_id_.clear();
    if (sel_layer_id_.empty() && !title_->layers.empty())
        sel_layer_id_ = title_->layers.back()->id;
    update_title_bar();
    canvas_->set_title(title_);
    layers_->set_title(title_);
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);
    if (!sel_layer_id_.empty()) on_layer_selected(sel_layer_id_);
    else props_->set_layer(nullptr, playhead_);
    on_playhead_changed(std::clamp(playhead_, 0.0, title_->duration));
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    restoring_undo_ = false;
    update_undo_redo_actions();
    setWindowTitle("OBS Titler Pro Editor  ·  modified");
}

void TitleEditor::update_undo_redo_actions()
{
    if (act_undo_) act_undo_->setEnabled(undo_index_ > 0);
    if (act_redo_) act_redo_->setEnabled(undo_index_ >= 0 && undo_index_ + 1 < (int)undo_stack_.size());
}

void TitleEditor::update_title_bar()
{
    if (title_)
        title_lbl_->setText(QString::fromStdString(title_->name));
}

/* ── Transport ───────────────────────────────────────────────────── */
void TitleEditor::play_pause()
{
    if (!title_) return;
    if (!playing_)
        full_loop_playback_ = false;
    playing_ = !playing_;
    if (playing_) {
        if (title_->playback_mode != 2 && playhead_ >= title_->duration)
            on_playhead_changed(0.0);
        if (title_->playback_mode == 2 && playhead_ >= std::clamp(title_->pause_time, 0.0, title_->duration))
            on_playhead_changed(0.0);
        act_play_->setText("⏸");
        act_play_->setIcon(obs_icon(this, {"media-playback-pause"}, QStyle::SP_MediaPause));
        playback_clock_.restart();
        play_timer_->start();
    } else {
        act_play_->setText("▶");
        act_play_->setIcon(obs_icon(this, {"media-playback-start"}, QStyle::SP_MediaPlay));
        play_timer_->stop();
    }
}

void TitleEditor::play_full_loop()
{
    if (!title_) return;
    full_loop_playback_ = true;
    playback_reverse_ = false;
    if (!playing_ || playhead_ >= title_->duration)
        on_playhead_changed(0.0);
    playing_ = true;
    act_play_->setText("⏸");
    act_play_->setIcon(obs_icon(this, {"media-playback-pause"}, QStyle::SP_MediaPause));
    playback_clock_.restart();
    play_timer_->start();
}

void TitleEditor::rewind()
{
    full_loop_playback_ = false;
    playback_reverse_ = false;
    on_playhead_changed(0.0);
}

void TitleEditor::step_forward()
{
    if (!title_) return;
    on_playhead_changed(std::min(snap_to_obs_frame(playhead_ + obs_frame_duration()), title_->duration));
}


static void collect_timeline_keyframes(const std::shared_ptr<Layer> &layer,
                                       std::vector<double> &times)
{
    if (!layer) return;
    for (auto *prop : timeline_properties(*layer)) {
        for (const auto &kf : prop->keyframes)
            times.push_back(layer->in_time + kf.time);
    }
}

void TitleEditor::previous_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = -1.0;
    for (double t : times) {
        if (t < playhead_ - kEpsilon)
            target = std::max(target, t);
    }
    if (target >= 0.0) on_playhead_changed(target);
}

void TitleEditor::next_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = title_->duration + 1.0;
    for (double t : times) {
        if (t > playhead_ + kEpsilon)
            target = std::min(target, t);
    }
    if (target <= title_->duration) on_playhead_changed(target);
}

void TitleEditor::tick()
{
    if (!title_ || !playing_) return;
    double dt = playback_clock_.isValid() ? playback_clock_.restart() / 1000.0 : 0.0;
    if (dt <= 0.0 || dt > 0.25) dt = play_timer_->interval() / 1000.0;

    double duration = std::max(0.001, title_->duration);
    double loop_start = std::clamp(title_->loop_start, 0.0, title_->duration);
    double loop_end = std::clamp(title_->loop_end, loop_start, title_->duration);
    double loop_len = std::max(0.001, loop_end - loop_start);
    double t = playhead_;

    if (full_loop_playback_) {
        t = std::fmod(playhead_ + dt, duration);
    } else {
        switch (title_->playback_mode) {
        case 1: /* Loop in/out between Loop Start and Loop End */
            if (loop_end <= loop_start + 0.0001) {
                t = std::fmod(playhead_ + dt, duration);
            } else if (title_->loop_type == 1) {
                t += (playback_reverse_ ? -dt : dt);
                if (!playback_reverse_ && t >= loop_end) {
                    t = loop_end - std::fmod(t - loop_end, loop_len);
                    playback_reverse_ = true;
                } else if (playback_reverse_ && t <= loop_start) {
                    t = loop_start + std::fmod(loop_start - t, loop_len);
                    playback_reverse_ = false;
                }
            } else {
                t = playhead_ + dt;
                if (t >= loop_end)
                    t = loop_start + std::fmod(t - loop_end, loop_len);
            }
            break;
        case 2: { /* Pause at timeline position */
            double pause_time = std::clamp(title_->pause_time, 0.0, title_->duration);
            t = playhead_ + dt;
            if (t >= pause_time) {
                t = pause_time;
                playing_ = false;
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon(this, {"media-playback-start"}, QStyle::SP_MediaPlay));
            }
            break;
        }
        default: /* Play once */
            t = playhead_ + dt;
            if (t >= title_->duration) {
                t = title_->duration;
                playing_ = false;
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon(this, {"media-playback-start"}, QStyle::SP_MediaPlay));
            }
            break;
        }
    }
    on_playhead_changed(snap_to_obs_frame(t));
}

void TitleEditor::keyPressEvent(QKeyEvent *ev)
{
    if (ev->matches(QKeySequence::Undo)) {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::Redo)) {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
        ev->accept();
        return;
    }
    if (ev->key() == Qt::Key_Space && !ev->isAutoRepeat()) {
        QWidget *fw = focusWidget();
        bool editing_text = qobject_cast<QLineEdit *>(fw) ||
                            qobject_cast<QTextEdit *>(fw) ||
                            qobject_cast<QAbstractSpinBox *>(fw) ||
                            qobject_cast<QComboBox *>(fw);
        if (!editing_text) {
            play_pause();
            ev->accept();
            return;
        }
    }
    QDialog::keyPressEvent(ev);
}

/* ── Signal handlers ─────────────────────────────────────────────── */
void TitleEditor::on_layer_selected(const std::string &lid)
{
    sel_layer_id_ = lid;
    layers_->set_selected_layer(lid);
    canvas_->set_selected_layer(lid);
    timeline_->set_selected_layer(lid);

    if (!title_) return;
    auto layer = title_->find_layer(lid);
    if (layer) props_->set_layer(layer, playhead_);
}

void TitleEditor::on_playhead_changed(double t)
{
    t = title_ ? std::clamp(snap_to_obs_frame(t), 0.0, title_->duration) : snap_to_obs_frame(t);
    playhead_ = t;
    canvas_->set_playhead(t);
    timeline_->set_playhead(t);

    if (!sel_layer_id_.empty() && title_) {
        auto l = title_->find_layer(sel_layer_id_);
        if (l) props_->set_layer(l, t);
    }

    if (time_lbl_)
        time_lbl_->setText(QString("%1  (%2 fps)").arg(format_timecode(t)).arg(obs_frame_rate(), 0, 'f', 2));
}

void TitleEditor::on_title_modified()
{
    if (title_) setWindowTitle("OBS Titler Pro Editor  ·  modified");
    canvas_->refresh_preview();
    if (title_props_) title_props_->set_title(title_);
    if (timeline_) timeline_->set_title(title_);
    push_undo_snapshot();
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
}

/* ══════════════════════════════════════════════════════════════════
 *  CanvasPreview
 * ══════════════════════════════════════════════════════════════════ */
CanvasPreview::CanvasPreview(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(400, 225);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background:#111;");
    setMouseTracking(true);
}

void CanvasPreview::set_title(std::shared_ptr<Title> t)
{
    title_ = t; dirty_ = true; update();
}

void CanvasPreview::set_playhead(double t)
{
    playhead_ = t; dirty_ = true; update();
}

void CanvasPreview::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid; update();
}

void CanvasPreview::set_safe_guides_visible(bool visible)
{
    safe_guides_visible_ = visible;
    update();
}

void CanvasPreview::refresh_preview()
{
    dirty_ = true;
    update();
}

std::shared_ptr<Layer> CanvasPreview::selected_layer() const
{
    return title_ ? title_->find_layer(sel_layer_id_) : nullptr;
}

QRectF CanvasPreview::layer_local_rect(const Layer &layer) const
{
    double lt = playhead_ - layer.in_time;
    double w = eval_box_width(layer, lt);
    double h = eval_box_height(layer, lt);
    double ox = eval_origin_x(layer, lt);
    double oy = eval_origin_y(layer, lt);
    return QRectF(-ox * w, -oy * h, w, h);
}

double CanvasPreview::view_scale() const
{
    if (!title_) return 1.0;
    return std::min((double)width() / title_->width,
                    (double)height() / title_->height) * zoom_;
}

QPointF CanvasPreview::view_origin() const
{
    if (!title_) return QPointF(0, 0);
    double scale = view_scale();
    return QPointF((width() - title_->width * scale) / 2.0,
                   (height() - title_->height * scale) / 2.0);
}

QPointF CanvasPreview::view_to_canvas(const QPointF &view_pt) const
{
    double scale = view_scale();
    QPointF origin = view_origin();
    return QPointF((view_pt.x() - origin.x()) / scale,
                   (view_pt.y() - origin.y()) / scale);
}

QPointF CanvasPreview::canvas_to_view(const QPointF &canvas_pt) const
{
    double scale = view_scale();
    QPointF origin = view_origin();
    return QPointF(origin.x() + canvas_pt.x() * scale,
                   origin.y() + canvas_pt.y() * scale);
}

QPointF CanvasPreview::canvas_to_layer(const Layer &layer, const QPointF &canvas_pt) const
{
    double lt = playhead_ - layer.in_time;
    double px = layer.pos_x.evaluate(lt);
    double py = layer.pos_y.evaluate(lt);
    double rot = -layer.rotation.evaluate(lt) * 3.14159265358979323846 / 180.0;
    double dx = canvas_pt.x() - px;
    double dy = canvas_pt.y() - py;
    double c = std::cos(rot);
    double ss = std::sin(rot);
    double sx = std::max(0.0001, layer.scale_x.evaluate(lt));
    double sy = std::max(0.0001, layer.scale_y.evaluate(lt));
    return QPointF((dx * c - dy * ss) / sx,
                   (dx * ss + dy * c) / sy);
}

QPointF CanvasPreview::layer_to_canvas(const Layer &layer, const QPointF &layer_pt) const
{
    double lt = playhead_ - layer.in_time;
    double px = layer.pos_x.evaluate(lt);
    double py = layer.pos_y.evaluate(lt);
    double rot = layer.rotation.evaluate(lt) * 3.14159265358979323846 / 180.0;
    double sx = layer.scale_x.evaluate(lt);
    double sy = layer.scale_y.evaluate(lt);
    double x = layer_pt.x() * sx;
    double y = layer_pt.y() * sy;
    double c = std::cos(rot);
    double ss = std::sin(rot);
    return QPointF(px + x * c - y * ss,
                   py + x * ss + y * c);
}

CanvasPreview::DragMode CanvasPreview::hit_test_selected(const QPointF &view_pt) const
{
    auto layer = selected_layer();
    if (!layer || layer->locked) return DragMode::None;

    double scale = view_scale();
    double handle = 8.0 / std::max(0.1, scale);
    QPointF local = canvas_to_layer(*layer, view_to_canvas(view_pt));
    QRectF r = layer_local_rect(*layer);

    auto near_pt = [&](const QPointF &p) {
        return std::abs(local.x() - p.x()) <= handle &&
               std::abs(local.y() - p.y()) <= handle;
    };

    if (near_pt(r.topLeft())) return DragMode::ResizeNW;
    if (near_pt(QPointF(r.center().x(), r.top()))) return DragMode::ResizeN;
    if (near_pt(r.topRight())) return DragMode::ResizeNE;
    if (near_pt(QPointF(r.right(), r.center().y()))) return DragMode::ResizeE;
    if (near_pt(r.bottomRight())) return DragMode::ResizeSE;
    if (near_pt(QPointF(r.center().x(), r.bottom()))) return DragMode::ResizeS;
    if (near_pt(r.bottomLeft())) return DragMode::ResizeSW;
    if (near_pt(QPointF(r.left(), r.center().y()))) return DragMode::ResizeW;
    if (std::hypot(local.x(), local.y()) <= handle * 1.25) return DragMode::Origin;
    if (r.adjusted(-handle, -handle, handle, handle).contains(local)) return DragMode::Move;
    return DragMode::None;
}

void CanvasPreview::apply_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    auto layer = selected_layer();
    if (!layer || drag_mode_ == DragMode::None) return;

    QPointF canvas = view_to_canvas(view_pt);
    QPointF delta = canvas - drag_start_canvas_;
    double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time));

    if (drag_mode_ == DragMode::Move) {
        if (modifiers & Qt::ShiftModifier) {
            if (std::abs(delta.x()) >= std::abs(delta.y()))
                delta.setY(0.0);
            else
                delta.setX(0.0);
        }
        set_animated_value(layer->pos_x, lt, drag_start_x_ + delta.x());
        set_animated_value(layer->pos_y, lt, drag_start_y_ + delta.y());
    } else if (drag_mode_ == DragMode::Origin) {
        double w = std::max(1.0f, drag_start_w_);
        double h = std::max(1.0f, drag_start_h_);
        layer->origin_x = (float)std::clamp(drag_start_origin_x_ + delta.x() / w, 0.0, 1.0);
        layer->origin_y = (float)std::clamp(drag_start_origin_y_ + delta.y() / h, 0.0, 1.0);
        set_animated_value(layer->origin_x_prop, lt, layer->origin_x);
        set_animated_value(layer->origin_y_prop, lt, layer->origin_y);
        set_animated_value(layer->pos_x, lt, drag_start_x_ + delta.x());
        set_animated_value(layer->pos_y, lt, drag_start_y_ + delta.y());
    } else {
        QPointF local = canvas_to_layer(*layer, canvas);
        double left = -drag_start_origin_x_ * drag_start_w_;
        double right = (1.0 - drag_start_origin_x_) * drag_start_w_;
        double top = -drag_start_origin_y_ * drag_start_h_;
        double bottom = (1.0 - drag_start_origin_y_) * drag_start_h_;

        bool resize_left = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeW;
        bool resize_right = drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeE;
        bool resize_top = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeN;
        bool resize_bottom = drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeS;

        if (resize_left)
            left = std::min(local.x(), right - 1.0);
        else if (resize_right)
            right = std::max(local.x(), left + 1.0);

        if (resize_top)
            top = std::min(local.y(), bottom - 1.0);
        else if (resize_bottom)
            bottom = std::max(local.y(), top + 1.0);

        double new_w = std::max(1.0, right - left);
        double new_h = std::max(1.0, bottom - top);
        if (layer->type == LayerType::Image && layer->lock_aspect_ratio && drag_start_h_ > 0.0f) {
            double aspect = drag_start_w_ / drag_start_h_;
            if (std::abs(new_w - drag_start_w_) > std::abs(new_h - drag_start_h_) * aspect)
                new_h = new_w / aspect;
            else
                new_w = new_h * aspect;
        }
        layer->rect_width = (float)new_w;
        layer->rect_height = (float)new_h;
        set_animated_value(layer->box_width, lt, new_w);
        set_animated_value(layer->box_height, lt, new_h);
    }

    dirty_ = true;
    drag_changed_ = true;
    update();
}

void CanvasPreview::render_to_pixmap()
{
    if (!title_) { frame_pixmap_ = QPixmap(); return; }

    QImage img(title_->width, title_->height, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    if (title_->bg_color >> 24) {
        QColor bg((title_->bg_color >> 16) & 0xFF,
                  (title_->bg_color >>  8) & 0xFF,
                  (title_->bg_color >>  0) & 0xFF,
                  (title_->bg_color >> 24) & 0xFF);
        p.fillRect(img.rect(), bg);
    }

    double t = playhead_;

    for (auto &layer : title_->layers) {
        if (!layer->visible) continue;
        if (t < layer->in_time || t > layer->out_time) continue;
        double lt = t - layer->in_time;

        p.save();
        p.setOpacity(layer->opacity.evaluate(lt));
        p.translate(layer->pos_x.evaluate(lt), layer->pos_y.evaluate(lt));
        p.rotate(layer->rotation.evaluate(lt));
        p.scale(layer->scale_x.evaluate(lt), layer->scale_y.evaluate(lt));

        QRectF box = layer_local_rect(*layer);

        if (layer->type == LayerType::SolidRect) {
            QColor fc = color_from_argb(eval_fill_color(*layer, lt));
            if (eval_shadow_enabled(*layer, lt)) {
                QColor sc = color_from_argb(eval_shadow_color(*layer, lt));
                sc.setAlphaF(std::clamp((double)sc.alphaF() * eval_shadow_opacity(*layer, lt), 0.0, 1.0));
                QPointF off = shadow_offset(*layer, lt);
                double blur = eval_shadow_blur(*layer, lt);
                double spread = eval_shadow_spread(*layer, lt);
                int passes = std::max(1, (int)std::ceil(blur / 3.0));
                for (int pass = passes; pass >= 1; --pass) {
                    QColor pass_color = sc;
                    pass_color.setAlphaF(sc.alphaF() / passes);
                    double radius = blur * pass / passes;
                    QRectF shadow_box = box.adjusted(-spread - radius, -spread - radius,
                                                     spread + radius, spread + radius).translated(off);
                    p.setBrush(pass_color);
                    p.setPen(Qt::NoPen);
                    double corner = std::max(0.0, layer->corner_radius + spread + radius);
                    if (corner > 0) p.drawRoundedRect(shadow_box, corner, corner);
                    else p.drawRect(shadow_box);
                }
            }
            if (layer->corner_radius > 0) {
                p.setBrush(fc);
                p.setPen(Qt::NoPen);
                p.drawRoundedRect(box, layer->corner_radius, layer->corner_radius);
            } else {
                p.fillRect(box, fc);
            }
        }

        if (layer->type == LayerType::Image) {
            QImage image(QString::fromStdString(layer->image_path));
            if (!image.isNull()) {
                p.drawImage(box, image);
            } else {
                p.setBrush(QColor(0x33, 0x33, 0x33));
                p.setPen(QPen(QColor(0xff, 0x55, 0x55), 2));
                p.drawRect(box);
                p.drawText(box, Qt::AlignCenter, "Missing Image");
            }
        }

        if (layer->type == LayerType::Text) {
            QColor tc = color_from_argb(eval_text_color(*layer, lt));
            QFont f(QString::fromStdString(layer->font_family));
            f.setPixelSize(layer->font_size);
            f.setBold(layer->font_bold);
            f.setItalic(layer->font_italic);
            p.setFont(f);
            if (eval_shadow_enabled(*layer, lt)) {
                QColor sc = color_from_argb(eval_shadow_color(*layer, lt));
                sc.setAlphaF(std::clamp((double)sc.alphaF() * eval_shadow_opacity(*layer, lt), 0.0, 1.0));
                Qt::AlignmentFlag sha = Qt::AlignHCenter;
                if (layer->align_h == 0) sha = Qt::AlignLeft;
                if (layer->align_h == 2) sha = Qt::AlignRight;
                Qt::AlignmentFlag sva = Qt::AlignVCenter;
                if (layer->align_v == 0) sva = Qt::AlignTop;
                if (layer->align_v == 2) sva = Qt::AlignBottom;
                QPointF off = shadow_offset(*layer, lt);
                double blur = eval_shadow_blur(*layer, lt);
                double spread = eval_shadow_spread(*layer, lt);
                int passes = std::max(1, (int)std::ceil(blur / 3.0));
                p.setPen(sc);
                for (int pass = passes; pass >= 1; --pass) {
                    QColor pass_color = sc;
                    pass_color.setAlphaF(sc.alphaF() / passes);
                    p.setPen(pass_color);
                    double radius = blur * pass / passes;
                    for (double dx : {-spread - radius, 0.0, spread + radius})
                        for (double dy : {-spread - radius, 0.0, spread + radius})
                            p.drawText(box.translated(off + QPointF(dx, dy)), sha | sva, QString::fromStdString(layer->text_content));
                }
            }
            p.setPen(tc);
            Qt::AlignmentFlag ha = Qt::AlignHCenter;
            if (layer->align_h == 0) ha = Qt::AlignLeft;
            if (layer->align_h == 2) ha = Qt::AlignRight;
            Qt::AlignmentFlag va = Qt::AlignVCenter;
            if (layer->align_v == 0) va = Qt::AlignTop;
            if (layer->align_v == 2) va = Qt::AlignBottom;
            p.drawText(box, ha | va, QString::fromStdString(layer->text_content));
        }

        p.restore();
    }

    frame_pixmap_ = QPixmap::fromImage(img);
    dirty_ = false;
}

void CanvasPreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x11, 0x11, 0x11));

    if (!title_) return;

    if (dirty_) render_to_pixmap();
    if (frame_pixmap_.isNull()) return;

    double scale = view_scale();
    QPointF origin = view_origin();
    int dw = (int)(title_->width * scale);
    int dh = (int)(title_->height * scale);
    int ox = (int)origin.x();
    int oy = (int)origin.y();

    p.setBrush(QBrush(QColor(0x44, 0x44, 0x44)));
    p.setPen(Qt::NoPen);
    for (int cy = oy; cy < oy + dh; cy += 12)
        for (int cx = ox; cx < ox + dw; cx += 12)
            if ((((cx - ox) / 12) + ((cy - oy) / 12)) % 2 == 0)
                p.drawRect(cx, cy, 12, 12);

    p.drawPixmap(ox, oy, dw, dh, frame_pixmap_);

    if (safe_guides_visible_) {
        auto draw_guide = [&](double inset, const QColor &color) {
            QRectF r(ox + dw * inset, oy + dh * inset, dw * (1.0 - 2.0 * inset), dh * (1.0 - 2.0 * inset));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color, 1.0, Qt::DashLine));
            p.drawRect(r);
        };
        draw_guide(0.05, QColor(0, 200, 255, 190));
        draw_guide(0.10, QColor(255, 220, 0, 190));
    }

    auto layer = selected_layer();
    if (!layer) return;

    double lt = playhead_ - layer->in_time;
    QRectF box = layer_local_rect(*layer);
    double handle = 8.0 / std::max(0.1, scale);

    p.save();
    QPointF layer_origin = canvas_to_view(QPointF(layer->pos_x.evaluate(lt),
                                                  layer->pos_y.evaluate(lt)));
    p.translate(layer_origin);
    p.rotate(layer->rotation.evaluate(lt));
    p.scale(scale * layer->scale_x.evaluate(lt),
            scale * layer->scale_y.evaluate(lt));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 120, 255, 230), 1.5 / scale, Qt::DashLine));
    p.drawRect(box);

    p.setPen(QPen(QColor(0, 120, 255, 255), 1.0 / scale));
    p.setBrush(QColor(255, 255, 255));
    const QPointF handle_points[] = {
        box.topLeft(), QPointF(box.center().x(), box.top()), box.topRight(),
        QPointF(box.right(), box.center().y()), box.bottomRight(),
        QPointF(box.center().x(), box.bottom()), box.bottomLeft(),
        QPointF(box.left(), box.center().y())
    };
    for (const QPointF &pt : handle_points)
        p.drawRect(QRectF(pt.x() - handle / 2.0, pt.y() - handle / 2.0, handle, handle));

    p.setPen(QPen(QColor(255, 160, 0), 1.5 / scale));
    p.setBrush(QColor(255, 220, 80));
    p.drawEllipse(QPointF(0, 0), handle * 0.45, handle * 0.45);
    p.drawLine(QPointF(-handle, 0), QPointF(handle, 0));
    p.drawLine(QPointF(0, -handle), QPointF(0, handle));
    p.restore();
}

void CanvasPreview::mousePressEvent(QMouseEvent *ev)
{
    if (!title_ || ev->button() != Qt::LeftButton) return;

    drag_mode_ = hit_test_selected(ev->pos());
    if (drag_mode_ == DragMode::None) {
        QPointF canvas = view_to_canvas(ev->pos());
        for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it) {
            auto &l = *it;
            if (!l->visible || l->locked) continue;
            QPointF local = canvas_to_layer(*l, canvas);
            if (layer_local_rect(*l).contains(local)) {
                emit layer_clicked(l->id);
                sel_layer_id_ = l->id;
                drag_mode_ = DragMode::Move;
                break;
            }
        }
    }

    auto layer = selected_layer();
    if (!layer || drag_mode_ == DragMode::None) return;

    drag_changed_ = false;
    drag_start_canvas_ = view_to_canvas(ev->pos());
    double lt = playhead_ - layer->in_time;
    drag_start_x_ = layer->pos_x.evaluate(lt);
    drag_start_y_ = layer->pos_y.evaluate(lt);
    drag_start_w_ = std::max(1.0f, layer->rect_width);
    drag_start_h_ = std::max(1.0f, layer->rect_height);
    drag_start_origin_x_ = layer->origin_x;
    drag_start_origin_y_ = layer->origin_y;
    auto cursor_for_mode = [](DragMode mode) {
        if (mode == DragMode::Move) return Qt::ClosedHandCursor;
        if (mode == DragMode::Origin) return Qt::CrossCursor;
        if (mode == DragMode::ResizeN || mode == DragMode::ResizeS) return Qt::SizeVerCursor;
        if (mode == DragMode::ResizeE || mode == DragMode::ResizeW) return Qt::SizeHorCursor;
        if (mode == DragMode::ResizeNE || mode == DragMode::ResizeSW) return Qt::SizeBDiagCursor;
        return Qt::SizeFDiagCursor;
    };
    setCursor(cursor_for_mode(drag_mode_));
    ev->accept();
}

void CanvasPreview::mouseMoveEvent(QMouseEvent *ev)
{
    if (drag_mode_ != DragMode::None && (ev->buttons() & Qt::LeftButton)) {
        apply_drag(ev->pos(), ev->modifiers());
        ev->accept();
        return;
    }

    DragMode mode = hit_test_selected(ev->pos());
    if (mode == DragMode::Move) setCursor(Qt::OpenHandCursor);
    else if (mode == DragMode::Origin) setCursor(Qt::CrossCursor);
    else if (mode == DragMode::ResizeN || mode == DragMode::ResizeS) setCursor(Qt::SizeVerCursor);
    else if (mode == DragMode::ResizeE || mode == DragMode::ResizeW) setCursor(Qt::SizeHorCursor);
    else if (mode == DragMode::ResizeNE || mode == DragMode::ResizeSW) setCursor(Qt::SizeBDiagCursor);
    else if (mode != DragMode::None) setCursor(Qt::SizeFDiagCursor);
    else unsetCursor();
}

void CanvasPreview::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton && drag_mode_ != DragMode::None) {
        bool changed = drag_changed_;
        drag_mode_ = DragMode::None;
        drag_changed_ = false;
        unsetCursor();
        if (changed)
            emit layer_geometry_changed();
        ev->accept();
    }
}

void CanvasPreview::wheelEvent(QWheelEvent *ev)
{
    if (ev->angleDelta().y() > 0) zoom_ = std::min(zoom_ * 1.1f, 4.0f);
    else zoom_ = std::max(zoom_ / 1.1f, 0.1f);
    update();
}

void CanvasPreview::resizeEvent(QResizeEvent *) { dirty_ = true; }

/* ══════════════════════════════════════════════════════════════════
 *  LayerStack
 * ══════════════════════════════════════════════════════════════════ */
LayerStack::LayerStack(QWidget *parent) : QWidget(parent)
{
    setStyleSheet("background:#1a1a1a;");
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    /* header buttons */
    auto *hdr = new QHBoxLayout();
    hdr->setContentsMargins(3, 3, 3, 3);
    hdr->setSpacing(2);
    btn_add_text_  = new QPushButton("T+",    this);
    btn_add_text_->setIcon(obs_icon(this, {"insert-text", "format-text-bold"}, QStyle::SP_FileIcon));
    btn_add_rect_  = new QPushButton("▭+",    this);
    btn_add_rect_->setIcon(obs_icon(this, {"draw-rectangle", "insert-shape"}, QStyle::SP_FileDialogNewFolder));
    btn_add_image_ = new QPushButton("Img+",  this);
    btn_add_image_->setIcon(obs_icon(this, {"insert-image", "image-x-generic"}, QStyle::SP_FileIcon));
    btn_del_       = new QPushButton("✕",     this);
    btn_del_->setIcon(obs_icon(this, {"edit-delete", "user-trash"}, QStyle::SP_TrashIcon));
    for (auto *b : {btn_add_text_, btn_add_rect_, btn_add_image_, btn_del_}) {
        b->setFixedWidth(34);
        b->setStyleSheet("QPushButton{color:#ccc;background:#2a2a2a;border:none;"
                         "border-radius:2px;} QPushButton:hover{background:#3a3a3a;}");
        hdr->addWidget(b);
    }
    hdr->addStretch();
    vl->addLayout(hdr);

    QWidget *columns = new QWidget(this);
    columns->setStyleSheet("background:#141414;border-top:1px solid #292929;border-bottom:1px solid #292929;");
    auto *ch = new QHBoxLayout(columns);
    ch->setContentsMargins(4, 0, 4, 0);
    ch->setSpacing(4);
    auto add_header = [&](const QString &txt, int w, Qt::Alignment align = Qt::AlignCenter) {
        QLabel *label = new QLabel(txt, columns);
        label->setFixedWidth(w);
        label->setAlignment(align);
        label->setStyleSheet("color:#8f8f8f;font-size:10px;font-weight:bold;");
        ch->addWidget(label);
    };
    add_header("◉", 20);
    add_header("🔒", 20);
    add_header("", 12);
    add_header("#", 24);
    QLabel *name = new QLabel("Layer Name", columns);
    name->setStyleSheet("color:#9a9a9a;font-size:10px;font-weight:bold;");
    ch->addWidget(name, 1);
    add_header("Mode", 46, Qt::AlignLeft | Qt::AlignVCenter);
    add_header("Parent", 58, Qt::AlignLeft | Qt::AlignVCenter);
    vl->addWidget(columns);

    list_ = new QListWidget(this);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setAlternatingRowColors(false);
    list_->setUniformItemSizes(false);
    list_->setStyleSheet(
        "QListWidget{background:#1a1a1a;border:none;color:#ccc;}"
        "QListWidget::item{border-bottom:1px solid #2a2a2a;}"
        "QListWidget::item:selected{background:#3b4f64;}"
        "QListWidget::item:hover{background:#252525;}");
    vl->addWidget(list_, 1);

    connect(btn_add_text_, &QPushButton::clicked, this, &LayerStack::on_add_text);
    connect(btn_add_rect_,  &QPushButton::clicked, this, &LayerStack::on_add_rect);
    connect(btn_add_image_, &QPushButton::clicked, this, &LayerStack::on_add_image);
    connect(btn_del_,       &QPushButton::clicked, this, &LayerStack::on_delete);
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &LayerStack::on_selection_changed);
    connect(list_->model(), &QAbstractItemModel::rowsMoved,
            this, [this]() { sync_order_from_list(); });
}

void LayerStack::set_title(std::shared_ptr<Title> t)
{
    title_ = t; populate();
}

void LayerStack::refresh() { populate(); }

void LayerStack::sync_order_from_list()
{
    if (!title_) return;

    std::vector<std::shared_ptr<Layer>> reordered;
    reordered.reserve(title_->layers.size());
    for (int i = list_->count() - 1; i >= 0; --i) {
        auto *item = list_->item(i);
        if (item->data(Qt::UserRole + 1).toString() == "property")
            continue;
        std::string id = item->data(Qt::UserRole).toString().toStdString();
        if (auto layer = title_->find_layer(id))
            reordered.push_back(layer);
    }
    if (reordered.size() == title_->layers.size()) {
        title_->layers = std::move(reordered);
        emit layer_order_changed();
    }
}

void LayerStack::populate()
{
    QString prev_id = list_->currentItem()
        ? list_->currentItem()->data(Qt::UserRole).toString()
        : QString();

    list_->blockSignals(true);
    list_->clear();
    if (!title_) { list_->blockSignals(false); return; }

    int row = 0;
    for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it, ++row) {
        auto &l = *it;
        auto *item = new QListWidgetItem();
        item->setData(Qt::UserRole, QString::fromStdString(l->id));
        item->setData(Qt::UserRole + 1, "layer");
        item->setFlags((item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                        Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled) & ~Qt::ItemIsUserCheckable);
        item->setSizeHint(QSize(0, 24));
        list_->addItem(item);

        QWidget *row_widget = new QWidget(list_);
        row_widget->setStyleSheet("background:transparent;color:#d0d0d0;");
        auto *hl = new QHBoxLayout(row_widget);
        hl->setContentsMargins(4, 0, 4, 0);
        hl->setSpacing(4);

        auto make_toggle = [&](const QString &on, const QString &off, bool checked,
                               const QString &tip) {
            auto *btn = new QToolButton(row_widget);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setText(checked ? on : off);
            btn->setToolTip(tip);
            btn->setFixedSize(20, 20);
            btn->setAutoRaise(true);
            btn->setStyleSheet("QToolButton{color:#bcbcbc;background:transparent;border:none;}"
                               "QToolButton:hover{background:#353535;border-radius:2px;}"
                               "QToolButton:checked{color:#eeeeee;}");
            connect(btn, &QToolButton::toggled, btn, [btn, on, off](bool state) {
                btn->setText(state ? on : off);
            });
            hl->addWidget(btn);
            return btn;
        };

        QToolButton *vis = make_toggle("●", "○", l->visible, "Layer visibility");
        QToolButton *lock = make_toggle("🔒", "", l->locked, "Lock layer editing");
        connect(vis, &QToolButton::toggled, this, [this, id = l->id, item](bool checked) {
            list_->setCurrentItem(item);
            emit layer_visibility_changed(id, checked);
        });
        connect(lock, &QToolButton::toggled, this, [this, id = l->id, item](bool checked) {
            list_->setCurrentItem(item);
            emit layer_lock_changed(id, checked);
        });

        QToolButton *expand = new QToolButton(row_widget);
        expand->setCheckable(true);
        expand->setChecked(l->properties_expanded);
        expand->setText(l->properties_expanded ? "▾" : "▸");
        expand->setToolTip("Show keyframed properties");
        expand->setFixedSize(16, 20);
        expand->setAutoRaise(true);
        expand->setStyleSheet("QToolButton{color:#aaa;background:transparent;border:none;}"
                              "QToolButton:hover{background:#353535;border-radius:2px;}");
        connect(expand, &QToolButton::toggled, this, [this, id = l->id](bool checked) {
            emit layer_expand_changed(id, checked);
        });
        hl->addWidget(expand);

        QLabel *idx = new QLabel(QString::number(row + 1), row_widget);
        idx->setFixedWidth(24);
        idx->setAlignment(Qt::AlignCenter);
        idx->setStyleSheet("color:#b5b5b5;font-weight:bold;");
        hl->addWidget(idx);

        QLabel *type = new QLabel(layer_type_short(l->type), row_widget);
        type->setFixedWidth(18);
        type->setAlignment(Qt::AlignCenter);
        type->setStyleSheet(QString("background:%1;border:1px solid #111;color:#fff;font-weight:bold;")
                                .arg(layer_color(*l, row).name()));
        hl->addWidget(type);

        QLineEdit *name = new QLineEdit(QString::fromStdString(l->name), row_widget);
        name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        name->setFrame(false);
        name->setReadOnly(l->locked);
        name->setToolTip("Rename layer");
        name->setStyleSheet(l->locked
            ? "QLineEdit{color:#8f8f8f;background:transparent;border:none;}"
            : "QLineEdit{color:#d0d0d0;background:transparent;border:none;padding:1px;} QLineEdit:focus{background:#101010;border:1px solid #0078d4;border-radius:2px;}");
        connect(name, &QLineEdit::editingFinished, this, [this, id = l->id, name]() {
            emit layer_name_changed(id, name->text().trimmed().toStdString());
        });
        hl->addWidget(name, 1);

        QLabel *mode = new QLabel("Normal", row_widget);
        mode->setFixedWidth(54);
        mode->setStyleSheet("color:#b0b0b0;background:#101010;border-radius:3px;padding-left:4px;");
        hl->addWidget(mode);

        QComboBox *parent = new QComboBox(row_widget);
        parent->setFixedWidth(86);
        parent->setStyleSheet("QComboBox{color:#b0b0b0;background:#101010;border:none;border-radius:3px;padding-left:4px;}"
                              "QComboBox::drop-down{border:none;}");
        parent->addItem("None", "");
        for (const auto &candidate : title_->layers) {
            if (candidate->id == l->id) continue;
            parent->addItem(QString::fromStdString(candidate->name), QString::fromStdString(candidate->id));
        }
        int parent_idx = parent->findData(QString::fromStdString(l->parent_id));
        parent->setCurrentIndex(parent_idx >= 0 ? parent_idx : 0);
        connect(parent, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, parent](int index) {
                    emit layer_parent_changed(id, parent->itemData(index).toString().toStdString());
                });
        hl->addWidget(parent);

        list_->setItemWidget(item, row_widget);
        if ((prev_id.isEmpty() && list_->currentItem() == nullptr) ||
            prev_id == item->data(Qt::UserRole).toString())
            list_->setCurrentItem(item);

        if (!l->properties_expanded) continue;

        std::set<std::string> seen;
        for (auto *prop : timeline_properties(*l)) {
            if (!prop->is_animated()) continue;
            QString label = property_label(prop->name);
            std::string key = label.toStdString();
            if (!seen.insert(key).second) continue;

            auto *prop_item = new QListWidgetItem();
            prop_item->setData(Qt::UserRole, QString::fromStdString(l->id));
            prop_item->setData(Qt::UserRole + 1, "property");
            prop_item->setData(Qt::UserRole + 2, label);
            prop_item->setFlags((prop_item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled) &
                                ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsUserCheckable));
            prop_item->setSizeHint(QSize(0, 24));
            list_->addItem(prop_item);

            QWidget *prop_widget = new QWidget(list_);
            auto *ph = new QHBoxLayout(prop_widget);
            ph->setContentsMargins(64, 0, 4, 0);
            ph->setSpacing(4);
            QLabel *stopwatch = new QLabel("◇", prop_widget);
            stopwatch->setFixedWidth(18);
            stopwatch->setAlignment(Qt::AlignCenter);
            stopwatch->setStyleSheet("color:#9aa5b1;");
            ph->addWidget(stopwatch);
            QLabel *prop_name = new QLabel(label, prop_widget);
            prop_name->setStyleSheet("color:#b8b8b8;");
            ph->addWidget(prop_name, 1);
            QLabel *value = new QLabel(property_value_text(*prop, *l), prop_widget);
            value->setFixedWidth(95);
            value->setStyleSheet("color:#4ab0ff;font-family:monospace;");
            ph->addWidget(value);
            list_->setItemWidget(prop_item, prop_widget);
        }
    }
    list_->blockSignals(false);
    on_selection_changed();
}

void LayerStack::set_selected_layer(const std::string &layer_id)
{
    QString qid = QString::fromStdString(layer_id);
    if (list_->currentItem() &&
        list_->currentItem()->data(Qt::UserRole).toString() == qid)
        return;

    QSignalBlocker blocker(list_);
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (item->data(Qt::UserRole).toString() == qid) {
            list_->setCurrentItem(item);
            return;
        }
    }
}

std::string LayerStack::selected_id() const
{
    auto *item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString().toStdString() : "";
}

std::vector<std::string> LayerStack::selected_ids() const
{
    std::vector<std::string> ids;
    for (auto *item : list_->selectedItems())
        ids.push_back(item->data(Qt::UserRole).toString().toStdString());
    if (ids.empty() && list_->currentItem())
        ids.push_back(list_->currentItem()->data(Qt::UserRole).toString().toStdString());
    return ids;
}

void LayerStack::on_selection_changed()
{
    std::string id = selected_id();
    if (!id.empty()) emit layer_selected(id);
}

void LayerStack::on_add_text() { emit add_layer_requested(LayerType::Text); }
void LayerStack::on_add_rect() { emit add_layer_requested(LayerType::SolidRect); }
void LayerStack::on_add_image() { emit add_layer_requested(LayerType::Image); }

void LayerStack::on_delete()
{
    std::string id = selected_id();
    if (!id.empty()) emit delete_layer_requested(id);
}

void LayerStack::on_item_changed(QListWidgetItem *item)
{
    std::string id = item->data(Qt::UserRole).toString().toStdString();
    bool v = (item->checkState() == Qt::Checked);
    emit layer_visibility_changed(id, v);
}

/* ══════════════════════════════════════════════════════════════════
 *  TimelineWidget
 * ══════════════════════════════════════════════════════════════════ */
TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setStyleSheet("background:#1e1e1e;");
    setMouseTracking(true);
}

void TimelineWidget::set_title(std::shared_ptr<Title> t)
{
    title_ = t;
    clamp_scroll();
    update();
}

void TimelineWidget::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid; update();
}

void TimelineWidget::set_playhead(double t)
{
    playhead_ = snap_time(t);
    if (title_) {
        int phx = time_to_x(playhead_);
        if (phx < 24) scroll_x_ = std::max(0, (int)(playhead_ * pixels_per_sec_) - 24);
        if (phx > width() - 24) scroll_x_ = std::max(0, (int)(playhead_ * pixels_per_sec_) - width() + 24);
        clamp_scroll();
    }
    update();
}

double TimelineWidget::x_to_time(int x) const
{
    return snap_time((x + scroll_x_) / pixels_per_sec_);
}

int TimelineWidget::time_to_x(double t) const
{
    return (int)std::round(t * pixels_per_sec_) - scroll_x_;
}

double TimelineWidget::snap_time(double t) const
{
    return snap_to_obs_frame(t);
}

void TimelineWidget::clamp_scroll()
{
    double dur = title_ ? title_->duration : 10.0;
    int max_scroll = std::max(0, (int)std::ceil(dur * pixels_per_sec_) - width() + 40);
    scroll_x_ = std::clamp(scroll_x_, 0, max_scroll);
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    int W = width(), H = height();
    int rh = ruler_height(), rowh = row_height();

    /* Background */
    p.fillRect(0, 0, W, H, QColor(0x1e, 0x1e, 0x1e));

    /* Ruler */
    p.fillRect(0, 0, W, rh, QColor(0x14, 0x14, 0x14));
    p.setPen(QColor(0x55, 0x55, 0x55));

    double dur = title_ ? title_->duration : 10.0;
    double fps = obs_frame_rate();
    double frame_step = obs_frame_duration();
    int first_frame = std::max(0, (int)std::floor(scroll_x_ / pixels_per_sec_ / frame_step) - 1);
    int last_frame = (int)std::ceil((scroll_x_ + W) / pixels_per_sec_ / frame_step) + 1;
    int label_every = std::max(1, (int)std::ceil(55.0 / (pixels_per_sec_ * frame_step)));

    for (int frame = first_frame; frame <= last_frame; ++frame) {
        double t = frame * frame_step;
        if (t > dur + frame_step) break;
        int x = time_to_x(t);
        if (x < 0 || x > W) continue;
        bool is_second = (frame % std::max(1, (int)std::round(fps)) == 0);
        bool label = (frame % label_every == 0) || is_second;
        p.setPen(is_second ? QColor(0x88,0x88,0x88) : QColor(0x4a,0x4a,0x4a));
        p.drawLine(x, rh - (is_second ? 9 : label ? 6 : 3), x, rh);
        if (label) {
            p.setPen(QColor(0x8c,0x8c,0x8c));
            int seconds = frame / std::max(1, (int)std::round(fps));
            int frame_in_second = frame % std::max(1, (int)std::round(fps));
            QString text = is_second
                ? QString("%1s").arg(seconds)
                : QString("+%1f").arg(frame_in_second, 2, 10, QChar('0'));
            p.drawText(x + 2, rh - 2, text);
        }
    }

    if (title_) {
        if (title_->playback_mode == 1) {
            int loop_x0 = time_to_x(std::clamp(title_->loop_start, 0.0, dur));
            int loop_x1 = time_to_x(std::clamp(title_->loop_end, title_->loop_start, dur));
            if (loop_x1 > loop_x0) {
                p.fillRect(loop_x0, 18, loop_x1 - loop_x0, rh - 18, QColor(0x00, 0x78, 0xd4, 45));
                p.setPen(QPen(QColor(0x20, 0xa0, 0xff), 2));
                p.drawLine(loop_x0, 18, loop_x0, H);
                p.drawLine(loop_x1, 18, loop_x1, H);
                p.setPen(QColor(0xa8, 0xd8, 0xff));
                p.drawText(loop_x0 + 4, 20, 80, 16, Qt::AlignVCenter, "Loop in");
                p.drawText(loop_x1 + 4, 20, 80, 16, Qt::AlignVCenter, "Loop out");
            }
        }
        if (title_->playback_mode == 2) {
            int pause_x = time_to_x(std::clamp(title_->pause_time, 0.0, dur));
            p.setPen(QPen(QColor(0xff, 0xc8, 0x32), 2));
            p.drawLine(pause_x, 12, pause_x, H);
            p.setBrush(QColor(0xff, 0xc8, 0x32));
            p.setPen(Qt::NoPen);
            QPolygon marker;
            marker << QPoint(pause_x - 6, 12) << QPoint(pause_x + 6, 12) << QPoint(pause_x, 22);
            p.drawPolygon(marker);
            p.setPen(QColor(0xff, 0xe0, 0x85));
            p.drawText(pause_x + 4, 22, 100, 16, Qt::AlignVCenter, "Pause");
        }
    }

    /* Layer/property rows.  This uses the same row model as LayerStack so
     * keyframed property rows stay vertically aligned with the layer list.
     */
    auto rows = timeline_rows(title_);
    for (int row = 0; row < (int)rows.size(); ++row) {
        auto &entry = rows[row];
        auto &layer = entry.layer;
        int y = rh + row * rowh;
        if (y > H) break;
        bool sel = (layer->id == sel_layer_id_);

        p.fillRect(0, y, W, rowh,
                   entry.is_property ? QColor(0x19,0x19,0x19) :
                   sel ? QColor(0x1e,0x3a,0x5a) : QColor(0x1e,0x1e,0x1e));
        p.setPen(QColor(0x2a,0x2a,0x2a));
        p.drawLine(0, y + rowh - 1, W, y + rowh - 1);

        int x0 = time_to_x(layer->in_time);
        int x1 = time_to_x(layer->out_time);
        if (!entry.is_property) {
            QColor bar_col = layer_color(*layer, row);
            if (sel) bar_col = bar_col.lighter(125);
            p.fillRect(x0, y + 3, x1 - x0, rowh - 6, bar_col);
            p.setPen(QColor(0x0d,0x0d,0x0d));
            p.drawRect(x0, y + 3, x1 - x0, rowh - 6);

            /* Trim handles for mouse resizing of layer in/out. */
            p.fillRect(x0, y + 3, 4, rowh - 6, QColor(0xdc,0xdc,0xdc,150));
            p.fillRect(x1 - 4, y + 3, 4, rowh - 6, QColor(0xdc,0xdc,0xdc,150));

            p.setPen(QColor(0xcc,0xcc,0xcc));
            p.drawText(std::max(x0, 0) + 6, y, std::max(1, x1 - x0 - 12), rowh,
                       Qt::AlignVCenter, QString::fromStdString(layer->name));
        } else {
            p.fillRect(x0, y + rowh / 2 - 1, x1 - x0, 2, QColor(0x36,0x36,0x36));
            p.setPen(QColor(0x77,0x77,0x77));
            p.drawText(6, y, 150, rowh, Qt::AlignVCenter, property_label(entry.prop->name));
        }

        auto draw_kf = [&](const AnimatedProperty &prop) {
            for (const auto &kf : prop.keyframes) {
                int kx = time_to_x(layer->in_time + kf.time);
                if (kx < 0 || kx > W) continue;
                int ky = y + rowh / 2;
                QPolygon diamond;
                diamond << QPoint(kx,     ky - 5)
                        << QPoint(kx + 5, ky)
                        << QPoint(kx,     ky + 5)
                        << QPoint(kx - 5, ky);
                p.setBrush(keyframe_color(kf.easing));
                p.setPen(QPen(QColor(0x10, 0x10, 0x10), 1));
                p.drawPolygon(diamond);
            }
        };

        if (entry.is_property)
            draw_kf(*entry.prop);
        else if (!layer->properties_expanded)
            for (auto *prop : timeline_properties(*layer)) draw_kf(*prop);
    }

    /* Playhead */
    int phx = time_to_x(playhead_);
    p.setPen(QPen(C_PLAYHEAD, 1.5));
    p.drawLine(phx, 0, phx, H);
    /* Playhead head triangle */
    p.setBrush(C_PLAYHEAD);
    p.setPen(Qt::NoPen);
    QPolygon tri;
    tri << QPoint(phx - 6, 0)
        << QPoint(phx + 6, 0)
        << QPoint(phx,     10);
    p.drawPolygon(tri);

    QString tc = format_timecode(playhead_);
    QRect tc_rect(phx + 8, 2, 96, 18);
    if (tc_rect.right() > W) tc_rect.moveRight(phx - 8);
    p.fillRect(tc_rect, QColor(0x00,0x78,0xd4));
    p.setPen(Qt::white);
    p.drawText(tc_rect.adjusted(4, 0, -4, 0), Qt::AlignVCenter, tc);
}

bool TimelineWidget::hit_keyframe(const QPoint &pos, std::shared_ptr<Layer> *hit_layer,
                                  AnimatedProperty **hit_prop, int *hit_kf_idx,
                                  int *hit_row_idx) const
{
    if (!title_ || pos.y() < ruler_height()) return false;
    auto rows = timeline_rows(title_);
    int row = (pos.y() - ruler_height()) / row_height();
    if (row < 0 || row >= (int)rows.size()) return false;

    auto &entry = rows[row];
    constexpr int kHitRadius = 7;
    auto test_prop = [&](AnimatedProperty *prop) -> bool {
        for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
            const auto &kf = prop->keyframes[i];
            int kx = time_to_x(entry.layer->in_time + kf.time);
            int ky = ruler_height() + row * row_height() + row_height() / 2;
            if (std::abs(pos.x() - kx) <= kHitRadius &&
                std::abs(pos.y() - ky) <= kHitRadius) {
                if (hit_layer) *hit_layer = entry.layer;
                if (hit_prop) *hit_prop = prop;
                if (hit_kf_idx) *hit_kf_idx = i;
                if (hit_row_idx) *hit_row_idx = row;
                return true;
            }
        }
        return false;
    };

    if (entry.is_property)
        return entry.prop && test_prop(entry.prop);
    if (!entry.layer->properties_expanded) {
        for (auto *prop : timeline_properties(*entry.layer)) {
            if (test_prop(prop)) return true;
        }
    }
    return false;
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent *ev)
{
    if (!title_) return;

    std::shared_ptr<Layer> layer;
    AnimatedProperty *hit_prop = nullptr;
    int hit_idx = -1;
    if (!hit_keyframe(ev->pos(), &layer, &hit_prop, &hit_idx, nullptr)) return;
    Keyframe *hit_keyframe = &hit_prop->keyframes[hit_idx];

    QMenu menu(this);
    menu.setTitle(QString("%1 easing").arg(QString::fromStdString(hit_prop->name)));

    auto add_easing = [&](const QString &label, EasingType easing) {
        QAction *action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(hit_keyframe->easing == easing);
        action->setData((int)easing);
        QPixmap swatch(12, 12);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(keyframe_color(easing));
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(1, 1, 10, 10);
        action->setIcon(QIcon(swatch));
        return action;
    };

    add_easing("Linear", EasingType::Linear);
    add_easing("Ease In", EasingType::EaseIn);
    add_easing("Ease Out", EasingType::EaseOut);
    add_easing("Ease In/Out", EasingType::EaseInOut);
    add_easing("Bezier", EasingType::Bezier);
    add_easing("Step / Hold", EasingType::Hold);

    QAction *chosen = menu.exec(ev->globalPos());
    if (!chosen) return;

    hit_keyframe->easing = (EasingType)chosen->data().toInt();
    update();
    emit keyframe_easing_changed();
}

void TimelineWidget::wheelEvent(QWheelEvent *ev)
{
    if (!title_) return;

    const QPoint angle = ev->angleDelta();
    if (ev->modifiers() & Qt::ShiftModifier) {
        int delta = angle.x() != 0 ? angle.x() : angle.y();
        scroll_x_ -= delta;
        clamp_scroll();
        update();
        ev->accept();
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    int cursor_x = (int)std::round(ev->position().x());
#else
    int cursor_x = ev->pos().x();
#endif
    double anchor_time = (cursor_x + scroll_x_) / pixels_per_sec_;
    int delta = angle.y() != 0 ? angle.y() : angle.x();
    if (delta == 0) return;

    double factor = std::pow(1.0015, delta);
    pixels_per_sec_ = std::clamp(pixels_per_sec_ * factor, 25.0, 1200.0);
    scroll_x_ = (int)std::round(anchor_time * pixels_per_sec_) - cursor_x;
    clamp_scroll();
    update();
    ev->accept();
}

void TimelineWidget::mousePressEvent(QMouseEvent *ev)
{
    if (!title_) return;
    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;

    if (ev->pos().y() < ruler_height()) {
        if (title_->playback_mode == 2) {
            int pause_x = time_to_x(std::clamp(title_->pause_time, 0.0, title_->duration));
            if (std::abs(ev->pos().x() - pause_x) <= 8) {
                drag_mode_ = DragMode::PauseMarker;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
        }
        if (title_->playback_mode == 1) {
            int loop_x0 = time_to_x(std::clamp(title_->loop_start, 0.0, title_->duration));
            int loop_x1 = time_to_x(std::clamp(title_->loop_end, title_->loop_start, title_->duration));
            if (std::abs(ev->pos().x() - loop_x0) <= 8) {
                drag_mode_ = DragMode::LoopStart;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
            if (std::abs(ev->pos().x() - loop_x1) <= 8) {
                drag_mode_ = DragMode::LoopEnd;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
        }
        drag_mode_ = DragMode::Playhead;
        double t = std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration);
        emit playhead_changed(t);
        ev->accept();
        return;
    }

    std::shared_ptr<Layer> hit_layer;
    AnimatedProperty *hit_prop = nullptr;
    int hit_idx = -1;
    if (hit_keyframe(ev->pos(), &hit_layer, &hit_prop, &hit_idx, nullptr)) {
        drag_mode_ = DragMode::Keyframe;
        drag_layer_id_ = hit_layer->id;
        drag_prop_name_ = hit_prop->name;
        drag_keyframe_index_ = hit_idx;
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height()) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        auto layer = rows[row].layer;
        int x0 = time_to_x(layer->in_time);
        int x1 = time_to_x(layer->out_time);
        constexpr int kTrimHit = 7;
        if (std::abs(ev->pos().x() - x0) <= kTrimHit) {
            drag_mode_ = DragMode::TrimIn;
            drag_layer_id_ = layer->id;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        if (std::abs(ev->pos().x() - x1) <= kTrimHit) {
            drag_mode_ = DragMode::TrimOut;
            drag_layer_id_ = layer->id;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        if (ev->pos().x() >= std::min(x0, x1) && ev->pos().x() <= std::max(x0, x1)) {
            drag_mode_ = DragMode::Layer;
            drag_layer_id_ = layer->id;
            drag_start_time_ = x_to_time(ev->pos().x());
            drag_start_in_ = layer->in_time;
            drag_start_out_ = layer->out_time;
            setCursor(Qt::ClosedHandCursor);
            ev->accept();
            return;
        }
    }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *ev)
{
    if (!title_) return;
    double t = std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration);

    if (drag_mode_ == DragMode::Playhead) {
        emit playhead_changed(t);
        return;
    }

    if (drag_mode_ == DragMode::PauseMarker) {
        title_->pause_time = t;
        update();
        return;
    }

    if (drag_mode_ == DragMode::LoopStart) {
        title_->loop_start = std::clamp(t, 0.0, title_->loop_end);
        update();
        return;
    }

    if (drag_mode_ == DragMode::LoopEnd) {
        title_->loop_end = std::clamp(t, title_->loop_start, title_->duration);
        update();
        return;
    }

    if (drag_mode_ == DragMode::Keyframe) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer) return;
        for (auto *prop : timeline_properties(*layer)) {
            if (prop->name != drag_prop_name_) continue;
            if (drag_keyframe_index_ < 0 || drag_keyframe_index_ >= (int)prop->keyframes.size()) return;
            prop->keyframes[drag_keyframe_index_].time =
                std::clamp(t - layer->in_time, 0.0, std::max(0.0, layer->out_time - layer->in_time));
            update();
            return;
        }
    }

    if (drag_mode_ == DragMode::TrimIn || drag_mode_ == DragMode::TrimOut) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer) return;
        if (drag_mode_ == DragMode::TrimIn)
            layer->in_time = std::clamp(t, 0.0, std::max(0.0, layer->out_time - obs_frame_duration()));
        else
            layer->out_time = std::clamp(t, layer->in_time + obs_frame_duration(), title_->duration);
        update();
        return;
    }

    if (drag_mode_ == DragMode::Layer) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer) return;
        double duration = std::max(obs_frame_duration(), drag_start_out_ - drag_start_in_);
        double new_in = drag_start_in_ + (t - drag_start_time_);
        new_in = std::clamp(new_in, 0.0, std::max(0.0, title_->duration - duration));
        layer->in_time = new_in;
        layer->out_time = std::min(title_->duration, new_in + duration);
        update();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height()) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        int x0 = time_to_x(rows[row].layer->in_time);
        int x1 = time_to_x(rows[row].layer->out_time);
        if (std::abs(ev->pos().x() - x0) <= 7 || std::abs(ev->pos().x() - x1) <= 7)
            setCursor(Qt::SizeHorCursor);
        else if (ev->pos().x() >= std::min(x0, x1) && ev->pos().x() <= std::max(x0, x1))
            setCursor(Qt::OpenHandCursor);
        else
            unsetCursor();
    } else {
        unsetCursor();
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *)
{
    bool changed = drag_mode_ == DragMode::Keyframe ||
                   drag_mode_ == DragMode::TrimIn ||
                   drag_mode_ == DragMode::TrimOut ||
                   drag_mode_ == DragMode::Layer ||
                   drag_mode_ == DragMode::LoopStart ||
                   drag_mode_ == DragMode::LoopEnd ||
                   drag_mode_ == DragMode::PauseMarker;
    if (drag_mode_ == DragMode::Keyframe && title_) {
        if (auto layer = title_->find_layer(drag_layer_id_)) {
            for (auto *prop : timeline_properties(*layer)) {
                if (prop->name != drag_prop_name_) continue;
                std::sort(prop->keyframes.begin(), prop->keyframes.end(),
                          [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
                break;
            }
        }
    }

    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;
    unsetCursor();
    if (changed) emit keyframe_easing_changed();
}

/* ══════════════════════════════════════════════════════════════════
 *  TitlePropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */
TitlePropertiesPanel::TitlePropertiesPanel(QWidget *parent)
    : QGroupBox("Title", parent)
{
    setStyleSheet(
        "QGroupBox{color:#aaa;background:#1a1a1a;border:1px solid #333;"
        "border-radius:3px;margin-top:6px;font-size:10px;padding-top:4px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;}"
        "QDoubleSpinBox,QSpinBox,QComboBox{color:#ccc;background:#2a2a2a;border:none;"
        "border-radius:2px;padding:2px;}");

    auto *fl = new QFormLayout(this);
    fl->setContentsMargins(8, 10, 8, 6);
    fl->setSpacing(3);

    cmb_playback_mode_ = new QComboBox(this);
    cmb_playback_mode_->addItem("Play Once", 0);
    cmb_playback_mode_->addItem("Loop In/Out", 1);
    cmb_playback_mode_->addItem("Pause at Timeline Position", 2);
    fl->addRow("Playback Mode:", cmb_playback_mode_);

    cmb_loop_type_ = new QComboBox(this);
    cmb_loop_type_->addItem("Restart Loop", 0);
    cmb_loop_type_->addItem("Ping-Pong Loop", 1);
    fl->addRow("Loop Type:", cmb_loop_type_);

    spn_pause_frame_ = new QSpinBox(this);
    spn_pause_frame_->setRange(0, 1000000);
    spn_pause_frame_->setToolTip("Frame where Play Once pauses indefinitely.");
    fl->addRow("Pause Frame:", spn_pause_frame_);

    spn_pause_time_ = new QDoubleSpinBox(this);
    spn_pause_time_->setRange(0.0, 3600.0);
    spn_pause_time_->setSingleStep(obs_frame_duration());
    spn_pause_time_->setDecimals(3);
    spn_pause_time_->setSuffix(" s");
    spn_pause_time_->setToolTip("Timeline timecode where playback pauses. Drag the yellow marker on the timeline to set this visually.");
    fl->addRow("Pause Timecode:", spn_pause_time_);

    spn_duration_ = new QDoubleSpinBox(this);
    spn_duration_->setRange(0.1, 3600.0);
    spn_duration_->setSingleStep(0.5);
    spn_duration_->setDecimals(2);
    spn_duration_->setSuffix(" s");
    fl->addRow("Length:", spn_duration_);

    spn_loop_start_ = new QDoubleSpinBox(this);
    spn_loop_start_->setRange(0.0, 3600.0);
    spn_loop_start_->setSingleStep(0.5);
    spn_loop_start_->setDecimals(2);
    spn_loop_start_->setSuffix(" s");
    spn_loop_start_->setToolTip("Cue playback runs from 0 to this point, then loops from here.");
    fl->addRow("Loop start:", spn_loop_start_);

    spn_loop_end_ = new QDoubleSpinBox(this);
    spn_loop_end_->setRange(0.0, 3600.0);
    spn_loop_end_->setSingleStep(0.5);
    spn_loop_end_->setDecimals(2);
    spn_loop_end_->setSuffix(" s");
    spn_loop_end_->setToolTip("Cue playback loops until this point; the next cue plays from here to the end.");
    fl->addRow("Loop end:", spn_loop_end_);

    connect(cmb_playback_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!title_ || loading_values_) return;
                title_->playback_mode = cmb_playback_mode_->currentData().toInt();
                if (title_->playback_mode == 2 && title_->pause_time <= 0.0)
                    title_->pause_time = title_->duration;
                load_values();
                emit title_changed();
            });

    connect(cmb_loop_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!title_ || loading_values_) return;
                title_->loop_type = cmb_loop_type_->currentData().toInt();
                emit title_changed();
            });

    connect(spn_pause_frame_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int frame) {
                if (!title_ || loading_values_) return;
                title_->pause_time = std::clamp(frame * obs_frame_duration(), 0.0, title_->duration);
                load_values();
                emit title_changed();
            });

    connect(spn_pause_time_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->pause_time = std::clamp(v, 0.0, title_->duration);
                load_values();
                emit title_changed();
            });

    connect(spn_duration_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                double old_duration = title_->duration;
                title_->duration = v;
                for (auto &layer : title_->layers) {
                    if (std::abs(layer->out_time - old_duration) < 0.001 || layer->out_time > v)
                        layer->out_time = v;
                }
                title_->loop_start = std::clamp(title_->loop_start, 0.0, title_->duration);
                title_->loop_end = std::clamp(title_->loop_end, title_->loop_start, title_->duration);
                title_->pause_time = std::clamp(title_->pause_time, 0.0, title_->duration);
                load_values();
                emit title_changed();
            });

    connect(spn_loop_start_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->loop_start = std::clamp(v, 0.0, title_->duration);
                title_->loop_end = std::clamp(title_->loop_end, title_->loop_start, title_->duration);
                load_values();
                emit title_changed();
            });

    connect(spn_loop_end_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->loop_end = std::clamp(v, title_->loop_start, title_->duration);
                load_values();
                emit title_changed();
            });
}

void TitlePropertiesPanel::set_title(std::shared_ptr<Title> t)
{
    title_ = t;
    load_values();
}

void TitlePropertiesPanel::load_values()
{
    loading_values_ = true;
    double duration = title_ ? title_->duration : 5.0;
    double loop_start = title_ ? title_->loop_start : 1.0;
    double loop_end = title_ ? title_->loop_end : 4.0;
    int playback_mode = title_ ? std::clamp(title_->playback_mode, 0, 2) : 0;
    int loop_type = title_ ? std::clamp(title_->loop_type, 0, 1) : 0;
    double pause_time = title_ ? std::clamp(title_->pause_time, 0.0, duration) : 0.0;

    cmb_playback_mode_->setCurrentIndex(std::max(0, cmb_playback_mode_->findData(playback_mode)));
    cmb_loop_type_->setCurrentIndex(std::max(0, cmb_loop_type_->findData(loop_type)));
    spn_duration_->setValue(duration);
    spn_loop_start_->setMaximum(duration);
    spn_loop_end_->setMaximum(duration);
    spn_loop_start_->setValue(std::clamp(loop_start, 0.0, duration));
    spn_loop_end_->setValue(std::clamp(loop_end, std::clamp(loop_start, 0.0, duration), duration));
    spn_pause_time_->setMaximum(duration);
    spn_pause_time_->setSingleStep(obs_frame_duration());
    spn_pause_time_->setValue(pause_time);
    spn_pause_frame_->setMaximum(std::max(0, (int)std::round(duration / obs_frame_duration())));
    spn_pause_frame_->setValue((int)std::round(pause_time / obs_frame_duration()));

    bool show_loop = playback_mode == 1;
    bool show_pause = playback_mode == 2;
    auto *form = qobject_cast<QFormLayout *>(layout());
    cmb_loop_type_->setVisible(show_loop);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(cmb_loop_type_))) label->setVisible(show_loop);
    spn_loop_start_->setVisible(show_loop);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(spn_loop_start_))) label->setVisible(show_loop);
    spn_loop_end_->setVisible(show_loop);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(spn_loop_end_))) label->setVisible(show_loop);
    spn_pause_frame_->setVisible(show_pause);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(spn_pause_frame_))) label->setVisible(show_pause);
    spn_pause_time_->setVisible(show_pause);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(spn_pause_time_))) label->setVisible(show_pause);
    loading_values_ = false;
}

/* ══════════════════════════════════════════════════════════════════
 *  PropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */
PropertiesPanel::PropertiesPanel(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet("QScrollArea{background:#1a1a1a;border:none;}");

    auto *inner = new QWidget(this);
    inner->setStyleSheet("background:#1a1a1a;");
    auto *vl = new QVBoxLayout(inner);
    vl->setContentsMargins(6, 6, 6, 6);
    vl->setSpacing(8);

    /* Header */
    auto *hdr = new QLabel("PROPERTIES", inner);
    hdr->setStyleSheet("color:#666;font-size:9px;font-weight:bold;");
    vl->addWidget(hdr);

    /* ── Transform ── */
    auto *tform_box = new QGroupBox("Transform", inner);
    tform_box->setStyleSheet(
        "QGroupBox{color:#aaa;border:1px solid #333;border-radius:3px;margin-top:6px;"
        "  font-size:10px;padding-top:4px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;}");
    auto *tfl = new QFormLayout(tform_box);
    tfl->setSpacing(3);

    auto mk_dspin = [&](double lo, double hi, double step) {
        auto *s = new QDoubleSpinBox(inner);
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(1);
        s->setStyleSheet("QDoubleSpinBox{color:#ccc;background:#2a2a2a;border:none;"
                         "border-radius:2px;padding:2px;}");
        return s;
    };

    auto mk_kf_button = [&](const QString &tip) {
        auto *b = new QPushButton("◇", inner);
        b->setFixedWidth(24);
        b->setToolTip(tip);
        b->setStyleSheet("QPushButton{color:#f0a020;background:#2a2a2a;border:none;"
                         "border-radius:3px;padding:2px;font-weight:bold;}"
                         "QPushButton:hover{background:#3a3a3a;color:#ffd27a;}");
        return b;
    };

    auto with_kf = [&](QWidget *field, QPushButton *button) {
        auto *row = new QWidget(inner);
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(3);
        hl->addWidget(field, 1);
        hl->addWidget(button);
        return row;
    };

    spn_px_      = mk_dspin(-9999, 9999, 1.0);
    spn_py_      = mk_dspin(-9999, 9999, 1.0);
    spn_rot_     = mk_dspin(-360,  360,  0.5);
    spn_opacity_ = mk_dspin(0.0,   1.0,  0.01);
    spn_origin_x_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_y_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_x_->setDecimals(2);
    spn_origin_y_->setDecimals(2);
    spn_origin_x_->setToolTip("Horizontal origin: 0=left, 0.5=center, 1=right.");
    spn_origin_y_->setToolTip("Vertical origin: 0=top, 0.5=center, 1=bottom.");
    cmb_anchor_ = new QComboBox(inner);
    for (const QString &label : QStringList{"Top Left", "Top Center", "Top Right", "Center Left", "Center", "Center Right", "Bottom Left", "Bottom Center", "Bottom Right"})
        cmb_anchor_->addItem(label);
    cmb_anchor_->setToolTip("Change layer anchor/origin while preserving visual position.");
    cmb_anchor_->setStyleSheet("QComboBox{color:#ccc;background:#2a2a2a;border:none;border-radius:2px;padding:2px;}");

    btn_kf_pos_x_ = mk_kf_button("Toggle X position keyframe");
    btn_kf_pos_y_ = mk_kf_button("Toggle Y position keyframe");
    btn_kf_rotation_ = mk_kf_button("Toggle rotation keyframe");
    btn_kf_opacity_ = mk_kf_button("Toggle opacity keyframe");
    btn_kf_origin_x_ = mk_kf_button("Toggle origin X keyframe");
    btn_kf_origin_y_ = mk_kf_button("Toggle origin Y keyframe");
    tfl->addRow("X:",       with_kf(spn_px_, btn_kf_pos_x_));
    tfl->addRow("Y:",       with_kf(spn_py_, btn_kf_pos_y_));
    tfl->addRow("Rotation:",with_kf(spn_rot_, btn_kf_rotation_));
    tfl->addRow("Opacity:", with_kf(spn_opacity_, btn_kf_opacity_));
    tfl->addRow("Anchor:", cmb_anchor_);
    tfl->addRow("Origin X:", with_kf(spn_origin_x_, btn_kf_origin_x_));
    tfl->addRow("Origin Y:", with_kf(spn_origin_y_, btn_kf_origin_y_));
    vl->addWidget(tform_box);

    /* ── Text ── */
    text_box_ = new QGroupBox("Text", inner);
    text_box_->setStyleSheet(tform_box->styleSheet());
    auto *txfl = new QFormLayout(text_box_);
    txfl->setSpacing(3);

    txt_content_ = new QLineEdit(inner);
    txt_content_->setStyleSheet("QLineEdit{color:#fff;background:#2a2a2a;border:none;"
                                "border-radius:2px;padding:2px;}");

    /* Font family combo populated from system */
    cmb_font_ = new QComboBox(inner);
    cmb_font_->setStyleSheet("QComboBox{color:#ccc;background:#2a2a2a;border:none;"
                             "border-radius:2px;padding:2px;}");
    QFontDatabase fdb;
    for (auto &fam : fdb.families())
        cmb_font_->addItem(fam, fam);

    spn_size_ = new QSpinBox(inner);
    spn_size_->setRange(6, 500);
    spn_size_->setStyleSheet("QSpinBox{color:#ccc;background:#2a2a2a;border:none;"
                             "border-radius:2px;padding:2px;}");

    chk_bold_   = new QCheckBox("Bold",   inner);
    chk_italic_ = new QCheckBox("Italic", inner);
    chk_expose_text_ = new QCheckBox("Expose in dock", inner);
    chk_expose_text_->setToolTip("Show this text layer in the OBS Titler Pro dock for fast live edits.");
    chk_bold_->setStyleSheet("color:#ccc;");
    chk_italic_->setStyleSheet("color:#ccc;");
    chk_expose_text_->setStyleSheet("color:#ccc;");

    txfl->addRow("Text:",   txt_content_);
    txfl->addRow("Font:",   cmb_font_);
    txfl->addRow("Size:",   spn_size_);
    auto *bi_row = new QHBoxLayout();
    bi_row->addWidget(chk_bold_);
    bi_row->addWidget(chk_italic_);
    bi_row->addStretch();
    txfl->addRow("Style:",  bi_row);
    cmb_text_align_ = new QComboBox(inner);
    cmb_text_align_->addItem("Align Left", 0);
    cmb_text_align_->addItem("Align Center", 1);
    cmb_text_align_->addItem("Align Right", 2);
    cmb_text_align_->setStyleSheet(cmb_font_->styleSheet());
    txfl->addRow("Alignment:", cmb_text_align_);
    txfl->addRow("Live edit:", chk_expose_text_);
    btn_text_color_ = new QPushButton(inner);
    btn_kf_text_color_ = mk_kf_button("Toggle text color keyframe");
    txfl->addRow("Color:", with_kf(btn_text_color_, btn_kf_text_color_));
    vl->addWidget(text_box_);

    /* ── Rectangle ── */
    rect_box_ = new QGroupBox("Rectangle", inner);
    rect_box_->setStyleSheet(tform_box->styleSheet());
    auto *rfl = new QFormLayout(rect_box_);
    rfl->setSpacing(3);
    spn_layer_w_ = mk_dspin(1.0, 9999.0, 10.0);
    spn_layer_h_ = mk_dspin(1.0, 9999.0, 10.0);
    spn_rect_corner_ = mk_dspin(0.0, 1000.0, 1.0);
    btn_kf_width_ = mk_kf_button("Toggle width keyframe");
    btn_kf_height_ = mk_kf_button("Toggle height keyframe");
    rfl->addRow("Width:", with_kf(spn_layer_w_, btn_kf_width_));
    rfl->addRow("Height:", with_kf(spn_layer_h_, btn_kf_height_));
    rfl->addRow("Corner:", spn_rect_corner_);
    btn_fill_color_ = new QPushButton(inner);
    btn_kf_fill_color_ = mk_kf_button("Toggle fill color keyframe");
    row_fill_color_ = with_kf(btn_fill_color_, btn_kf_fill_color_);
    rfl->addRow("Color:", row_fill_color_);
    vl->addWidget(rect_box_);

    /* ── Image ── */
    image_box_ = new QGroupBox("Image", inner);
    image_box_->setStyleSheet(tform_box->styleSheet());
    auto *ifl = new QFormLayout(image_box_);
    ifl->setSpacing(3);
    edit_image_path_ = new QLineEdit(inner);
    edit_image_path_->setStyleSheet(txt_content_->styleSheet());
    btn_pick_image_ = new QPushButton("Browse…", inner);
    btn_pick_image_->setStyleSheet("QPushButton{color:#fff;background:#0078d4;border:none;"
                                     "border-radius:3px;padding:3px 8px;}");
    spn_layer_w_->setToolTip("For image layers, this is the displayed width.");
    spn_layer_h_->setToolTip("For image layers, this is the displayed height.");
    chk_lock_aspect_ = new QCheckBox("Lock aspect ratio", inner);
    chk_lock_aspect_->setStyleSheet("color:#ccc;");
    ifl->addRow("Path:", edit_image_path_);
    ifl->addRow("", btn_pick_image_);
    ifl->addRow("", chk_lock_aspect_);
    vl->addWidget(image_box_);

    shadow_box_ = new QGroupBox("Drop Shadow", inner);
    shadow_box_->setStyleSheet(tform_box->styleSheet());
    auto *sfl = new QFormLayout(shadow_box_);
    sfl->setSpacing(3);
    chk_shadow_enabled_ = new QCheckBox("Enable shadow", inner);
    chk_shadow_enabled_->setStyleSheet("color:#ccc;");
    cmb_shadow_preset_ = new QComboBox(inner);
    cmb_shadow_preset_->addItems({"Custom", "Soft", "Medium", "Strong", "Broadcast"});
    cmb_shadow_preset_->setStyleSheet(cmb_font_->styleSheet());
    btn_shadow_color_ = new QPushButton(inner);
    spn_shadow_opacity_ = mk_dspin(0.0, 1.0, 0.05);
    spn_shadow_opacity_->setDecimals(2);
    spn_shadow_distance_ = mk_dspin(0.0, 200.0, 1.0);
    spn_shadow_angle_ = mk_dspin(-360.0, 360.0, 5.0);
    spn_shadow_blur_ = mk_dspin(0.0, 100.0, 1.0);
    spn_shadow_spread_ = mk_dspin(0.0, 100.0, 1.0);
    btn_kf_shadow_enabled_ = mk_kf_button("Toggle shadow enabled keyframe");
    btn_kf_shadow_color_ = mk_kf_button("Toggle shadow color keyframe");
    btn_kf_shadow_opacity_ = mk_kf_button("Toggle shadow opacity keyframe");
    btn_kf_shadow_distance_ = mk_kf_button("Toggle shadow distance keyframe");
    btn_kf_shadow_angle_ = mk_kf_button("Toggle shadow angle keyframe");
    btn_kf_shadow_blur_ = mk_kf_button("Toggle shadow blur keyframe");
    btn_kf_shadow_spread_ = mk_kf_button("Toggle shadow spread keyframe");
    sfl->addRow("", with_kf(chk_shadow_enabled_, btn_kf_shadow_enabled_));
    sfl->addRow("Preset:", cmb_shadow_preset_);
    sfl->addRow("Color:", with_kf(btn_shadow_color_, btn_kf_shadow_color_));
    sfl->addRow("Opacity:", with_kf(spn_shadow_opacity_, btn_kf_shadow_opacity_));
    sfl->addRow("Distance:", with_kf(spn_shadow_distance_, btn_kf_shadow_distance_));
    sfl->addRow("Angle:", with_kf(spn_shadow_angle_, btn_kf_shadow_angle_));
    sfl->addRow("Blur:", with_kf(spn_shadow_blur_, btn_kf_shadow_blur_));
    sfl->addRow("Spread:", with_kf(spn_shadow_spread_, btn_kf_shadow_spread_));
    vl->addWidget(shadow_box_);

    vl->addStretch();
    setWidget(inner);

    /* ── Connect signals → property_changed ── */
    auto emit_change = [this]() { if (!loading_values_) emit property_changed(); };
    auto can_edit = [this]() { return layer_ && !loading_values_; };
    auto local_time = [this]() {
        return layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                   std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
    };

    connect(spn_px_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->pos_x, local_time(), v); emit_change(); }
            });
    connect(spn_py_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->pos_y, local_time(), v); emit_change(); }
            });
    connect(spn_rot_,      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->rotation, local_time(), v); emit_change(); }
            });
    connect(spn_opacity_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->opacity, local_time(), v); emit_change(); }
            });
    connect(spn_origin_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { layer_->origin_x = (float)v; set_animated_value(layer_->origin_x_prop, local_time(), v); emit_change(); }
            });
    connect(spn_origin_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { layer_->origin_y = (float)v; set_animated_value(layer_->origin_y_prop, local_time(), v); emit_change(); }
            });
    connect(cmb_anchor_, QOverload<int>::of(&QComboBox::activated),
            this, [this, can_edit, local_time, emit_change](int idx) {
                if (!can_edit()) return;
                double t = local_time();
                QPointF next = anchor_point_from_index(idx);
                double w = eval_box_width(*layer_, t);
                double h = eval_box_height(*layer_, t);
                QPointF keep = rotated_scaled_delta((next.x() - layer_->origin_x) * w,
                                                    (next.y() - layer_->origin_y) * h,
                                                    layer_->rotation.evaluate(t),
                                                    layer_->scale_x.evaluate(t),
                                                    layer_->scale_y.evaluate(t));
                layer_->origin_x = (float)next.x();
                layer_->origin_y = (float)next.y();
                set_animated_value(layer_->origin_x_prop, t, next.x());
                set_animated_value(layer_->origin_y_prop, t, next.y());
                set_animated_value(layer_->pos_x, t, layer_->pos_x.evaluate(t) + keep.x());
                set_animated_value(layer_->pos_y, t, layer_->pos_y.evaluate(t) + keep.y());
                load_values();
                emit_change();
            });
    connect(txt_content_, &QLineEdit::textChanged,
            this, [this, can_edit, emit_change](const QString &s){
                if (can_edit()) { layer_->text_content = s.toStdString(); emit_change(); }
            });
    connect(cmb_font_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change](const QString &s){
                if (can_edit()) { layer_->font_family = s.toStdString(); emit_change(); }
            });
    connect(spn_size_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, emit_change](int v){
                if (can_edit()) { layer_->font_size = v; emit_change(); }
            });
    connect(chk_bold_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->font_bold = v; emit_change(); }
            });
    connect(chk_italic_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->font_italic = v; emit_change(); }
            });
    connect(chk_expose_text_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->expose_text = v; emit_change(); }
            });
    connect(cmb_text_align_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->align_h = cmb_text_align_->itemData(idx).toInt(); emit_change(); }
            });
    connect(btn_text_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(eval_text_color(*layer_, local_time()));
                QColor picked = QColorDialog::getColor(initial, this, "Text Color",
                                                        QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                layer_->text_color = argb_from_color(picked);
                set_color_channels_at(*layer_, true, local_time(), layer_->text_color);
                style_color_button(btn_text_color_, layer_->text_color);
                emit_change();
            });
    connect(chk_shadow_enabled_, &QCheckBox::toggled, this, [this, can_edit, local_time, emit_change](bool v) {
        if (can_edit()) {
            layer_->shadow_enabled = v;
            set_animated_value(layer_->shadow_enabled_prop, local_time(), v ? 1.0 : 0.0);
            emit_change();
        }
    });
    connect(cmb_shadow_preset_, QOverload<int>::of(&QComboBox::activated), this, [this, can_edit, local_time, emit_change](int idx) {
        if (!can_edit() || idx <= 0) return;
        static const struct { float opacity, distance, blur, spread, angle; uint32_t color; } presets[] = {
            {0.35f, 5.0f, 8.0f, 1.0f, 135.0f, 0x99000000}, {0.55f, 8.0f, 5.0f, 2.0f, 135.0f, 0xAA000000},
            {0.75f, 12.0f, 3.0f, 3.0f, 135.0f, 0xCC000000}, {0.65f, 10.0f, 4.0f, 4.0f, 135.0f, 0xCC001428},
        };
        const auto &p = presets[std::clamp(idx - 1, 0, 3)];
        double t = local_time();
        layer_->shadow_enabled = true;
        layer_->shadow_opacity = p.opacity;
        layer_->shadow_distance = p.distance;
        layer_->shadow_blur = p.blur;
        layer_->shadow_spread = p.spread;
        layer_->shadow_angle = p.angle;
        layer_->shadow_color = p.color;
        set_animated_value(layer_->shadow_enabled_prop, t, 1.0);
        set_animated_value(layer_->shadow_opacity_prop, t, p.opacity);
        set_animated_value(layer_->shadow_distance_prop, t, p.distance);
        set_animated_value(layer_->shadow_blur_prop, t, p.blur);
        set_animated_value(layer_->shadow_spread_prop, t, p.spread);
        set_animated_value(layer_->shadow_angle_prop, t, p.angle);
        set_shadow_color_channels_at(*layer_, t, p.color);
        load_values(); emit_change();
    });
    connect(btn_shadow_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        QColor picked = QColorDialog::getColor(color_from_argb(eval_shadow_color(*layer_, local_time())), this, "Shadow Color", QColorDialog::ShowAlphaChannel);
        if (!picked.isValid()) return;
        layer_->shadow_color = argb_from_color(picked);
        set_shadow_color_channels_at(*layer_, local_time(), layer_->shadow_color);
        style_color_button(btn_shadow_color_, layer_->shadow_color);
        emit_change();
    });
    connect(spn_shadow_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                layer_->shadow_opacity = (float)v;
                set_animated_value(layer_->shadow_opacity_prop, local_time(), v);
                emit_change();
            });
    connect(spn_shadow_distance_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                layer_->shadow_distance = (float)v;
                set_animated_value(layer_->shadow_distance_prop, local_time(), v);
                emit_change();
            });
    connect(spn_shadow_angle_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                layer_->shadow_angle = (float)v;
                set_animated_value(layer_->shadow_angle_prop, local_time(), v);
                emit_change();
            });
    connect(spn_shadow_blur_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                layer_->shadow_blur = (float)v;
                set_animated_value(layer_->shadow_blur_prop, local_time(), v);
                emit_change();
            });
    connect(spn_shadow_spread_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                layer_->shadow_spread = (float)v;
                set_animated_value(layer_->shadow_spread_prop, local_time(), v);
                emit_change();
            });

    connect(spn_layer_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                double t = local_time();
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_value(layer_->box_width, t, v);
                if (layer_->type == LayerType::Image && layer_->lock_aspect_ratio && old_h > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_value(layer_->box_height, t, layer_->rect_height);
                    QSignalBlocker block(spn_layer_h_);
                    spn_layer_h_->setValue(layer_->rect_height);
                }
                emit_change();
            });
    connect(spn_layer_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                double t = local_time();
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_value(layer_->box_height, t, v);
                if (layer_->type == LayerType::Image && layer_->lock_aspect_ratio && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_value(layer_->box_width, t, layer_->rect_width);
                    QSignalBlocker block(spn_layer_w_);
                    spn_layer_w_->setValue(layer_->rect_width);
                }
                emit_change();
            });
    connect(spn_rect_corner_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (can_edit()) { layer_->corner_radius = (float)v; emit_change(); }
            });
    connect(btn_fill_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(eval_fill_color(*layer_, local_time()));
                QColor picked = QColorDialog::getColor(initial, this, "Fill Color",
                                                        QColorDialog::ShowAlphaChannel);
                if (!picked.isValid()) return;
                layer_->fill_color = argb_from_color(picked);
                set_color_channels_at(*layer_, false, local_time(), layer_->fill_color);
                style_color_button(btn_fill_color_, layer_->fill_color);
                emit_change();
            });
    connect(edit_image_path_, &QLineEdit::textChanged,
            this, [this, can_edit, emit_change](const QString &path){
                if (can_edit()) { layer_->image_path = path.toStdString(); emit_change(); }
            });
    connect(chk_lock_aspect_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->lock_aspect_ratio = v; emit_change(); }
            });
    connect(btn_pick_image_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QString path = QFileDialog::getOpenFileName(
                    this, "Choose Image",
                    QString::fromStdString(layer_->image_path),
                    "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)");
                if (path.isEmpty()) return;
                layer_->image_path = path.toStdString();
                QImage img(path);
                if (!img.isNull()) {
                    double t = local_time();
                    layer_->rect_width = (float)img.width();
                    layer_->rect_height = (float)img.height();
                    set_animated_value(layer_->box_width, t, layer_->rect_width);
                    set_animated_value(layer_->box_height, t, layer_->rect_height);
                }
                load_values();
                emit_change();
            });

    connect(btn_kf_pos_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->pos_x, local_time(), spn_px_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_pos_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->pos_y, local_time(), spn_py_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_rotation_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->rotation, local_time(), spn_rot_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_opacity_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->opacity, local_time(), spn_opacity_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_origin_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->origin_x_prop, local_time(), spn_origin_x_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_origin_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->origin_y_prop, local_time(), spn_origin_y_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_width_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->box_width, local_time(), spn_layer_w_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_height_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->box_height, local_time(), spn_layer_h_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_text_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_text_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->text_color_a, &layer_->text_color_r,
                                  &layer_->text_color_g, &layer_->text_color_b}, t)) {
            remove_keyframe_at(layer_->text_color_a, t);
            remove_keyframe_at(layer_->text_color_r, t);
            remove_keyframe_at(layer_->text_color_g, t);
            remove_keyframe_at(layer_->text_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->text_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });

    connect(btn_kf_shadow_enabled_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->shadow_enabled_prop, local_time(), chk_shadow_enabled_->isChecked() ? 1.0 : 0.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_shadow_opacity_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->shadow_opacity_prop, local_time(), spn_shadow_opacity_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_shadow_distance_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->shadow_distance_prop, local_time(), spn_shadow_distance_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_shadow_angle_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->shadow_angle_prop, local_time(), spn_shadow_angle_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_shadow_blur_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->shadow_blur_prop, local_time(), spn_shadow_blur_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_shadow_spread_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->shadow_spread_prop, local_time(), spn_shadow_spread_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_shadow_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_shadow_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->shadow_color_a, &layer_->shadow_color_r,
                                  &layer_->shadow_color_g, &layer_->shadow_color_b}, t)) {
            remove_keyframe_at(layer_->shadow_color_a, t);
            remove_keyframe_at(layer_->shadow_color_r, t);
            remove_keyframe_at(layer_->shadow_color_g, t);
            remove_keyframe_at(layer_->shadow_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->shadow_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->shadow_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->shadow_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->shadow_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });
    connect(btn_kf_fill_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_fill_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->fill_color_a, &layer_->fill_color_r,
                                  &layer_->fill_color_g, &layer_->fill_color_b}, t)) {
            remove_keyframe_at(layer_->fill_color_a, t);
            remove_keyframe_at(layer_->fill_color_r, t);
            remove_keyframe_at(layer_->fill_color_g, t);
            remove_keyframe_at(layer_->fill_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->fill_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });
}

void PropertiesPanel::set_title(std::shared_ptr<Title> t)
{
    title_ = t;
}

void PropertiesPanel::set_layer(std::shared_ptr<Layer> layer, double t)
{
    layer_    = layer;
    playhead_ = t;
    load_values();
}

void PropertiesPanel::load_values()
{
    loading_values_ = true;
    if (!layer_) {
        text_box_->setVisible(false);
        rect_box_->setVisible(false);
        image_box_->setVisible(false);
        spn_px_->setValue(0.0);
        spn_py_->setValue(0.0);
        spn_rot_->setValue(0.0);
        spn_opacity_->setValue(1.0);
        spn_origin_x_->setValue(0.5);
        spn_origin_y_->setValue(0.5);
        chk_lock_aspect_->setChecked(true);
        txt_content_->clear();
        edit_image_path_->clear();
        style_color_button(btn_text_color_, 0xFFFFFFFF);
        style_color_button(btn_fill_color_, 0xFF222222);
        spn_layer_w_->setValue(1.0);
        spn_layer_h_->setValue(1.0);
        spn_rect_corner_->setValue(0.0);
        spn_size_->setValue(72);
        chk_bold_->setChecked(false);
        chk_italic_->setChecked(false);
        if (cmb_text_align_) cmb_text_align_->setCurrentIndex(1);
        if (cmb_anchor_) cmb_anchor_->setCurrentIndex(4);
        if (chk_shadow_enabled_) chk_shadow_enabled_->setChecked(false);
        if (cmb_shadow_preset_) cmb_shadow_preset_->setCurrentIndex(0);
        if (btn_shadow_color_) style_color_button(btn_shadow_color_, 0x99000000);
        if (spn_shadow_opacity_) spn_shadow_opacity_->setValue(0.6);
        if (spn_shadow_distance_) spn_shadow_distance_->setValue(8.0);
        if (spn_shadow_angle_) spn_shadow_angle_->setValue(135.0);
        if (spn_shadow_blur_) spn_shadow_blur_->setValue(4.0);
        if (spn_shadow_spread_) spn_shadow_spread_->setValue(0.0);
        for (auto *b : {btn_kf_pos_x_, btn_kf_pos_y_, btn_kf_rotation_, btn_kf_opacity_,
                        btn_kf_origin_x_, btn_kf_origin_y_, btn_kf_width_, btn_kf_height_,
                        btn_kf_text_color_, btn_kf_fill_color_})
            if (b) b->setText("◇");
        loading_values_ = false;
        return;
    }

    const bool is_text = layer_->type == LayerType::Text;
    const bool is_rect = layer_->type == LayerType::SolidRect;
    const bool is_image = layer_->type == LayerType::Image;
    text_box_->setVisible(is_text);
    rect_box_->setVisible(is_text || is_rect || is_image);
    rect_box_->setTitle(is_text ? "Text Box" : (is_image ? "Image Size" : "Rectangle"));
    spn_rect_corner_->setVisible(is_rect);
    btn_fill_color_->setVisible(is_rect);
    btn_kf_text_color_->setVisible(is_text);
    btn_kf_fill_color_->setVisible(is_rect);
    if (row_fill_color_) row_fill_color_->setVisible(is_rect);
    if (auto *form = qobject_cast<QFormLayout *>(rect_box_->layout())) {
        if (auto *label = form->labelForField(spn_rect_corner_))
            label->setVisible(is_rect);
        if (auto *label = form->labelForField(row_fill_color_))
            label->setVisible(is_rect);
    }
    image_box_->setVisible(is_image);

    double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                           std::max(0.0, layer_->out_time - layer_->in_time));
    spn_px_->setValue(layer_->pos_x.is_animated()
                      ? layer_->pos_x.evaluate(lt)
                      : layer_->pos_x.static_value);
    spn_py_->setValue(layer_->pos_y.is_animated()
                      ? layer_->pos_y.evaluate(lt)
                      : layer_->pos_y.static_value);
    spn_rot_->setValue(layer_->rotation.is_animated()
                       ? layer_->rotation.evaluate(lt)
                       : layer_->rotation.static_value);
    spn_opacity_->setValue(layer_->opacity.is_animated()
                           ? layer_->opacity.evaluate(lt)
                           : layer_->opacity.static_value);
    spn_origin_x_->setValue(eval_origin_x(*layer_, lt));
    spn_origin_y_->setValue(eval_origin_y(*layer_, lt));
    cmb_anchor_->setCurrentIndex(anchor_index_from_layer(*layer_));

    spn_layer_w_->setValue(eval_box_width(*layer_, lt));
    spn_layer_h_->setValue(eval_box_height(*layer_, lt));
    spn_rect_corner_->setValue(layer_->corner_radius);
    edit_image_path_->setText(QString::fromStdString(layer_->image_path));
    chk_lock_aspect_->setChecked(layer_->lock_aspect_ratio);
    style_color_button(btn_text_color_, eval_text_color(*layer_, lt));
    style_color_button(btn_fill_color_, eval_fill_color(*layer_, lt));

    auto set_kf_icon = [](QPushButton *button, bool active) {
        if (!button) return;
        button->setText(active ? "◆" : "◇");
        button->setProperty("active", active);
    };
    set_kf_icon(btn_kf_pos_x_, keyframe_at_time(layer_->pos_x, lt));
    set_kf_icon(btn_kf_pos_y_, keyframe_at_time(layer_->pos_y, lt));
    set_kf_icon(btn_kf_rotation_, keyframe_at_time(layer_->rotation, lt));
    set_kf_icon(btn_kf_opacity_, keyframe_at_time(layer_->opacity, lt));
    set_kf_icon(btn_kf_origin_x_, keyframe_at_time(layer_->origin_x_prop, lt));
    set_kf_icon(btn_kf_origin_y_, keyframe_at_time(layer_->origin_y_prop, lt));
    set_kf_icon(btn_kf_width_, keyframe_at_time(layer_->box_width, lt));
    set_kf_icon(btn_kf_height_, keyframe_at_time(layer_->box_height, lt));
    set_kf_icon(btn_kf_text_color_, any_keyframe_at_time({&layer_->text_color_a, &layer_->text_color_r,
                                                          &layer_->text_color_g, &layer_->text_color_b}, lt));
    set_kf_icon(btn_kf_fill_color_, any_keyframe_at_time({&layer_->fill_color_a, &layer_->fill_color_r,
                                                          &layer_->fill_color_g, &layer_->fill_color_b}, lt));
    set_kf_icon(btn_kf_shadow_enabled_, keyframe_at_time(layer_->shadow_enabled_prop, lt));
    set_kf_icon(btn_kf_shadow_opacity_, keyframe_at_time(layer_->shadow_opacity_prop, lt));
    set_kf_icon(btn_kf_shadow_distance_, keyframe_at_time(layer_->shadow_distance_prop, lt));
    set_kf_icon(btn_kf_shadow_angle_, keyframe_at_time(layer_->shadow_angle_prop, lt));
    set_kf_icon(btn_kf_shadow_blur_, keyframe_at_time(layer_->shadow_blur_prop, lt));
    set_kf_icon(btn_kf_shadow_spread_, keyframe_at_time(layer_->shadow_spread_prop, lt));
    set_kf_icon(btn_kf_shadow_color_, any_keyframe_at_time({&layer_->shadow_color_a, &layer_->shadow_color_r,
                                                            &layer_->shadow_color_g, &layer_->shadow_color_b}, lt));

    txt_content_->setText(QString::fromStdString(layer_->text_content));
    int fi = cmb_font_->findText(QString::fromStdString(layer_->font_family));
    if (fi >= 0) cmb_font_->setCurrentIndex(fi);
    spn_size_->setValue(layer_->font_size);
    chk_bold_->setChecked(layer_->font_bold);
    chk_italic_->setChecked(layer_->font_italic);
    chk_expose_text_->setChecked(layer_->expose_text);
    int ai = cmb_text_align_->findData(layer_->align_h);
    cmb_text_align_->setCurrentIndex(ai >= 0 ? ai : 1);

    chk_shadow_enabled_->setChecked(eval_shadow_enabled(*layer_, lt));
    cmb_shadow_preset_->setCurrentIndex(0);
    style_color_button(btn_shadow_color_, eval_shadow_color(*layer_, lt));
    spn_shadow_opacity_->setValue(eval_shadow_opacity(*layer_, lt));
    spn_shadow_distance_->setValue(eval_shadow_distance(*layer_, lt));
    spn_shadow_angle_->setValue(eval_shadow_angle(*layer_, lt));
    spn_shadow_blur_->setValue(eval_shadow_blur(*layer_, lt));
    spn_shadow_spread_->setValue(eval_shadow_spread(*layer_, lt));

    QFontDatabase fdb;
    cmb_font_->setToolTip(fdb.families().contains(QString::fromStdString(layer_->font_family))
        ? QString()
        : QString("Font '%1' is not installed in Qt; OBS/Pango may substitute it.").arg(QString::fromStdString(layer_->font_family)));

    loading_values_ = false;
}
